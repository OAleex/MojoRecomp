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
    PortableZip,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Compatibility {
    #[serde(default)]
    pub min_launcher: Option<String>,
    #[serde(default)]
    pub max_launcher: Option<String>,
    #[serde(default, rename = "require")]
    pub requirements: Vec<ComponentRequirement>,
}

#[derive(Clone, Debug, Deserialize)]
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
    pub compatibility: Compatibility,
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
    validate_component_id(&release.id)?;
    Version::parse(&release.version)
        .map_err(|_| format!("Component {} has an invalid semantic version", release.id))?;
    if release.platform != "windows" || release.arch != "x86_64" {
        return Err(format!(
            "Component {} must target windows-x86_64",
            release.id
        ));
    }
    validate_public_https_url(&release.url)?;
    if release.size == 0 {
        return Err(format!(
            "Component {} has an empty artifact size",
            release.id
        ));
    }
    validate_sha256(&release.sha256)?;
    validate_date(&release.published)?;
    validate_public_https_url(&release.notes_url)?;
    validate_compatibility(&release.compatibility)?;
    for path in &release.required_files {
        safe_relative_path(path)?;
    }
    match release.kind {
        ComponentKind::Launcher => {
            if release.id != "launcher"
                || release.game_id.is_some()
                || release.locale.is_some()
                || release.entrypoint.is_some()
                || release.unpacked_size.is_some()
                || release.package != PackageFormat::PortableZip
                || !release.required_files.is_empty()
            {
                return Err("Launcher releases must use the launcher portable-zip schema".into());
            }
        }
        ComponentKind::Runtime => {
            let game = release
                .game_id
                .as_deref()
                .ok_or_else(|| format!("Runtime {} is missing game_id", release.id))?;
            validate_game_id(game)?;
            if release.id != format!("runtime.{game}") || release.locale.is_some() {
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
            if release.id != format!("language.{game}.{}", locale.to_ascii_lowercase())
                || release.entrypoint.is_some()
            {
                return Err(format!(
                    "Language component ID does not match game_id/locale: {}",
                    release.id
                ));
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

pub fn fetch_catalog(url: &str) -> Result<UpdateCatalog, String> {
    validate_resolved_public_https_url(url)?;
    ensure_tls_crypto_provider()?;
    let client = reqwest::blocking::Client::builder()
        .redirect(public_https_redirect_policy())
        .connect_timeout(Duration::from_secs(20))
        .timeout(Duration::from_secs(30))
        .build()
        .map_err(|error| format!("Could not initialize update client: {error}"))?;
    let response = client
        .get(url)
        .send()
        .and_then(|response| response.error_for_status())
        .map_err(|error| format!("Could not download update catalog: {error}"))?;
    validate_resolved_public_https_url(response.url().as_str())?;
    let text = response
        .text()
        .map_err(|error| format!("Could not read update catalog: {error}"))?;
    parse_and_validate_catalog(&text)
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
    pub last_action: Option<String>,
}

#[derive(Clone, Debug)]
pub struct ActiveComponentPayload {
    pub version: String,
    pub kind: ComponentKind,
    pub root: PathBuf,
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
        self.downloads_root
            .join(&release.id)
            .join(&release.version)
            .join("artifact.zip")
    }

    pub fn launcher_ready_root(&self) -> Result<PathBuf, String> {
        let root = self.launcher_ready_root_path();
        if !root.is_dir() {
            return Err("No verified launcher update is ready".into());
        }
        Ok(root)
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
            last_action: self.last_action(id),
        }))
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
        validate_release(release)?;
        if release.kind != ComponentKind::Launcher {
            return Err("Only launcher packages can be promoted to manual replacement".into());
        }
        let artifact = self.staged_artifact_path(release);
        verify_file_exact(&artifact, release.size, &release.sha256)?;
        validate_launcher_portable_zip(&artifact)?;
        let ready_root = self.launcher_ready_root_path();
        if ready_root.exists() {
            fs::remove_dir_all(&ready_root)
                .map_err(|error| format!("Could not clear previous launcher update: {error}"))?;
        }
        fs::create_dir_all(&ready_root)
            .map_err(|error| format!("Could not create launcher update directory: {error}"))?;
        let file_name = format!(
            "MojoRecomp-Launcher-{}-windows-x64-portable.zip",
            release.version
        );
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
            "ready_manual",
            "Verified launcher package is ready for manual replacement",
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
        validate_release(release)?;
        if release.kind == ComponentKind::Launcher {
            return Err(
                "Launcher packages are staged for manual replacement and are not activated in-process"
                    .into(),
            );
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
            files,
        };
        write_toml_atomic(&staging.join("installation.toml"), &manifest)?;

        let previous = read_toml_optional::<ActiveComponent>(&self.active_path(&release.id))?
            .map(|state| state.active_version);
        let journal = UpdateJournal {
            schema_version: INSTALL_SCHEMA_VERSION,
            id: release.id.clone(),
            kind: release.kind.clone(),
            new_version: release.version.clone(),
            previous_version: previous.clone(),
        };
        write_toml_atomic(&self.journal_path(&release.id), &journal)?;

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

    pub fn rollback(&self, id: &str) -> Result<(), String> {
        let active = read_toml_optional::<ActiveComponent>(&self.active_path(id))?
            .ok_or_else(|| format!("Component {id} has no active version"))?;
        let previous = active
            .previous_version
            .clone()
            .ok_or_else(|| format!("Component {id} has no previous version to restore"))?;
        self.verify_version(id, &previous)?;
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

fn validate_launcher_portable_zip(artifact: &Path) -> Result<(), String> {
    let file = File::open(artifact)
        .map_err(|error| format!("Could not open launcher update package: {error}"))?;
    let mut archive = ZipArchive::new(file)
        .map_err(|error| format!("Launcher update is not a valid ZIP archive: {error}"))?;
    if archive.is_empty() || archive.len() > 4096 {
        return Err("Launcher update ZIP has an invalid entry count".into());
    }
    let mut launchers = 0usize;
    for index in 0..archive.len() {
        let entry = archive
            .by_index(index)
            .map_err(|error| format!("Could not inspect launcher update ZIP entry: {error}"))?;
        let Some(enclosed) = entry.enclosed_name() else {
            return Err("Launcher update ZIP contains an unsafe path".into());
        };
        safe_relative_path_buf(&enclosed)?;
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err("Launcher update ZIP contains a symbolic link".into());
        }
        if !entry.is_dir()
            && enclosed
                .file_name()
                .and_then(|value| value.to_str())
                .is_some_and(|name| name.eq_ignore_ascii_case("mojorecomp-launcher.exe"))
        {
            launchers += 1;
        }
    }
    if launchers != 1 {
        return Err("Launcher update ZIP must contain exactly one mojorecomp-launcher.exe".into());
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
    use std::io::Cursor;
    use zip::ZipWriter;
    use zip::write::SimpleFileOptions;

    #[test]
    fn tls_crypto_provider_is_available() {
        ensure_tls_crypto_provider().expect("TLS crypto provider should initialize");
        assert!(rustls::crypto::CryptoProvider::get_default().is_some());
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
url = "https://example.com/MojoRecomp-Launcher-1.1.0.zip"
size = 1234
sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
published = "2026-09-27"
notes_url = "https://example.com/releases/launcher-1.1.0"
package = "portable-zip"

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
            compatibility: Compatibility::default(),
        }
    }

    fn create_launcher_package(store: &ComponentStore, version: &str) -> ComponentRelease {
        let artifact = store
            .downloads_root
            .join("launcher")
            .join(version)
            .join("artifact.zip");
        fs::create_dir_all(artifact.parent().unwrap()).expect("download staging");
        let file = File::create(&artifact).expect("artifact");
        let mut writer = ZipWriter::new(file);
        let path =
            format!("MojoRecomp-Launcher-{version}-windows-x64-portable/mojorecomp-launcher.exe");
        writer
            .start_file(path, SimpleFileOptions::default())
            .expect("launcher entry");
        writer
            .write_all(b"launcher-binary")
            .expect("launcher payload");
        writer.finish().expect("finish zip");
        let package = fs::read(&artifact).expect("read artifact");
        ComponentRelease {
            id: "launcher".into(),
            kind: ComponentKind::Launcher,
            version: version.into(),
            platform: "windows".into(),
            arch: "x86_64".into(),
            url: "https://example.com/launcher.zip".into(),
            size: package.len() as u64,
            sha256: format!("{:x}", Sha256::digest(&package)),
            published: "2026-09-27".into(),
            notes_url: "https://example.com/releases/launcher".into(),
            package: PackageFormat::PortableZip,
            unpacked_size: None,
            entrypoint: None,
            required_files: Vec::new(),
            game_id: None,
            locale: None,
            compatibility: Compatibility::default(),
        }
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
        assert!(
            stage_reader(
                Cursor::new(payload),
                &root.join("bad-size.zip"),
                payload.len() as u64 + 1,
                &sha256,
                &mut |_, _| {},
            )
            .is_err()
        );
        assert!(
            stage_reader(
                Cursor::new(payload),
                &root.join("bad-hash.zip"),
                payload.len() as u64,
                &"0".repeat(64),
                &mut |_, _| {},
            )
            .is_err()
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn activation_keeps_previous_version_and_rollback_restores_it() {
        let root = test_root("activation");
        let store = ComponentStore::new(root.join("components"));
        let first = create_runtime_package(&store, "0.2.0", b"runtime-v2", b"ffmpeg-v2");
        store.install_staged(&first).expect("install first");
        let second = create_runtime_package(&store, "0.3.0", b"runtime-v3", b"ffmpeg-v3");
        store.install_staged(&second).expect("install second");
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
    fn launcher_package_is_verified_and_preserved_outside_temporary_staging() {
        let root = test_root("launcher-ready");
        let store = ComponentStore::new(root.join("components"));
        let release = create_launcher_package(&store, "1.1.0");
        let ready = store
            .stage_launcher_ready(&release)
            .expect("stage launcher package");
        assert!(ready.join("ready.toml").is_file());
        assert!(
            ready
                .join("MojoRecomp-Launcher-1.1.0-windows-x64-portable.zip")
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
