use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use semver::Version;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::net::{IpAddr, ToSocketAddrs};
#[cfg(windows)]
use std::os::windows::ffi::OsStrExt;
use std::path::{Component, Path, PathBuf};
use std::time::Duration;
use zip::ZipArchive;

const CATALOG_SCHEMA_VERSION: u32 = 1;
const INSTALL_SCHEMA_VERSION: u32 = 1;
const OFFLINE_PACKAGE_SCHEMA_VERSION: u32 = 2;
const OFFLINE_PACKAGE_MANIFEST: &str = "mojorecomp-package.toml";
const OFFLINE_PACKAGE_SIGNATURE: &str = "mojorecomp-package.sig";
const LOCALIZATION_PACK_SCHEMA_VERSION: u32 = 1;
const LOCALIZATION_PACK_MANIFEST: &str = "localization-pack.toml";
const LOCALIZATION_PACK_SIGNATURE: &str = "localization-pack.sig";
const LAUNCHER_EXE_SIGNATURE_MAGIC: &[u8] = b"MOJORECOMP-LAUNCHER-SIG-V1";
const LAUNCHER_EXE_SIGNATURE_DOMAIN: &[u8] = b"MojoRecomp launcher executable signature v1\0";
const RELEASE_SIGNING_PUBLIC_KEY_HEX: &str = include_str!("../../release-signing-public-key.hex");
const UPDATE_DISK_MARGIN_BYTES: u64 = 128 * 1024 * 1024;
pub const COT_RUNTIME_ENTRYPOINT: &str = "cot-runtime.exe";
pub const COT_RUNTIME_REQUIRED_FILES: [&str; 6] = [
    COT_RUNTIME_ENTRYPOINT,
    "dxcompiler.dll",
    "dxil.dll",
    "mojorecomp-ffmpeg.dll",
    "mojorecomp-lzx.dll",
    "extract-xiso.exe",
];

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UpdateCatalog {
    pub schema_version: u32,
    pub channel: String,
    #[serde(rename = "release")]
    pub releases: Vec<ComponentRelease>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LocalizationCatalog {
    pub schema_version: u32,
    pub game_id: String,
    pub runtime_version: String,
    pub pack_version: String,
    pub url: String,
    pub size: u64,
    pub sha256: String,
    pub published: String,
    pub notes_url: String,
    pub min_launcher: String,
    #[serde(rename = "language")]
    pub languages: Vec<LocalizationCatalogLanguage>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LocalizationCatalogLanguage {
    pub id: String,
    pub game_id: String,
    pub locale: String,
    pub display_name: String,
    pub xbox_language: u32,
    pub version: String,
    pub component_size: u64,
    pub component_sha256: String,
    pub unpacked_size: u64,
    pub required_files: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum ComponentKind {
    Launcher,
    Runtime,
    Language,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum PackageFormat {
    Zip,
    PortableExe,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Compatibility {
    #[serde(default)]
    pub min_launcher: Option<String>,
    #[serde(default)]
    pub max_launcher: Option<String>,
    #[serde(default, rename = "require")]
    pub requirements: Vec<ComponentRequirement>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ComponentRequirement {
    pub id: String,
    #[serde(default)]
    pub min_version: Option<String>,
    #[serde(default)]
    pub max_version: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ComponentRelease {
    pub id: String,
    pub kind: ComponentKind,
    pub version: String,
    pub platform: String,
    pub arch: String,
    pub url: String,
    pub size: u64,
    pub sha256: String,
    pub published: String,
    pub notes_url: String,
    pub package: PackageFormat,
    #[serde(default)]
    pub unpacked_size: Option<u64>,
    #[serde(default)]
    pub entrypoint: Option<String>,
    #[serde(default)]
    pub required_files: Vec<String>,
    #[serde(default)]
    pub game_id: Option<String>,
    #[serde(default)]
    pub locale: Option<String>,
    #[serde(default)]
    pub display_name: Option<String>,
    #[serde(default)]
    pub xbox_language: Option<u32>,
    #[serde(default)]
    pub localization_pack: Option<LocalizationPackReference>,
    #[serde(default)]
    pub localization_catalog_url: Option<String>,
    pub compatibility: Compatibility,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LocalizationPackReference {
    pub version: String,
    pub component_size: u64,
    pub component_sha256: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct OfflinePackageManifest {
    schema_version: u32,
    id: String,
    kind: ComponentKind,
    version: String,
    platform: String,
    arch: String,
    package: PackageFormat,
    #[serde(default)]
    entrypoint: Option<String>,
    #[serde(default)]
    required_files: Vec<String>,
    #[serde(default)]
    game_id: Option<String>,
    #[serde(default)]
    locale: Option<String>,
    #[serde(default)]
    display_name: Option<String>,
    #[serde(default)]
    xbox_language: Option<u32>,
    #[serde(default)]
    min_launcher: Option<String>,
    #[serde(default)]
    max_launcher: Option<String>,
    #[serde(default, rename = "require")]
    requirements: Vec<ComponentRequirement>,
    files: Vec<OfflinePackageFile>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct OfflinePackageFile {
    path: String,
    size: u64,
    sha256: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct LocalizationPackManifest {
    schema_version: u32,
    game_id: String,
    version: String,
    #[serde(rename = "language")]
    languages: Vec<LocalizationPackLanguage>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct LocalizationPackLanguage {
    id: String,
    locale: String,
    display_name: String,
    xbox_language: u32,
    version: String,
    file: String,
    size: u64,
    sha256: String,
}

#[derive(Clone, Debug)]
pub struct OfflineLocalizationPack {
    pub game_id: String,
    pub version: String,
    pub languages: Vec<OfflineLocalizationPackLanguage>,
}

#[derive(Clone, Debug)]
pub struct OfflineLocalizationPackLanguage {
    pub id: String,
    pub locale: String,
    pub display_name: String,
    pub xbox_language: u32,
    pub version: String,
    pub package_path: PathBuf,
}

#[derive(Clone, Debug)]
pub struct InstalledComponent {
    pub id: String,
    pub version: String,
    pub healthy: bool,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum PlanState {
    UpToDate,
    UpdateAvailable,
    Available,
    Corrupted,
    RepairUnavailable,
    Incompatible,
}

#[derive(Clone, Debug, Serialize)]
pub struct ComponentPlan {
    pub id: String,
    pub kind: ComponentKind,
    pub game_id: Option<String>,
    pub locale: Option<String>,
    pub display_name: Option<String>,
    pub xbox_language: Option<u32>,
    pub installed_version: Option<String>,
    pub latest_version: Option<String>,
    pub state: PlanState,
    pub download_url: Option<String>,
    pub size: Option<u64>,
    pub published: Option<String>,
    pub notes_url: Option<String>,
}

pub fn parse_and_validate_catalog(text: &str) -> Result<UpdateCatalog, String> {
    let catalog: UpdateCatalog =
        toml::from_str(text).map_err(|error| format!("Update catalog is invalid TOML: {error}"))?;
    validate_catalog(&catalog)?;
    Ok(catalog)
}

pub fn parse_and_validate_localization_catalog(text: &str) -> Result<LocalizationCatalog, String> {
    let catalog: LocalizationCatalog = toml::from_str(text)
        .map_err(|error| format!("Localization catalog is invalid TOML: {error}"))?;
    if catalog.schema_version != 2 {
        return Err(format!(
            "Unsupported localization catalog schema version: {}",
            catalog.schema_version
        ));
    }
    validate_game_id(&catalog.game_id)?;
    Version::parse(&catalog.runtime_version)
        .map_err(|_| "Localization catalog has an invalid runtime version".to_string())?;
    Version::parse(&catalog.pack_version)
        .map_err(|_| "Localization catalog has an invalid pack version".to_string())?;
    validate_public_https_url(&catalog.url)?;
    if catalog.size == 0 {
        return Err("Localization catalog has an empty pack size".into());
    }
    validate_sha256(&catalog.sha256)?;
    validate_date(&catalog.published)?;
    validate_public_https_url(&catalog.notes_url)?;
    Version::parse(&catalog.min_launcher)
        .map_err(|_| "Localization catalog has an invalid minimum launcher version".to_string())?;
    if catalog.languages.is_empty() || catalog.languages.len() > 256 {
        return Err("Localization catalog must contain between 1 and 256 languages".into());
    }
    let mut ids = HashSet::new();
    let mut game_locales = HashSet::new();
    for language in &catalog.languages {
        validate_component_id(&language.id)?;
        validate_game_id(&language.game_id)?;
        if language.game_id != catalog.game_id {
            return Err(format!(
                "Localization catalog language {} belongs to a different game",
                language.id
            ));
        }
        validate_locale(&language.locale)?;
        Version::parse(&language.version).map_err(|_| {
            format!(
                "Localization catalog language {} has an invalid version",
                language.id
            )
        })?;
        if language.id
            != format!(
                "language.{}.{}",
                language.game_id,
                language.locale.to_ascii_lowercase()
            )
        {
            return Err(format!(
                "Localization catalog language ID does not match game/locale: {}",
                language.id
            ));
        }
        if language.display_name.trim().is_empty() || language.display_name.len() > 80 {
            return Err(format!(
                "Localization catalog language {} has an invalid display name",
                language.id
            ));
        }
        if language.xbox_language == 0 || language.xbox_language > 255 {
            return Err(format!(
                "Localization catalog language {} has an invalid Xbox language",
                language.id
            ));
        }
        if language.component_size == 0 || language.unpacked_size == 0 {
            return Err(format!(
                "Localization catalog language {} has an invalid package size",
                language.id
            ));
        }
        validate_sha256(&language.component_sha256)?;
        if language.required_files.is_empty() {
            return Err(format!(
                "Localization catalog language {} has no required files",
                language.id
            ));
        }
        for path in &language.required_files {
            safe_relative_path(path)?;
        }
        if !language
            .required_files
            .iter()
            .any(|path| path == OFFLINE_PACKAGE_MANIFEST)
            || !language
                .required_files
                .iter()
                .any(|path| path == OFFLINE_PACKAGE_SIGNATURE)
        {
            return Err(format!(
                "Localization catalog language {} is missing signed package metadata",
                language.id
            ));
        }
        if !ids.insert(language.id.to_ascii_lowercase())
            || !game_locales.insert((
                language.game_id.to_ascii_lowercase(),
                language.locale.to_ascii_lowercase(),
            ))
        {
            return Err("Localization catalog contains duplicate language metadata".into());
        }
    }
    Ok(catalog)
}

pub fn apply_localization_catalog_metadata(
    catalog: &mut UpdateCatalog,
    localization_catalog: &LocalizationCatalog,
) -> Result<(), String> {
    let mut existing = catalog
        .releases
        .iter()
        .map(|release| (release.id.clone(), release.version.clone()))
        .collect::<HashSet<_>>();
    for language in &localization_catalog.languages {
        if !existing.insert((language.id.clone(), language.version.clone())) {
            if let Some(release) = catalog.releases.iter_mut().find(|release| {
                release.id == language.id && release.version == language.version
            }) {
                release.display_name = Some(language.display_name.clone());
                release.xbox_language = Some(language.xbox_language);
            }
            continue;
        }
        catalog.releases.push(ComponentRelease {
            id: language.id.clone(),
            kind: ComponentKind::Language,
            version: language.version.clone(),
            platform: "windows".into(),
            arch: "x86_64".into(),
            url: localization_catalog.url.clone(),
            size: localization_catalog.size,
            sha256: localization_catalog.sha256.clone(),
            published: localization_catalog.published.clone(),
            notes_url: localization_catalog.notes_url.clone(),
            package: PackageFormat::Zip,
            unpacked_size: Some(language.unpacked_size),
            entrypoint: None,
            required_files: language.required_files.clone(),
            game_id: Some(language.game_id.clone()),
            locale: Some(language.locale.clone()),
            display_name: Some(language.display_name.clone()),
            xbox_language: Some(language.xbox_language),
            localization_pack: Some(LocalizationPackReference {
                version: localization_catalog.pack_version.clone(),
                component_size: language.component_size,
                component_sha256: language.component_sha256.clone(),
            }),
            localization_catalog_url: None,
            compatibility: Compatibility {
                min_launcher: Some(localization_catalog.min_launcher.clone()),
                max_launcher: None,
                requirements: vec![ComponentRequirement {
                    id: format!("runtime.{}", language.game_id),
                    min_version: Some(localization_catalog.runtime_version.clone()),
                    max_version: None,
                }],
            },
        });
    }
    apply_builtin_language_metadata(catalog);
    validate_catalog(catalog)
}

pub fn apply_builtin_language_metadata(catalog: &mut UpdateCatalog) {
    for release in &mut catalog.releases {
        if release.kind == ComponentKind::Language
            && release.id == "language.cot.pt-br"
            && release.locale.as_deref() == Some("pt-BR")
        {
            release
                .display_name
                .get_or_insert_with(|| "Brazilian Portuguese".into());
            release.xbox_language.get_or_insert(1);
        }
    }
}

pub fn validate_catalog(catalog: &UpdateCatalog) -> Result<(), String> {
    if catalog.schema_version != CATALOG_SCHEMA_VERSION {
        return Err(format!(
            "Unsupported update catalog schema version: {}",
            catalog.schema_version
        ));
    }
    if !matches!(catalog.channel.as_str(), "stable" | "beta" | "development") {
        return Err("Update catalog channel must be stable, beta, or development".into());
    }
    if catalog.releases.is_empty() {
        return Err("Update catalog contains no component releases".into());
    }
    let mut seen = HashSet::new();
    for release in &catalog.releases {
        validate_release(release)?;
        let key = (
            release.id.clone(),
            release.version.clone(),
            release.platform.clone(),
            release.arch.clone(),
        );
        if !seen.insert(key) {
            return Err(format!(
                "Duplicate release {} {} for {}-{}",
                release.id, release.version, release.platform, release.arch
            ));
        }
    }
    Ok(())
}

fn validate_release(release: &ComponentRelease) -> Result<(), String> {
    validate_release_installable(release)?;
    validate_public_https_url(&release.url)?;
    if let Some(url) = release.localization_catalog_url.as_deref() {
        validate_public_https_url(url)?;
    }
    validate_date(&release.published)?;
    validate_public_https_url(&release.notes_url)?;
    Ok(())
}

fn validate_release_installable(release: &ComponentRelease) -> Result<(), String> {
    validate_component_id(&release.id)?;
    Version::parse(&release.version)
        .map_err(|_| format!("Component {} has an invalid semantic version", release.id))?;
    if release.platform != "windows" || release.arch != "x86_64" {
        return Err(format!(
            "Component {} must target windows-x86_64",
            release.id
        ));
    }
    if release.size == 0 {
        return Err(format!(
            "Component {} has an empty artifact size",
            release.id
        ));
    }
    validate_sha256(&release.sha256)?;
    validate_compatibility(&release.compatibility)?;
    for path in &release.required_files {
        safe_relative_path(path)?;
    }
    match release.kind {
        ComponentKind::Launcher => {
            if release.id != "launcher"
                || release.game_id.is_some()
                || release.locale.is_some()
                || release.display_name.is_some()
                || release.xbox_language.is_some()
                || release.entrypoint.is_some()
                || release.unpacked_size.is_some()
                || release.localization_pack.is_some()
                || release.localization_catalog_url.is_some()
                || release.package != PackageFormat::PortableExe
                || !release.required_files.is_empty()
            {
                return Err("Launcher releases must use the launcher executable schema".into());
            }
        }
        ComponentKind::Runtime => {
            let game = release
                .game_id
                .as_deref()
                .ok_or_else(|| format!("Runtime {} is missing game_id", release.id))?;
            validate_game_id(game)?;
            if release.id != format!("runtime.{game}")
                || release.locale.is_some()
                || release.display_name.is_some()
                || release.xbox_language.is_some()
                || release.localization_pack.is_some()
            {
                return Err(format!(
                    "Runtime component ID does not match game_id: {}",
                    release.id
                ));
            }
            validate_zip_release(release)?;
            let entrypoint = release
                .entrypoint
                .as_deref()
                .ok_or_else(|| format!("Runtime {} is missing entrypoint", release.id))?;
            safe_relative_path(entrypoint)?;
            if !entrypoint.to_ascii_lowercase().ends_with(".exe") {
                return Err(format!(
                    "Runtime {} entrypoint must be an executable",
                    release.id
                ));
            }
            if !release.required_files.iter().any(|path| path == entrypoint) {
                return Err(format!(
                    "Runtime {} entrypoint must be listed in required_files",
                    release.id
                ));
            }
            if game == "cot"
                && (entrypoint != COT_RUNTIME_ENTRYPOINT
                    || COT_RUNTIME_REQUIRED_FILES.iter().any(|required| {
                        !release.required_files.iter().any(|path| path == required)
                    }))
            {
                return Err(
                    "Crash of the Titans runtime packages must contain the complete runtime payload"
                        .into(),
                );
            }
        }
        ComponentKind::Language => {
            let game = release
                .game_id
                .as_deref()
                .ok_or_else(|| format!("Language component {} is missing game_id", release.id))?;
            validate_game_id(game)?;
            let locale = release
                .locale
                .as_deref()
                .ok_or_else(|| format!("Language component {} is missing locale", release.id))?;
            validate_locale(locale)?;
            if release.display_name.as_deref().is_some_and(|display_name| {
                display_name.trim().is_empty() || display_name.len() > 80
            }) {
                return Err(format!(
                    "Language component {} has an invalid display_name",
                    release.id
                ));
            }
            if release
                .xbox_language
                .is_some_and(|xbox_language| xbox_language == 0 || xbox_language > 255)
            {
                return Err(format!(
                    "Language component {} has an invalid xbox_language",
                    release.id
                ));
            }
            if release.id != format!("language.{game}.{}", locale.to_ascii_lowercase())
                || release.entrypoint.is_some()
                || release.localization_catalog_url.is_some()
            {
                return Err(format!(
                    "Language component ID does not match game_id/locale: {}",
                    release.id
                ));
            }
            if let Some(pack) = &release.localization_pack {
                Version::parse(&pack.version).map_err(|_| {
                    format!(
                        "Language component {} has an invalid Localization Pack version",
                        release.id
                    )
                })?;
                if pack.component_size == 0 {
                    return Err(format!(
                        "Language component {} has an empty Localization Pack component size",
                        release.id
                    ));
                }
                validate_sha256(&pack.component_sha256)?;
            }
            validate_zip_release(release)?;
        }
    }
    Ok(())
}

fn validate_zip_release(release: &ComponentRelease) -> Result<(), String> {
    if release.package != PackageFormat::Zip {
        return Err(format!("Component {} must use a zip package", release.id));
    }
    if release.unpacked_size.unwrap_or(0) == 0 {
        return Err(format!(
            "Component {} must declare unpacked_size",
            release.id
        ));
    }
    if release.required_files.is_empty() {
        return Err(format!(
            "Component {} must declare required_files",
            release.id
        ));
    }
    Ok(())
}

fn validate_compatibility(value: &Compatibility) -> Result<(), String> {
    let min_launcher = value
        .min_launcher
        .as_deref()
        .map(Version::parse)
        .transpose()
        .map_err(|_| "min_launcher is not a semantic version".to_string())?;
    let max_launcher = value
        .max_launcher
        .as_deref()
        .map(Version::parse)
        .transpose()
        .map_err(|_| "max_launcher is not a semantic version".to_string())?;
    if let (Some(min), Some(max)) = (&min_launcher, &max_launcher)
        && min > max
    {
        return Err("min_launcher is greater than max_launcher".into());
    }
    let mut ids = HashSet::new();
    for requirement in &value.requirements {
        validate_component_id(&requirement.id)?;
        if !ids.insert(requirement.id.clone()) {
            return Err(format!(
                "Duplicate compatibility requirement: {}",
                requirement.id
            ));
        }
        let min = requirement
            .min_version
            .as_deref()
            .map(Version::parse)
            .transpose()
            .map_err(|_| format!("Invalid minimum version for {}", requirement.id))?;
        let max = requirement
            .max_version
            .as_deref()
            .map(Version::parse)
            .transpose()
            .map_err(|_| format!("Invalid maximum version for {}", requirement.id))?;
        if min.is_none() && max.is_none() {
            return Err(format!(
                "Compatibility requirement {} has no version constraint",
                requirement.id
            ));
        }
        if let (Some(min), Some(max)) = (&min, &max)
            && min > max
        {
            return Err(format!(
                "Compatibility range is reversed for {}",
                requirement.id
            ));
        }
    }
    Ok(())
}

fn hash_file(path: &Path) -> Result<(u64, String), String> {
    let metadata = fs::metadata(path)
        .map_err(|error| format!("Could not inspect offline update package: {error}"))?;
    if !metadata.is_file() || metadata.len() == 0 {
        return Err("Offline update package is empty or is not a file".into());
    }
    let mut file = File::open(path)
        .map_err(|error| format!("Could not open offline update package: {error}"))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|error| format!("Could not read offline update package: {error}"))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok((metadata.len(), format!("{:x}", hasher.finalize())))
}

fn decode_hex<const N: usize>(value: &str, label: &str) -> Result<[u8; N], String> {
    let value = value.trim();
    if value.len() != N * 2 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(format!(
            "{label} must contain exactly {} hexadecimal bytes",
            N
        ));
    }
    let mut decoded = [0u8; N];
    for (index, output) in decoded.iter_mut().enumerate() {
        let offset = index * 2;
        *output = u8::from_str_radix(&value[offset..offset + 2], 16)
            .map_err(|_| format!("{label} contains invalid hexadecimal data"))?;
    }
    Ok(decoded)
}

fn release_verifying_key() -> Result<VerifyingKey, String> {
    let bytes = decode_hex::<32>(RELEASE_SIGNING_PUBLIC_KEY_HEX, "Release public key")?;
    VerifyingKey::from_bytes(&bytes)
        .map_err(|_| "The embedded release public key is invalid".to_string())
}

fn launcher_exe_signature_message(version: &str, payload_sha256: &[u8; 32]) -> Vec<u8> {
    let mut message = Vec::with_capacity(
        LAUNCHER_EXE_SIGNATURE_DOMAIN.len() + version.len() + 1 + payload_sha256.len(),
    );
    message.extend_from_slice(LAUNCHER_EXE_SIGNATURE_DOMAIN);
    message.extend_from_slice(version.as_bytes());
    message.push(0);
    message.extend_from_slice(payload_sha256);
    message
}

fn verify_signed_launcher_executable_with_key(
    path: &Path,
    expected_version: Option<&str>,
    verifying_key: &VerifyingKey,
) -> Result<String, String> {
    if let Some(expected) = expected_version {
        Version::parse(expected)
            .map_err(|_| "Launcher executable has an invalid expected version".to_string())?;
    }
    let bytes =
        fs::read(path).map_err(|error| format!("Could not read launcher executable: {error}"))?;
    let minimum = LAUNCHER_EXE_SIGNATURE_MAGIC.len() + 2 + 64 + 1;
    if bytes.len() < minimum || !bytes.starts_with(b"MZ") {
        return Err("Launcher update is not a valid signed Windows executable".into());
    }
    let magic_start = bytes.len() - LAUNCHER_EXE_SIGNATURE_MAGIC.len();
    if &bytes[magic_start..] != LAUNCHER_EXE_SIGNATURE_MAGIC {
        return Err("Launcher executable does not contain an official MojoRecomp signature".into());
    }
    if magic_start < 2 + 64 {
        return Err("Launcher executable signature trailer is truncated".into());
    }
    let version_len_pos = magic_start - 2;
    let version_len =
        u16::from_le_bytes([bytes[version_len_pos], bytes[version_len_pos + 1]]) as usize;
    let signature_start = version_len_pos
        .checked_sub(64)
        .ok_or_else(|| "Launcher executable signature trailer is truncated".to_string())?;
    let version_start = signature_start
        .checked_sub(version_len)
        .ok_or_else(|| "Launcher executable signature trailer is truncated".to_string())?;
    if version_len == 0 || version_start < 2 {
        return Err("Launcher executable signature trailer is invalid".into());
    }
    let version = std::str::from_utf8(&bytes[version_start..signature_start])
        .map_err(|_| "Launcher executable signature version is not UTF-8".to_string())?;
    Version::parse(version)
        .map_err(|_| "Launcher executable signature contains an invalid version".to_string())?;
    if let Some(expected) = expected_version
        && version != expected
    {
        return Err(format!(
            "Signed launcher executable version {version} does not match expected version {expected}"
        ));
    }
    let payload = &bytes[..version_start];
    if !payload.starts_with(b"MZ") {
        return Err("Launcher executable payload is not a Windows executable".into());
    }
    let payload_hash = Sha256::digest(payload);
    let mut hash_bytes = [0u8; 32];
    hash_bytes.copy_from_slice(&payload_hash);
    let mut signature_bytes = [0u8; 64];
    signature_bytes.copy_from_slice(&bytes[signature_start..version_len_pos]);
    let signature = Signature::from_bytes(&signature_bytes);
    verifying_key
        .verify(
            &launcher_exe_signature_message(version, &hash_bytes),
            &signature,
        )
        .map_err(|_| {
            "Launcher executable signature is invalid or is not an official MojoRecomp release"
                .to_string()
        })?;
    Ok(version.to_string())
}

pub fn verify_signed_launcher_executable(
    path: &Path,
    expected_version: &str,
) -> Result<(), String> {
    verify_signed_launcher_executable_with_key(
        path,
        Some(expected_version),
        &release_verifying_key()?,
    )
    .map(|_| ())
}

pub fn signed_launcher_executable_version(path: &Path) -> Result<String, String> {
    verify_signed_launcher_executable_with_key(path, None, &release_verifying_key()?)
}

fn verify_offline_manifest_signature_with_key(
    manifest: &[u8],
    signature_text: &str,
    verifying_key: &VerifyingKey,
) -> Result<(), String> {
    let signature_bytes = decode_hex::<64>(signature_text, "Offline package signature")?;
    let signature = Signature::from_bytes(&signature_bytes);
    verifying_key.verify(manifest, &signature).map_err(|_| {
        "Offline package signature is invalid or is not an official MojoRecomp package".to_string()
    })
}

fn offline_package_files(kind: &ComponentKind) -> Result<Option<&'static [&'static str]>, String> {
    match kind {
        ComponentKind::Launcher => {
            Err("Offline installation accepts signed game runtime ZIPs only".into())
        }
        ComponentKind::Runtime => Ok(None),
        ComponentKind::Language => Ok(None),
    }
}

fn expected_offline_package_root(manifest: &OfflinePackageManifest) -> Result<String, String> {
    match &manifest.kind {
        ComponentKind::Launcher => {
            Err("Offline installation accepts signed game runtime ZIPs only".into())
        }
        ComponentKind::Runtime => {
            let game = manifest
                .id
                .strip_prefix("runtime.")
                .ok_or_else(|| "Offline runtime package has an invalid component ID".to_string())?;
            validate_game_id(game)?;
            Ok(format!(
                "MojoRecomp-{}-Runtime-{}-windows-x64",
                game.to_ascii_uppercase(),
                manifest.version
            ))
        }
        ComponentKind::Language => {
            let game = manifest
                .game_id
                .as_deref()
                .ok_or_else(|| "Offline language package is missing game_id".to_string())?;
            let locale = manifest
                .locale
                .as_deref()
                .ok_or_else(|| "Offline language package is missing locale".to_string())?;
            validate_game_id(game)?;
            validate_locale(locale)?;
            Ok(format!(
                "MojoRecomp-{}-Language-{}-{}",
                game.to_ascii_uppercase(),
                locale,
                manifest.version
            ))
        }
    }
}

fn inspect_offline_zip_manifest_with_key(
    path: &Path,
    verifying_key: &VerifyingKey,
) -> Result<(OfflinePackageManifest, u64), String> {
    let file = File::open(path)
        .map_err(|error| format!("Could not open offline update package: {error}"))?;
    let mut archive = ZipArchive::new(file)
        .map_err(|error| format!("Offline update is not a valid ZIP archive: {error}"))?;
    if archive.is_empty() || archive.len() > 4096 {
        return Err("Offline update ZIP has an invalid entry count".into());
    }

    let mut package_manifest: Option<(OfflinePackageManifest, Vec<u8>, PathBuf)> = None;
    let mut package_signature: Option<String> = None;
    let mut unpacked_size = 0u64;
    let mut files = HashSet::new();
    let mut payload_files = HashMap::<String, (String, u64, String)>::new();
    let mut top_level_root: Option<PathBuf> = None;
    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| format!("Could not inspect offline update ZIP entry: {error}"))?;
        let Some(enclosed) = entry.enclosed_name() else {
            return Err("Offline update ZIP contains an unsafe path".into());
        };
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err("Offline update ZIP contains a symbolic link".into());
        }
        let relative = safe_relative_path_buf(&enclosed)?;
        let mut components = relative.components();
        let Some(Component::Normal(first)) = components.next() else {
            return Err("Offline update ZIP has an invalid package root".into());
        };
        let root = PathBuf::from(first);
        if let Some(existing) = &top_level_root {
            if !existing.as_os_str().eq_ignore_ascii_case(root.as_os_str()) {
                return Err(
                    "Offline update ZIP must contain one top-level package directory".into(),
                );
            }
        } else {
            top_level_root = Some(root.clone());
        }
        let remainder: PathBuf = components.collect();
        if remainder.as_os_str().is_empty() || entry.is_dir() {
            continue;
        }
        let remainder_text = path_to_catalog_string(&remainder)?;
        if !files.insert(remainder_text.to_ascii_lowercase()) {
            return Err(format!(
                "Offline update ZIP contains a duplicate path: {remainder_text}"
            ));
        }
        unpacked_size = unpacked_size
            .checked_add(entry.size())
            .ok_or_else(|| "Offline update ZIP unpacked size overflowed".to_string())?;
        if remainder_text.eq_ignore_ascii_case(OFFLINE_PACKAGE_MANIFEST) {
            if entry.size() > 64 * 1024 {
                return Err("Offline package manifest is too large".into());
            }
            let mut bytes = Vec::with_capacity(entry.size() as usize);
            entry
                .read_to_end(&mut bytes)
                .map_err(|error| format!("Could not read offline package manifest: {error}"))?;
            let text = std::str::from_utf8(&bytes)
                .map_err(|_| "Offline package manifest must be UTF-8".to_string())?;
            let manifest: OfflinePackageManifest = toml::from_str(text)
                .map_err(|error| format!("Offline package manifest is invalid: {error}"))?;
            if package_manifest.is_some() {
                return Err("Offline update ZIP contains more than one package manifest".into());
            }
            package_manifest = Some((manifest, bytes, root));
        } else if remainder_text.eq_ignore_ascii_case(OFFLINE_PACKAGE_SIGNATURE) {
            if entry.size() > 1024 {
                return Err("Offline package signature is too large".into());
            }
            let mut text = String::new();
            entry
                .read_to_string(&mut text)
                .map_err(|error| format!("Could not read offline package signature: {error}"))?;
            if package_signature.replace(text).is_some() {
                return Err("Offline update ZIP contains more than one package signature".into());
            }
        } else {
            let mut hasher = Sha256::new();
            let mut buffer = [0u8; 64 * 1024];
            loop {
                let count = entry
                    .read(&mut buffer)
                    .map_err(|error| format!("Could not hash offline package file: {error}"))?;
                if count == 0 {
                    break;
                }
                hasher.update(&buffer[..count]);
            }
            payload_files.insert(
                remainder_text.to_ascii_lowercase(),
                (
                    remainder_text,
                    entry.size(),
                    format!("{:x}", hasher.finalize()),
                ),
            );
        }
    }

    let Some((manifest, manifest_bytes, manifest_root)) = package_manifest else {
        return Err(format!(
            "Offline update ZIP is missing {OFFLINE_PACKAGE_MANIFEST}"
        ));
    };
    let signature = package_signature
        .ok_or_else(|| format!("Offline update ZIP is missing {OFFLINE_PACKAGE_SIGNATURE}"))?;
    verify_offline_manifest_signature_with_key(&manifest_bytes, &signature, verifying_key)?;
    if manifest.schema_version != OFFLINE_PACKAGE_SCHEMA_VERSION {
        return Err(format!(
            "Unsupported offline package schema version: {}",
            manifest.schema_version
        ));
    }
    let expected_root = expected_offline_package_root(&manifest)?;
    if !manifest_root
        .as_os_str()
        .eq_ignore_ascii_case(Path::new(&expected_root).as_os_str())
    {
        return Err(format!(
            "Offline update ZIP package root must be {expected_root}"
        ));
    }

    if let Some(required_payload) = offline_package_files(&manifest.kind)? {
        for required in required_payload {
            if !files.contains(&required.to_ascii_lowercase()) {
                return Err(format!(
                    "Offline update package is missing required file: {required}"
                ));
            }
        }
        if files.len() != required_payload.len() {
            return Err(
                "Offline update ZIP contains files outside the supported package schema".into(),
            );
        }
    } else {
        let companion_files: &[&str] = match manifest.kind {
            ComponentKind::Runtime => &["LICENSE", "THIRD_PARTY_NOTICES.txt"],
            ComponentKind::Language => &["LICENSE"],
            ComponentKind::Launcher => &[],
        };
        for required in manifest
            .required_files
            .iter()
            .map(String::as_str)
            .chain(companion_files.iter().copied())
        {
            if !files.contains(&required.to_ascii_lowercase()) {
                return Err(format!(
                    "Offline component package is missing required file: {required}"
                ));
            }
        }
    }
    if manifest.files.len() != payload_files.len() {
        return Err("Offline package manifest does not describe every payload file".into());
    }
    let mut declared_files = HashSet::new();
    for declared in &manifest.files {
        safe_relative_path(&declared.path)?;
        validate_sha256(&declared.sha256)?;
        let key = declared.path.to_ascii_lowercase();
        if key == OFFLINE_PACKAGE_MANIFEST.to_ascii_lowercase()
            || key == OFFLINE_PACKAGE_SIGNATURE.to_ascii_lowercase()
            || !declared_files.insert(key.clone())
        {
            return Err(format!(
                "Offline package manifest contains an invalid or duplicate file: {}",
                declared.path
            ));
        }
        let Some((actual_path, actual_size, actual_sha256)) = payload_files.get(&key) else {
            return Err(format!(
                "Offline package manifest references a missing file: {}",
                declared.path
            ));
        };
        if actual_path != &declared.path
            || actual_size != &declared.size
            || actual_sha256 != &declared.sha256
        {
            return Err(format!(
                "Offline package payload failed signed integrity verification: {}",
                declared.path
            ));
        }
    }
    Ok((manifest, unpacked_size))
}

pub fn inspect_offline_package(
    path: &Path,
    current_launcher_version: &str,
) -> Result<ComponentRelease, String> {
    let verifying_key = release_verifying_key()?;
    inspect_offline_package_with_key(path, current_launcher_version, &verifying_key)
}

pub fn extract_offline_localization_pack(
    path: &Path,
    destination_root: &Path,
) -> Result<OfflineLocalizationPack, String> {
    let verifying_key = release_verifying_key()?;
    extract_offline_localization_pack_with_key(path, destination_root, &verifying_key)
}

fn extract_offline_localization_pack_with_key(
    path: &Path,
    destination_root: &Path,
    verifying_key: &VerifyingKey,
) -> Result<OfflineLocalizationPack, String> {
    let file =
        File::open(path).map_err(|error| format!("Could not open Localization Pack: {error}"))?;
    let mut archive = ZipArchive::new(file)
        .map_err(|error| format!("Localization Pack is not a valid ZIP archive: {error}"))?;
    if archive.is_empty() || archive.len() > 1024 {
        return Err("Localization Pack ZIP has an invalid entry count".into());
    }

    let mut top_level_root: Option<PathBuf> = None;
    let mut manifest: Option<(LocalizationPackManifest, Vec<u8>, PathBuf)> = None;
    let mut signature: Option<String> = None;
    let mut payloads = HashMap::<String, (String, u64, String)>::new();

    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| format!("Could not inspect Localization Pack ZIP entry: {error}"))?;
        let Some(enclosed) = entry.enclosed_name() else {
            return Err("Localization Pack ZIP contains an unsafe path".into());
        };
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err("Localization Pack ZIP contains a symbolic link".into());
        }
        let relative = safe_relative_path_buf(&enclosed)?;
        let mut components = relative.components();
        let Some(Component::Normal(first)) = components.next() else {
            return Err("Localization Pack ZIP has an invalid package root".into());
        };
        let root = PathBuf::from(first);
        if let Some(existing) = &top_level_root {
            if !existing.as_os_str().eq_ignore_ascii_case(root.as_os_str()) {
                return Err("Localization Pack ZIP must contain one top-level directory".into());
            }
        } else {
            top_level_root = Some(root.clone());
        }
        let remainder: PathBuf = components.collect();
        if remainder.as_os_str().is_empty() || entry.is_dir() {
            continue;
        }
        let remainder_text = path_to_catalog_string(&remainder)?;
        if remainder_text.eq_ignore_ascii_case(LOCALIZATION_PACK_MANIFEST) {
            if entry.size() > 64 * 1024 {
                return Err("Localization Pack manifest is too large".into());
            }
            let mut bytes = Vec::with_capacity(entry.size() as usize);
            entry
                .read_to_end(&mut bytes)
                .map_err(|error| format!("Could not read Localization Pack manifest: {error}"))?;
            let text = std::str::from_utf8(&bytes)
                .map_err(|_| "Localization Pack manifest must be UTF-8".to_string())?;
            let parsed: LocalizationPackManifest = toml::from_str(text)
                .map_err(|error| format!("Localization Pack manifest is invalid: {error}"))?;
            if manifest.replace((parsed, bytes, root)).is_some() {
                return Err("Localization Pack contains more than one manifest".into());
            }
            continue;
        }
        if remainder_text.eq_ignore_ascii_case(LOCALIZATION_PACK_SIGNATURE) {
            if entry.size() > 1024 {
                return Err("Localization Pack signature is too large".into());
            }
            let mut text = String::new();
            entry
                .read_to_string(&mut text)
                .map_err(|error| format!("Could not read Localization Pack signature: {error}"))?;
            if signature.replace(text).is_some() {
                return Err("Localization Pack contains more than one signature".into());
            }
            continue;
        }

        let key = remainder_text.to_ascii_lowercase();
        if payloads.contains_key(&key) {
            return Err(format!(
                "Localization Pack contains a duplicate path: {remainder_text}"
            ));
        }
        let mut hasher = Sha256::new();
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let count = entry
                .read(&mut buffer)
                .map_err(|error| format!("Could not hash Localization Pack payload: {error}"))?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
        }
        payloads.insert(
            key,
            (
                remainder_text,
                entry.size(),
                format!("{:x}", hasher.finalize()),
            ),
        );
    }

    let Some((manifest, manifest_bytes, manifest_root)) = manifest else {
        return Err(format!(
            "Localization Pack is missing {LOCALIZATION_PACK_MANIFEST}"
        ));
    };
    let signature = signature
        .ok_or_else(|| format!("Localization Pack is missing {LOCALIZATION_PACK_SIGNATURE}"))?;
    verify_offline_manifest_signature_with_key(&manifest_bytes, &signature, verifying_key)?;
    if manifest.schema_version != LOCALIZATION_PACK_SCHEMA_VERSION {
        return Err(format!(
            "Unsupported Localization Pack schema version: {}",
            manifest.schema_version
        ));
    }
    validate_game_id(&manifest.game_id)?;
    Version::parse(&manifest.version)
        .map_err(|_| "Localization Pack has an invalid semantic version".to_string())?;
    if manifest.languages.is_empty() || manifest.languages.len() > 128 {
        return Err("Localization Pack must contain between 1 and 128 languages".into());
    }
    let expected_root = format!(
        "MojoRecomp-{}-Localization-Pack-{}",
        manifest.game_id.to_ascii_uppercase(),
        manifest.version
    );
    if !manifest_root
        .as_os_str()
        .eq_ignore_ascii_case(Path::new(&expected_root).as_os_str())
    {
        return Err(format!(
            "Localization Pack ZIP package root must be {expected_root}"
        ));
    }

    let mut seen_ids = HashSet::new();
    let mut seen_locales = HashSet::new();
    let mut seen_files = HashSet::new();
    for language in &manifest.languages {
        validate_component_id(&language.id)?;
        validate_locale(&language.locale)?;
        Version::parse(&language.version)
            .map_err(|_| format!("Language {} has an invalid semantic version", language.id))?;
        validate_sha256(&language.sha256)?;
        if language.display_name.trim().is_empty() || language.display_name.len() > 80 {
            return Err(format!(
                "Language {} has an invalid display name",
                language.id
            ));
        }
        if language.xbox_language == 0 || language.xbox_language > 255 {
            return Err(format!(
                "Language {} has an invalid Xbox language",
                language.id
            ));
        }
        if language.id
            != format!(
                "language.{}.{}",
                manifest.game_id,
                language.locale.to_ascii_lowercase()
            )
        {
            return Err(format!(
                "Language component ID does not match game/locale: {}",
                language.id
            ));
        }
        safe_relative_path(&language.file)?;
        if !language.file.to_ascii_lowercase().starts_with("languages/")
            || !language.file.to_ascii_lowercase().ends_with(".zip")
        {
            return Err(format!(
                "Localization Pack language file has an invalid path: {}",
                language.file
            ));
        }
        let file_key = language.file.to_ascii_lowercase();
        if !seen_ids.insert(language.id.to_ascii_lowercase())
            || !seen_locales.insert(language.locale.to_ascii_lowercase())
            || !seen_files.insert(file_key.clone())
        {
            return Err("Localization Pack contains duplicate language metadata".into());
        }
        let Some((actual_path, actual_size, actual_sha256)) = payloads.get(&file_key) else {
            return Err(format!(
                "Localization Pack is missing language package: {}",
                language.file
            ));
        };
        if actual_path != &language.file
            || *actual_size != language.size
            || actual_sha256 != &language.sha256
        {
            return Err(format!(
                "Localization Pack language payload failed integrity verification: {}",
                language.file
            ));
        }
    }
    if payloads.len() != seen_files.len() {
        return Err("Localization Pack contains files not declared by its signed manifest".into());
    }

    if destination_root.exists() {
        fs::remove_dir_all(destination_root)
            .map_err(|error| format!("Could not clean Localization Pack staging: {error}"))?;
    }
    fs::create_dir_all(destination_root)
        .map_err(|error| format!("Could not create Localization Pack staging: {error}"))?;

    let file =
        File::open(path).map_err(|error| format!("Could not reopen Localization Pack: {error}"))?;
    let mut archive = ZipArchive::new(file)
        .map_err(|error| format!("Could not reopen Localization Pack ZIP: {error}"))?;
    let root_text = manifest_root.to_string_lossy();
    let mut languages = Vec::with_capacity(manifest.languages.len());
    for language in &manifest.languages {
        let archive_name = format!("{root_text}/{}", language.file);
        let mut entry = archive
            .by_name(&archive_name)
            .map_err(|_| format!("Localization Pack payload disappeared: {}", language.file))?;
        let destination = destination_root.join(format!(
            "{}-{}.zip",
            language.id.replace('.', "_"),
            language.version
        ));
        let mut output = File::create(&destination)
            .map_err(|error| format!("Could not stage language package: {error}"))?;
        std::io::copy(&mut entry, &mut output)
            .map_err(|error| format!("Could not extract language package: {error}"))?;
        output
            .sync_all()
            .map_err(|error| format!("Could not flush language package staging: {error}"))?;
        let (size, sha256) = hash_file(&destination)?;
        if size != language.size || sha256 != language.sha256 {
            let _ = fs::remove_dir_all(destination_root);
            return Err(format!(
                "Extracted language package failed verification: {}",
                language.id
            ));
        }
        languages.push(OfflineLocalizationPackLanguage {
            id: language.id.clone(),
            locale: language.locale.clone(),
            display_name: language.display_name.clone(),
            xbox_language: language.xbox_language,
            version: language.version.clone(),
            package_path: destination,
        });
    }

    Ok(OfflineLocalizationPack {
        game_id: manifest.game_id,
        version: manifest.version,
        languages,
    })
}

pub fn inspect_signed_release_artifact(
    path: &Path,
    current_launcher_version: &str,
    expected: &ComponentRelease,
) -> Result<ComponentRelease, String> {
    let verifying_key = release_verifying_key()?;
    inspect_signed_release_artifact_with_key(
        path,
        current_launcher_version,
        expected,
        &verifying_key,
    )
}

fn inspect_signed_release_artifact_with_key(
    path: &Path,
    current_launcher_version: &str,
    expected: &ComponentRelease,
    verifying_key: &VerifyingKey,
) -> Result<ComponentRelease, String> {
    let signed = inspect_offline_package_with_key(path, current_launcher_version, verifying_key)?;
    let expected_size = expected
        .localization_pack
        .as_ref()
        .map(|pack| pack.component_size)
        .unwrap_or(expected.size);
    let expected_sha256 = expected
        .localization_pack
        .as_ref()
        .map(|pack| pack.component_sha256.as_str())
        .unwrap_or(expected.sha256.as_str());
    let expected_required_files = expected
        .required_files
        .iter()
        .filter(|path| {
            !path.eq_ignore_ascii_case(OFFLINE_PACKAGE_MANIFEST)
                && !path.eq_ignore_ascii_case(OFFLINE_PACKAGE_SIGNATURE)
        })
        .cloned()
        .collect::<Vec<_>>();
    if signed.id != expected.id
        || signed.kind != expected.kind
        || signed.version != expected.version
        || signed.platform != expected.platform
        || signed.arch != expected.arch
        || signed.size != expected_size
        || signed.sha256 != expected_sha256
        || signed.package != expected.package
        || signed.unpacked_size != expected.unpacked_size
        || signed.entrypoint != expected.entrypoint
        || signed.required_files != expected_required_files
        || signed.game_id != expected.game_id
        || signed.locale != expected.locale
        || signed.display_name != expected.display_name
        || signed.xbox_language != expected.xbox_language
        || signed.compatibility != expected.compatibility
    {
        return Err(format!(
            "Signed package metadata does not match the update catalog for {} {}",
            expected.id, expected.version
        ));
    }
    Ok(signed)
}

fn inspect_offline_package_with_key(
    path: &Path,
    current_launcher_version: &str,
    verifying_key: &VerifyingKey,
) -> Result<ComponentRelease, String> {
    let (manifest, unpacked_size) = inspect_offline_zip_manifest_with_key(path, verifying_key)?;
    if manifest.platform != "windows" || manifest.arch != "x86_64" {
        return Err("Offline update package must target windows-x86_64".into());
    }
    Version::parse(&manifest.version)
        .map_err(|_| "Offline update package has an invalid semantic version".to_string())?;
    let launcher = Version::parse(current_launcher_version)
        .map_err(|_| "Current launcher version is not valid semantic versioning".to_string())?;
    let compatibility = Compatibility {
        min_launcher: manifest.min_launcher.clone(),
        max_launcher: manifest.max_launcher.clone(),
        requirements: manifest.requirements.clone(),
    };
    validate_compatibility(&compatibility)?;
    if let Some(minimum) = compatibility.min_launcher.as_deref() {
        let minimum = Version::parse(minimum)
            .map_err(|_| "Offline package minimum launcher version is invalid".to_string())?;
        if launcher < minimum {
            return Err(format!(
                "This package requires MojoRecomp Launcher {minimum} or newer"
            ));
        }
    }
    if let Some(maximum) = compatibility.max_launcher.as_deref() {
        let maximum = Version::parse(maximum)
            .map_err(|_| "Offline package maximum launcher version is invalid".to_string())?;
        if launcher > maximum {
            return Err(format!(
                "This package supports MojoRecomp Launcher {maximum} or older"
            ));
        }
    }

    let (required_files, game_id, locale, display_name, xbox_language) = match manifest.kind {
        ComponentKind::Runtime => {
            let game = manifest
                .id
                .strip_prefix("runtime.")
                .ok_or_else(|| "Offline runtime package has an invalid component ID".to_string())?;
            validate_game_id(game)?;
            let entrypoint = manifest
                .entrypoint
                .as_deref()
                .ok_or_else(|| "Offline runtime package is missing its entrypoint".to_string())?;
            if manifest.package != PackageFormat::Zip
                || !entrypoint.to_ascii_lowercase().ends_with(".exe")
                || !manifest
                    .required_files
                    .iter()
                    .any(|path| path == entrypoint)
            {
                return Err(
                    "Offline runtime package metadata is incomplete or incompatible".into(),
                );
            }
            if game == "cot"
                && (entrypoint != COT_RUNTIME_ENTRYPOINT
                    || COT_RUNTIME_REQUIRED_FILES.iter().any(|required| {
                        !manifest.required_files.iter().any(|path| path == required)
                    }))
            {
                return Err("Offline COT runtime package is missing required runtime files".into());
            }
            (
                manifest.required_files.clone(),
                Some(game.to_string()),
                None,
                None,
                None,
            )
        }
        ComponentKind::Launcher => {
            return Err("Offline installation accepts signed game runtime ZIPs only".into());
        }
        ComponentKind::Language => {
            let game = manifest
                .game_id
                .as_deref()
                .ok_or_else(|| "Offline language package is missing game_id".to_string())?;
            let locale = manifest
                .locale
                .as_deref()
                .ok_or_else(|| "Offline language package is missing locale".to_string())?;
            let display_name = manifest
                .display_name
                .as_deref()
                .ok_or_else(|| "Offline language package is missing display_name".to_string())?;
            let xbox_language = manifest
                .xbox_language
                .ok_or_else(|| "Offline language package is missing xbox_language".to_string())?;
            validate_game_id(game)?;
            validate_locale(locale)?;
            if manifest.id != format!("language.{game}.{}", locale.to_ascii_lowercase()) {
                return Err("Offline language package ID does not match game_id/locale".into());
            }
            if display_name.trim().is_empty() || display_name.len() > 80 {
                return Err("Offline language package has an invalid display_name".into());
            }
            if xbox_language == 0 || xbox_language > 255 {
                return Err("Offline language package has an invalid xbox_language".into());
            }
            if manifest.package != PackageFormat::Zip
                || manifest.entrypoint.is_some()
                || manifest.required_files.is_empty()
            {
                return Err(
                    "Offline language package metadata is incomplete or incompatible".into(),
                );
            }
            (
                manifest.required_files.clone(),
                Some(game.to_string()),
                Some(locale.to_string()),
                Some(display_name.to_string()),
                Some(xbox_language),
            )
        }
    };
    let (size, sha256) = hash_file(path)?;
    let is_zip_component = matches!(
        manifest.kind,
        ComponentKind::Runtime | ComponentKind::Language
    );
    let release = ComponentRelease {
        id: manifest.id,
        kind: manifest.kind,
        version: manifest.version,
        platform: manifest.platform,
        arch: manifest.arch,
        url: String::new(),
        size,
        sha256,
        published: String::new(),
        notes_url: String::new(),
        package: manifest.package,
        unpacked_size: is_zip_component.then_some(unpacked_size),
        entrypoint: manifest.entrypoint,
        required_files,
        game_id,
        locale,
        display_name,
        xbox_language,
        localization_pack: None,
        localization_catalog_url: None,
        compatibility,
    };
    validate_release_installable(&release)?;
    Ok(release)
}

pub fn plan_updates(
    catalog: &UpdateCatalog,
    installed: &[InstalledComponent],
    launcher_version: &str,
) -> Result<Vec<ComponentPlan>, String> {
    validate_catalog(catalog)?;
    let launcher = Version::parse(launcher_version)
        .map_err(|_| "Installed launcher version is not valid semantic versioning".to_string())?;
    let installed_map = installed
        .iter()
        .map(|component| (component.id.as_str(), component))
        .collect::<HashMap<_, _>>();
    let installed_versions = installed
        .iter()
        .filter(|component| component.healthy)
        .filter_map(|component| {
            Version::parse(&component.version)
                .ok()
                .map(|version| (component.id.as_str(), version))
        })
        .collect::<HashMap<_, _>>();
    let mut grouped = BTreeMap::<String, Vec<&ComponentRelease>>::new();
    for release in &catalog.releases {
        grouped.entry(release.id.clone()).or_default().push(release);
    }
    let mut plans = Vec::new();
    for (id, mut releases) in grouped {
        releases.sort_by(|left, right| {
            Version::parse(&right.version)
                .ok()
                .cmp(&Version::parse(&left.version).ok())
        });
        let compatible = releases
            .iter()
            .copied()
            .filter(|release| is_compatible(release, &launcher, &installed_versions))
            .collect::<Vec<_>>();
        let latest = compatible.first().copied();
        let installed_component = installed_map.get(id.as_str()).copied();
        let state = match (installed_component, latest) {
            (Some(component), Some(_)) if !component.healthy => PlanState::Corrupted,
            (Some(component), None) if !component.healthy => PlanState::RepairUnavailable,
            (_, None) => PlanState::Incompatible,
            (None, Some(_)) => PlanState::Available,
            (Some(component), Some(release)) => {
                let current = Version::parse(&component.version)
                    .map_err(|_| format!("Installed component {id} has an invalid version"))?;
                if Version::parse(&release.version).unwrap() > current {
                    PlanState::UpdateAvailable
                } else {
                    PlanState::UpToDate
                }
            }
        };
        let exemplar = latest.or_else(|| releases.first().copied()).unwrap();
        plans.push(ComponentPlan {
            id: id.clone(),
            kind: exemplar.kind.clone(),
            game_id: exemplar.game_id.clone(),
            locale: exemplar.locale.clone(),
            display_name: exemplar.display_name.clone(),
            xbox_language: exemplar.xbox_language,
            installed_version: installed_component.map(|component| component.version.clone()),
            latest_version: latest.map(|release| release.version.clone()),
            state,
            download_url: latest.map(|release| release.url.clone()),
            size: latest.map(|release| release.size),
            published: latest.map(|release| release.published.clone()),
            notes_url: latest.map(|release| release.notes_url.clone()),
        });
    }
    Ok(plans)
}

pub fn compatible_releases_for_component(
    catalog: &UpdateCatalog,
    installed: &[InstalledComponent],
    launcher_version: &str,
    component_id: &str,
) -> Result<Vec<ComponentRelease>, String> {
    validate_catalog(catalog)?;
    let launcher = Version::parse(launcher_version)
        .map_err(|_| "Installed launcher version is not valid semantic versioning".to_string())?;
    let installed_versions = installed
        .iter()
        .filter(|component| component.healthy)
        .filter_map(|component| {
            Version::parse(&component.version)
                .ok()
                .map(|version| (component.id.as_str(), version))
        })
        .collect::<HashMap<_, _>>();
    let mut releases = catalog
        .releases
        .iter()
        .filter(|release| release.id == component_id)
        .filter(|release| is_compatible(release, &launcher, &installed_versions))
        .cloned()
        .collect::<Vec<_>>();
    releases.sort_by(|left, right| {
        Version::parse(&right.version)
            .ok()
            .cmp(&Version::parse(&left.version).ok())
    });
    Ok(releases)
}

fn is_compatible(
    release: &ComponentRelease,
    launcher: &Version,
    installed: &HashMap<&str, Version>,
) -> bool {
    let compatibility = &release.compatibility;
    if compatibility
        .min_launcher
        .as_deref()
        .and_then(|value| Version::parse(value).ok())
        .is_some_and(|minimum| launcher < &minimum)
    {
        return false;
    }
    if compatibility
        .max_launcher
        .as_deref()
        .and_then(|value| Version::parse(value).ok())
        .is_some_and(|maximum| launcher > &maximum)
    {
        return false;
    }
    compatibility.requirements.iter().all(|requirement| {
        let Some(current) = installed.get(requirement.id.as_str()) else {
            return false;
        };
        let minimum_ok = requirement
            .min_version
            .as_deref()
            .and_then(|value| Version::parse(value).ok())
            .is_none_or(|minimum| current >= &minimum);
        let maximum_ok = requirement
            .max_version
            .as_deref()
            .and_then(|value| Version::parse(value).ok())
            .is_none_or(|maximum| current <= &maximum);
        minimum_ok && maximum_ok
    })
}

pub fn validate_release_compatibility(
    release: &ComponentRelease,
    launcher_version: &str,
    installed: &[InstalledComponent],
) -> Result<(), String> {
    let launcher = Version::parse(launcher_version)
        .map_err(|_| "Installed launcher version is not valid semantic versioning".to_string())?;
    let installed_versions = installed
        .iter()
        .filter(|component| component.healthy)
        .filter_map(|component| {
            Version::parse(&component.version)
                .ok()
                .map(|version| (component.id.as_str(), version))
        })
        .collect::<HashMap<_, _>>();
    if is_compatible(release, &launcher, &installed_versions) {
        return Ok(());
    }

    let compatibility = &release.compatibility;
    if compatibility
        .min_launcher
        .as_deref()
        .and_then(|value| Version::parse(value).ok())
        .is_some_and(|minimum| launcher < minimum)
    {
        return Err(format!(
            "Component {} {} requires MojoRecomp Launcher {} or newer",
            release.id,
            release.version,
            compatibility.min_launcher.as_deref().unwrap_or_default()
        ));
    }
    if compatibility
        .max_launcher
        .as_deref()
        .and_then(|value| Version::parse(value).ok())
        .is_some_and(|maximum| launcher > maximum)
    {
        return Err(format!(
            "Component {} {} supports MojoRecomp Launcher {} or older",
            release.id,
            release.version,
            compatibility.max_launcher.as_deref().unwrap_or_default()
        ));
    }
    for requirement in &compatibility.requirements {
        let current = installed_versions.get(requirement.id.as_str());
        if current.is_none() {
            return Err(format!(
                "Component {} {} requires component {} to be installed",
                release.id, release.version, requirement.id
            ));
        }
        let current = current.unwrap();
        if requirement
            .min_version
            .as_deref()
            .and_then(|value| Version::parse(value).ok())
            .is_some_and(|minimum| current < &minimum)
        {
            return Err(format!(
                "Component {} {} requires {} {} or newer",
                release.id,
                release.version,
                requirement.id,
                requirement.min_version.as_deref().unwrap_or_default()
            ));
        }
        if requirement
            .max_version
            .as_deref()
            .and_then(|value| Version::parse(value).ok())
            .is_some_and(|maximum| current > &maximum)
        {
            return Err(format!(
                "Component {} {} requires {} {} or older",
                release.id,
                release.version,
                requirement.id,
                requirement.max_version.as_deref().unwrap_or_default()
            ));
        }
    }
    Err(format!(
        "Component {} {} has unsatisfied compatibility requirements",
        release.id, release.version
    ))
}

pub fn fetch_catalog(url: &str) -> Result<UpdateCatalog, String> {
    let source_url = validate_resolved_public_https_url(url)?;
    ensure_tls_crypto_provider()?;
    let client = reqwest::blocking::Client::builder()
        .redirect(public_https_redirect_policy())
        .connect_timeout(Duration::from_secs(20))
        .timeout(Duration::from_secs(30))
        .user_agent(concat!("MojoRecomp-Launcher/", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|error| format!("Could not initialize update client: {error}"))?;
    let response = client
        .get(url)
        .header(reqwest::header::ACCEPT, "application/vnd.github+json")
        .send()
        .and_then(|response| response.error_for_status())
        .map_err(|error| format!("Could not download update catalog: {error}"))?;
    validate_resolved_public_https_url(response.url().as_str())?;
    let text = if is_github_release_feed_url(&source_url) {
        let releases = response
            .json::<Vec<GitHubReleaseEntry>>()
            .map_err(|error| format!("Could not read GitHub release feed: {error}"))?;
        let asset_url =
            newest_github_asset_url(&releases, "update-catalog.toml").ok_or_else(|| {
                "No published GitHub release or pre-release contains update-catalog.toml"
                    .to_string()
            })?;
        validate_resolved_public_https_url(&asset_url)?;
        let asset_response = client
            .get(&asset_url)
            .send()
            .and_then(|response| response.error_for_status())
            .map_err(|error| format!("Could not download update catalog asset: {error}"))?;
        validate_resolved_public_https_url(asset_response.url().as_str())?;
        asset_response
            .text()
            .map_err(|error| format!("Could not read update catalog asset: {error}"))?
    } else {
        response
            .text()
            .map_err(|error| format!("Could not read update catalog: {error}"))?
    };
    parse_and_validate_catalog(&text)
}

pub fn fetch_localization_catalog(url: &str) -> Result<LocalizationCatalog, String> {
    let source_url = validate_resolved_public_https_url(url)?;
    ensure_tls_crypto_provider()?;
    let client = reqwest::blocking::Client::builder()
        .redirect(public_https_redirect_policy())
        .connect_timeout(Duration::from_secs(20))
        .timeout(Duration::from_secs(30))
        .user_agent(concat!("MojoRecomp-Launcher/", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|error| format!("Could not initialize localization catalog client: {error}"))?;
    let response = client
        .get(url)
        .header(reqwest::header::ACCEPT, "application/vnd.github+json")
        .send()
        .and_then(|response| response.error_for_status())
        .map_err(|error| format!("Could not download localization catalog: {error}"))?;
    validate_resolved_public_https_url(response.url().as_str())?;
    let text = if is_github_release_feed_url(&source_url) {
        let releases = response
            .json::<Vec<GitHubReleaseEntry>>()
            .map_err(|error| format!("Could not read GitHub release feed: {error}"))?;
        let asset_url = newest_github_asset_url(&releases, "localization-catalog.toml")
            .ok_or_else(|| {
                "No published GitHub release or pre-release contains localization-catalog.toml"
                    .to_string()
            })?;
        validate_resolved_public_https_url(&asset_url)?;
        let asset_response = client
            .get(&asset_url)
            .send()
            .and_then(|response| response.error_for_status())
            .map_err(|error| format!("Could not download localization catalog asset: {error}"))?;
        validate_resolved_public_https_url(asset_response.url().as_str())?;
        asset_response
            .text()
            .map_err(|error| format!("Could not read localization catalog asset: {error}"))?
    } else {
        response
            .text()
            .map_err(|error| format!("Could not read localization catalog: {error}"))?
    };
    parse_and_validate_localization_catalog(&text)
}

#[derive(Debug, Deserialize)]
struct GitHubReleaseAsset {
    name: String,
    browser_download_url: String,
}

#[derive(Debug, Deserialize)]
struct GitHubReleaseEntry {
    #[serde(default)]
    draft: bool,
    published_at: Option<String>,
    created_at: Option<String>,
    #[serde(default)]
    assets: Vec<GitHubReleaseAsset>,
}

fn is_github_release_feed_url(url: &reqwest::Url) -> bool {
    if !url
        .host_str()
        .is_some_and(|host| host.eq_ignore_ascii_case("api.github.com"))
    {
        return false;
    }
    let segments = url
        .path_segments()
        .map(|segments| {
            segments
                .filter(|segment| !segment.is_empty())
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    segments.len() == 4
        && segments[0] == "repos"
        && !segments[1].is_empty()
        && !segments[2].is_empty()
        && segments[3] == "releases"
}

fn newest_github_asset_url(releases: &[GitHubReleaseEntry], asset_name: &str) -> Option<String> {
    releases
        .iter()
        .filter(|release| !release.draft)
        .filter_map(|release| {
            let asset = release
                .assets
                .iter()
                .find(|asset| asset.name == asset_name)?;
            Some((
                release
                    .published_at
                    .as_deref()
                    .or(release.created_at.as_deref())
                    .unwrap_or(""),
                asset.browser_download_url.as_str(),
            ))
        })
        .max_by(|left, right| left.0.cmp(right.0))
        .map(|(_, url)| url.to_string())
}

fn ensure_tls_crypto_provider() -> Result<(), String> {
    if rustls::crypto::CryptoProvider::get_default().is_none() {
        let _ = rustls::crypto::ring::default_provider().install_default();
    }
    rustls::crypto::CryptoProvider::get_default()
        .map(|_| ())
        .ok_or_else(|| "Could not initialize the TLS crypto provider".to_string())
}

pub fn validate_public_https_url(value: &str) -> Result<reqwest::Url, String> {
    let url = reqwest::Url::parse(value).map_err(|_| "Update URL is invalid".to_string())?;
    if url.scheme() != "https"
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
    {
        return Err("Update URLs must use public HTTPS without embedded credentials".into());
    }
    let host = url.host_str().unwrap();
    if host.eq_ignore_ascii_case("localhost") || host.ends_with(".localhost") {
        return Err("Update URLs must use a public HTTPS host".into());
    }
    if let Ok(ip) = host.parse::<IpAddr>()
        && !is_public_ip(ip)
    {
        return Err("Update URLs must use a public HTTPS host".into());
    }
    Ok(url)
}

fn is_public_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(value) => {
            let octets = value.octets();
            !(value.is_private()
                || value.is_loopback()
                || value.is_link_local()
                || value.is_broadcast()
                || value.is_documentation()
                || value.is_unspecified()
                || value.is_multicast()
                || octets[0] == 0
                || (octets[0] == 100 && (64..=127).contains(&octets[1]))
                || (octets[0] == 192 && octets[1] == 0 && octets[2] == 0)
                || (octets[0] == 192 && octets[1] == 88 && octets[2] == 99)
                || (octets[0] == 198 && matches!(octets[1], 18 | 19))
                || octets[0] >= 240)
        }
        IpAddr::V6(value) => {
            let segments = value.segments();
            if let Some(mapped) = value.to_ipv4_mapped() {
                return is_public_ip(IpAddr::V4(mapped));
            }
            !(value.is_loopback()
                || value.is_unspecified()
                || value.is_unique_local()
                || value.is_unicast_link_local()
                || value.is_multicast()
                || (segments[0] == 0x2001 && segments[1] == 0x0db8))
        }
    }
}

fn validate_resolved_addresses(addresses: impl IntoIterator<Item = IpAddr>) -> Result<(), String> {
    let mut found = false;
    for address in addresses {
        found = true;
        if !is_public_ip(address) {
            return Err("Update URL resolved to a non-public network address".into());
        }
    }
    if !found {
        return Err("Update URL host did not resolve to an address".into());
    }
    Ok(())
}

pub fn validate_resolved_public_https_url(value: &str) -> Result<reqwest::Url, String> {
    let url = validate_public_https_url(value)?;
    let host = url
        .host_str()
        .ok_or_else(|| "Update URL has no host".to_string())?;
    let port = url
        .port_or_known_default()
        .ok_or_else(|| "Update URL has no usable port".to_string())?;
    let addresses = (host, port)
        .to_socket_addrs()
        .map_err(|error| format!("Could not resolve update URL host: {error}"))?
        .map(|address| address.ip());
    validate_resolved_addresses(addresses)?;
    Ok(url)
}

fn public_https_redirect_policy() -> reqwest::redirect::Policy {
    reqwest::redirect::Policy::custom(|attempt| {
        if attempt.previous().len() >= 5 {
            return attempt.error(std::io::Error::other("Too many update redirects"));
        }
        if validate_resolved_public_https_url(attempt.url().as_str()).is_err() {
            return attempt.error(std::io::Error::other(
                "Update redirect left the public HTTPS trust boundary",
            ));
        }
        attempt.follow()
    })
}

fn validate_component_id(value: &str) -> Result<(), String> {
    if value.is_empty()
        || value.len() > 96
        || !value.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'.' | b'-')
        })
        || value.starts_with(['.', '-'])
        || value.ends_with(['.', '-'])
        || value.contains("..")
    {
        return Err(format!("Invalid component ID: {value}"));
    }
    Ok(())
}

fn component_kind_matches_id(id: &str, kind: &ComponentKind) -> bool {
    match kind {
        ComponentKind::Launcher => id == "launcher",
        ComponentKind::Runtime => id.starts_with("runtime."),
        ComponentKind::Language => id.starts_with("language."),
    }
}

fn validate_game_id(value: &str) -> Result<(), String> {
    if !matches!(value, "cot" | "mom") {
        return Err(format!("Unsupported game ID in update catalog: {value}"));
    }
    Ok(())
}

fn validate_locale(value: &str) -> Result<(), String> {
    if value.len() < 2
        || value.len() > 24
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
        || value.starts_with('-')
        || value.ends_with('-')
    {
        return Err(format!("Invalid locale in update catalog: {value}"));
    }
    Ok(())
}

fn validate_sha256(value: &str) -> Result<(), String> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(
            "SHA-256 values must contain exactly 64 lowercase hexadecimal characters".into(),
        );
    }
    Ok(())
}

fn validate_date(value: &str) -> Result<(), String> {
    if !value.is_ascii()
        || value.len() != 10
        || value.as_bytes()[4] != b'-'
        || value.as_bytes()[7] != b'-'
    {
        return Err(format!("Invalid release date: {value}"));
    }
    let year = value[0..4]
        .parse::<u32>()
        .map_err(|_| format!("Invalid release date: {value}"))?;
    let month = value[5..7]
        .parse::<u32>()
        .map_err(|_| format!("Invalid release date: {value}"))?;
    let day = value[8..10]
        .parse::<u32>()
        .map_err(|_| format!("Invalid release date: {value}"))?;
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let days = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if leap => 29,
        2 => 28,
        _ => 0,
    };
    if year < 2020 || day == 0 || day > days {
        return Err(format!("Invalid release date: {value}"));
    }
    Ok(())
}

fn safe_relative_path(value: &str) -> Result<PathBuf, String> {
    safe_relative_path_buf(Path::new(value))
}

fn safe_relative_path_buf(value: &Path) -> Result<PathBuf, String> {
    if value.as_os_str().is_empty() || value.is_absolute() {
        return Err("Component package paths must be non-empty relative paths".into());
    }
    let mut clean = PathBuf::new();
    for component in value.components() {
        match component {
            Component::Normal(part) => clean.push(part),
            _ => {
                return Err(format!(
                    "Unsafe component package path: {}",
                    value.display()
                ));
            }
        }
    }
    Ok(clean)
}

fn path_to_catalog_string(value: &Path) -> Result<String, String> {
    let text = value
        .to_str()
        .ok_or_else(|| "Component package path is not valid UTF-8".to_string())?;
    Ok(text.replace('\\', "/"))
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ActiveComponent {
    schema_version: u32,
    id: String,
    kind: ComponentKind,
    active_version: String,
    previous_version: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct UpdateJournal {
    schema_version: u32,
    id: String,
    kind: ComponentKind,
    new_version: String,
    previous_version: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct InstalledManifest {
    schema_version: u32,
    id: String,
    kind: ComponentKind,
    version: String,
    package_sha256: String,
    entrypoint: Option<String>,
    #[serde(default)]
    game_id: Option<String>,
    #[serde(default)]
    locale: Option<String>,
    #[serde(default)]
    display_name: Option<String>,
    #[serde(default)]
    xbox_language: Option<u32>,
    #[serde(default)]
    compatibility: Compatibility,
    files: Vec<InstalledFile>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct InstalledFile {
    path: String,
    size: u64,
    sha256: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct LastAction {
    state: String,
    detail: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReadyLauncherPackage {
    schema_version: u32,
    version: String,
    size: u64,
    sha256: String,
    file_name: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct ActiveComponentStatus {
    pub id: String,
    pub version: String,
    pub healthy: bool,
    pub repair_required: bool,
    pub can_rollback: bool,
    pub last_action: Option<String>,
}

#[derive(Clone, Debug)]
pub struct InstalledVersionStatus {
    pub version: String,
    pub healthy: bool,
}

#[derive(Clone, Debug)]
pub struct InstalledLanguageStatus {
    pub id: String,
    pub version: String,
    pub healthy: bool,
    pub game_id: String,
    pub locale: String,
    pub display_name: String,
    pub xbox_language: u32,
}

#[derive(Clone, Debug)]
pub struct ActiveComponentPayload {
    pub version: String,
    pub kind: ComponentKind,
    pub root: PathBuf,
}

#[derive(Clone, Debug)]
pub struct PreparedLauncherReplacement {
    pub version: String,
    pub source_root: PathBuf,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RecoveryResult {
    Completed(String),
    RolledBack(String),
}

#[derive(Clone, Debug)]
pub struct ComponentStore {
    root: PathBuf,
    downloads_root: PathBuf,
    staging_root_base: PathBuf,
}

impl ComponentStore {
    pub fn new(root: PathBuf) -> Self {
        let downloads_root = root.join(".downloads");
        let staging_root_base = root.join(".staging");
        Self {
            root,
            downloads_root,
            staging_root_base,
        }
    }

    pub fn with_roots(root: PathBuf, downloads_root: PathBuf, staging_root: PathBuf) -> Self {
        Self {
            root,
            downloads_root,
            staging_root_base: staging_root,
        }
    }

    fn component_root(&self, id: &str) -> PathBuf {
        self.root.join(id)
    }

    fn versions_root(&self, id: &str) -> PathBuf {
        self.component_root(id).join("versions")
    }

    fn version_root(&self, id: &str, version: &str) -> PathBuf {
        self.versions_root(id).join(version)
    }

    fn active_path(&self, id: &str) -> PathBuf {
        self.component_root(id).join("active.toml")
    }

    fn journal_path(&self, id: &str) -> PathBuf {
        self.component_root(id).join("update-journal.toml")
    }

    fn last_action_path(&self, id: &str) -> PathBuf {
        self.component_root(id).join("last-action.toml")
    }

    fn launcher_ready_root_path(&self) -> PathBuf {
        self.component_root("launcher").join("ready")
    }

    fn staging_root(&self, id: &str, version: &str) -> PathBuf {
        self.staging_root_base.join(id).join(version)
    }

    pub fn staging_component_root(&self, id: &str) -> Result<PathBuf, String> {
        validate_component_id(id)?;
        Ok(self.staging_root_base.join(id))
    }

    fn download_component_root(&self, id: &str) -> Result<PathBuf, String> {
        validate_component_id(id)?;
        Ok(self.downloads_root.join(id))
    }

    pub fn staged_artifact_path(&self, release: &ComponentRelease) -> PathBuf {
        let artifact_name = match release.package {
            PackageFormat::PortableExe => "artifact.exe",
            PackageFormat::Zip => "artifact.zip",
        };
        self.downloads_root
            .join(&release.id)
            .join(&release.version)
            .join(artifact_name)
    }

    pub fn stage_local_artifact(
        &self,
        release: &ComponentRelease,
        source: &Path,
    ) -> Result<(), String> {
        validate_release_installable(release)?;
        verify_file_exact(source, release.size, &release.sha256)?;
        let destination = self.staged_artifact_path(release);
        if source != destination {
            copy_file_atomic(source, &destination)?;
        }
        verify_file_exact(&destination, release.size, &release.sha256)
    }

    pub fn prepare_launcher_replacement(&self) -> Result<PreparedLauncherReplacement, String> {
        let verifying_key = release_verifying_key()?;
        self.prepare_launcher_replacement_with_key(&verifying_key)
    }

    fn prepare_launcher_replacement_with_key(
        &self,
        verifying_key: &VerifyingKey,
    ) -> Result<PreparedLauncherReplacement, String> {
        let ready_root = self.launcher_ready_root_path();
        let ready = read_toml::<ReadyLauncherPackage>(&ready_root.join("ready.toml"))?;
        if ready.schema_version != INSTALL_SCHEMA_VERSION {
            return Err("Verified launcher update uses an unsupported schema".into());
        }
        Version::parse(&ready.version)
            .map_err(|_| "Verified launcher update has an invalid version".to_string())?;
        let artifact = ready_root.join(&ready.file_name);
        verify_file_exact(&artifact, ready.size, &ready.sha256)?;
        verify_signed_launcher_executable_with_key(&artifact, Some(&ready.version), verifying_key)?;

        let replacement = ready_root.join("replacement");
        if replacement.exists() {
            fs::remove_dir_all(&replacement).map_err(|error| {
                format!("Could not reset launcher replacement staging: {error}")
            })?;
        }
        fs::create_dir_all(&replacement)
            .map_err(|error| format!("Could not create launcher replacement staging: {error}"))?;
        copy_file_atomic(&artifact, &replacement.join("mojorecomp-launcher.exe"))?;
        Ok(PreparedLauncherReplacement {
            version: ready.version,
            source_root: replacement,
        })
    }

    pub fn active_status(&self, id: &str) -> Result<Option<ActiveComponentStatus>, String> {
        validate_component_id(id)?;
        let Some(active) = read_toml_optional::<ActiveComponent>(&self.active_path(id))? else {
            return Ok(None);
        };
        if active.schema_version != INSTALL_SCHEMA_VERSION || active.id != id {
            return Ok(Some(ActiveComponentStatus {
                id: id.to_string(),
                version: active.active_version,
                healthy: false,
                repair_required: true,
                can_rollback: false,
                last_action: self.last_action(id),
            }));
        }
        let healthy = component_kind_matches_id(id, &active.kind)
            && self
                .verify_version(id, &active.active_version)
                .is_ok_and(|manifest| manifest.kind == active.kind);
        Ok(Some(ActiveComponentStatus {
            id: id.to_string(),
            version: active.active_version,
            healthy,
            repair_required: !healthy,
            can_rollback: active.previous_version.as_deref().is_some_and(|previous| {
                self.verify_version(id, previous).is_ok()
                    && self
                        .ensure_change_keeps_dependents_compatible(id, previous)
                        .is_ok()
            }),
            last_action: self.last_action(id),
        }))
    }

    pub fn installed_versions(&self, id: &str) -> Result<Vec<InstalledVersionStatus>, String> {
        validate_component_id(id)?;
        let root = self.versions_root(id);
        if !root.is_dir() {
            return Ok(Vec::new());
        }

        let mut versions = Vec::new();
        for entry in fs::read_dir(&root)
            .map_err(|error| format!("Could not inspect installed versions for {id}: {error}"))?
        {
            let entry = entry.map_err(|error| {
                format!("Could not inspect installed version for {id}: {error}")
            })?;
            if !entry
                .file_type()
                .map_err(|error| format!("Could not inspect installed version for {id}: {error}"))?
                .is_dir()
            {
                continue;
            }
            let Ok(version) = entry.file_name().into_string() else {
                continue;
            };
            if Version::parse(&version).is_err() {
                continue;
            }
            versions.push(InstalledVersionStatus {
                healthy: self
                    .verify_version(id, &version)
                    .is_ok_and(|manifest| component_kind_matches_id(id, &manifest.kind)),
                version,
            });
        }
        versions.sort_by(|left, right| {
            let left_version = Version::parse(&left.version).ok();
            let right_version = Version::parse(&right.version).ok();
            right_version.cmp(&left_version)
        });
        Ok(versions)
    }

    pub fn active_languages(&self, game_id: &str) -> Result<Vec<InstalledLanguageStatus>, String> {
        validate_game_id(game_id)?;
        let mut languages = Vec::new();
        for manifest in self.active_installed_manifests()? {
            if manifest.kind != ComponentKind::Language
                || manifest.game_id.as_deref() != Some(game_id)
            {
                continue;
            }
            let Some(locale) = manifest.locale.clone() else {
                continue;
            };
            let Some(display_name) = manifest.display_name.clone() else {
                continue;
            };
            let Some(xbox_language) = manifest.xbox_language else {
                continue;
            };
            languages.push(InstalledLanguageStatus {
                id: manifest.id,
                version: manifest.version,
                healthy: true,
                game_id: game_id.to_string(),
                locale,
                display_name,
                xbox_language,
            });
        }
        languages.sort_by(|left, right| left.locale.cmp(&right.locale));
        Ok(languages)
    }

    fn active_installed_manifests(&self) -> Result<Vec<InstalledManifest>, String> {
        if !self.root.is_dir() {
            return Ok(Vec::new());
        }
        let mut manifests = Vec::new();
        for entry in fs::read_dir(&self.root)
            .map_err(|error| format!("Could not inspect installed components: {error}"))?
        {
            let entry =
                entry.map_err(|error| format!("Could not inspect installed component: {error}"))?;
            let metadata = entry
                .metadata()
                .map_err(|error| format!("Could not inspect installed component: {error}"))?;
            if !metadata.is_dir() {
                continue;
            }
            let Ok(id) = entry.file_name().into_string() else {
                continue;
            };
            if validate_component_id(&id).is_err() {
                continue;
            }
            let Some(active) = read_toml_optional::<ActiveComponent>(&self.active_path(&id))?
            else {
                continue;
            };
            if active.schema_version != INSTALL_SCHEMA_VERSION || active.id != id {
                continue;
            }
            if let Ok(manifest) = self.verify_version(&id, &active.active_version) {
                manifests.push(manifest);
            }
        }
        Ok(manifests)
    }

    fn ensure_change_keeps_dependents_compatible(
        &self,
        changed_id: &str,
        proposed_version: &str,
    ) -> Result<(), String> {
        validate_component_id(changed_id)?;
        let proposed = Version::parse(proposed_version)
            .map_err(|_| format!("Component {changed_id} has an invalid proposed version"))?;
        for manifest in self.active_installed_manifests()? {
            if manifest.id == changed_id {
                continue;
            }
            for requirement in &manifest.compatibility.requirements {
                if requirement.id != changed_id {
                    continue;
                }
                let minimum_ok = requirement
                    .min_version
                    .as_deref()
                    .and_then(|value| Version::parse(value).ok())
                    .is_none_or(|minimum| proposed >= minimum);
                let maximum_ok = requirement
                    .max_version
                    .as_deref()
                    .and_then(|value| Version::parse(value).ok())
                    .is_none_or(|maximum| proposed <= maximum);
                if minimum_ok && maximum_ok {
                    continue;
                }
                let required = match (&requirement.min_version, &requirement.max_version) {
                    (Some(min), Some(max)) if min == max => min.clone(),
                    (Some(min), Some(max)) => format!("{min} through {max}"),
                    (Some(min), None) => format!("{min} or newer"),
                    (None, Some(max)) => format!("{max} or older"),
                    (None, None) => "a compatible version".into(),
                };
                return Err(format!(
                    "Cannot activate {changed_id} {proposed_version}: {} {} requires {changed_id} {required}",
                    manifest.id, manifest.version
                ));
            }
        }
        Ok(())
    }

    pub fn ensure_launcher_version_compatible(&self, proposed_version: &str) -> Result<(), String> {
        let proposed = Version::parse(proposed_version).map_err(|_| {
            "Proposed launcher version is not valid semantic versioning".to_string()
        })?;
        for manifest in self.active_installed_manifests()? {
            let compatibility = &manifest.compatibility;
            if compatibility
                .min_launcher
                .as_deref()
                .and_then(|value| Version::parse(value).ok())
                .is_some_and(|minimum| proposed < minimum)
            {
                return Err(format!(
                    "{} {} requires MojoRecomp Launcher {} or newer",
                    manifest.id,
                    manifest.version,
                    compatibility.min_launcher.as_deref().unwrap_or_default()
                ));
            }
            if compatibility
                .max_launcher
                .as_deref()
                .and_then(|value| Version::parse(value).ok())
                .is_some_and(|maximum| proposed > maximum)
            {
                return Err(format!(
                    "{} {} supports MojoRecomp Launcher {} or older",
                    manifest.id,
                    manifest.version,
                    compatibility.max_launcher.as_deref().unwrap_or_default()
                ));
            }
        }
        Ok(())
    }

    pub fn active_payload(&self, id: &str) -> Result<Option<ActiveComponentPayload>, String> {
        validate_component_id(id)?;
        let Some(active) = read_toml_optional::<ActiveComponent>(&self.active_path(id))? else {
            return Ok(None);
        };
        let manifest = self.verify_version(id, &active.active_version)?;
        if !component_kind_matches_id(id, &active.kind) || manifest.kind != active.kind {
            return Err(format!(
                "Component {id} has inconsistent component metadata"
            ));
        }
        Ok(Some(ActiveComponentPayload {
            version: active.active_version.clone(),
            kind: active.kind,
            root: self
                .version_root(id, &active.active_version)
                .join("payload"),
        }))
    }

    pub fn active_runtime_is_ready(
        &self,
        id: &str,
        expected_entrypoint: &str,
        required_files: &[&str],
    ) -> Result<bool, String> {
        validate_component_id(id)?;
        safe_relative_path(expected_entrypoint)?;
        let Some(active) = read_toml_optional::<ActiveComponent>(&self.active_path(id))? else {
            return Ok(false);
        };
        if active.schema_version != INSTALL_SCHEMA_VERSION
            || active.id != id
            || active.kind != ComponentKind::Runtime
        {
            return Ok(false);
        }
        let manifest = match self.verify_version(id, &active.active_version) {
            Ok(manifest) => manifest,
            Err(_) => return Ok(false),
        };
        if manifest.kind != ComponentKind::Runtime
            || manifest.entrypoint.as_deref() != Some(expected_entrypoint)
        {
            return Ok(false);
        }
        let installed = manifest
            .files
            .iter()
            .map(|file| file.path.as_str())
            .collect::<HashSet<_>>();
        for path in required_files {
            safe_relative_path(path)?;
            if !installed.contains(path) {
                return Ok(false);
            }
        }
        Ok(true)
    }

    pub fn last_action_state(&self, id: &str) -> Option<String> {
        self.last_action(id)
    }

    pub fn record_action(&self, id: &str, state: &str, detail: &str) -> Result<(), String> {
        validate_component_id(id)?;
        self.write_last_action(id, state, detail)
    }

    pub fn stage_launcher_ready(&self, release: &ComponentRelease) -> Result<PathBuf, String> {
        let verifying_key = release_verifying_key()?;
        self.stage_launcher_ready_with_key(release, &verifying_key)
    }

    fn stage_launcher_ready_with_key(
        &self,
        release: &ComponentRelease,
        verifying_key: &VerifyingKey,
    ) -> Result<PathBuf, String> {
        validate_release_installable(release)?;
        if release.kind != ComponentKind::Launcher {
            return Err("Only launcher packages can be promoted to manual replacement".into());
        }
        if release.package != PackageFormat::PortableExe {
            return Err("Launcher updates must use the signed executable format".into());
        }
        let artifact = self.staged_artifact_path(release);
        verify_file_exact(&artifact, release.size, &release.sha256)?;
        verify_signed_launcher_executable_with_key(
            &artifact,
            Some(&release.version),
            verifying_key,
        )?;
        let ready_root = self.launcher_ready_root_path();
        if ready_root.exists() {
            fs::remove_dir_all(&ready_root)
                .map_err(|error| format!("Could not clear previous launcher update: {error}"))?;
        }
        fs::create_dir_all(&ready_root)
            .map_err(|error| format!("Could not create launcher update directory: {error}"))?;
        let file_name = "mojorecomp-launcher.exe".to_string();
        let destination = ready_root.join(&file_name);
        fs::rename(&artifact, &destination)
            .map_err(|error| format!("Could not preserve verified launcher update: {error}"))?;
        let ready = ReadyLauncherPackage {
            schema_version: INSTALL_SCHEMA_VERSION,
            version: release.version.clone(),
            size: release.size,
            sha256: release.sha256.clone(),
            file_name,
        };
        write_toml_atomic(&ready_root.join("ready.toml"), &ready)?;
        let component_staging = self.staging_component_root("launcher")?;
        if component_staging.exists() {
            let _ = fs::remove_dir_all(component_staging);
        }
        if let Ok(download_root) = self.download_component_root("launcher")
            && download_root.exists()
        {
            let _ = fs::remove_dir_all(download_root);
        }
        self.write_last_action(
            "launcher",
            "ready_restart",
            "Verified launcher executable is ready for replacement",
        )?;
        Ok(ready_root)
    }

    pub fn cleanup_launcher_ready(&self, current_version: &str) -> Result<(), String> {
        let ready_root = self.launcher_ready_root_path();
        let Some(ready) =
            read_toml_optional::<ReadyLauncherPackage>(&ready_root.join("ready.toml"))?
        else {
            return Ok(());
        };
        let current = Version::parse(current_version)
            .map_err(|_| "Current launcher version is not valid semantic versioning".to_string())?;
        let staged = Version::parse(&ready.version)
            .map_err(|_| "Staged launcher version is not valid semantic versioning".to_string())?;
        if current >= staged {
            fs::remove_dir_all(&ready_root)
                .map_err(|error| format!("Could not remove obsolete launcher update: {error}"))?;
        }
        Ok(())
    }

    pub fn prepare_runtime_launch(
        &self,
        id: &str,
        override_root: &Path,
    ) -> Result<Option<PathBuf>, String> {
        let Some(active) = read_toml_optional::<ActiveComponent>(&self.active_path(id))? else {
            return Ok(None);
        };
        if active.kind != ComponentKind::Runtime || !component_kind_matches_id(id, &active.kind) {
            return Err(format!("Component {id} is not a runtime"));
        }
        let manifest = self.verify_version(id, &active.active_version)?;
        if manifest.kind != ComponentKind::Runtime {
            return Err(format!("Runtime component {id} has inconsistent metadata"));
        }
        let entrypoint = manifest
            .entrypoint
            .clone()
            .ok_or_else(|| format!("Runtime component {id} has no entrypoint"))?;
        if !manifest.files.iter().any(|file| file.path == entrypoint) {
            return Err(format!(
                "Runtime component {id} does not contain its entrypoint"
            ));
        }
        let version_root = self.version_root(id, &active.active_version);
        let payload_root = version_root.join("payload");
        let run_root = version_root.join("run");
        fs::create_dir_all(&run_root)
            .map_err(|error| format!("Could not create runtime launch directory: {error}"))?;
        for file in &manifest.files {
            let relative = safe_relative_path(&file.path)?;
            let source = payload_root.join(&relative);
            let destination = run_root.join(&relative);
            let file_name = relative
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or("");
            let override_path = override_root.join(file_name);
            if matches!(file_name, "mojorecomp-ffmpeg.dll" | "mojorecomp-lzx.dll")
                && override_path.is_file()
            {
                copy_file_atomic(&override_path, &destination)?;
            } else {
                copy_file_atomic(&source, &destination)?;
            }
        }
        Ok(Some(run_root.join(entrypoint)))
    }

    pub fn migrate_component_from(
        &self,
        source: &ComponentStore,
        id: &str,
        required_files: &[&str],
    ) -> Result<bool, String> {
        validate_component_id(id)?;
        if self.active_status(id)?.is_some_and(|status| status.healthy)
            && let Some(payload) = self.active_payload(id)?
        {
            let complete = required_files.iter().try_fold(true, |complete, path| {
                let relative = safe_relative_path(path)?;
                Ok::<bool, String>(complete && payload.root.join(relative).is_file())
            })?;
            if complete {
                return Ok(false);
            }
        }
        let Some(source_active) = read_toml_optional::<ActiveComponent>(&source.active_path(id))?
        else {
            return Ok(false);
        };
        if source_active.schema_version != INSTALL_SCHEMA_VERSION || source_active.id != id {
            return Err(format!(
                "Legacy component {id} has unsupported active metadata"
            ));
        }
        if !component_kind_matches_id(id, &source_active.kind) {
            return Err(format!(
                "Legacy component {id} has the wrong component kind"
            ));
        }
        let source_manifest = source.verify_version(id, &source_active.active_version)?;
        if source_manifest.kind != source_active.kind {
            return Err(format!(
                "Legacy component {id} has inconsistent component metadata"
            ));
        }

        let mut versions = vec![source_active.active_version.clone()];
        if let Some(previous) = source_active.previous_version.as_deref()
            && source.verify_version(id, previous).is_ok()
        {
            versions.push(previous.to_string());
        }

        for version in &versions {
            let source_manifest = source.verify_version(id, version)?;
            if source_manifest.kind != source_active.kind {
                return Err(format!(
                    "Legacy component {id} has inconsistent component metadata"
                ));
            }
            if let Ok(manifest) = self.verify_version(id, version) {
                let required_payload_is_complete = if version == &source_active.active_version {
                    let installed = manifest
                        .files
                        .iter()
                        .map(|file| file.path.as_str())
                        .collect::<HashSet<_>>();
                    required_files.iter().try_fold(true, |complete, path| {
                        safe_relative_path(path)?;
                        Ok::<bool, String>(complete && installed.contains(path))
                    })?
                } else {
                    true
                };
                if manifest.kind == source_active.kind && required_payload_is_complete {
                    continue;
                }
            }
            let final_root = self.version_root(id, version);
            if final_root.exists() {
                fs::remove_dir_all(&final_root).map_err(|error| {
                    format!("Could not replace invalid migrated component version: {error}")
                })?;
            }
            let staging = self.staging_root(id, version).join("migration");
            if staging.exists() {
                fs::remove_dir_all(&staging).map_err(|error| {
                    format!("Could not reset component migration staging: {error}")
                })?;
            }
            fs::create_dir_all(&staging).map_err(|error| {
                format!("Could not create component migration staging: {error}")
            })?;
            copy_directory_tree(
                &source.version_root(id, version).join("payload"),
                &staging.join("payload"),
            )?;
            fs::copy(
                source.version_root(id, version).join("installation.toml"),
                staging.join("installation.toml"),
            )
            .map_err(|error| format!("Could not copy component installation metadata: {error}"))?;
            if version == &source_active.active_version {
                for path in required_files {
                    let relative = safe_relative_path(path)?;
                    if !staging.join("payload").join(relative).is_file() {
                        let _ = fs::remove_dir_all(&staging);
                        return Err(format!(
                            "Legacy component {id} is missing a required file: {path}"
                        ));
                    }
                }
            }
            fs::create_dir_all(self.versions_root(id)).map_err(|error| {
                format!("Could not create migrated component versions: {error}")
            })?;
            fs::rename(&staging, &final_root).map_err(|error| {
                format!("Could not activate migrated component version: {error}")
            })?;
            match self.verify_version(id, version) {
                Ok(manifest) if manifest.kind == source_active.kind => {}
                Ok(_) => {
                    let _ = fs::remove_dir_all(&final_root);
                    return Err(format!(
                        "Migrated component {id} has inconsistent component metadata"
                    ));
                }
                Err(error) => {
                    let _ = fs::remove_dir_all(&final_root);
                    return Err(format!("Migrated component validation failed: {error}"));
                }
            }
        }

        let previous_version = source_active
            .previous_version
            .filter(|version| self.verify_version(id, version).is_ok());
        let migrated = ActiveComponent {
            schema_version: INSTALL_SCHEMA_VERSION,
            id: id.to_string(),
            kind: source_active.kind,
            active_version: source_active.active_version,
            previous_version,
        };
        write_toml_atomic(&self.active_path(id), &migrated)?;
        let status = self
            .active_status(id)?
            .ok_or_else(|| format!("Migrated component {id} did not become active"))?;
        if !status.healthy {
            return Err(format!("Migrated component {id} failed validation"));
        }
        let payload = self
            .active_payload(id)?
            .ok_or_else(|| format!("Migrated component {id} has no active payload"))?;
        for path in required_files {
            let relative = safe_relative_path(path)?;
            if !payload.root.join(relative).is_file() {
                return Err(format!(
                    "Migrated component {id} is missing a required file: {path}"
                ));
            }
        }
        self.write_last_action(
            id,
            "migrated",
            "Component migrated from legacy Local AppData storage",
        )?;

        let source_root = source.component_root(id);
        if source_root.exists() {
            fs::remove_dir_all(&source_root)
                .map_err(|error| format!("Could not remove validated legacy component: {error}"))?;
        }
        Ok(true)
    }

    pub fn import_migrated_runtime(
        &self,
        id: &str,
        version: &str,
        entrypoint: &str,
        source_root: &Path,
        required_files: &[&str],
    ) -> Result<bool, String> {
        validate_component_id(id)?;
        let game_id = id
            .strip_prefix("runtime.")
            .ok_or_else(|| format!("Component {id} is not a runtime"))?;
        validate_game_id(game_id)?;
        Version::parse(version)
            .map_err(|_| "Migrated runtime version is not semantic versioning")?;
        safe_relative_path(entrypoint)?;
        if !required_files.contains(&entrypoint) {
            return Err("Migrated runtime entrypoint is not in the required file set".into());
        }
        if self.active_runtime_is_ready(id, entrypoint, required_files)? {
            return Ok(false);
        }

        self.recover_component(id)?;
        let staging = self.staging_root(id, version).join("legacy-runtime");
        if staging.exists() {
            fs::remove_dir_all(&staging)
                .map_err(|error| format!("Could not reset legacy runtime migration: {error}"))?;
        }
        let payload_root = staging.join("payload");
        fs::create_dir_all(&payload_root).map_err(|error| {
            format!("Could not create legacy runtime migration staging: {error}")
        })?;

        let mut files = Vec::new();
        let mut package_hasher = Sha256::new();
        for path in required_files {
            let relative = safe_relative_path(path)?;
            let source = source_root.join(&relative);
            let metadata = fs::metadata(&source).map_err(|error| {
                format!(
                    "Legacy runtime file is unavailable {}: {error}",
                    source.display()
                )
            })?;
            if !metadata.is_file() {
                return Err(format!(
                    "Legacy runtime path is not a file: {}",
                    source.display()
                ));
            }
            let mut input = File::open(&source)
                .map_err(|error| format!("Could not read legacy runtime file: {error}"))?;
            let mut hasher = Sha256::new();
            let mut buffer = [0u8; 64 * 1024];
            loop {
                let count = input
                    .read(&mut buffer)
                    .map_err(|error| format!("Could not hash legacy runtime file: {error}"))?;
                if count == 0 {
                    break;
                }
                hasher.update(&buffer[..count]);
            }
            let sha256 = format!("{:x}", hasher.finalize());
            let destination = payload_root.join(&relative);
            copy_file_atomic(&source, &destination)?;
            verify_file_exact(&destination, metadata.len(), &sha256)?;
            package_hasher.update(path.as_bytes());
            package_hasher.update(sha256.as_bytes());
            files.push(InstalledFile {
                path: (*path).to_string(),
                size: metadata.len(),
                sha256,
            });
        }

        let manifest = InstalledManifest {
            schema_version: INSTALL_SCHEMA_VERSION,
            id: id.to_string(),
            kind: ComponentKind::Runtime,
            version: version.to_string(),
            package_sha256: format!("{:x}", package_hasher.finalize()),
            entrypoint: Some(entrypoint.to_string()),
            game_id: id.strip_prefix("runtime.").map(str::to_string),
            locale: None,
            display_name: None,
            xbox_language: None,
            compatibility: Compatibility::default(),
            files,
        };
        write_toml_atomic(&staging.join("installation.toml"), &manifest)?;

        let previous = match read_toml_optional::<ActiveComponent>(&self.active_path(id))? {
            Some(state)
                if state.active_version != version
                    && self.verify_version(id, &state.active_version).is_ok() =>
            {
                Some(state.active_version)
            }
            Some(state) => state
                .previous_version
                .filter(|previous| self.verify_version(id, previous).is_ok()),
            None => None,
        };
        write_toml_atomic(
            &self.journal_path(id),
            &UpdateJournal {
                schema_version: INSTALL_SCHEMA_VERSION,
                id: id.to_string(),
                kind: ComponentKind::Runtime,
                new_version: version.to_string(),
                previous_version: previous.clone(),
            },
        )?;
        let final_root = self.version_root(id, version);
        fs::create_dir_all(self.versions_root(id))
            .map_err(|error| format!("Could not create migrated runtime versions: {error}"))?;
        if final_root.exists() {
            fs::remove_dir_all(&final_root)
                .map_err(|error| format!("Could not replace migrated runtime version: {error}"))?;
        }
        fs::rename(&staging, &final_root)
            .map_err(|error| format!("Could not activate migrated runtime files: {error}"))?;
        self.verify_version(id, version)?;
        write_toml_atomic(
            &self.active_path(id),
            &ActiveComponent {
                schema_version: INSTALL_SCHEMA_VERSION,
                id: id.to_string(),
                kind: ComponentKind::Runtime,
                active_version: version.to_string(),
                previous_version: previous,
            },
        )?;
        fs::remove_file(self.journal_path(id))
            .map_err(|error| format!("Could not finalize migrated runtime activation: {error}"))?;
        self.write_last_action(
            id,
            "migrated",
            "Runtime migrated from the legacy embedded Local AppData cache",
        )?;
        let stage_parent = self.staging_root(id, version);
        if stage_parent.exists() {
            let _ = fs::remove_dir_all(stage_parent);
        }
        Ok(true)
    }

    pub fn recover_all(&self) -> Result<Vec<RecoveryResult>, String> {
        let mut recovered = Vec::new();
        if self.root.is_dir() {
            for entry in fs::read_dir(&self.root)
                .map_err(|error| format!("Could not inspect component storage: {error}"))?
            {
                let entry = entry
                    .map_err(|error| format!("Could not inspect component storage: {error}"))?;
                if !entry
                    .file_type()
                    .map_err(|error| error.to_string())?
                    .is_dir()
                {
                    continue;
                }
                let Some(id) = entry.file_name().to_str().map(str::to_string) else {
                    continue;
                };
                if validate_component_id(&id).is_err() {
                    continue;
                }
                if let Some(result) = self.recover_component(&id)? {
                    recovered.push(result);
                }
            }
        }
        if self.staging_root_base.exists() {
            fs::remove_dir_all(&self.staging_root_base)
                .map_err(|error| format!("Could not clean interrupted update staging: {error}"))?;
        }
        Ok(recovered)
    }

    pub fn recover_component(&self, id: &str) -> Result<Option<RecoveryResult>, String> {
        let Some(journal) = read_toml_optional::<UpdateJournal>(&self.journal_path(id))? else {
            return Ok(None);
        };
        if journal.schema_version != INSTALL_SCHEMA_VERSION || journal.id != id {
            return Err(format!("Update journal for {id} has an unsupported schema"));
        }
        let active = read_toml_optional::<ActiveComponent>(&self.active_path(id))?;
        if active
            .as_ref()
            .is_some_and(|state| state.active_version == journal.new_version)
        {
            if self.verify_version(id, &journal.new_version).is_ok() {
                fs::remove_file(self.journal_path(id)).map_err(|error| {
                    format!("Could not finalize recovered update journal: {error}")
                })?;
                self.write_last_action(
                    id,
                    "recovered",
                    "Interrupted activation completed successfully",
                )?;
                return Ok(Some(RecoveryResult::Completed(id.to_string())));
            }

            let new_root = self.version_root(id, &journal.new_version);
            if new_root.exists() {
                fs::remove_dir_all(&new_root).map_err(|error| {
                    format!("Could not remove the invalid recovered component version: {error}")
                })?;
            }
            let restored = journal
                .previous_version
                .as_deref()
                .filter(|version| self.verify_version(id, version).is_ok())
                .map(|version| ActiveComponent {
                    schema_version: INSTALL_SCHEMA_VERSION,
                    id: id.to_string(),
                    kind: journal.kind.clone(),
                    active_version: version.to_string(),
                    previous_version: None,
                });
            if let Some(restored) = restored {
                write_toml_atomic(&self.active_path(id), &restored)?;
            } else if self.active_path(id).exists() {
                fs::remove_file(self.active_path(id)).map_err(|error| {
                    format!("Could not deactivate the invalid recovered component: {error}")
                })?;
            }
            fs::remove_file(self.journal_path(id)).map_err(|error| {
                format!("Could not finalize rolled-back update journal: {error}")
            })?;
            self.write_last_action(
                id,
                "rolled_back",
                "Interrupted activation failed validation and was rolled back",
            )?;
            return Ok(Some(RecoveryResult::RolledBack(id.to_string())));
        }
        let new_root = self.version_root(id, &journal.new_version);
        if new_root.exists() {
            fs::remove_dir_all(&new_root).map_err(|error| {
                format!("Could not roll back interrupted component update: {error}")
            })?;
        }
        fs::remove_file(self.journal_path(id))
            .map_err(|error| format!("Could not remove rolled-back update journal: {error}"))?;
        self.write_last_action(id, "rolled_back", "Interrupted activation was rolled back")?;
        Ok(Some(RecoveryResult::RolledBack(id.to_string())))
    }

    pub fn install_staged(&self, release: &ComponentRelease) -> Result<(), String> {
        self.install_staged_with_activation(release, true)
    }

    pub fn install_staged_inactive(&self, release: &ComponentRelease) -> Result<(), String> {
        if release.kind != ComponentKind::Runtime {
            return Err("Only runtime versions can be downloaded without activation".into());
        }
        self.install_staged_with_activation(release, false)
    }

    fn install_staged_with_activation(
        &self,
        release: &ComponentRelease,
        activate: bool,
    ) -> Result<(), String> {
        validate_release_installable(release)?;
        if release.kind == ComponentKind::Launcher {
            return Err(
                "Launcher packages are staged for manual replacement and are not activated in-process"
                    .into(),
            );
        }
        if activate {
            self.ensure_change_keeps_dependents_compatible(&release.id, &release.version)?;
        } else if read_toml_optional::<ActiveComponent>(&self.active_path(&release.id))?
            .is_some_and(|active| active.active_version == release.version)
        {
            return Err(format!(
                "Component {} version {} is already active",
                release.id, release.version
            ));
        }
        self.recover_component(&release.id)?;
        let artifact = self.staged_artifact_path(release);
        verify_file_exact(&artifact, release.size, &release.sha256)?;
        let staging = self
            .staging_root(&release.id, &release.version)
            .join("unpacked");
        if staging.exists() {
            fs::remove_dir_all(&staging)
                .map_err(|error| format!("Could not reset component unpack staging: {error}"))?;
        }
        fs::create_dir_all(&staging)
            .map_err(|error| format!("Could not create component unpack staging: {error}"))?;
        let files = extract_verified_zip(release, &artifact, &staging)?;
        let manifest = InstalledManifest {
            schema_version: INSTALL_SCHEMA_VERSION,
            id: release.id.clone(),
            kind: release.kind.clone(),
            version: release.version.clone(),
            package_sha256: release.sha256.clone(),
            entrypoint: release.entrypoint.clone(),
            game_id: release.game_id.clone(),
            locale: release.locale.clone(),
            display_name: release.display_name.clone(),
            xbox_language: release.xbox_language,
            compatibility: release.compatibility.clone(),
            files,
        };
        write_toml_atomic(&staging.join("installation.toml"), &manifest)?;

        let previous = if activate {
            read_toml_optional::<ActiveComponent>(&self.active_path(&release.id))?.and_then(
                |state| {
                    if state.active_version == release.version {
                        state
                            .previous_version
                            .filter(|version| version != &release.version)
                    } else {
                        Some(state.active_version)
                    }
                },
            )
        } else {
            None
        };
        if activate {
            let journal = UpdateJournal {
                schema_version: INSTALL_SCHEMA_VERSION,
                id: release.id.clone(),
                kind: release.kind.clone(),
                new_version: release.version.clone(),
                previous_version: previous.clone(),
            };
            write_toml_atomic(&self.journal_path(&release.id), &journal)?;
        }

        let final_root = self.version_root(&release.id, &release.version);
        fs::create_dir_all(self.versions_root(&release.id))
            .map_err(|error| format!("Could not create component versions directory: {error}"))?;
        if final_root.exists() {
            fs::remove_dir_all(&final_root).map_err(|error| {
                format!("Could not replace an existing component version: {error}")
            })?;
        }
        fs::rename(&staging, &final_root)
            .map_err(|error| format!("Could not move component version into storage: {error}"))?;

        if activate {
            let active = ActiveComponent {
                schema_version: INSTALL_SCHEMA_VERSION,
                id: release.id.clone(),
                kind: release.kind.clone(),
                active_version: release.version.clone(),
                previous_version: previous,
            };
            write_toml_atomic(&self.active_path(&release.id), &active)?;
            fs::remove_file(self.journal_path(&release.id)).map_err(|error| {
                format!("Component activated but update journal could not be finalized: {error}")
            })?;
            self.write_last_action(
                &release.id,
                "installed",
                "Component update installed successfully",
            )?;
        } else {
            self.write_last_action(
                &release.id,
                "downloaded",
                "Runtime version downloaded and verified",
            )?;
        }
        let stage_parent = self.staging_root(&release.id, &release.version);
        if stage_parent.exists() {
            let _ = fs::remove_dir_all(stage_parent);
        }
        if let Ok(download_root) = self.download_component_root(&release.id)
            && download_root.exists()
        {
            let _ = fs::remove_dir_all(download_root);
        }
        Ok(())
    }

    pub fn activate_version(&self, id: &str, version: &str) -> Result<(), String> {
        validate_component_id(id)?;
        Version::parse(version)
            .map_err(|_| format!("Component {id} has an invalid version: {version}"))?;
        self.recover_component(id)?;
        let manifest = self.verify_version(id, version)?;
        if manifest.kind == ComponentKind::Launcher
            || !component_kind_matches_id(id, &manifest.kind)
        {
            return Err(format!(
                "Installed version {id} {version} has inconsistent metadata"
            ));
        }
        self.ensure_change_keeps_dependents_compatible(id, version)?;

        let existing = read_toml_optional::<ActiveComponent>(&self.active_path(id))?;
        if existing
            .as_ref()
            .is_some_and(|active| active.active_version == version)
        {
            return Ok(());
        }
        let previous_version = existing.and_then(|active| {
            (active.schema_version == INSTALL_SCHEMA_VERSION
                && active.id == id
                && active.kind == manifest.kind)
                .then_some(active.active_version)
        });
        let active = ActiveComponent {
            schema_version: INSTALL_SCHEMA_VERSION,
            id: id.to_string(),
            kind: manifest.kind,
            active_version: version.to_string(),
            previous_version,
        };
        write_toml_atomic(&self.active_path(id), &active)?;
        self.write_last_action(id, "activated", "Installed component version activated")
    }

    pub fn remove_version(&self, id: &str, version: &str) -> Result<(), String> {
        validate_component_id(id)?;
        Version::parse(version)
            .map_err(|_| format!("Component {id} has an invalid version: {version}"))?;
        self.recover_component(id)?;
        let version_root = self.version_root(id, version);
        if !version_root.is_dir() {
            return Err(format!("Component {id} version {version} is not installed"));
        }

        if let Some(mut active) = read_toml_optional::<ActiveComponent>(&self.active_path(id))? {
            if active.active_version == version {
                fs::remove_file(self.active_path(id))
                    .map_err(|error| format!("Could not deactivate {id} {version}: {error}"))?;
            } else if active.previous_version.as_deref() == Some(version) {
                active.previous_version = None;
                write_toml_atomic(&self.active_path(id), &active)?;
            }
        }

        fs::remove_dir_all(&version_root)
            .map_err(|error| format!("Could not remove {id} {version}: {error}"))?;
        self.write_last_action(id, "removed", "Installed component version removed")
    }

    pub fn rollback(&self, id: &str) -> Result<(), String> {
        let active = read_toml_optional::<ActiveComponent>(&self.active_path(id))?
            .ok_or_else(|| format!("Component {id} has no active version"))?;
        let previous = active
            .previous_version
            .clone()
            .ok_or_else(|| format!("Component {id} has no previous version to restore"))?;
        self.verify_version(id, &previous)?;
        self.ensure_change_keeps_dependents_compatible(id, &previous)?;
        let restored = ActiveComponent {
            schema_version: INSTALL_SCHEMA_VERSION,
            id: id.to_string(),
            kind: active.kind,
            active_version: previous,
            previous_version: Some(active.active_version),
        };
        write_toml_atomic(&self.active_path(id), &restored)?;
        self.write_last_action(id, "rolled_back", "Previous component version restored")?;
        Ok(())
    }

    pub fn deactivate(&self, id: &str) -> Result<(), String> {
        validate_component_id(id)?;
        let path = self.active_path(id);
        if !path.is_file() {
            return Err(format!("Component {id} has no active downloaded version"));
        }
        fs::remove_file(&path)
            .map_err(|error| format!("Could not deactivate component {id}: {error}"))?;
        self.write_last_action(id, "rolled_back", "Bundled component version restored")
    }

    pub fn prepare_repair(&self, id: &str, version: &str) -> Result<(), String> {
        let Some(status) = self.active_status(id)? else {
            return Ok(());
        };
        if status.healthy || status.version != version {
            return Ok(());
        }
        if self.rollback(id).is_err() {
            self.deactivate(id)?;
        }
        Ok(())
    }

    fn verify_version(&self, id: &str, version: &str) -> Result<InstalledManifest, String> {
        let version_root = self.version_root(id, version);
        let payload_root = version_root.join("payload");
        let manifest_path = version_root.join("installation.toml");
        let manifest: InstalledManifest = read_toml(&manifest_path)?;
        if manifest.schema_version != INSTALL_SCHEMA_VERSION
            || manifest.id != id
            || manifest.version != version
        {
            return Err(format!(
                "Installed component metadata is invalid for {id} {version}"
            ));
        }
        for file in &manifest.files {
            let relative = safe_relative_path(&file.path)?;
            verify_file_exact(&payload_root.join(relative), file.size, &file.sha256)?;
        }
        Ok(manifest)
    }

    fn last_action(&self, id: &str) -> Option<String> {
        read_toml_optional::<LastAction>(&self.last_action_path(id))
            .ok()
            .flatten()
            .map(|value| value.state)
    }

    fn write_last_action(&self, id: &str, state: &str, detail: &str) -> Result<(), String> {
        write_toml_atomic(
            &self.last_action_path(id),
            &LastAction {
                state: state.to_string(),
                detail: detail.to_string(),
            },
        )
    }
}

fn copy_directory_tree(source: &Path, destination: &Path) -> Result<(), String> {
    fs::create_dir_all(destination)
        .map_err(|error| format!("Could not create component migration directory: {error}"))?;
    for entry in fs::read_dir(source)
        .map_err(|error| format!("Could not inspect component migration source: {error}"))?
    {
        let entry = entry
            .map_err(|error| format!("Could not inspect component migration source: {error}"))?;
        let file_type = entry
            .file_type()
            .map_err(|error| format!("Could not inspect component migration entry: {error}"))?;
        if file_type.is_symlink() {
            return Err("Legacy component contains a symbolic link".into());
        }
        let target = destination.join(entry.file_name());
        if file_type.is_dir() {
            copy_directory_tree(&entry.path(), &target)?;
        } else if file_type.is_file() {
            fs::copy(entry.path(), target)
                .map_err(|error| format!("Could not copy legacy component file: {error}"))?;
        }
    }
    Ok(())
}

pub fn download_release<F>(
    store: &ComponentStore,
    release: &ComponentRelease,
    mut progress: F,
) -> Result<PathBuf, String>
where
    F: FnMut(u64, u64),
{
    validate_release(release)?;
    validate_resolved_public_https_url(&release.url)?;
    let component_staging = store.staging_component_root(&release.id)?;
    if component_staging.exists() {
        fs::remove_dir_all(&component_staging)
            .map_err(|error| format!("Could not reset update staging: {error}"))?;
    }
    let component_downloads = store.download_component_root(&release.id)?;
    if component_downloads.exists() {
        fs::remove_dir_all(&component_downloads)
            .map_err(|error| format!("Could not reset update downloads: {error}"))?;
    }
    let staging = store.staging_root(&release.id, &release.version);
    fs::create_dir_all(&staging)
        .map_err(|error| format!("Could not create update staging: {error}"))?;
    let required_space = release
        .size
        .saturating_add(release.unpacked_size.unwrap_or(0))
        .saturating_add(UPDATE_DISK_MARGIN_BYTES);
    let available = fs2::available_space(&staging)
        .map_err(|error| format!("Could not query update staging free space: {error}"))?;
    if available < required_space {
        return Err(format!(
            "Not enough free space for update staging: need {required_space} bytes, {available} bytes available"
        ));
    }
    ensure_tls_crypto_provider()?;
    let client = reqwest::blocking::Client::builder()
        .redirect(public_https_redirect_policy())
        .connect_timeout(Duration::from_secs(20))
        .timeout(Duration::from_secs(120))
        .build()
        .map_err(|error| format!("Could not initialize update client: {error}"))?;
    let response = client
        .get(&release.url)
        .send()
        .and_then(|response| response.error_for_status())
        .map_err(|error| format!("Could not download component {}: {error}", release.id))?;
    validate_resolved_public_https_url(response.url().as_str())?;
    if response
        .content_length()
        .is_some_and(|length| length != release.size)
    {
        return Err(format!(
            "Component {} download size does not match the catalog",
            release.id
        ));
    }
    let artifact = store.staged_artifact_path(release);
    stage_reader(
        response,
        &artifact,
        release.size,
        &release.sha256,
        &mut progress,
    )?;
    Ok(artifact)
}

fn stage_reader<R: Read, F: FnMut(u64, u64)>(
    mut reader: R,
    destination: &Path,
    expected_size: u64,
    expected_sha256: &str,
    progress: &mut F,
) -> Result<(), String> {
    validate_sha256(expected_sha256)?;
    let parent = destination
        .parent()
        .ok_or_else(|| "Update staging path has no parent directory".to_string())?;
    fs::create_dir_all(parent)
        .map_err(|error| format!("Could not create update staging directory: {error}"))?;
    let temporary = destination.with_extension("zip.part");
    if temporary.exists() {
        fs::remove_file(&temporary)
            .map_err(|error| format!("Could not remove stale update download: {error}"))?;
    }
    let result = (|| {
        let mut file = File::create(&temporary)
            .map_err(|error| format!("Could not create staged update: {error}"))?;
        let mut hasher = Sha256::new();
        let mut total = 0u64;
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let count = reader
                .read(&mut buffer)
                .map_err(|error| format!("Could not read update download: {error}"))?;
            if count == 0 {
                break;
            }
            total = total.saturating_add(count as u64);
            if total > expected_size {
                return Err("Downloaded component exceeded the catalog size".into());
            }
            file.write_all(&buffer[..count])
                .map_err(|error| format!("Could not write staged update: {error}"))?;
            hasher.update(&buffer[..count]);
            progress(total, expected_size);
        }
        file.sync_all()
            .map_err(|error| format!("Could not flush staged update: {error}"))?;
        if total != expected_size {
            return Err(format!(
                "Downloaded component size mismatch: expected {expected_size}, got {total}"
            ));
        }
        let actual = format!("{:x}", hasher.finalize());
        if actual != expected_sha256 {
            return Err("Downloaded component SHA-256 does not match the catalog".into());
        }
        atomic_replace(&temporary, destination)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
        let _ = fs::remove_file(destination);
    }
    result
}

fn extract_verified_zip(
    release: &ComponentRelease,
    artifact: &Path,
    staging_root: &Path,
) -> Result<Vec<InstalledFile>, String> {
    let file = File::open(artifact)
        .map_err(|error| format!("Could not open verified update package: {error}"))?;
    let mut archive = ZipArchive::new(file)
        .map_err(|error| format!("Component update is not a valid ZIP archive: {error}"))?;
    if archive.len() > 4096 {
        return Err("Update ZIP contains too many entries".into());
    }
    let payload_root = staging_root.join("payload");
    fs::create_dir_all(&payload_root)
        .map_err(|error| format!("Could not create component payload staging: {error}"))?;
    let unpacked_limit = release.unpacked_size.unwrap_or(0);
    let mut unpacked = 0u64;
    let mut installed = Vec::new();
    let mut seen = HashSet::new();

    let archive_root = {
        let mut candidate: Option<PathBuf> = None;
        let mut stripped_files = HashSet::new();
        let mut can_strip = true;
        for index in 0..archive.len() {
            let entry = archive
                .by_index(index)
                .map_err(|error| format!("Could not inspect update ZIP entry: {error}"))?;
            let Some(enclosed) = entry.enclosed_name() else {
                return Err("Update ZIP contains an unsafe path".into());
            };
            if entry.is_dir() {
                continue;
            }
            let relative = safe_relative_path_buf(&enclosed)?;
            let mut components = relative.components();
            let Some(Component::Normal(first)) = components.next() else {
                can_strip = false;
                break;
            };
            let remainder: PathBuf = components.collect();
            if remainder.as_os_str().is_empty() {
                can_strip = false;
                break;
            }
            let first = PathBuf::from(first);
            if let Some(existing) = &candidate {
                if !existing.as_os_str().eq_ignore_ascii_case(first.as_os_str()) {
                    can_strip = false;
                    break;
                }
            } else {
                candidate = Some(first);
            }
            stripped_files.insert(path_to_catalog_string(&remainder)?.to_ascii_lowercase());
        }
        if can_strip
            && !release.required_files.is_empty()
            && release
                .required_files
                .iter()
                .all(|required| stripped_files.contains(&required.to_ascii_lowercase()))
        {
            candidate
        } else {
            None
        }
    };

    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| format!("Could not inspect update ZIP entry: {error}"))?;
        let Some(enclosed) = entry.enclosed_name() else {
            return Err("Update ZIP contains an unsafe path".into());
        };
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err("Update ZIP contains a symbolic link".into());
        }
        let relative = safe_relative_path_buf(&enclosed)?;
        let relative = if let Some(root) = &archive_root {
            relative
                .strip_prefix(root)
                .map(Path::to_path_buf)
                .unwrap_or(relative)
        } else {
            relative
        };
        if relative.as_os_str().is_empty() {
            continue;
        }
        let destination = payload_root.join(&relative);
        if entry.is_dir() {
            fs::create_dir_all(&destination)
                .map_err(|error| format!("Could not create update package directory: {error}"))?;
            continue;
        }
        let relative_text = path_to_catalog_string(&relative)?;
        let path_key = relative_text.to_ascii_lowercase();
        if !seen.insert(path_key) {
            return Err(format!(
                "Update ZIP contains a duplicate path: {relative_text}"
            ));
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)
                .map_err(|error| format!("Could not create update package path: {error}"))?;
        }
        let mut output = File::create(&destination)
            .map_err(|error| format!("Could not create extracted update file: {error}"))?;
        let mut hasher = Sha256::new();
        let mut size = 0u64;
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let count = entry
                .read(&mut buffer)
                .map_err(|error| format!("Could not extract update file: {error}"))?;
            if count == 0 {
                break;
            }
            size = size.saturating_add(count as u64);
            unpacked = unpacked.saturating_add(count as u64);
            if unpacked > unpacked_limit {
                return Err("Update ZIP exceeds the declared unpacked size".into());
            }
            output
                .write_all(&buffer[..count])
                .map_err(|error| format!("Could not write extracted update file: {error}"))?;
            hasher.update(&buffer[..count]);
        }
        output
            .sync_all()
            .map_err(|error| format!("Could not flush extracted update file: {error}"))?;
        installed.push(InstalledFile {
            path: relative_text,
            size,
            sha256: format!("{:x}", hasher.finalize()),
        });
    }

    if unpacked != unpacked_limit {
        return Err(format!(
            "Update ZIP unpacked size mismatch: expected {unpacked_limit}, got {unpacked}"
        ));
    }
    for required in &release.required_files {
        if !seen.contains(&required.to_ascii_lowercase()) {
            return Err(format!(
                "Update package is missing required file: {required}"
            ));
        }
    }
    Ok(installed)
}

pub fn apply_launcher_replacement_tree(
    source_root: &Path,
    target_root: &Path,
) -> Result<(), String> {
    if !source_root.is_dir() || !target_root.is_dir() {
        return Err("Launcher replacement source or destination directory is unavailable".into());
    }
    let source_exe = source_root.join("mojorecomp-launcher.exe");
    if !source_exe.is_file() {
        return Err("Launcher replacement source has no mojorecomp-launcher.exe".into());
    }
    copy_file_atomic(&source_exe, &target_root.join("mojorecomp-launcher.exe"))?;
    for name in [
        "LICENSE",
        "THIRD_PARTY_NOTICES.txt",
        "README.txt",
        OFFLINE_PACKAGE_MANIFEST,
        OFFLINE_PACKAGE_SIGNATURE,
    ] {
        let legacy = target_root.join(name);
        if legacy.exists() {
            fs::remove_file(&legacy).map_err(|error| {
                format!(
                    "Could not remove legacy launcher companion file {}: {error}",
                    legacy.display()
                )
            })?;
        }
    }
    Ok(())
}

pub fn backup_launcher_replacement_tree(
    target_root: &Path,
    backup_root: &Path,
) -> Result<(), String> {
    if !target_root.is_dir() || backup_root.exists() {
        return Err("Launcher replacement backup paths are invalid".into());
    }
    fs::create_dir_all(backup_root)
        .map_err(|error| format!("Could not create launcher update backup: {error}"))?;
    for name in [
        "mojorecomp-launcher.exe",
        "LICENSE",
        "THIRD_PARTY_NOTICES.txt",
        "README.txt",
        OFFLINE_PACKAGE_MANIFEST,
        OFFLINE_PACKAGE_SIGNATURE,
    ] {
        let source = target_root.join(name);
        match fs::symlink_metadata(&source) {
            Ok(metadata) if metadata.is_file() && !metadata.file_type().is_symlink() => {
                fs::copy(&source, backup_root.join(name)).map_err(|error| {
                    format!(
                        "Could not back up launcher file {}: {error}",
                        source.display()
                    )
                })?;
            }
            Ok(_) => {
                return Err(format!(
                    "Launcher update target is not a regular file: {}",
                    source.display()
                ));
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => {
                return Err(format!(
                    "Could not inspect launcher file {}: {error}",
                    source.display()
                ));
            }
        }
    }
    Ok(())
}

pub fn restore_launcher_replacement_tree(
    target_root: &Path,
    backup_root: &Path,
) -> Result<(), String> {
    if !target_root.is_dir() || !backup_root.is_dir() {
        return Err("Launcher replacement restore paths are invalid".into());
    }
    for name in [
        "mojorecomp-launcher.exe",
        "LICENSE",
        "THIRD_PARTY_NOTICES.txt",
        "README.txt",
        OFFLINE_PACKAGE_MANIFEST,
        OFFLINE_PACKAGE_SIGNATURE,
    ] {
        let backup = backup_root.join(name);
        let target = target_root.join(name);
        if backup.is_file() {
            copy_file_atomic(&backup, &target)?;
        } else if target.exists() {
            fs::remove_file(&target).map_err(|error| {
                format!(
                    "Could not remove newly installed launcher file {}: {error}",
                    target.display()
                )
            })?;
        }
    }
    Ok(())
}

fn verify_file_exact(path: &Path, expected_size: u64, expected_sha256: &str) -> Result<(), String> {
    let metadata = fs::metadata(path).map_err(|error| {
        format!(
            "Required component file is unavailable {}: {error}",
            path.display()
        )
    })?;
    if !metadata.is_file() || metadata.len() != expected_size {
        return Err(format!(
            "Component file size is invalid: {}",
            path.display()
        ));
    }
    let mut file = File::open(path)
        .map_err(|error| format!("Could not read component file {}: {error}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer).map_err(|error| {
            format!("Could not hash component file {}: {error}", path.display())
        })?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    let actual = format!("{:x}", hasher.finalize());
    if actual != expected_sha256 {
        return Err(format!(
            "Component file SHA-256 is invalid: {}",
            path.display()
        ));
    }
    Ok(())
}

fn read_toml<T: for<'de> Deserialize<'de>>(path: &Path) -> Result<T, String> {
    let text = fs::read_to_string(path)
        .map_err(|error| format!("Could not read {}: {error}", path.display()))?;
    toml::from_str(&text).map_err(|error| format!("Could not parse {}: {error}", path.display()))
}

fn read_toml_optional<T: for<'de> Deserialize<'de>>(path: &Path) -> Result<Option<T>, String> {
    match fs::read_to_string(path) {
        Ok(text) => toml::from_str(&text)
            .map(Some)
            .map_err(|error| format!("Could not parse {}: {error}", path.display())),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(format!("Could not read {}: {error}", path.display())),
    }
}

fn write_toml_atomic<T: Serialize>(path: &Path, value: &T) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Could not create component state directory: {error}"))?;
    }
    let text = toml::to_string_pretty(value)
        .map_err(|error| format!("Could not serialize component state: {error}"))?;
    let temporary = path.with_extension(format!("toml.{}.tmp", std::process::id()));
    let mut file = File::create(&temporary)
        .map_err(|error| format!("Could not create component state: {error}"))?;
    file.write_all(text.as_bytes())
        .map_err(|error| format!("Could not write component state: {error}"))?;
    file.sync_all()
        .map_err(|error| format!("Could not flush component state: {error}"))?;
    atomic_replace(&temporary, path)
}

fn copy_file_atomic(source: &Path, destination: &Path) -> Result<(), String> {
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Could not create runtime materialization path: {error}"))?;
    }
    let temporary = destination.with_extension(format!("{}.tmp", std::process::id()));
    fs::copy(source, &temporary).map_err(|error| {
        format!(
            "Could not materialize runtime file {}: {error}",
            source.display()
        )
    })?;
    atomic_replace(&temporary, destination)
}

#[cfg(windows)]
fn atomic_replace(source: &Path, destination: &Path) -> Result<(), String> {
    use windows_sys::Win32::Storage::FileSystem::{
        MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    };
    let mut source_wide = source.as_os_str().encode_wide().collect::<Vec<_>>();
    source_wide.push(0);
    let mut destination_wide = destination.as_os_str().encode_wide().collect::<Vec<_>>();
    destination_wide.push(0);
    let result = unsafe {
        MoveFileExW(
            source_wide.as_ptr(),
            destination_wide.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if result == 0 {
        return Err(format!(
            "Could not atomically replace {}: {}",
            destination.display(),
            std::io::Error::last_os_error()
        ));
    }
    Ok(())
}

#[cfg(not(windows))]
fn atomic_replace(source: &Path, destination: &Path) -> Result<(), String> {
    if destination.exists() {
        fs::remove_file(destination)
            .map_err(|error| format!("Could not replace {}: {error}", destination.display()))?;
    }
    fs::rename(source, destination)
        .map_err(|error| format!("Could not replace {}: {error}", destination.display()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};
    use std::io::Cursor;
    use zip::ZipWriter;
    use zip::write::SimpleFileOptions;

    fn test_signing_key() -> SigningKey {
        SigningKey::from_bytes(&[0x5au8; 32])
    }

    fn signed_manifest_files(files: &[(&str, &[u8])]) -> String {
        files
            .iter()
            .map(|(path, bytes)| {
                format!(
                    "[[files]]\npath = \"{path}\"\nsize = {}\nsha256 = \"{:x}\"",
                    bytes.len(),
                    Sha256::digest(bytes)
                )
            })
            .collect::<Vec<_>>()
            .join("\n\n")
    }

    fn write_signed_package_metadata(
        writer: &mut ZipWriter<File>,
        package_root: &str,
        manifest: &str,
        signing_key: &SigningKey,
    ) {
        let options = SimpleFileOptions::default();
        writer
            .start_file(
                format!("{package_root}/{OFFLINE_PACKAGE_MANIFEST}"),
                options,
            )
            .expect("offline package manifest");
        writer
            .write_all(manifest.as_bytes())
            .expect("offline package manifest bytes");
        let signature = signing_key.sign(manifest.as_bytes()).to_bytes();
        let signature_hex = signature
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        writer
            .start_file(
                format!("{package_root}/{OFFLINE_PACKAGE_SIGNATURE}"),
                options,
            )
            .expect("offline package signature");
        writer
            .write_all(signature_hex.as_bytes())
            .expect("offline package signature bytes");
    }

    fn write_signed_launcher_executable(
        path: &Path,
        version: &str,
        payload: &[u8],
        signing_key: &SigningKey,
    ) {
        assert!(payload.starts_with(b"MZ"));
        let payload_hash = Sha256::digest(payload);
        let mut hash_bytes = [0u8; 32];
        hash_bytes.copy_from_slice(&payload_hash);
        let signature = signing_key
            .sign(&launcher_exe_signature_message(version, &hash_bytes))
            .to_bytes();
        let version_bytes = version.as_bytes();
        let version_len = u16::try_from(version_bytes.len()).expect("launcher version length");
        let mut bytes = Vec::with_capacity(
            payload.len()
                + version_bytes.len()
                + signature.len()
                + 2
                + LAUNCHER_EXE_SIGNATURE_MAGIC.len(),
        );
        bytes.extend_from_slice(payload);
        bytes.extend_from_slice(version_bytes);
        bytes.extend_from_slice(&signature);
        bytes.extend_from_slice(&version_len.to_le_bytes());
        bytes.extend_from_slice(LAUNCHER_EXE_SIGNATURE_MAGIC);
        fs::create_dir_all(path.parent().expect("launcher executable parent"))
            .expect("launcher executable directory");
        fs::write(path, bytes).expect("signed launcher executable");
    }

    #[test]
    fn tls_crypto_provider_is_available() {
        ensure_tls_crypto_provider().expect("TLS crypto provider should initialize");
        assert!(rustls::crypto::CryptoProvider::get_default().is_some());
    }

    #[test]
    fn github_release_feed_accepts_prerelease_and_release_catalog_assets() {
        let feed = reqwest::Url::parse(
            "https://api.github.com/repos/OAleex/MojoRecomp/releases?per_page=30",
        )
        .expect("valid GitHub feed URL");
        assert!(is_github_release_feed_url(&feed));

        let releases: Vec<GitHubReleaseEntry> = serde_json::from_str(
            r#"[
                {
                    "draft": false,
                    "prerelease": false,
                    "published_at": "2026-09-20T12:00:00Z",
                    "created_at": "2026-09-20T11:00:00Z",
                    "assets": [{
                        "name": "update-catalog.toml",
                        "browser_download_url": "https://github.com/OAleex/MojoRecomp/releases/download/v0.9.0/update-catalog.toml"
                    }]
                },
                {
                    "draft": false,
                    "prerelease": true,
                    "published_at": "2026-09-29T12:00:00Z",
                    "created_at": "2026-09-29T11:00:00Z",
                    "assets": [{
                        "name": "update-catalog.toml",
                        "browser_download_url": "https://github.com/OAleex/MojoRecomp/releases/download/v1.0.0/update-catalog.toml"
                    }]
                },
                {
                    "draft": true,
                    "prerelease": false,
                    "published_at": "2026-09-30T12:00:00Z",
                    "created_at": "2026-09-30T11:00:00Z",
                    "assets": [{
                        "name": "update-catalog.toml",
                        "browser_download_url": "https://github.com/OAleex/MojoRecomp/releases/download/v1.1.0/update-catalog.toml"
                    }]
                }
            ]"#,
        )
        .expect("valid GitHub releases JSON");

        assert_eq!(
            newest_github_asset_url(&releases, "update-catalog.toml").as_deref(),
            Some(
                "https://github.com/OAleex/MojoRecomp/releases/download/v1.0.0/update-catalog.toml"
            )
        );
    }

    fn valid_catalog() -> String {
        r#"
schema_version = 1
channel = "stable"

[[release]]
id = "launcher"
kind = "launcher"
version = "1.1.0"
platform = "windows"
arch = "x86_64"
url = "https://example.com/MojoRecomp-Launcher-1.1.0.exe"
size = 1234
sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
published = "2026-09-27"
notes_url = "https://example.com/releases/launcher-1.1.0"
package = "portable-exe"

[release.compatibility]

[[release]]
id = "runtime.cot"
kind = "runtime"
version = "0.2.0"
platform = "windows"
arch = "x86_64"
url = "https://example.com/cot-runtime-0.2.0.zip"
size = 4321
sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
published = "2026-09-27"
notes_url = "https://example.com/releases/cot-runtime-0.2.0"
package = "zip"
unpacked_size = 10000
entrypoint = "cot-runtime.exe"
required_files = ["cot-runtime.exe", "dxcompiler.dll", "dxil.dll", "mojorecomp-ffmpeg.dll", "mojorecomp-lzx.dll", "extract-xiso.exe"]
game_id = "cot"

[release.compatibility]
min_launcher = "1.0.0"
"#
        .to_string()
    }

    fn test_root(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "mojorecomp-update-test-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    fn create_runtime_package(
        store: &ComponentStore,
        version: &str,
        runtime: &[u8],
        ffmpeg: &[u8],
    ) -> ComponentRelease {
        let artifact = store
            .downloads_root
            .join("runtime.cot")
            .join(version)
            .join("artifact.zip");
        fs::create_dir_all(artifact.parent().unwrap()).expect("download staging");
        let file = File::create(&artifact).expect("artifact");
        let mut writer = ZipWriter::new(file);
        let options = SimpleFileOptions::default();
        let package_root = format!("MojoRecomp-COT-Runtime-{version}-windows-x64");
        writer
            .start_file(format!("{package_root}/cot-runtime.exe"), options)
            .expect("runtime entry");
        writer.write_all(runtime).expect("runtime");
        writer
            .start_file(format!("{package_root}/mojorecomp-ffmpeg.dll"), options)
            .expect("ffmpeg entry");
        writer.write_all(ffmpeg).expect("ffmpeg");
        for (name, bytes) in [
            ("dxcompiler.dll", b"dxc".as_slice()),
            ("dxil.dll", b"dxil".as_slice()),
            ("mojorecomp-lzx.dll", b"lzx".as_slice()),
            ("extract-xiso.exe", b"xiso".as_slice()),
        ] {
            writer
                .start_file(format!("{package_root}/{name}"), options)
                .expect("dependency entry");
            writer.write_all(bytes).expect("dependency");
        }
        writer.finish().expect("finish zip");
        let package = fs::read(&artifact).expect("read artifact");
        let unpacked_size = runtime.len() + ffmpeg.len() + 3 + 4 + 3 + 4;
        ComponentRelease {
            id: "runtime.cot".into(),
            kind: ComponentKind::Runtime,
            version: version.into(),
            platform: "windows".into(),
            arch: "x86_64".into(),
            url: "https://example.com/cot-runtime.zip".into(),
            size: package.len() as u64,
            sha256: format!("{:x}", Sha256::digest(&package)),
            published: "2026-09-27".into(),
            notes_url: "https://example.com/releases/cot-runtime".into(),
            package: PackageFormat::Zip,
            unpacked_size: Some(unpacked_size as u64),
            entrypoint: Some(COT_RUNTIME_ENTRYPOINT.into()),
            required_files: COT_RUNTIME_REQUIRED_FILES
                .iter()
                .map(|path| (*path).to_string())
                .collect(),
            game_id: Some("cot".into()),
            locale: None,
            display_name: None,
            xbox_language: None,
            localization_pack: None,
            localization_catalog_url: None,
            compatibility: Compatibility::default(),
        }
    }

    fn create_language_package(
        store: &ComponentStore,
        version: &str,
        payload: &[u8],
    ) -> ComponentRelease {
        let artifact = store
            .downloads_root
            .join("language.cot.pt-br")
            .join(version)
            .join("artifact.zip");
        fs::create_dir_all(artifact.parent().unwrap()).expect("download staging");
        let file = File::create(&artifact).expect("artifact");
        let mut writer = ZipWriter::new(file);
        writer
            .start_file("source/strings.bin", SimpleFileOptions::default())
            .expect("language entry");
        writer.write_all(payload).expect("language payload");
        writer.finish().expect("finish zip");
        let package = fs::read(&artifact).expect("read artifact");
        ComponentRelease {
            id: "language.cot.pt-br".into(),
            kind: ComponentKind::Language,
            version: version.into(),
            platform: "windows".into(),
            arch: "x86_64".into(),
            url: "https://example.com/cot-pt-br.zip".into(),
            size: package.len() as u64,
            sha256: format!("{:x}", Sha256::digest(&package)),
            published: "2026-09-27".into(),
            notes_url: "https://example.com/releases/cot-pt-br".into(),
            package: PackageFormat::Zip,
            unpacked_size: Some(payload.len() as u64),
            entrypoint: None,
            required_files: vec!["source/strings.bin".into()],
            game_id: Some("cot".into()),
            locale: Some("pt-BR".into()),
            display_name: Some("Brazilian Portuguese".into()),
            xbox_language: Some(1),
            localization_pack: None,
            localization_catalog_url: None,
            compatibility: Compatibility::default(),
        }
    }

    fn create_launcher_package(store: &ComponentStore, version: &str) -> ComponentRelease {
        let artifact = store
            .downloads_root
            .join("launcher")
            .join(version)
            .join("artifact.exe");
        write_signed_launcher_executable(
            &artifact,
            version,
            b"MZlauncher-binary",
            &test_signing_key(),
        );
        let package = fs::read(&artifact).expect("read artifact");
        ComponentRelease {
            id: "launcher".into(),
            kind: ComponentKind::Launcher,
            version: version.into(),
            platform: "windows".into(),
            arch: "x86_64".into(),
            url: "https://example.com/launcher.exe".into(),
            size: package.len() as u64,
            sha256: format!("{:x}", Sha256::digest(&package)),
            published: "2026-09-27".into(),
            notes_url: "https://example.com/releases/launcher".into(),
            package: PackageFormat::PortableExe,
            unpacked_size: None,
            entrypoint: None,
            required_files: Vec::new(),
            game_id: None,
            locale: None,
            display_name: None,
            xbox_language: None,
            localization_pack: None,
            localization_catalog_url: None,
            compatibility: Compatibility::default(),
        }
    }

    fn create_offline_runtime_package(root: &Path, version: &str, min_launcher: &str) -> PathBuf {
        create_offline_runtime_package_with_runtime(
            root,
            version,
            min_launcher,
            b"runtime",
            b"runtime",
        )
    }

    fn create_offline_runtime_package_with_runtime(
        root: &Path,
        version: &str,
        min_launcher: &str,
        runtime_payload: &[u8],
        signed_runtime_payload: &[u8],
    ) -> PathBuf {
        fs::create_dir_all(root).expect("offline package root");
        let path = root.join(format!("runtime-{version}.zip"));
        let file = File::create(&path).expect("offline runtime package");
        let mut writer = ZipWriter::new(file);
        let options = SimpleFileOptions::default();
        let package_root = format!("MojoRecomp-COT-Runtime-{version}-windows-x64");
        let payload: [(&str, &[u8]); 8] = [
            ("cot-runtime.exe", runtime_payload),
            ("dxcompiler.dll", b"dxc".as_slice()),
            ("dxil.dll", b"dxil".as_slice()),
            ("mojorecomp-ffmpeg.dll", b"ffmpeg".as_slice()),
            ("mojorecomp-lzx.dll", b"lzx".as_slice()),
            ("extract-xiso.exe", b"xiso".as_slice()),
            ("LICENSE", b"license".as_slice()),
            ("THIRD_PARTY_NOTICES.txt", b"notices".as_slice()),
        ];
        for (name, bytes) in payload {
            writer
                .start_file(format!("{package_root}/{name}"), options)
                .expect("offline runtime entry");
            writer.write_all(bytes).expect("offline runtime bytes");
        }
        let signed_payload: [(&str, &[u8]); 8] = [
            ("cot-runtime.exe", signed_runtime_payload),
            ("dxcompiler.dll", b"dxc".as_slice()),
            ("dxil.dll", b"dxil".as_slice()),
            ("mojorecomp-ffmpeg.dll", b"ffmpeg".as_slice()),
            ("mojorecomp-lzx.dll", b"lzx".as_slice()),
            ("extract-xiso.exe", b"xiso".as_slice()),
            ("LICENSE", b"license".as_slice()),
            ("THIRD_PARTY_NOTICES.txt", b"notices".as_slice()),
        ];
        let manifest = format!(
            "schema_version = 2\nid = \"runtime.cot\"\nkind = \"runtime\"\nversion = \"{version}\"\nplatform = \"windows\"\narch = \"x86_64\"\npackage = \"zip\"\nentrypoint = \"cot-runtime.exe\"\nrequired_files = [\"cot-runtime.exe\", \"dxcompiler.dll\", \"dxil.dll\", \"mojorecomp-ffmpeg.dll\", \"mojorecomp-lzx.dll\", \"extract-xiso.exe\"]\nmin_launcher = \"{min_launcher}\"\n\n{}\n",
            signed_manifest_files(&signed_payload)
        );
        write_signed_package_metadata(&mut writer, &package_root, &manifest, &test_signing_key());
        writer.finish().expect("finish offline runtime");
        path
    }

    fn create_offline_language_package(
        root: &Path,
        locale: &str,
        display_name: &str,
        version: &str,
        xbox_language: u32,
    ) -> PathBuf {
        fs::create_dir_all(root).expect("offline language package root");
        let path = root.join(format!("language-{locale}-{version}.zip"));
        let file = File::create(&path).expect("offline language package");
        let mut writer = ZipWriter::new(file);
        let options = SimpleFileOptions::default();
        let package_root = format!("MojoRecomp-COT-Language-{locale}-{version}");
        let patch_manifest =
            format!("schema_version = 1\ngame_id = \"cot\"\nlocale = \"{locale}\"\n");
        let delta = b"MJRDIF01-test-delta".as_slice();
        let payload: [(&str, &[u8]); 3] = [
            ("language-patches.toml", patch_manifest.as_bytes()),
            ("patches/test.mjdelta", delta),
            ("LICENSE", b"license".as_slice()),
        ];
        for (name, bytes) in payload {
            writer
                .start_file(format!("{package_root}/{name}"), options)
                .expect("offline language entry");
            writer.write_all(bytes).expect("offline language bytes");
        }
        let component_id = format!("language.cot.{}", locale.to_ascii_lowercase());
        let manifest = format!(
            "schema_version = 2\nid = \"{component_id}\"\nkind = \"language\"\nversion = \"{version}\"\nplatform = \"windows\"\narch = \"x86_64\"\npackage = \"zip\"\nrequired_files = [\"language-patches.toml\", \"patches/test.mjdelta\"]\ngame_id = \"cot\"\nlocale = \"{locale}\"\ndisplay_name = \"{display_name}\"\nxbox_language = {xbox_language}\nmin_launcher = \"1.1.0\"\n\n{}\n",
            signed_manifest_files(&payload)
        );
        write_signed_package_metadata(&mut writer, &package_root, &manifest, &test_signing_key());
        writer.finish().expect("finish offline language package");
        path
    }

    fn create_offline_localization_pack(root: &Path) -> PathBuf {
        fs::create_dir_all(root).expect("offline localization pack root");
        let language = create_offline_language_package(
            &root.join("nested"),
            "pt-BR",
            "Brazilian Portuguese",
            "1.0.0",
            1,
        );
        let language_bytes = fs::read(&language).expect("language package bytes");
        let language_sha256 = format!("{:x}", Sha256::digest(&language_bytes));
        let pack_version = "1.0.0";
        let package_root = format!("MojoRecomp-COT-Localization-Pack-{pack_version}");
        let manifest = format!(
            "schema_version = 1\ngame_id = \"cot\"\nversion = \"{pack_version}\"\n\n[[language]]\nid = \"language.cot.pt-br\"\nlocale = \"pt-BR\"\ndisplay_name = \"Brazilian Portuguese\"\nxbox_language = 1\nversion = \"1.0.0\"\nfile = \"languages/pt-BR.zip\"\nsize = {}\nsha256 = \"{}\"\n",
            language_bytes.len(),
            language_sha256
        );
        let signature = test_signing_key().sign(manifest.as_bytes()).to_bytes();
        let signature_hex = signature
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        let path = root.join("Localization Pack.zip");
        let file = File::create(&path).expect("localization pack");
        let mut writer = ZipWriter::new(file);
        let options = SimpleFileOptions::default();
        writer
            .start_file(
                format!("{package_root}/{LOCALIZATION_PACK_MANIFEST}"),
                options,
            )
            .expect("pack manifest");
        writer
            .write_all(manifest.as_bytes())
            .expect("pack manifest bytes");
        writer
            .start_file(
                format!("{package_root}/{LOCALIZATION_PACK_SIGNATURE}"),
                options,
            )
            .expect("pack signature");
        writer
            .write_all(signature_hex.as_bytes())
            .expect("pack signature bytes");
        writer
            .start_file(format!("{package_root}/languages/pt-BR.zip"), options)
            .expect("pack language");
        writer
            .write_all(&language_bytes)
            .expect("pack language bytes");
        writer.finish().expect("finish localization pack");
        path
    }

    fn create_offline_mom_runtime_package(root: &Path, version: &str) -> PathBuf {
        fs::create_dir_all(root).expect("offline MOM package root");
        let path = root.join(format!("mom-runtime-{version}.zip"));
        let file = File::create(&path).expect("offline MOM runtime package");
        let mut writer = ZipWriter::new(file);
        let options = SimpleFileOptions::default();
        let package_root = format!("MojoRecomp-MOM-Runtime-{version}-windows-x64");
        let payload: [(&str, &[u8]); 3] = [
            ("mom-runtime.exe", b"mom-runtime".as_slice()),
            ("LICENSE", b"license".as_slice()),
            ("THIRD_PARTY_NOTICES.txt", b"notices".as_slice()),
        ];
        for (name, bytes) in payload {
            writer
                .start_file(format!("{package_root}/{name}"), options)
                .expect("offline MOM runtime entry");
            writer.write_all(bytes).expect("offline MOM runtime bytes");
        }
        let manifest = format!(
            "schema_version = 2\nid = \"runtime.mom\"\nkind = \"runtime\"\nversion = \"{version}\"\nplatform = \"windows\"\narch = \"x86_64\"\npackage = \"zip\"\nentrypoint = \"mom-runtime.exe\"\nrequired_files = [\"mom-runtime.exe\"]\nmin_launcher = \"1.1.0\"\n\n{}\n",
            signed_manifest_files(&payload)
        );
        write_signed_package_metadata(&mut writer, &package_root, &manifest, &test_signing_key());
        writer.finish().expect("finish offline MOM runtime");
        path
    }

    #[test]
    fn offline_runtime_package_installs_without_an_update_catalog() {
        let root = test_root("offline-runtime");
        let package = create_offline_runtime_package(&root, "0.2.0", "1.1.0");
        let key = test_signing_key().verifying_key();
        let release = inspect_offline_package_with_key(&package, "1.1.0", &key)
            .expect("inspect offline runtime");
        assert_eq!(release.id, "runtime.cot");
        assert_eq!(release.version, "0.2.0");
        let store = ComponentStore::new(root.join("components"));
        store
            .stage_local_artifact(&release, &package)
            .expect("stage offline runtime");
        store
            .install_staged(&release)
            .expect("install offline runtime");
        let active = store
            .active_status("runtime.cot")
            .expect("runtime status")
            .expect("active runtime");
        assert_eq!(active.version, "0.2.0");
        assert!(active.healthy);
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn localization_pack_can_contain_signed_language_components() {
        let root = test_root("offline-localization-pack");
        let package = create_offline_localization_pack(&root);
        let key = test_signing_key().verifying_key();
        let staging = root.join("staging");
        let pack = extract_offline_localization_pack_with_key(&package, &staging, &key)
            .expect("inspect localization pack");
        assert_eq!(pack.game_id, "cot");
        assert_eq!(pack.version, "1.0.0");
        assert_eq!(pack.languages.len(), 1);
        let language = &pack.languages[0];
        assert_eq!(language.id, "language.cot.pt-br");
        assert_eq!(language.locale, "pt-BR");
        assert_eq!(language.display_name, "Brazilian Portuguese");
        assert_eq!(language.xbox_language, 1);
        let release = inspect_offline_package_with_key(&language.package_path, "1.1.0", &key)
            .expect("inspect nested language");
        assert_eq!(release.id, language.id);
        assert_eq!(release.locale.as_deref(), Some("pt-BR"));
        assert_eq!(
            release.display_name.as_deref(),
            Some("Brazilian Portuguese")
        );
        assert_eq!(release.xbox_language, Some(1));

        let component_size = release.size;
        let component_sha256 = release.sha256.clone();
        let pack_bytes = fs::read(&package).expect("pack bytes");
        let mut catalog_release = release.clone();
        catalog_release.url = "https://example.com/MojoRecomp-COT-Localization-Pack-1.0.0.zip".into();
        catalog_release.published = "2026-10-05".into();
        catalog_release.notes_url = "https://example.com/releases/localization-pack-1.0.0".into();
        catalog_release.size = pack_bytes.len() as u64;
        catalog_release.sha256 = format!("{:x}", Sha256::digest(&pack_bytes));
        catalog_release.localization_pack = Some(LocalizationPackReference {
            version: "1.0.0".into(),
            component_size,
            component_sha256,
        });
        validate_release(&catalog_release).expect("bundled language catalog release");
        let verified = inspect_signed_release_artifact_with_key(
            &language.package_path,
            "1.1.0",
            &catalog_release,
            &key,
        )
        .expect("verify nested language against pack-backed catalog release");
        assert_eq!(verified.id, "language.cot.pt-br");

        let store = ComponentStore::new(root.join("components"));
        store
            .stage_local_artifact(&release, &language.package_path)
            .expect("stage language");
        store.install_staged(&release).expect("install language");
        let installed = store.active_languages("cot").expect("active languages");
        assert_eq!(installed.len(), 1);
        assert_eq!(installed[0].locale, "pt-BR");
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn signed_runtime_package_schema_supports_future_games() {
        let root = test_root("offline-runtime-mom");
        let package = create_offline_mom_runtime_package(&root, "0.1.0");
        let key = test_signing_key().verifying_key();
        let release = inspect_offline_package_with_key(&package, "1.1.0", &key)
            .expect("inspect signed MOM runtime");
        assert_eq!(release.id, "runtime.mom");
        assert_eq!(release.game_id.as_deref(), Some("mom"));
        assert_eq!(release.entrypoint.as_deref(), Some("mom-runtime.exe"));
        assert_eq!(release.required_files, vec!["mom-runtime.exe"]);
        assert_eq!(release.version, "0.1.0");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn offline_runtime_package_enforces_launcher_compatibility() {
        let root = test_root("offline-runtime-compat");
        let package = create_offline_runtime_package(&root, "0.2.0", "9.0.0");
        let key = test_signing_key().verifying_key();
        let error = inspect_offline_package_with_key(&package, "1.1.0", &key)
            .expect_err("incompatible package");
        assert!(error.contains("requires MojoRecomp Launcher 9.0.0 or newer"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn offline_launcher_import_is_disabled() {
        let error = offline_package_files(&ComponentKind::Launcher)
            .expect_err("non-runtime offline import must stay disabled");
        assert!(error.contains("signed game runtime ZIPs only"));
    }

    #[test]
    fn offline_package_rejects_a_well_formed_but_unofficial_signature() {
        let root = test_root("offline-unofficial-signature");
        let package = create_offline_runtime_package(&root, "0.2.0", "1.1.0");
        let untrusted_key = SigningKey::from_bytes(&[0x33u8; 32]).verifying_key();
        let error = inspect_offline_package_with_key(&package, "1.1.0", &untrusted_key)
            .expect_err("package signed by another key must be rejected");
        assert!(error.contains("signature is invalid"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn offline_package_rejects_payload_modified_after_signing() {
        let root = test_root("offline-tampered-payload");
        let package = create_offline_runtime_package_with_runtime(
            &root,
            "0.2.0",
            "1.1.0",
            b"tampered-runtime",
            b"official-runtime",
        );
        let key = test_signing_key().verifying_key();
        let error = inspect_offline_package_with_key(&package, "1.1.0", &key)
            .expect_err("modified payload must be rejected");
        assert!(error.contains("signed integrity verification"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn online_signed_package_metadata_must_match_the_catalog_release() {
        let root = test_root("online-signed-catalog-match");
        let package = create_offline_runtime_package(&root, "0.2.0", "1.1.0");
        let key = test_signing_key().verifying_key();
        let expected = inspect_offline_package_with_key(&package, "1.1.0", &key)
            .expect("signed runtime package");
        inspect_signed_release_artifact_with_key(&package, "1.1.0", &expected, &key)
            .expect("matching catalog metadata");

        let mut altered = expected.clone();
        altered
            .compatibility
            .requirements
            .push(ComponentRequirement {
                id: "runtime.mom".into(),
                min_version: Some("0.4.0".into()),
                max_version: Some("0.4.0".into()),
            });
        let error = inspect_signed_release_artifact_with_key(&package, "1.1.0", &altered, &key)
            .expect_err("catalog compatibility must not override signed metadata");
        assert!(error.contains("does not match the update catalog"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn launcher_replacement_backup_restores_old_files_and_removes_new_files() {
        let root = test_root("launcher-replacement-rollback");
        let target = root.join("target");
        let source = root.join("source");
        let backup = root.join("backup");
        fs::create_dir_all(&target).expect("target");
        fs::create_dir_all(&source).expect("source");
        fs::write(target.join("mojorecomp-launcher.exe"), b"old").expect("old launcher");
        fs::write(target.join("README.txt"), b"old readme").expect("old readme");
        fs::write(target.join(OFFLINE_PACKAGE_MANIFEST), b"legacy manifest")
            .expect("legacy manifest");
        fs::write(source.join("mojorecomp-launcher.exe"), b"new").expect("new launcher");

        backup_launcher_replacement_tree(&target, &backup).expect("backup launcher");
        apply_launcher_replacement_tree(&source, &target).expect("replace launcher");
        assert!(!target.join("README.txt").exists());
        assert!(!target.join(OFFLINE_PACKAGE_MANIFEST).exists());
        restore_launcher_replacement_tree(&target, &backup).expect("restore launcher");

        assert_eq!(
            fs::read(target.join("mojorecomp-launcher.exe")).unwrap(),
            b"old"
        );
        assert_eq!(fs::read(target.join("README.txt")).unwrap(), b"old readme");
        assert_eq!(
            fs::read(target.join(OFFLINE_PACKAGE_MANIFEST)).unwrap(),
            b"legacy manifest"
        );
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn strict_catalog_accepts_component_releases_and_rejects_unknown_fields() {
        let parsed = parse_and_validate_catalog(&valid_catalog()).expect("valid catalog");
        assert_eq!(parsed.releases.len(), 2);
        let invalid = valid_catalog().replacen(
            "channel = \"stable\"",
            "channel = \"stable\"\nunexpected = true",
            1,
        );
        assert!(parse_and_validate_catalog(&invalid).is_err());
    }

    #[test]
    fn catalog_requires_release_notes_and_explicit_compatibility_metadata() {
        let missing_notes = valid_catalog().replacen(
            "notes_url = \"https://example.com/releases/launcher-1.1.0\"\n",
            "",
            1,
        );
        assert!(parse_and_validate_catalog(&missing_notes).is_err());

        let missing_compatibility = valid_catalog().replacen(
            "\n[release.compatibility]\n\n[[release]]",
            "\n\n[[release]]",
            1,
        );
        assert!(parse_and_validate_catalog(&missing_compatibility).is_err());
    }

    #[test]
    fn resolved_update_hosts_reject_any_non_public_address() {
        assert!(
            validate_resolved_addresses([
                "93.184.216.34".parse().unwrap(),
                "127.0.0.1".parse().unwrap(),
            ])
            .is_err()
        );
        assert!(validate_resolved_addresses(["10.0.0.8".parse().unwrap()]).is_err());
        assert!(validate_resolved_addresses(["93.184.216.34".parse().unwrap()]).is_ok());
    }

    #[test]
    fn catalog_rejects_insecure_urls_hashes_and_unsupported_targets() {
        assert!(
            parse_and_validate_catalog(
                &valid_catalog().replace("https://example.com/", "http://example.com/")
            )
            .is_err()
        );
        assert!(
            parse_and_validate_catalog(&valid_catalog().replace(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
            ))
            .is_err()
        );
        assert!(
            parse_and_validate_catalog(&valid_catalog().replacen(
                "arch = \"x86_64\"",
                "arch = \"x86\"",
                1
            ))
            .is_err()
        );
        assert!(validate_public_https_url("https://127.0.0.1/update.toml").is_err());
    }

    #[test]
    fn public_https_validation_accepts_official_repository_links() {
        for url in [
            "https://github.com/OAleex/MojoRecomp",
            "https://github.com/OAleex/MojoRecomp/issues/new?title=%5BLauncher%5D%20",
            "https://github.com/OAleex/MojoRecomp/issues/new?title=%5BGame%5D%20",
        ] {
            let parsed = validate_public_https_url(url).expect("official repository URL");
            assert_eq!(parsed.scheme(), "https");
            assert_eq!(parsed.host_str(), Some("github.com"));
        }
    }

    #[test]
    fn planner_updates_components_independently_and_honors_compatibility() {
        let catalog = parse_and_validate_catalog(&valid_catalog()).expect("catalog");
        let installed = vec![
            InstalledComponent {
                id: "launcher".into(),
                version: "1.0.0".into(),
                healthy: true,
            },
            InstalledComponent {
                id: "runtime.cot".into(),
                version: "0.1.0-alpha".into(),
                healthy: true,
            },
        ];
        let plans = plan_updates(&catalog, &installed, "1.0.0").expect("plans");
        assert_eq!(
            plans
                .iter()
                .find(|plan| plan.id == "launcher")
                .unwrap()
                .state,
            PlanState::UpdateAvailable
        );
        assert_eq!(
            plans
                .iter()
                .find(|plan| plan.id == "runtime.cot")
                .unwrap()
                .state,
            PlanState::UpdateAvailable
        );
        let incompatible = plan_updates(&catalog, &installed, "0.9.0").expect("plans");
        assert_eq!(
            incompatible
                .iter()
                .find(|plan| plan.id == "runtime.cot")
                .unwrap()
                .state,
            PlanState::Incompatible
        );
    }

    #[test]
    fn explicit_component_requirements_enforce_the_active_version_range() {
        let catalog = parse_and_validate_catalog(&valid_catalog()).expect("catalog");
        let mut release = catalog
            .releases
            .iter()
            .find(|release| release.id == "runtime.cot")
            .expect("runtime release")
            .clone();
        release.compatibility.requirements = vec![ComponentRequirement {
            id: "runtime.mom".into(),
            min_version: Some("0.4.0".into()),
            max_version: Some("0.4.9".into()),
        }];
        let launcher = InstalledComponent {
            id: "launcher".into(),
            version: "1.1.0".into(),
            healthy: true,
        };
        let required = InstalledComponent {
            id: "runtime.mom".into(),
            version: "0.4.5".into(),
            healthy: true,
        };
        assert!(
            validate_release_compatibility(&release, "1.1.0", &[launcher.clone(), required])
                .is_ok()
        );
        let wrong_version = InstalledComponent {
            id: "runtime.mom".into(),
            version: "0.5.0".into(),
            healthy: true,
        };
        assert!(
            validate_release_compatibility(&release, "1.1.0", &[launcher.clone(), wrong_version])
                .is_err()
        );
        let corrupted = InstalledComponent {
            id: "runtime.mom".into(),
            version: "0.4.5".into(),
            healthy: false,
        };
        assert!(validate_release_compatibility(&release, "1.1.0", &[launcher, corrupted]).is_err());
    }

    #[test]
    fn planner_exposes_missing_runtime_as_downloadable() {
        let catalog = parse_and_validate_catalog(&valid_catalog()).expect("catalog");
        let installed = vec![InstalledComponent {
            id: "launcher".into(),
            version: "1.0.0".into(),
            healthy: true,
        }];
        let plans = plan_updates(&catalog, &installed, "1.0.0").expect("plans");
        let runtime = plans
            .iter()
            .find(|plan| plan.id == "runtime.cot")
            .expect("runtime plan");

        assert_eq!(runtime.state, PlanState::Available);
        assert!(runtime.installed_version.is_none());
        assert!(runtime.latest_version.is_some());
        assert!(runtime.download_url.is_some());
    }

    #[test]
    fn compatible_release_history_is_sorted_newest_first() {
        let mut catalog = parse_and_validate_catalog(&valid_catalog()).expect("catalog");
        let mut older = catalog
            .releases
            .iter()
            .find(|release| release.id == "runtime.cot")
            .expect("runtime release")
            .clone();
        older.version = "0.1.0".into();
        older.url = "https://example.com/cot-runtime-0.1.0.zip".into();
        older.notes_url = "https://example.com/releases/cot-runtime-0.1.0".into();
        older.published = "2026-08-01".into();
        catalog.releases.push(older);

        let installed = vec![InstalledComponent {
            id: "launcher".into(),
            version: "1.0.0".into(),
            healthy: true,
        }];
        let releases =
            compatible_releases_for_component(&catalog, &installed, "1.0.0", "runtime.cot")
                .expect("compatible releases");

        assert_eq!(
            releases
                .iter()
                .map(|release| release.version.as_str())
                .collect::<Vec<_>>(),
            vec!["0.2.0", "0.1.0"]
        );
    }

    #[test]
    fn staged_download_hashes_while_writing_and_rejects_size_or_hash_mismatch() {
        let root = test_root("download");
        fs::create_dir_all(&root).expect("root");
        let payload = b"verified-update-payload";
        let sha256 = format!("{:x}", Sha256::digest(payload));
        let destination = root.join("artifact.zip");
        let mut events = Vec::new();
        stage_reader(
            Cursor::new(payload),
            &destination,
            payload.len() as u64,
            &sha256,
            &mut |done, total| events.push((done, total)),
        )
        .expect("stage verified payload");
        assert_eq!(fs::read(&destination).unwrap(), payload);
        assert!(!events.is_empty());
        let bad_size = root.join("bad-size.zip");
        assert!(
            stage_reader(
                Cursor::new(payload),
                &bad_size,
                payload.len() as u64 + 1,
                &sha256,
                &mut |_, _| {},
            )
            .is_err()
        );
        assert!(!bad_size.exists());
        assert!(!bad_size.with_extension("zip.part").exists());
        let bad_hash = root.join("bad-hash.zip");
        assert!(
            stage_reader(
                Cursor::new(payload),
                &bad_hash,
                payload.len() as u64,
                &"0".repeat(64),
                &mut |_, _| {},
            )
            .is_err()
        );
        assert!(!bad_hash.exists());
        assert!(!bad_hash.with_extension("zip.part").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn activation_keeps_previous_version_and_rollback_restores_it() {
        let root = test_root("activation");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store.install_staged(&first).expect("install first");
        assert!(
            !store
                .active_status("runtime.cot")
                .unwrap()
                .unwrap()
                .can_rollback
        );
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store.install_staged(&second).expect("install second");
        assert!(
            store
                .active_status("runtime.cot")
                .unwrap()
                .unwrap()
                .can_rollback
        );
        let active: ActiveComponent = read_toml(&store.active_path("runtime.cot")).expect("active");
        assert_eq!(active.active_version, "0.3.0");
        assert_eq!(active.previous_version.as_deref(), Some("0.2.0"));
        assert!(store.version_root("runtime.cot", "0.2.0").is_dir());
        store.rollback("runtime.cot").expect("rollback");
        assert_eq!(
            store.active_status("runtime.cot").unwrap().unwrap().version,
            "0.2.0"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn active_dependents_block_incompatible_runtime_switches_and_rollback() {
        let root = test_root("dependent-runtime-range");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.1.0", b"runtime-v1", b"ffmpeg-v1");
        store.install_staged(&first).expect("install runtime 0.1.0");
        let second = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store
            .install_staged(&second)
            .expect("install runtime 0.2.0");

        let mut language = create_language_package(&store, "1.0.0", b"localized-payload");
        language.compatibility.requirements = vec![ComponentRequirement {
            id: "runtime.cot".into(),
            min_version: Some("0.2.0".into()),
            max_version: Some("0.2.0".into()),
        }];
        store
            .install_staged(&language)
            .expect("install runtime-bound localization pack");

        let runtime_status = store
            .active_status("runtime.cot")
            .expect("runtime status")
            .expect("active runtime");
        assert!(!runtime_status.can_rollback);
        assert!(store.rollback("runtime.cot").is_err());

        let third = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        let error = store
            .install_staged(&third)
            .expect_err("dependent must block incompatible runtime activation");
        assert!(error.contains("language.cot.pt-br 1.0.0 requires runtime.cot 0.2.0"));
        assert_eq!(
            store.active_status("runtime.cot").unwrap().unwrap().version,
            "0.2.0"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn active_runtime_can_bound_future_launcher_versions() {
        let root = test_root("runtime-launcher-range");
        let store = ComponentStore::new(root.join("components"));
        let mut runtime = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        runtime.compatibility.min_launcher = Some("1.0.0".into());
        runtime.compatibility.max_launcher = Some("1.1.0".into());
        store
            .install_staged(&runtime)
            .expect("install launcher-bounded runtime");

        assert!(store.ensure_launcher_version_compatible("1.0.0").is_ok());
        assert!(store.ensure_launcher_version_compatible("1.1.0").is_ok());
        let error = store
            .ensure_launcher_version_compatible("1.2.0")
            .expect_err("runtime maximum launcher version must block the update");
        assert!(error.contains("runtime.cot 0.2.0 supports MojoRecomp Launcher 1.1.0 or older"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn reinstalling_active_version_preserves_previous_version() {
        let root = test_root("reinstall");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store.install_staged(&first).expect("install first");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store.install_staged(&second).expect("install second");

        let reinstall =
            create_runtime_package(&store, "0.3.0", b"runtime-v3-new", b"ffmpeg-v3-new");
        store
            .install_staged(&reinstall)
            .expect("reinstall active version");

        let active: ActiveComponent = read_toml(&store.active_path("runtime.cot")).expect("active");
        assert_eq!(active.active_version, "0.3.0");
        assert_eq!(active.previous_version.as_deref(), Some("0.2.0"));
        store.rollback("runtime.cot").expect("rollback");
        assert_eq!(
            store.active_status("runtime.cot").unwrap().unwrap().version,
            "0.2.0"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn custom_component_download_and_staging_roots_stay_separate() {
        let root = test_root("custom-roots");
        let store = ComponentStore::with_roots(
            root.join("components"),
            root.join("downloads"),
            root.join("staging"),
        );
        let release = create_runtime_package(&store, "0.2.0", b"runtime", b"ffmpeg");
        assert!(
            root.join("downloads")
                .join("runtime.cot")
                .join("0.2.0")
                .join("artifact.zip")
                .is_file()
        );
        store.install_staged(&release).expect("install runtime");
        assert!(
            root.join("components")
                .join("runtime.cot")
                .join("versions")
                .join("0.2.0")
                .is_dir()
        );
        assert!(!root.join("downloads").join("runtime.cot").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn legacy_component_migration_validates_destination_before_removing_source() {
        let root = test_root("component-migration");
        let source = ComponentStore::new(root.join("legacy-components"));
        let destination = ComponentStore::with_roots(
            root.join("library").join("components"),
            root.join("library").join("downloads"),
            root.join("library").join("staging"),
        );
        let release = create_runtime_package(&source, "0.2.0", b"runtime", b"ffmpeg");
        source
            .install_staged(&release)
            .expect("install legacy runtime");

        assert!(
            destination
                .migrate_component_from(&source, "runtime.cot", &COT_RUNTIME_REQUIRED_FILES)
                .expect("migrate runtime")
        );
        let active = destination
            .active_status("runtime.cot")
            .expect("destination status")
            .expect("destination active runtime");
        assert!(active.healthy);
        assert_eq!(active.version, "0.2.0");
        assert!(!source.component_root("runtime.cot").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn incomplete_legacy_runtime_is_preserved_until_a_complete_component_exists() {
        let root = test_root("incomplete-component-migration");
        let source = ComponentStore::new(root.join("legacy-components"));
        let destination = ComponentStore::with_roots(
            root.join("library").join("components"),
            root.join("library").join("downloads"),
            root.join("library").join("staging"),
        );
        let release = create_runtime_package(&source, "0.2.0", b"runtime", b"ffmpeg");
        source
            .install_staged(&release)
            .expect("install legacy runtime");
        let version_root = source.version_root("runtime.cot", "0.2.0");
        let manifest_path = version_root.join("installation.toml");
        let mut manifest: InstalledManifest = read_toml(&manifest_path).expect("legacy manifest");
        manifest
            .files
            .retain(|file| file.path != "extract-xiso.exe");
        write_toml_atomic(&manifest_path, &manifest).expect("rewrite legacy manifest");
        fs::remove_file(version_root.join("payload").join("extract-xiso.exe"))
            .expect("remove legacy extract-xiso");
        assert!(source.verify_version("runtime.cot", "0.2.0").is_ok());

        assert!(
            destination
                .migrate_component_from(&source, "runtime.cot", &COT_RUNTIME_REQUIRED_FILES)
                .is_err()
        );
        assert!(source.component_root("runtime.cot").exists());
        assert!(destination.active_status("runtime.cot").unwrap().is_none());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn legacy_component_with_inconsistent_kind_is_preserved() {
        let root = test_root("wrong-kind-component-migration");
        let source = ComponentStore::new(root.join("legacy-components"));
        let destination = ComponentStore::with_roots(
            root.join("library").join("components"),
            root.join("library").join("downloads"),
            root.join("library").join("staging"),
        );
        let release = create_runtime_package(&source, "0.2.0", b"runtime", b"ffmpeg");
        source
            .install_staged(&release)
            .expect("install legacy runtime");
        let active_path = source.active_path("runtime.cot");
        let mut active: ActiveComponent = read_toml(&active_path).expect("active component");
        active.kind = ComponentKind::Language;
        write_toml_atomic(&active_path, &active).expect("write inconsistent kind");

        assert!(
            destination
                .migrate_component_from(&source, "runtime.cot", &COT_RUNTIME_REQUIRED_FILES,)
                .is_err()
        );
        assert!(source.component_root("runtime.cot").exists());
        assert!(destination.active_status("runtime.cot").unwrap().is_none());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn legacy_component_with_unsupported_active_metadata_is_preserved() {
        let root = test_root("unsupported-legacy-active-metadata");
        let source = ComponentStore::new(root.join("legacy-components"));
        let destination = ComponentStore::with_roots(
            root.join("library").join("components"),
            root.join("library").join("downloads"),
            root.join("library").join("staging"),
        );
        let release = create_runtime_package(&source, "0.2.0", b"runtime", b"ffmpeg");
        source
            .install_staged(&release)
            .expect("install legacy runtime");
        let active_path = source.active_path("runtime.cot");
        let mut active: ActiveComponent = read_toml(&active_path).expect("active component");
        active.schema_version += 1;
        write_toml_atomic(&active_path, &active).expect("write unsupported active metadata");

        assert!(
            destination
                .migrate_component_from(&source, "runtime.cot", &COT_RUNTIME_REQUIRED_FILES)
                .is_err()
        );
        assert!(source.component_root("runtime.cot").exists());
        assert!(destination.active_status("runtime.cot").unwrap().is_none());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn complete_legacy_component_replaces_incomplete_same_version_destination() {
        let root = test_root("replace-incomplete-migration-destination");
        let source = ComponentStore::new(root.join("legacy-components"));
        let destination = ComponentStore::with_roots(
            root.join("library").join("components"),
            root.join("library").join("downloads"),
            root.join("library").join("staging"),
        );
        let source_release =
            create_runtime_package(&source, "0.2.0", b"new-runtime", b"new-ffmpeg");
        source
            .install_staged(&source_release)
            .expect("install legacy runtime");
        let destination_release =
            create_runtime_package(&destination, "0.2.0", b"old-runtime", b"old-ffmpeg");
        destination
            .install_staged(&destination_release)
            .expect("install destination runtime");

        let destination_version = destination.version_root("runtime.cot", "0.2.0");
        let manifest_path = destination_version.join("installation.toml");
        let mut manifest: InstalledManifest = read_toml(&manifest_path).expect("runtime manifest");
        manifest
            .files
            .retain(|file| file.path != "extract-xiso.exe");
        write_toml_atomic(&manifest_path, &manifest).expect("write incomplete manifest");
        fs::remove_file(destination_version.join("payload").join("extract-xiso.exe"))
            .expect("remove extractor");

        assert!(
            destination
                .migrate_component_from(&source, "runtime.cot", &COT_RUNTIME_REQUIRED_FILES)
                .expect("replace incomplete destination")
        );
        assert!(
            destination
                .active_runtime_is_ready(
                    "runtime.cot",
                    COT_RUNTIME_ENTRYPOINT,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("inspect migrated runtime")
        );
        assert_eq!(
            fs::read(
                destination
                    .version_root("runtime.cot", "0.2.0")
                    .join("payload")
                    .join("cot-runtime.exe")
            )
            .expect("read migrated runtime"),
            b"new-runtime"
        );
        assert!(!source.component_root("runtime.cot").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn validated_legacy_runtime_files_can_be_promoted_without_a_download() {
        let root = test_root("legacy-runtime-import");
        let source = root.join("legacy-runtime");
        fs::create_dir_all(&source).expect("legacy runtime root");
        for (name, bytes) in [
            ("cot-runtime.exe", b"runtime".as_slice()),
            ("dxcompiler.dll", b"dxc".as_slice()),
            ("dxil.dll", b"dxil".as_slice()),
            ("mojorecomp-ffmpeg.dll", b"ffmpeg".as_slice()),
            ("mojorecomp-lzx.dll", b"lzx".as_slice()),
            ("extract-xiso.exe", b"xiso".as_slice()),
        ] {
            fs::write(source.join(name), bytes).expect("legacy runtime file");
        }
        let store = ComponentStore::with_roots(
            root.join("library").join("components"),
            root.join("library").join("downloads"),
            root.join("library").join("staging"),
        );
        assert!(
            store
                .import_migrated_runtime(
                    "runtime.cot",
                    "0.1.0-alpha",
                    COT_RUNTIME_ENTRYPOINT,
                    &source,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("import legacy runtime")
        );
        let active = store
            .active_status("runtime.cot")
            .expect("runtime status")
            .expect("active runtime");
        assert!(active.healthy);
        assert!(
            store
                .active_payload("runtime.cot")
                .unwrap()
                .unwrap()
                .root
                .join("extract-xiso.exe")
                .is_file()
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn active_runtime_readiness_requires_runtime_kind_entrypoint_and_complete_payload() {
        let root = test_root("active-runtime-readiness");
        let store = ComponentStore::new(root.join("components"));
        let release = create_runtime_package(&store, "0.2.0", b"runtime", b"ffmpeg");
        store.install_staged(&release).expect("install runtime");
        assert!(
            store
                .active_runtime_is_ready(
                    "runtime.cot",
                    COT_RUNTIME_ENTRYPOINT,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("inspect valid runtime")
        );

        let active_path = store.active_path("runtime.cot");
        let mut active: ActiveComponent = read_toml(&active_path).expect("active component");
        active.kind = ComponentKind::Language;
        write_toml_atomic(&active_path, &active).expect("write wrong active kind");
        assert!(!store.active_status("runtime.cot").unwrap().unwrap().healthy);
        assert!(
            !store
                .active_runtime_is_ready(
                    "runtime.cot",
                    COT_RUNTIME_ENTRYPOINT,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("reject wrong active kind")
        );

        active.kind = ComponentKind::Runtime;
        write_toml_atomic(&active_path, &active).expect("restore active kind");
        let manifest_path = store
            .version_root("runtime.cot", "0.2.0")
            .join("installation.toml");
        let mut manifest: InstalledManifest = read_toml(&manifest_path).expect("runtime manifest");
        manifest.entrypoint = Some("wrong-runtime.exe".into());
        write_toml_atomic(&manifest_path, &manifest).expect("write wrong entrypoint");
        assert!(
            !store
                .active_runtime_is_ready(
                    "runtime.cot",
                    COT_RUNTIME_ENTRYPOINT,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("reject wrong entrypoint")
        );

        manifest.entrypoint = Some(COT_RUNTIME_ENTRYPOINT.into());
        manifest
            .files
            .retain(|file| file.path != "extract-xiso.exe");
        write_toml_atomic(&manifest_path, &manifest).expect("write incomplete manifest");
        assert!(
            !store
                .active_runtime_is_ready(
                    "runtime.cot",
                    COT_RUNTIME_ENTRYPOINT,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("reject incomplete runtime")
        );

        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn complete_legacy_runtime_replaces_incomplete_active_component() {
        let root = test_root("replace-incomplete-active-runtime");
        let store = ComponentStore::new(root.join("components"));
        let release = create_runtime_package(&store, "0.2.0", b"old-runtime", b"old-ffmpeg");
        store.install_staged(&release).expect("install old runtime");
        let version_root = store.version_root("runtime.cot", "0.2.0");
        let manifest_path = version_root.join("installation.toml");
        let mut manifest: InstalledManifest = read_toml(&manifest_path).expect("runtime manifest");
        manifest
            .files
            .retain(|file| file.path != "extract-xiso.exe");
        write_toml_atomic(&manifest_path, &manifest).expect("write incomplete manifest");
        fs::remove_file(version_root.join("payload").join("extract-xiso.exe"))
            .expect("remove extractor");
        assert!(store.active_status("runtime.cot").unwrap().unwrap().healthy);

        let source = root.join("legacy-runtime");
        fs::create_dir_all(&source).expect("legacy runtime root");
        for (name, bytes) in COT_RUNTIME_REQUIRED_FILES.iter().zip([
            b"new-runtime".as_slice(),
            b"dxc".as_slice(),
            b"dxil".as_slice(),
            b"new-ffmpeg".as_slice(),
            b"lzx".as_slice(),
            b"xiso".as_slice(),
        ]) {
            fs::write(source.join(name), bytes).expect("legacy runtime file");
        }

        assert!(
            store
                .import_migrated_runtime(
                    "runtime.cot",
                    "0.2.0",
                    COT_RUNTIME_ENTRYPOINT,
                    &source,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("replace incomplete runtime")
        );
        assert!(
            store
                .active_runtime_is_ready(
                    "runtime.cot",
                    COT_RUNTIME_ENTRYPOINT,
                    &COT_RUNTIME_REQUIRED_FILES,
                )
                .expect("inspect replacement runtime")
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn repairing_corrupted_active_version_restores_previous_before_replacement() {
        let root = test_root("repair");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store.install_staged(&first).expect("install first");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store.install_staged(&second).expect("install second");
        fs::write(
            store
                .version_root("runtime.cot", "0.3.0")
                .join("payload")
                .join("cot-runtime.exe"),
            b"corrupted",
        )
        .expect("corrupt active runtime");
        assert!(!store.active_status("runtime.cot").unwrap().unwrap().healthy);
        store
            .prepare_repair("runtime.cot", "0.3.0")
            .expect("prepare repair");
        assert_eq!(
            store.active_status("runtime.cot").unwrap().unwrap().version,
            "0.2.0"
        );
        let repaired = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store
            .install_staged(&repaired)
            .expect("install repaired version");
        assert!(store.active_status("runtime.cot").unwrap().unwrap().healthy);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn runtime_versions_can_be_downloaded_without_changing_the_active_version() {
        let root = test_root("download-inactive-runtime");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store
            .install_staged(&first)
            .expect("install active runtime");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store
            .install_staged_inactive(&second)
            .expect("download inactive runtime");

        let active = store
            .active_status("runtime.cot")
            .expect("runtime status")
            .expect("active runtime");
        assert_eq!(active.version, "0.2.0");
        let versions = store.installed_versions("runtime.cot").unwrap();
        assert!(
            versions
                .iter()
                .any(|entry| entry.version == "0.2.0" && entry.healthy)
        );
        assert!(
            versions
                .iter()
                .any(|entry| entry.version == "0.3.0" && entry.healthy)
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn downloaded_runtime_can_be_activated_without_redownloading() {
        let root = test_root("activate-downloaded-runtime");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store
            .install_staged(&first)
            .expect("install active runtime");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store
            .install_staged_inactive(&second)
            .expect("download inactive runtime");

        store
            .activate_version("runtime.cot", "0.3.0")
            .expect("activate downloaded runtime");
        let active = store.active_status("runtime.cot").unwrap().unwrap();
        assert_eq!(active.version, "0.3.0");
        assert!(active.can_rollback);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn removing_active_runtime_leaves_other_downloaded_versions_available() {
        let root = test_root("remove-active-runtime");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store
            .install_staged(&first)
            .expect("install active runtime");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store
            .install_staged_inactive(&second)
            .expect("download inactive runtime");

        store
            .remove_version("runtime.cot", "0.2.0")
            .expect("remove active runtime");
        assert!(store.active_status("runtime.cot").unwrap().is_none());
        let versions = store.installed_versions("runtime.cot").unwrap();
        assert_eq!(versions.len(), 1);
        assert_eq!(versions[0].version, "0.3.0");
        assert!(versions[0].healthy);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn language_package_activates_in_component_storage() {
        let root = test_root("language");
        let store = ComponentStore::new(root.join("components"));
        let release = create_language_package(&store, "1.0.0", b"localized-payload");
        store
            .install_staged(&release)
            .expect("install localization pack");
        let status = store
            .active_status("language.cot.pt-br")
            .expect("status")
            .expect("active language");
        assert_eq!(status.version, "1.0.0");
        assert!(status.healthy);
        let languages = store.active_languages("cot").expect("active languages");
        assert_eq!(languages.len(), 1);
        assert_eq!(languages[0].id, "language.cot.pt-br");
        assert_eq!(languages[0].locale, "pt-BR");
        assert_eq!(languages[0].display_name, "Brazilian Portuguese");
        assert_eq!(languages[0].xbox_language, 1);
        let payload = store
            .active_payload("language.cot.pt-br")
            .expect("payload")
            .expect("active payload");
        assert_eq!(payload.version, "1.0.0");
        assert_eq!(payload.kind, ComponentKind::Language);
        assert_eq!(
            payload.root,
            store
                .version_root("language.cot.pt-br", "1.0.0")
                .join("payload")
        );
        assert_eq!(
            fs::read(
                store
                    .version_root("language.cot.pt-br", "1.0.0")
                    .join("payload")
                    .join("source")
                    .join("strings.bin")
            )
            .unwrap(),
            b"localized-payload"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn launcher_executable_is_verified_and_preserved_outside_temporary_staging() {
        let root = test_root("launcher-ready");
        let store = ComponentStore::new(root.join("components"));
        let release = create_launcher_package(&store, "1.1.0");
        let key = test_signing_key().verifying_key();
        let ready = store
            .stage_launcher_ready_with_key(&release, &key)
            .expect("stage launcher executable");
        assert!(ready.join("ready.toml").is_file());
        assert!(ready.join("mojorecomp-launcher.exe").is_file());
        let prepared = store
            .prepare_launcher_replacement_with_key(&key)
            .expect("prepare signed launcher executable");
        assert_eq!(prepared.version, "1.1.0");
        assert!(
            prepared
                .source_root
                .join("mojorecomp-launcher.exe")
                .is_file()
        );
        assert!(!store.staging_component_root("launcher").unwrap().exists());
        store.recover_all().expect("startup recovery");
        assert!(ready.is_dir());
        store
            .cleanup_launcher_ready("1.1.0")
            .expect("cleanup installed launcher package");
        assert!(!ready.exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn launcher_executable_signature_binds_payload_and_version() {
        let root = test_root("launcher-exe-signature");
        let path = root.join("launcher.exe");
        let signing_key = test_signing_key();
        let verifying_key = signing_key.verifying_key();
        write_signed_launcher_executable(&path, "1.2.0", b"MZlauncher", &signing_key);

        assert_eq!(
            verify_signed_launcher_executable_with_key(&path, Some("1.2.0"), &verifying_key)
                .expect("valid signed launcher"),
            "1.2.0"
        );
        let wrong_version =
            verify_signed_launcher_executable_with_key(&path, Some("1.3.0"), &verifying_key)
                .expect_err("signed version must be bound");
        assert!(wrong_version.contains("does not match expected version"));

        let mut tampered = fs::read(&path).expect("signed launcher bytes");
        tampered[2] ^= 0x01;
        fs::write(&path, tampered).expect("tampered launcher");
        let tampered_error =
            verify_signed_launcher_executable_with_key(&path, Some("1.2.0"), &verifying_key)
                .expect_err("tampered launcher must fail");
        assert!(tampered_error.contains("signature is invalid"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn interrupted_activation_rolls_back_uncommitted_version() {
        let root = test_root("recovery");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store.install_staged(&first).expect("install first");
        let interrupted_root = store.version_root("runtime.cot", "0.3.0");
        fs::create_dir_all(&interrupted_root).expect("interrupted version");
        write_toml_atomic(
            &store.journal_path("runtime.cot"),
            &UpdateJournal {
                schema_version: INSTALL_SCHEMA_VERSION,
                id: "runtime.cot".into(),
                kind: ComponentKind::Runtime,
                new_version: "0.3.0".into(),
                previous_version: Some("0.2.0".into()),
            },
        )
        .expect("journal");
        assert_eq!(
            store.recover_component("runtime.cot").unwrap(),
            Some(RecoveryResult::RolledBack("runtime.cot".into()))
        );
        assert!(!interrupted_root.exists());
        assert_eq!(
            store.active_status("runtime.cot").unwrap().unwrap().version,
            "0.2.0"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn interrupted_activation_revalidates_committed_version_before_finishing() {
        let root = test_root("recovery-committed-corrupt");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store.install_staged(&first).expect("install first");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store.install_staged(&second).expect("install second");
        fs::write(
            store
                .version_root("runtime.cot", "0.3.0")
                .join("payload")
                .join("cot-runtime.exe"),
            b"interrupted-corrupt-runtime",
        )
        .expect("corrupt committed version");
        write_toml_atomic(
            &store.journal_path("runtime.cot"),
            &UpdateJournal {
                schema_version: INSTALL_SCHEMA_VERSION,
                id: "runtime.cot".into(),
                kind: ComponentKind::Runtime,
                new_version: "0.3.0".into(),
                previous_version: Some("0.2.0".into()),
            },
        )
        .expect("journal");

        assert_eq!(
            store.recover_component("runtime.cot").unwrap(),
            Some(RecoveryResult::RolledBack("runtime.cot".into()))
        );
        let active = store
            .active_status("runtime.cot")
            .expect("status")
            .expect("active runtime");
        assert_eq!(active.version, "0.2.0");
        assert!(active.healthy);
        assert!(!store.version_root("runtime.cot", "0.3.0").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn corrupted_component_without_compatible_release_is_not_repairable() {
        let catalog = parse_and_validate_catalog(
            &valid_catalog().replace("min_launcher = \"1.0.0\"", "min_launcher = \"9.0.0\""),
        )
        .expect("catalog");
        let installed = vec![
            InstalledComponent {
                id: "launcher".into(),
                version: "1.0.0".into(),
                healthy: true,
            },
            InstalledComponent {
                id: "runtime.cot".into(),
                version: "0.1.0-alpha".into(),
                healthy: false,
            },
        ];
        let plans = plan_updates(&catalog, &installed, "1.0.0").expect("plans");
        let runtime = plans
            .iter()
            .find(|plan| plan.id == "runtime.cot")
            .expect("runtime plan");
        assert_eq!(runtime.state, PlanState::RepairUnavailable);
        assert!(runtime.latest_version.is_none());
        assert!(runtime.download_url.is_none());
    }

    #[test]
    fn corrupted_dependencies_do_not_satisfy_compatibility_constraints() {
        let catalog_text = format!(
            "{}\n{}",
            valid_catalog(),
            r#"
[[release]]
id = "language.cot.pt-br"
kind = "language"
version = "1.0.0"
platform = "windows"
arch = "x86_64"
url = "https://example.com/cot-pt-br.zip"
size = 100
sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
published = "2026-09-27"
notes_url = "https://example.com/releases/cot-pt-br-1.0.0"
package = "zip"
unpacked_size = 200
required_files = ["source/strings.bin"]
game_id = "cot"
locale = "pt-BR"
display_name = "Brazilian Portuguese"
xbox_language = 1

[[release.compatibility.require]]
id = "runtime.cot"
min_version = "0.1.0-alpha"
"#
        );
        let catalog = parse_and_validate_catalog(&catalog_text).expect("catalog");
        let installed = vec![
            InstalledComponent {
                id: "launcher".into(),
                version: "1.0.0".into(),
                healthy: true,
            },
            InstalledComponent {
                id: "runtime.cot".into(),
                version: "0.1.0-alpha".into(),
                healthy: false,
            },
        ];
        let plans = plan_updates(&catalog, &installed, "1.0.0").expect("plans");
        assert_eq!(
            plans
                .iter()
                .find(|plan| plan.id == "language.cot.pt-br")
                .expect("language plan")
                .state,
            PlanState::Incompatible
        );
    }

    #[test]
    fn localization_catalog_synthesizes_pack_backed_language_releases() {
        let mut catalog = parse_and_validate_catalog(&valid_catalog()).expect("update catalog");
        assert!(
            catalog
                .releases
                .iter()
                .all(|release| release.kind != ComponentKind::Language)
        );
        let metadata = parse_and_validate_localization_catalog(
            r#"
schema_version = 2
game_id = "cot"
runtime_version = "0.2.0"
pack_version = "1.0.0"
url = "https://example.com/MojoRecomp-COT-Localization-Pack-1.0.0.zip"
size = 900
sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
published = "2026-10-05"
notes_url = "https://example.com/releases/localization-pack-1.0.0"
min_launcher = "1.1.0"

[[language]]
id = "language.cot.ar"
game_id = "cot"
locale = "ar"
display_name = "العربية"
xbox_language = 1
version = "1.0.0"
component_size = 400
component_sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
unpacked_size = 800
required_files = ["language-patches.toml", "mojorecomp-package.toml", "mojorecomp-package.sig"]
"#,
        )
        .expect("localization catalog");
        apply_localization_catalog_metadata(&mut catalog, &metadata)
            .expect("apply localization catalog");
        let language = catalog
            .releases
            .iter()
            .find(|release| release.id == "language.cot.ar")
            .expect("synthesized Arabic release");
        assert_eq!(language.display_name.as_deref(), Some("العربية"));
        assert_eq!(language.xbox_language, Some(1));
        assert_eq!(
            language.url,
            "https://example.com/MojoRecomp-COT-Localization-Pack-1.0.0.zip"
        );
        assert_eq!(language.size, 900);
        assert_eq!(language.sha256, "a".repeat(64));
        let pack = language
            .localization_pack
            .as_ref()
            .expect("pack-backed language release");
        assert_eq!(pack.version, "1.0.0");
        assert_eq!(pack.component_size, 400);
        assert_eq!(pack.component_sha256, "b".repeat(64));
        assert_eq!(language.compatibility.requirements.len(), 1);
        assert_eq!(language.compatibility.requirements[0].id, "runtime.cot");
        assert_eq!(
            language.compatibility.requirements[0].min_version.as_deref(),
            Some("0.2.0")
        );
        assert_eq!(language.compatibility.requirements[0].max_version, None);
    }

    #[test]
    fn runtime_launch_uses_lgpl_override_without_modifying_verified_payload() {
        let root = test_root("override");
        let store = ComponentStore::new(root.join("components"));
        let release = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"official-ffmpeg");
        store.install_staged(&release).expect("install");
        let overrides = root.join("lgpl-overrides");
        fs::create_dir_all(&overrides).expect("override root");
        fs::write(overrides.join("mojorecomp-ffmpeg.dll"), b"modified-ffmpeg").expect("override");
        let runtime = store
            .prepare_runtime_launch("runtime.cot", &overrides)
            .expect("prepare")
            .expect("active runtime");
        assert_eq!(fs::read(&runtime).unwrap(), b"runtime-v2");
        assert_eq!(
            fs::read(runtime.parent().unwrap().join("mojorecomp-ffmpeg.dll")).unwrap(),
            b"modified-ffmpeg"
        );
        assert_eq!(
            fs::read(
                store
                    .version_root("runtime.cot", "0.2.0")
                    .join("payload")
                    .join("mojorecomp-ffmpeg.dll")
            )
            .unwrap(),
            b"official-ffmpeg"
        );
        fs::remove_dir_all(root).unwrap();
    }
}
