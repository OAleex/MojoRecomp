use crate::rcf::{self, Replacement};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::Read;
use std::path::{Path, PathBuf};

pub const PT_BR_PROFILE: &str = "pt-BR";
const CACHE_SCHEMA_VERSION: u32 = 2;
const BUILDER_VERSION: &str = "cot-localization-rcf-v2";
const DISK_MARGIN_BYTES: u64 = 256 * 1024 * 1024;
const MAX_DISCOVERED_FILES: usize = 100_000;
const LANGUAGE_PATCH_SCHEMA_VERSION: u32 = 1;
const LANGUAGE_PATCH_MANIFEST: &str = "language-patches.toml";
const DELTA_MAGIC: &[u8; 8] = b"MJRDIF01";
const MAX_PATCHED_RESOURCE_BYTES: u64 = 512 * 1024 * 1024;
const MAX_DELTA_OPERATIONS: u32 = 1_000_000;

const COT_LOCALIZATION_TARGETS: [&str; 14] = [
    r"levels\L3_E3\statics.lua",
    r"package\5000af12.p3d",
    r"mdl\c80d681a.p3d",
    r"levels\L4_E4\props_normal.lua",
    r"package\7a8185b0.p3d",
    r"package\c1e387c7.p3d",
    r"package\7efdcd91.p3d",
    r"package\7a88cba0.p3d",
    r"package\cdd70a8c.p3d",
    r"package\97597d1b.p3d",
    r"package\b4c85fe7.p3d",
    r"package\bea9f18c.p3d",
    r"package\79aea37a.p3d",
    r"package\a2c6e833.p3d",
];

const COT_LEGACY_LOCALIZATION_TARGETS: [&str; 14] = [
    r"cinematics\348a5480.p3d",
    r"cinematics\348a54bd.p3d",
    r"cinematics\3eac4648.p3d",
    r"cinematics\c1b269e4.p3d",
    r"package\L2_E4\a7e44a5e.p3d",
    r"package\454d192.p3d",
    r"package\59712a90.p3d",
    r"package\7a8185b0.p3d",
    r"package\7a88cba0.p3d",
    r"package\7efdcd91.p3d",
    r"package\97597d1b.p3d",
    r"package\b4c85fe7.p3d",
    r"package\c1e387c7.p3d",
    r"package\cdd70a8c.p3d",
];

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct LanguagePatchManifest {
    schema_version: u32,
    game_id: String,
    locale: String,
    #[serde(rename = "patch")]
    patches: Vec<LanguagePatchEntry>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct LanguagePatchEntry {
    archive_path: String,
    file: String,
}

#[derive(Clone, Serialize)]
pub struct LocalizationStatus {
    pub profile: String,
    pub source_installed: bool,
    pub overlay_ready: bool,
    pub detail: String,
}

#[derive(Serialize, Deserialize)]
struct CacheManifest {
    schema_version: u32,
    profile: String,
    builder_version: String,
    fingerprint: String,
    archive_size: u64,
    source_size: u64,
    source_modified_ns: u64,
    source_sha256: String,
    replacements_sha256: String,
}

#[derive(Clone, Copy)]
struct SourceIdentity {
    size: u64,
    modified_ns: u64,
}

fn profile_root(game_root: &Path, profile: &str) -> PathBuf {
    game_root.join("localization").join(profile)
}

fn source_root(game_root: &Path, profile: &str) -> PathBuf {
    profile_root(game_root, profile).join("source")
}

pub fn overlay_root(game_root: &Path, profile: &str) -> PathBuf {
    profile_root(game_root, profile)
}

pub fn overlay_archive(game_root: &Path, profile: &str) -> PathBuf {
    profile_root(game_root, profile).join("default.rcf")
}

fn cache_manifest_path(game_root: &Path, profile: &str) -> PathBuf {
    profile_root(game_root, profile).join("manifest.toml")
}

fn bundled_marker_path(game_root: &Path) -> PathBuf {
    profile_root(game_root, PT_BR_PROFILE).join(".bundled-source")
}

fn component_marker_path(game_root: &Path, profile: &str) -> PathBuf {
    profile_root(game_root, profile).join(".component-source")
}

pub fn uses_bundled_source(game_root: &Path) -> bool {
    bundled_marker_path(game_root).is_file()
}

pub fn uses_component_source(game_root: &Path, profile: &str) -> bool {
    component_marker_path(game_root, profile).is_file()
}

pub fn component_source_version(game_root: &Path, profile: &str) -> Option<String> {
    let text = fs::read_to_string(component_marker_path(game_root, profile)).ok()?;
    let mut lines = text.lines();
    let component_id = lines.next()?;
    let version = lines.next()?;
    if component_id != format!("language.cot.{}", profile.to_ascii_lowercase())
        || version.is_empty()
    {
        return None;
    }
    Some(version.to_string())
}

fn acquire_profile_lock(game_root: &Path, profile: &str) -> Result<File, String> {
    let root = overlay_root(game_root, profile);
    fs::create_dir_all(&root)
        .map_err(|error| format!("Could not create localization overlay directory: {error}"))?;
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join(".localization.lock"))
        .map_err(|error| format!("Could not open localization lock file: {error}"))?;
    fs2::FileExt::try_lock_exclusive(&lock).map_err(|_| {
        "Localization resources are being changed by another MojoRecomp Launcher instance"
            .to_string()
    })?;
    Ok(lock)
}

fn cleanup_stale_build_files(root: &Path) -> Result<(), String> {
    if !root.is_dir() {
        return Ok(());
    }
    for entry in fs::read_dir(root)
        .map_err(|error| format!("Could not inspect localization overlay directory: {error}"))?
    {
        let entry =
            entry.map_err(|error| format!("Could not inspect localization build file: {error}"))?;
        let kind = entry
            .file_type()
            .map_err(|error| format!("Could not inspect localization build file type: {error}"))?;
        if !kind.is_file() {
            continue;
        }
        let name = entry.file_name();
        let name = name.to_string_lossy();
        let stale_archive = name.starts_with(".default.rcf.") && name.ends_with(".tmp");
        let stale_manifest = name.starts_with(".manifest-") && name.ends_with(".tmp");
        if stale_archive
            || stale_manifest
            || name == ".default.rcf.building.tmp"
            || name == ".manifest.tmp"
        {
            fs::remove_file(entry.path()).map_err(|error| {
                format!("Could not remove stale localization build file: {error}")
            })?;
        }
    }
    Ok(())
}

fn archive_relative_path(root: &Path, archive_path: &str) -> PathBuf {
    archive_path
        .split('\\')
        .fold(root.to_path_buf(), |path, component| path.join(component))
}

fn validate_p3d(path: &Path) -> Result<(), String> {
    let mut signature = [0u8; 4];
    File::open(path)
        .and_then(|mut file| file.read_exact(&mut signature))
        .map_err(|error| format!("Could not validate Pure3D file {}: {error}", path.display()))?;
    if signature != *b"P3D\xFF" {
        return Err(format!(
            "File is not a valid Pure3D resource: {}",
            path.display()
        ));
    }
    Ok(())
}

fn validate_localization_resource(path: &Path, archive_path: &str) -> Result<(), String> {
    let metadata = fs::metadata(path).map_err(|error| {
        format!(
            "Could not inspect localization resource {}: {error}",
            path.display()
        )
    })?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > MAX_PATCHED_RESOURCE_BYTES {
        return Err(format!(
            "Localization resource has an invalid size: {}",
            path.display()
        ));
    }
    if archive_path.to_ascii_lowercase().ends_with(".p3d") {
        validate_p3d(path)?;
    }
    Ok(())
}

fn sha256_bytes(bytes: &[u8]) -> [u8; 32] {
    let digest = Sha256::digest(bytes);
    let mut output = [0u8; 32];
    output.copy_from_slice(&digest);
    output
}

fn read_delta_u32(bytes: &[u8], cursor: &mut usize) -> Result<u32, String> {
    let end = cursor
        .checked_add(4)
        .ok_or_else(|| "Localization delta cursor overflow".to_string())?;
    let value = bytes
        .get(*cursor..end)
        .ok_or_else(|| "Localization delta ended unexpectedly".to_string())?;
    *cursor = end;
    Ok(u32::from_le_bytes(value.try_into().unwrap()))
}

fn read_delta_u64(bytes: &[u8], cursor: &mut usize) -> Result<u64, String> {
    let end = cursor
        .checked_add(8)
        .ok_or_else(|| "Localization delta cursor overflow".to_string())?;
    let value = bytes
        .get(*cursor..end)
        .ok_or_else(|| "Localization delta ended unexpectedly".to_string())?;
    *cursor = end;
    Ok(u64::from_le_bytes(value.try_into().unwrap()))
}

fn localization_payload_path(root: &Path, value: &str) -> Result<PathBuf, String> {
    if value.is_empty() || value.starts_with(['/', '\\']) || value.contains(':') {
        return Err(format!("Unsafe Localization Pack path: {value}"));
    }
    let mut path = root.to_path_buf();
    for component in value.split(['/', '\\']) {
        if component.is_empty() || component == "." || component == ".." {
            return Err(format!("Unsafe Localization Pack path: {value}"));
        }
        path.push(component);
    }
    Ok(path)
}

fn apply_delta_patch(
    source: &[u8],
    patch_path: &Path,
    destination: &Path,
    archive_path: &str,
) -> Result<(), String> {
    let metadata = fs::metadata(patch_path)
        .map_err(|error| format!("Could not inspect localization delta: {error}"))?;
    if !metadata.is_file()
        || metadata.len() > MAX_PATCHED_RESOURCE_BYTES.saturating_add(1024 * 1024)
    {
        return Err("Localization delta is missing or too large".into());
    }
    let patch = fs::read(patch_path)
        .map_err(|error| format!("Could not read localization delta: {error}"))?;
    let header_size = DELTA_MAGIC.len() + 32 + 32 + 8 + 4;
    if patch.len() < header_size || &patch[..DELTA_MAGIC.len()] != DELTA_MAGIC {
        return Err("Localization delta has an invalid signature".into());
    }
    let mut cursor = DELTA_MAGIC.len();
    let expected_source = patch[cursor..cursor + 32].to_vec();
    cursor += 32;
    let expected_result = patch[cursor..cursor + 32].to_vec();
    cursor += 32;
    let result_size = read_delta_u64(&patch, &mut cursor)?;
    let operation_count = read_delta_u32(&patch, &mut cursor)?;
    if result_size == 0
        || result_size > MAX_PATCHED_RESOURCE_BYTES
        || operation_count == 0
        || operation_count > MAX_DELTA_OPERATIONS
    {
        return Err("Localization delta declares unsafe output metadata".into());
    }
    if sha256_bytes(source).as_slice() != expected_source.as_slice() {
        return Err("Localization delta does not match this game resource".into());
    }

    let capacity: usize = result_size
        .try_into()
        .map_err(|_| "Localization delta output is too large for this host".to_string())?;
    let mut output = Vec::with_capacity(capacity);
    for _ in 0..operation_count {
        let operation = *patch
            .get(cursor)
            .ok_or_else(|| "Localization delta ended before all operations".to_string())?;
        cursor += 1;
        match operation {
            1 => {
                let offset = read_delta_u64(&patch, &mut cursor)?;
                let length = read_delta_u64(&patch, &mut cursor)?;
                let end = offset
                    .checked_add(length)
                    .ok_or_else(|| "Localization delta copy range overflow".to_string())?;
                if end > source.len() as u64 || length == 0 {
                    return Err("Localization delta contains an invalid copy range".into());
                }
                let start: usize = offset
                    .try_into()
                    .map_err(|_| "Delta copy offset overflow")?;
                let end: usize = end.try_into().map_err(|_| "Delta copy end overflow")?;
                output.extend_from_slice(&source[start..end]);
            }
            2 => {
                let length = read_delta_u64(&patch, &mut cursor)?;
                if length == 0 {
                    return Err("Localization delta contains an empty literal".into());
                }
                let length: usize = length
                    .try_into()
                    .map_err(|_| "Localization delta literal is too large".to_string())?;
                let end = cursor
                    .checked_add(length)
                    .ok_or_else(|| "Localization delta literal range overflow".to_string())?;
                let literal = patch
                    .get(cursor..end)
                    .ok_or_else(|| "Localization delta literal is truncated".to_string())?;
                output.extend_from_slice(literal);
                cursor = end;
            }
            _ => {
                return Err(format!(
                    "Localization delta has an unknown operation: {operation}"
                ));
            }
        }
        if output.len() > capacity {
            return Err("Localization delta produced more data than declared".into());
        }
    }
    if cursor != patch.len() || output.len() != capacity {
        return Err("Localization delta output length is invalid".into());
    }
    if sha256_bytes(&output).as_slice() != expected_result.as_slice() {
        return Err("Localization delta output failed SHA-256 verification".into());
    }
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Could not create localization delta output path: {error}"))?;
    }
    fs::write(destination, output)
        .map_err(|error| format!("Could not write patched localization resource: {error}"))?;
    validate_localization_resource(destination, archive_path)
}

fn install_delta_component_patch(
    selected_root: &Path,
    component_id: &str,
    version: &str,
    profile: &str,
    game_root: &Path,
) -> Result<(), String> {
    let manifest_path = selected_root.join(LANGUAGE_PATCH_MANIFEST);
    let text = fs::read_to_string(&manifest_path)
        .map_err(|error| format!("Could not read language patch manifest: {error}"))?;
    let manifest: LanguagePatchManifest = toml::from_str(&text)
        .map_err(|error| format!("Language patch manifest is invalid: {error}"))?;
    if manifest.schema_version != LANGUAGE_PATCH_SCHEMA_VERSION
        || manifest.game_id != "cot"
        || !manifest.locale.eq_ignore_ascii_case(profile)
        || manifest.patches.len() != COT_LOCALIZATION_TARGETS.len()
    {
        return Err("Language patch manifest does not match this COT localization profile".into());
    }

    let mut patch_by_target = HashMap::new();
    for patch in &manifest.patches {
        let key = patch.archive_path.replace('/', "\\").to_ascii_lowercase();
        if !COT_LOCALIZATION_TARGETS
            .iter()
            .any(|target| target.eq_ignore_ascii_case(&key))
            || patch_by_target.insert(key, patch).is_some()
        {
            return Err(format!(
                "Language patch manifest contains an unexpected or duplicate target: {}",
                patch.archive_path
            ));
        }
    }

    let _profile_lock = acquire_profile_lock(game_root, profile)?;
    let parent = profile_root(game_root, profile);
    fs::create_dir_all(&parent)
        .map_err(|error| format!("Could not create localization source storage: {error}"))?;
    let staging = parent.join(".source-component.tmp");
    if staging.exists() {
        fs::remove_dir_all(&staging)
            .map_err(|error| format!("Could not remove stale localization staging: {error}"))?;
    }
    fs::create_dir_all(&staging)
        .map_err(|error| format!("Could not create localization staging: {error}"))?;
    let original_archive = game_root.join("default.rcf");

    let result = (|| {
        for target in COT_LOCALIZATION_TARGETS {
            let key = target.to_ascii_lowercase();
            let patch = patch_by_target
                .get(&key)
                .ok_or_else(|| format!("Language patch is missing target: {target}"))?;
            let patch_path = localization_payload_path(selected_root, &patch.file)?;
            let source = rcf::read_entry(&original_archive, target, MAX_PATCHED_RESOURCE_BYTES)?;
            let destination = archive_relative_path(&staging, target);
            apply_delta_patch(&source, &patch_path, &destination, target)?;
        }
        replacements_from_root_for_targets(&staging, &COT_LOCALIZATION_TARGETS)?;
        Ok::<(), String>(())
    })();
    if let Err(error) = result {
        let _ = fs::remove_dir_all(&staging);
        return Err(error);
    }

    let finalize = (|| {
        promote_directory(&staging, &source_root(game_root, profile))?;
        let bundled_marker = bundled_marker_path(game_root);
        if profile == PT_BR_PROFILE && bundled_marker.exists() {
            fs::remove_file(bundled_marker)
                .map_err(|error| format!("Could not clear legacy bundled PT-BR marker: {error}"))?;
        }
        let managed_marker = component_marker_path(game_root, profile);
        if managed_marker.exists() {
            fs::remove_file(&managed_marker)
                .map_err(|error| format!("Could not replace language component marker: {error}"))?;
        }
        if component_id.lines().count() != 1 || version.lines().count() != 1 {
            return Err("Language component metadata contains an invalid newline".into());
        }
        fs::write(&managed_marker, format!("{component_id}\n{version}\n"))
            .map_err(|error| format!("Could not write language component marker: {error}"))
    })();
    if finalize.is_err() && staging.exists() {
        let _ = fs::remove_dir_all(&staging);
    }
    finalize
}

fn discover_p3d_files(root: &Path) -> Result<HashMap<String, Vec<PathBuf>>, String> {
    if !root.is_dir() {
        return Err("Selected localization source folder does not exist".into());
    }
    let mut result: HashMap<String, Vec<PathBuf>> = HashMap::new();
    let mut stack = vec![root.to_path_buf()];
    let mut discovered = 0usize;
    while let Some(directory) = stack.pop() {
        for entry in fs::read_dir(&directory)
            .map_err(|error| format!("Could not read localization source folder: {error}"))?
        {
            let entry = entry
                .map_err(|error| format!("Could not inspect localization source entry: {error}"))?;
            let kind = entry.file_type().map_err(|error| {
                format!("Could not inspect localization source entry type: {error}")
            })?;
            if kind.is_symlink() {
                return Err(format!(
                    "Localization source folder contains a symbolic link: {}",
                    entry.path().display()
                ));
            }
            if kind.is_dir() {
                stack.push(entry.path());
                continue;
            }
            if !kind.is_file() {
                continue;
            }
            discovered += 1;
            if discovered > MAX_DISCOVERED_FILES {
                return Err(
                    "Selected folder contains too many files to be a localization source".into(),
                );
            }
            let path = entry.path();
            let is_p3d = path
                .extension()
                .and_then(|value| value.to_str())
                .map(|value| value.eq_ignore_ascii_case("p3d"))
                .unwrap_or(false);
            if !is_p3d {
                continue;
            }
            let Some(file_name) = path.file_name().and_then(|value| value.to_str()) else {
                continue;
            };
            result
                .entry(file_name.to_ascii_lowercase())
                .or_default()
                .push(path);
        }
    }
    Ok(result)
}

fn expected_basename(target: &str) -> &str {
    target.rsplit('\\').next().unwrap_or(target)
}

fn replacements_from_root_for_targets(
    root: &Path,
    targets: &[&str],
) -> Result<Vec<Replacement>, String> {
    let mut replacements = Vec::with_capacity(targets.len());
    for target in targets {
        let source = archive_relative_path(root, target);
        if !source.is_file() {
            return Err(format!(
                "Localization source is incomplete: missing {}",
                expected_basename(target)
            ));
        }
        validate_localization_resource(&source, target)?;
        replacements.push(Replacement {
            archive_path: (*target).to_string(),
            source_path: source,
        });
    }
    Ok(replacements)
}

fn replacements_from_root(root: &Path) -> Result<Vec<Replacement>, String> {
    match replacements_from_root_for_targets(root, &COT_LOCALIZATION_TARGETS) {
        Ok(replacements) => Ok(replacements),
        Err(current_error) => {
            replacements_from_root_for_targets(root, &COT_LEGACY_LOCALIZATION_TARGETS)
                .map_err(|_| current_error)
        }
    }
}

fn managed_replacements(game_root: &Path, profile: &str) -> Result<Vec<Replacement>, String> {
    replacements_from_root(&source_root(game_root, profile))
}

fn promote_directory(staging: &Path, destination: &Path) -> Result<(), String> {
    let parent = destination
        .parent()
        .ok_or_else(|| "Managed localization source has no parent directory".to_string())?;
    fs::create_dir_all(parent)
        .map_err(|error| format!("Could not create localization source directory: {error}"))?;
    let destination_name = destination
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "Managed localization source has an invalid directory name".to_string())?;
    let backup = parent.join(format!(".{destination_name}-previous"));
    if backup.exists() {
        if destination.exists() {
            fs::remove_dir_all(&backup)
                .map_err(|error| format!("Could not remove stale localization backup: {error}"))?;
        } else {
            fs::rename(&backup, destination).map_err(|error| {
                format!("Could not restore interrupted localization backup: {error}")
            })?;
        }
    }
    let had_existing = destination.exists();
    if had_existing {
        fs::rename(destination, &backup)
            .map_err(|error| format!("Could not back up previous localization source: {error}"))?;
    }
    if let Err(error) = fs::rename(staging, destination) {
        if had_existing && backup.exists() && !destination.exists() {
            let _ = fs::rename(&backup, destination);
        }
        return Err(format!(
            "Could not activate imported localization source: {error}"
        ));
    }
    if backup.exists() {
        let _ = fs::remove_dir_all(backup);
    }
    Ok(())
}

fn import_patch_with_component_marker(
    selected_root: &Path,
    game_root: &Path,
    profile: &str,
    component_marker: Option<(&str, &str)>,
) -> Result<(), String> {
    let discovered = discover_p3d_files(selected_root)?;
    let _profile_lock = acquire_profile_lock(game_root, profile)?;
    let parent = profile_root(game_root, profile);
    fs::create_dir_all(&parent)
        .map_err(|error| format!("Could not create localization source storage: {error}"))?;
    let staging = parent.join(".source-import.tmp");
    if staging.exists() {
        fs::remove_dir_all(&staging)
            .map_err(|error| format!("Could not remove stale localization import: {error}"))?;
    }
    fs::create_dir_all(&staging)
        .map_err(|error| format!("Could not create localization import staging: {error}"))?;

    let result = (|| {
        for target in COT_LEGACY_LOCALIZATION_TARGETS {
            let basename = expected_basename(target).to_ascii_lowercase();
            let candidates = discovered.get(&basename).cloned().unwrap_or_default();
            if candidates.len() != 1 {
                return Err(if candidates.is_empty() {
                    format!("Selected folder is missing required localization file: {basename}")
                } else {
                    format!("Selected folder contains multiple copies of required file: {basename}")
                });
            }
            validate_p3d(&candidates[0])?;
            let destination = archive_relative_path(&staging, target);
            if let Some(parent) = destination.parent() {
                fs::create_dir_all(parent).map_err(|error| {
                    format!("Could not create localization staging path: {error}")
                })?;
            }
            fs::copy(&candidates[0], &destination).map_err(|error| {
                format!("Could not import required localization file {basename}: {error}")
            })?;
        }
        Ok::<(), String>(())
    })();

    if let Err(error) = result {
        let _ = fs::remove_dir_all(&staging);
        return Err(error);
    }
    let finalize = (|| {
        replacements_from_root_for_targets(&staging, &COT_LEGACY_LOCALIZATION_TARGETS)?;
        promote_directory(&staging, &source_root(game_root, profile))?;
        let bundled_marker = bundled_marker_path(game_root);
        if profile == PT_BR_PROFILE && bundled_marker.exists() {
            fs::remove_file(bundled_marker).map_err(|error| {
                format!("Could not clear legacy bundled localization marker: {error}")
            })?;
        }
        let managed_marker = component_marker_path(game_root, profile);
        if managed_marker.exists() {
            fs::remove_file(&managed_marker).map_err(|error| {
                format!("Could not clear localization component marker: {error}")
            })?;
        }
        if let Some((component_id, version)) = component_marker {
            if component_id.lines().count() != 1 || version.lines().count() != 1 {
                return Err("Language component metadata contains an invalid newline".into());
            }
            fs::write(&managed_marker, format!("{component_id}\n{version}\n")).map_err(
                |error| format!("Could not write localization component marker: {error}"),
            )?;
        }
        Ok(())
    })();
    if finalize.is_err() && staging.exists() {
        let _ = fs::remove_dir_all(&staging);
    }
    finalize
}

#[cfg(test)]
fn import_patch(selected_root: &Path, game_root: &Path) -> Result<(), String> {
    import_patch_with_component_marker(selected_root, game_root, PT_BR_PROFILE, None)
}

pub fn install_component_patch(
    selected_root: &Path,
    component_id: &str,
    version: &str,
    profile: &str,
    game_root: &Path,
) -> Result<(), String> {
    if selected_root.join(LANGUAGE_PATCH_MANIFEST).is_file() {
        return install_delta_component_patch(
            selected_root,
            component_id,
            version,
            profile,
            game_root,
        );
    }
    import_patch_with_component_marker(
        selected_root,
        game_root,
        profile,
        Some((component_id, version)),
    )
}

fn hash_reader(
    reader: &mut impl Read,
    mut remaining: Option<u64>,
    hasher: &mut Sha256,
) -> Result<(), String> {
    let mut buffer = vec![0u8; 1024 * 1024];
    loop {
        let limit = remaining
            .map(|value| value.min(buffer.len() as u64) as usize)
            .unwrap_or(buffer.len());
        if limit == 0 {
            break;
        }
        let read = reader
            .read(&mut buffer[..limit])
            .map_err(|error| format!("Could not hash localization input: {error}"))?;
        if read == 0 {
            if remaining.is_some() {
                return Err("Localization input ended while computing its fingerprint".into());
            }
            break;
        }
        hasher.update(&buffer[..read]);
        if let Some(value) = &mut remaining {
            *value -= read as u64;
        }
    }
    Ok(())
}

fn digest_hex(hasher: Sha256) -> String {
    hasher
        .finalize()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

fn source_identity(original_archive: &Path) -> Result<SourceIdentity, String> {
    let metadata = fs::metadata(original_archive)
        .map_err(|error| format!("Could not query original RCF metadata: {error}"))?;
    let modified_ns = metadata
        .modified()
        .ok()
        .and_then(|value| value.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|value| value.as_nanos())
        .map(|value| value.min(u64::MAX as u128) as u64)
        .unwrap_or_default();
    Ok(SourceIdentity {
        size: metadata.len(),
        modified_ns,
    })
}

fn source_sha256(original_archive: &Path) -> Result<String, String> {
    let mut file = File::open(original_archive)
        .map_err(|error| format!("Could not open original RCF for hashing: {error}"))?;
    let mut hasher = Sha256::new();
    hash_reader(&mut file, None, &mut hasher)?;
    Ok(digest_hex(hasher))
}

fn replacements_sha256(replacements: &[Replacement]) -> Result<String, String> {
    let mut hasher = Sha256::new();
    for replacement in replacements {
        hasher.update(replacement.archive_path.as_bytes());
        let metadata = fs::metadata(&replacement.source_path).map_err(|error| {
            format!(
                "Could not query localization resource {}: {error}",
                replacement.source_path.display()
            )
        })?;
        hasher.update(metadata.len().to_le_bytes());
        let mut file = File::open(&replacement.source_path).map_err(|error| {
            format!(
                "Could not open localization resource {}: {error}",
                replacement.source_path.display()
            )
        })?;
        hash_reader(&mut file, None, &mut hasher)?;
    }
    Ok(digest_hex(hasher))
}

fn combined_fingerprint(profile: &str, source_sha256: &str, replacements_sha256: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(BUILDER_VERSION.as_bytes());
    hasher.update(profile.as_bytes());
    hasher.update(source_sha256.as_bytes());
    hasher.update(replacements_sha256.as_bytes());
    digest_hex(hasher)
}

fn manifest_format_matches(manifest: &CacheManifest, profile: &str) -> bool {
    manifest.schema_version == CACHE_SCHEMA_VERSION
        && manifest.profile == profile
        && manifest.builder_version == BUILDER_VERSION
}

fn manifest_fast_inputs_match(
    manifest: &CacheManifest,
    profile: &str,
    source: SourceIdentity,
    replacements_sha256: &str,
) -> bool {
    manifest_format_matches(manifest, profile)
        && manifest.source_size == source.size
        && manifest.source_modified_ns == source.modified_ns
        && manifest.replacements_sha256 == replacements_sha256
        && manifest.fingerprint
            == combined_fingerprint(profile, &manifest.source_sha256, replacements_sha256)
}

fn cached_archive_matches(manifest: &CacheManifest, game_root: &Path, profile: &str) -> bool {
    let archive = overlay_archive(game_root, profile);
    fs::metadata(&archive)
        .map(|metadata| metadata.len() == manifest.archive_size)
        .unwrap_or(false)
        && rcf::inspect(&archive).is_ok()
}

fn read_cache_manifest(game_root: &Path, profile: &str) -> Option<CacheManifest> {
    let text = fs::read_to_string(cache_manifest_path(game_root, profile)).ok()?;
    toml::from_str(&text).ok()
}

fn write_cache_manifest(
    game_root: &Path,
    profile: &str,
    fingerprint: &str,
    archive_size: u64,
    source: SourceIdentity,
    source_sha256: &str,
    replacements_sha256: &str,
) -> Result<(), String> {
    let root = overlay_root(game_root, profile);
    fs::create_dir_all(&root)
        .map_err(|error| format!("Could not create localization overlay directory: {error}"))?;
    let manifest = CacheManifest {
        schema_version: CACHE_SCHEMA_VERSION,
        profile: profile.into(),
        builder_version: BUILDER_VERSION.into(),
        fingerprint: fingerprint.into(),
        archive_size,
        source_size: source.size,
        source_modified_ns: source.modified_ns,
        source_sha256: source_sha256.into(),
        replacements_sha256: replacements_sha256.into(),
    };
    let text = toml::to_string_pretty(&manifest)
        .map_err(|error| format!("Could not serialize localization cache metadata: {error}"))?;
    let temp = root.join(".manifest.tmp");
    if temp.exists() {
        fs::remove_file(&temp).map_err(|error| {
            format!("Could not remove stale localization cache metadata: {error}")
        })?;
    }
    fs::write(&temp, text)
        .map_err(|error| format!("Could not write localization cache metadata: {error}"))?;
    let destination = cache_manifest_path(game_root, profile);
    if destination.exists() {
        fs::remove_file(&destination)
            .map_err(|error| format!("Could not replace localization cache metadata: {error}"))?;
    }
    fs::rename(&temp, &destination)
        .map_err(|error| format!("Could not activate localization cache metadata: {error}"))
}

pub fn status(original_archive: &Path, game_root: &Path, profile: &str) -> LocalizationStatus {
    let replacements = match managed_replacements(game_root, profile) {
        Ok(value) => value,
        Err(error) => {
            return LocalizationStatus {
                profile: profile.into(),
                source_installed: false,
                overlay_ready: false,
                detail: error,
            };
        }
    };
    let Some(manifest) = read_cache_manifest(game_root, profile) else {
        return LocalizationStatus {
            profile: profile.into(),
            source_installed: true,
            overlay_ready: false,
            detail: "The Localization Pack needs to be installed.".into(),
        };
    };
    if let Err(error) = rcf::inspect(original_archive) {
        return LocalizationStatus {
            profile: profile.into(),
            source_installed: true,
            overlay_ready: false,
            detail: error,
        };
    }
    let source = match source_identity(original_archive) {
        Ok(value) => value,
        Err(error) => {
            return LocalizationStatus {
                profile: profile.into(),
                source_installed: true,
                overlay_ready: false,
                detail: error,
            };
        }
    };
    let replacements_sha256 = match replacements_sha256(&replacements) {
        Ok(value) => value,
        Err(error) => {
            return LocalizationStatus {
                profile: profile.into(),
                source_installed: true,
                overlay_ready: false,
                detail: error,
            };
        }
    };
    let valid = manifest_fast_inputs_match(&manifest, profile, source, &replacements_sha256)
        && cached_archive_matches(&manifest, game_root, profile);
    LocalizationStatus {
        profile: profile.into(),
        source_installed: true,
        overlay_ready: valid,
        detail: if valid {
            "Localization Pack installed.".into()
        } else {
            "The Localization Pack needs to be installed again.".into()
        },
    }
}

fn activate_archive(temp: &Path, destination: &Path) -> Result<(), String> {
    let parent = destination
        .parent()
        .ok_or_else(|| "Localization overlay has no parent directory".to_string())?;
    let backup = parent.join(".default.rcf.previous");
    if backup.exists() {
        if destination.exists() {
            fs::remove_file(&backup).map_err(|error| {
                format!("Could not remove stale localization archive backup: {error}")
            })?;
        } else {
            fs::rename(&backup, destination).map_err(|error| {
                format!("Could not restore interrupted localization archive backup: {error}")
            })?;
        }
    }
    let had_existing = destination.exists();
    if had_existing {
        fs::rename(destination, &backup)
            .map_err(|error| format!("Could not back up previous localization archive: {error}"))?;
    }
    if let Err(error) = fs::rename(temp, destination) {
        if had_existing && backup.exists() && !destination.exists() {
            let _ = fs::rename(&backup, destination);
        }
        return Err(format!(
            "Could not activate rebuilt localization archive: {error}"
        ));
    }
    if backup.exists() {
        let _ = fs::remove_file(backup);
    }
    Ok(())
}

pub fn prepare_overlay(
    original_archive: &Path,
    game_root: &Path,
    profile: &str,
    mut on_progress: impl FnMut(u64, u64) -> Result<(), String>,
) -> Result<PathBuf, String> {
    let _profile_lock = acquire_profile_lock(game_root, profile)?;
    let root = overlay_root(game_root, profile);
    cleanup_stale_build_files(&root)?;
    let replacements = managed_replacements(game_root, profile)?;
    rcf::inspect(original_archive)?;
    let source = source_identity(original_archive)?;
    let replacements_sha256 = replacements_sha256(&replacements)?;
    let existing_manifest = read_cache_manifest(game_root, profile);
    if let Some(manifest) = existing_manifest.as_ref()
        && manifest_fast_inputs_match(manifest, profile, source, &replacements_sha256)
        && cached_archive_matches(manifest, game_root, profile)
    {
        return Ok(overlay_archive(game_root, profile));
    }

    let source_sha256 = match existing_manifest.as_ref() {
        Some(manifest)
            if manifest.source_size == source.size
                && manifest.source_modified_ns == source.modified_ns
                && !manifest.source_sha256.is_empty() =>
        {
            manifest.source_sha256.clone()
        }
        _ => source_sha256(original_archive)?,
    };
    let expected_fingerprint = combined_fingerprint(profile, &source_sha256, &replacements_sha256);
    if let Some(manifest) = existing_manifest.as_ref()
        && manifest_format_matches(manifest, profile)
        && manifest.source_sha256 == source_sha256
        && manifest.replacements_sha256 == replacements_sha256
        && manifest.fingerprint == expected_fingerprint
        && cached_archive_matches(manifest, game_root, profile)
    {
        write_cache_manifest(
            game_root,
            profile,
            &expected_fingerprint,
            manifest.archive_size,
            source,
            &source_sha256,
            &replacements_sha256,
        )?;
        return Ok(overlay_archive(game_root, profile));
    }

    let estimated_size = rcf::estimate_rebuild_size(original_archive, &replacements)?;
    let available = fs2::available_space(&root).map_err(|error| {
        format!("Could not query free disk space for localization cache: {error}")
    })?;
    let required = estimated_size.saturating_add(DISK_MARGIN_BYTES);
    if available < required {
        return Err(format!(
            "Not enough free disk space to install the Localization Pack: {available} bytes available, {required} bytes required"
        ));
    }

    let temp = root.join(".default.rcf.building.tmp");
    if temp.exists() {
        fs::remove_file(&temp)
            .map_err(|error| format!("Could not remove stale localization build file: {error}"))?;
    }
    let rebuild = rcf::rebuild_to_path(original_archive, &temp, &replacements, |done, total| {
        on_progress(done, total)
    });
    let rebuilt = match rebuild {
        Ok(value) => value,
        Err(error) => {
            let _ = fs::remove_file(&temp);
            return Err(error);
        }
    };
    if rebuilt.source_size != estimated_size {
        let _ = fs::remove_file(&temp);
        return Err(format!(
            "Rebuilt localization archive size mismatch: expected {estimated_size}, got {}",
            rebuilt.source_size
        ));
    }

    let destination = overlay_archive(game_root, profile);
    activate_archive(&temp, &destination)?;
    write_cache_manifest(
        game_root,
        profile,
        &expected_fingerprint,
        rebuilt.source_size,
        source,
        &source_sha256,
        &replacements_sha256,
    )?;
    Ok(destination)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Seek, SeekFrom, Write};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_root(name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "mojorecomp-localization-{name}-{}-{nonce}",
            std::process::id()
        ))
    }

    fn create_synthetic_cot_archive(path: &Path) -> Vec<(String, Vec<u8>)> {
        const HEADER_SIZE: usize = 60;
        const ALIGNMENT: u64 = 2048;
        let mut targets = COT_LOCALIZATION_TARGETS.to_vec();
        for target in COT_LEGACY_LOCALIZATION_TARGETS {
            if !targets
                .iter()
                .any(|existing| existing.eq_ignore_ascii_case(target))
            {
                targets.push(target);
            }
        }
        let mut files = targets
            .iter()
            .enumerate()
            .map(|(index, target)| {
                let mut payload = if target.to_ascii_lowercase().ends_with(".p3d") {
                    b"P3D\xFForiginal-".to_vec()
                } else {
                    b"-- original-".to_vec()
                };
                payload.extend_from_slice(index.to_string().as_bytes());
                ((*target).to_string(), payload)
            })
            .collect::<Vec<_>>();
        files.push((
            r"script\unchanged.lua".into(),
            b"print('unchanged')".to_vec(),
        ));

        let count = files.len() as u32;
        let table1_offset = HEADER_SIZE as u32;
        let table1_size = count * 12;
        let table2_offset = ALIGNMENT as u32;
        let mut table2 = Vec::new();
        table2.extend_from_slice(&(ALIGNMENT as u32).to_le_bytes());
        table2.extend_from_slice(&0u32.to_le_bytes());
        for (index, (name, _)) in files.iter().enumerate() {
            table2.extend_from_slice(&(0x4500_0000u32 + index as u32).to_le_bytes());
            table2.extend_from_slice(&(ALIGNMENT as u32).to_le_bytes());
            table2.extend_from_slice(&0u32.to_le_bytes());
            table2.extend_from_slice(&((name.len() + 1) as u32).to_le_bytes());
            table2.extend_from_slice(name.as_bytes());
            table2.push(0);
            table2.extend_from_slice(&[0, 0, 0]);
        }
        let table2_size = table2.len() as u32;
        let align = |value: u64| (value + ALIGNMENT - 1) & !(ALIGNMENT - 1);
        let data_start = align(table2_offset as u64 + table2_size as u64);
        let mut offsets = Vec::with_capacity(files.len());
        let mut next = data_start;
        for (_, payload) in &files {
            next = align(next);
            offsets.push(next);
            next += payload.len() as u64;
        }

        let mut output = File::create(path).expect("archive");
        let mut header = [0u8; HEADER_SIZE];
        header[..b"ATG CORE CEMENT LIBRARY".len()].copy_from_slice(b"ATG CORE CEMENT LIBRARY");
        header[32..36].copy_from_slice(&[2, 1, 1, 1]);
        header[36..40].copy_from_slice(&table1_offset.to_be_bytes());
        header[40..44].copy_from_slice(&table1_size.to_be_bytes());
        header[44..48].copy_from_slice(&table2_offset.to_be_bytes());
        header[48..52].copy_from_slice(&table2_size.to_be_bytes());
        header[56..60].copy_from_slice(&count.to_be_bytes());
        output.write_all(&header).expect("header");
        for (index, ((_, payload), offset)) in files.iter().zip(&offsets).enumerate() {
            output
                .write_all(&(0xA100_0000u32 + index as u32).to_be_bytes())
                .expect("id");
            output
                .write_all(&(*offset as u32).to_be_bytes())
                .expect("offset");
            output
                .write_all(&(payload.len() as u32).to_be_bytes())
                .expect("size");
        }
        let current = output.stream_position().expect("position");
        output
            .write_all(&vec![0u8; table2_offset as usize - current as usize])
            .expect("table padding");
        output.write_all(&table2).expect("table2");
        let current = output.stream_position().expect("position");
        output
            .write_all(&vec![0u8; data_start as usize - current as usize])
            .expect("data padding");
        for ((_, payload), offset) in files.iter().zip(&offsets) {
            let current = output.stream_position().expect("position");
            output
                .write_all(&vec![0u8; *offset as usize - current as usize])
                .expect("entry padding");
            output.write_all(payload).expect("payload");
        }
        files
    }

    fn write_literal_delta(source: &[u8], target: &[u8], path: &Path) {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(DELTA_MAGIC);
        bytes.extend_from_slice(&sha256_bytes(source));
        bytes.extend_from_slice(&sha256_bytes(target));
        bytes.extend_from_slice(&(target.len() as u64).to_le_bytes());
        bytes.extend_from_slice(&1u32.to_le_bytes());
        bytes.push(2);
        bytes.extend_from_slice(&(target.len() as u64).to_le_bytes());
        bytes.extend_from_slice(target);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).expect("delta parent");
        }
        fs::write(path, bytes).expect("delta");
    }

    #[test]
    fn patch_discovery_ignores_pal_backups_and_requires_unique_expected_names() {
        let root = test_root("discovery");
        fs::create_dir_all(root.join("nested")).expect("root");
        fs::write(root.join("nested").join("348a5480.p3d"), b"P3D\xFFtest").expect("p3d");
        fs::write(root.join("nested").join("348a5480.pal"), b"backup").expect("pal");
        let discovered = discover_p3d_files(&root).expect("discover");
        assert_eq!(discovered.get("348a5480.p3d").map(Vec::len), Some(1));
        assert!(!discovered.contains_key("348a5480.pal"));
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn p3d_validation_rejects_wrong_signature() {
        let root = test_root("signature");
        fs::create_dir_all(&root).expect("root");
        let valid = root.join("valid.p3d");
        let invalid = root.join("invalid.p3d");
        fs::write(&valid, b"P3D\xFFpayload").expect("valid");
        fs::write(&invalid, b"BAD!payload").expect("invalid");
        assert!(validate_p3d(&valid).is_ok());
        assert!(validate_p3d(&invalid).is_err());
        assert!(validate_localization_resource(&valid, r"package\valid.p3d").is_ok());
        assert!(validate_localization_resource(&invalid, r"package\invalid.p3d").is_err());
        assert!(validate_localization_resource(&invalid, r"levels\L3_E3\statics.lua").is_ok());
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn import_patch_normalizes_all_expected_resources_into_managed_storage() {
        let root = test_root("import");
        let selected = root.join("selected");
        let game_root = root.join("game");
        fs::create_dir_all(&selected).expect("selected");
        fs::create_dir_all(&game_root).expect("game root");
        for (index, target) in COT_LEGACY_LOCALIZATION_TARGETS.iter().enumerate() {
            let directory = selected.join(format!("source-{index}"));
            fs::create_dir_all(&directory).expect("source directory");
            let mut resource = b"P3D\xFF".to_vec();
            resource.extend_from_slice(format!("resource-{index}").as_bytes());
            fs::write(directory.join(expected_basename(target)), resource).expect("resource");
            fs::write(
                directory.join(format!("{}.pal", expected_basename(target))),
                b"backup",
            )
            .expect("backup");
        }

        import_patch(&selected, &game_root).expect("import");
        let replacements =
            managed_replacements(&game_root, PT_BR_PROFILE).expect("managed replacements");
        assert_eq!(replacements.len(), COT_LEGACY_LOCALIZATION_TARGETS.len());
        for replacement in replacements {
            assert!(replacement.source_path.is_file());
            assert!(
                replacement
                    .source_path
                    .starts_with(source_root(&game_root, PT_BR_PROFILE))
            );
        }
        assert_eq!(
            source_root(&game_root, PT_BR_PROFILE),
            game_root
                .join("localization")
                .join(PT_BR_PROFILE)
                .join("source")
        );

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn component_language_source_is_marked_and_manual_import_takes_precedence() {
        let root = test_root("component-source");
        let component = root.join("component");
        let manual = root.join("manual");
        let game_root = root.join("game");
        fs::create_dir_all(&component).expect("component");
        fs::create_dir_all(&manual).expect("manual");
        fs::create_dir_all(&game_root).expect("game root");
        for (index, target) in COT_LEGACY_LOCALIZATION_TARGETS.iter().enumerate() {
            let name = expected_basename(target);
            let mut component_bytes = b"P3D\xFFcomponent-".to_vec();
            component_bytes.extend_from_slice(index.to_string().as_bytes());
            fs::write(component.join(name), component_bytes).expect("component resource");

            let mut manual_bytes = b"P3D\xFFmanual-".to_vec();
            manual_bytes.extend_from_slice(index.to_string().as_bytes());
            fs::write(manual.join(name), manual_bytes).expect("manual resource");
        }

        install_component_patch(
            &component,
            "language.cot.pt-br",
            "1.2.3",
            PT_BR_PROFILE,
            &game_root,
        )
        .expect("install component source");
        assert!(uses_component_source(&game_root, PT_BR_PROFILE));
        assert!(!uses_bundled_source(&game_root));
        assert_eq!(
            component_source_version(&game_root, PT_BR_PROFILE).as_deref(),
            Some("1.2.3")
        );

        import_patch(&manual, &game_root).expect("manual import");
        assert!(!uses_component_source(&game_root, PT_BR_PROFILE));
        assert!(!uses_bundled_source(&game_root));
        assert!(component_source_version(&game_root, PT_BR_PROFILE).is_none());
        let first = archive_relative_path(
            &source_root(&game_root, PT_BR_PROFILE),
            COT_LEGACY_LOCALIZATION_TARGETS[0],
        );
        assert!(fs::read(first).unwrap().starts_with(b"P3D\xFFmanual-"));

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn delta_language_component_uses_original_rcf_and_builds_generic_profile() {
        let root = test_root("delta-component");
        let game_root = root.join("game");
        let component = root.join("component");
        fs::create_dir_all(&game_root).expect("game root");
        fs::create_dir_all(&component).expect("component root");
        let original_archive = game_root.join("default.rcf");
        let original_files = create_synthetic_cot_archive(&original_archive);

        let mut manifest =
            String::from("schema_version = 1\ngame_id = \"cot\"\nlocale = \"ar\"\n\n");
        for (index, target) in COT_LOCALIZATION_TARGETS.iter().enumerate() {
            let source = &original_files
                .iter()
                .find(|(name, _)| name.eq_ignore_ascii_case(target))
                .expect("original target")
                .1;
            let mut translated = if target.to_ascii_lowercase().ends_with(".p3d") {
                b"P3D\xFFdelta-language-".to_vec()
            } else {
                b"-- delta-language-".to_vec()
            };
            translated.extend_from_slice(index.to_string().as_bytes());
            let patch_name = format!("patches/{index:02}.mjdelta");
            write_literal_delta(source, &translated, &component.join(&patch_name));
            manifest.push_str("[[patch]]\n");
            manifest.push_str(&format!(
                "archive_path = \"{}\"\n",
                target.replace('\\', "\\\\")
            ));
            manifest.push_str(&format!("file = \"{patch_name}\"\n\n"));
        }
        fs::write(component.join(LANGUAGE_PATCH_MANIFEST), manifest).expect("patch manifest");

        install_component_patch(&component, "language.cot.ar", "1.0.0", "ar", &game_root)
            .expect("install delta component");
        assert!(uses_component_source(&game_root, "ar"));
        assert_eq!(
            component_source_version(&game_root, "ar").as_deref(),
            Some("1.0.0")
        );
        let first =
            archive_relative_path(&source_root(&game_root, "ar"), COT_LOCALIZATION_TARGETS[0]);
        assert_eq!(fs::read(first).unwrap(), b"-- delta-language-0");

        let overlay = prepare_overlay(&original_archive, &game_root, "ar", |_, _| Ok(()))
            .expect("prepare generic language overlay");
        assert!(overlay.is_file());
        assert!(status(&original_archive, &game_root, "ar").overlay_ready);
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn prepare_overlay_rebuilds_once_uses_cache_and_preserves_original_archive() {
        let root = test_root("prepare");
        let game_root = root.join("game");
        let selected = root.join("selected");
        fs::create_dir_all(&selected).expect("selected");
        fs::create_dir_all(&game_root).expect("game root");
        let original_archive = game_root.join("default.rcf");
        let original_files = create_synthetic_cot_archive(&original_archive);
        let original_bytes = fs::read(&original_archive).expect("original bytes");

        for (index, target) in COT_LEGACY_LOCALIZATION_TARGETS.iter().enumerate() {
            let directory = selected.join(format!("patch-{index}"));
            fs::create_dir_all(&directory).expect("patch directory");
            let mut payload = b"P3D\xFFtranslated-resource-".to_vec();
            payload.extend_from_slice(index.to_string().as_bytes());
            fs::write(directory.join(expected_basename(target)), payload).expect("patch file");
        }
        import_patch(&selected, &game_root).expect("import");
        let stale_temp = overlay_root(&game_root, PT_BR_PROFILE).join(".default.rcf.98765.tmp");
        fs::write(&stale_temp, b"stale").expect("stale temp");

        let mut progress_calls = 0usize;
        let derived = prepare_overlay(
            &original_archive,
            &game_root,
            PT_BR_PROFILE,
            |done, total| {
                assert!(done <= total);
                progress_calls += 1;
                Ok(())
            },
        )
        .expect("prepare");
        assert!(progress_calls > 0);
        assert!(!stale_temp.exists());
        assert!(derived.is_file());
        assert_eq!(
            derived,
            game_root
                .join("localization")
                .join(PT_BR_PROFILE)
                .join("default.rcf")
        );
        assert_eq!(
            fs::read(&original_archive).expect("original after"),
            original_bytes
        );
        let ready = status(&original_archive, &game_root, PT_BR_PROFILE);
        assert!(ready.source_installed);
        assert!(ready.overlay_ready);

        let derived_index = rcf::inspect(&derived).expect("derived index");
        let first = derived_index
            .entries
            .iter()
            .find(|entry| {
                entry
                    .name
                    .eq_ignore_ascii_case(COT_LEGACY_LOCALIZATION_TARGETS[0])
            })
            .expect("first target");
        let mut derived_file = File::open(&derived).expect("derived file");
        derived_file
            .seek(SeekFrom::Start(first.offset))
            .expect("seek target");
        let mut first_payload = vec![0u8; first.size as usize];
        derived_file
            .read_exact(&mut first_payload)
            .expect("target payload");
        assert!(first_payload.starts_with(b"P3D\xFFtranslated-resource-0"));

        let unchanged = derived_index
            .entries
            .iter()
            .find(|entry| entry.name == r"script\unchanged.lua")
            .expect("unchanged entry");
        derived_file
            .seek(SeekFrom::Start(unchanged.offset))
            .expect("seek unchanged");
        let mut unchanged_payload = vec![0u8; unchanged.size as usize];
        derived_file
            .read_exact(&mut unchanged_payload)
            .expect("unchanged payload");
        assert_eq!(
            unchanged_payload,
            original_files
                .iter()
                .find(|(name, _)| name == r"script\unchanged.lua")
                .unwrap()
                .1
        );

        let mut cached_progress_calls = 0usize;
        let cached = prepare_overlay(&original_archive, &game_root, PT_BR_PROFILE, |_, _| {
            cached_progress_calls += 1;
            Ok(())
        })
        .expect("cached prepare");
        assert_eq!(cached, derived);
        assert_eq!(cached_progress_calls, 0);
        assert_eq!(
            fs::read(&original_archive).expect("original final"),
            original_bytes
        );

        let mut stale_builder_manifest = read_cache_manifest(&game_root, PT_BR_PROFILE)
            .expect("cache manifest before builder invalidation");
        stale_builder_manifest.builder_version = "older-builder".into();
        fs::write(
            cache_manifest_path(&game_root, PT_BR_PROFILE),
            toml::to_string_pretty(&stale_builder_manifest).expect("serialize stale manifest"),
        )
        .expect("write stale builder manifest");
        assert!(!status(&original_archive, &game_root, PT_BR_PROFILE).overlay_ready);
        let mut builder_rebuild_progress_calls = 0usize;
        prepare_overlay(&original_archive, &game_root, PT_BR_PROFILE, |_, _| {
            builder_rebuild_progress_calls += 1;
            Ok(())
        })
        .expect("rebuild after builder version invalidation");
        assert!(builder_rebuild_progress_calls > 0);
        assert!(status(&original_archive, &game_root, PT_BR_PROFILE).overlay_ready);

        drop(derived_file);
        std::thread::sleep(std::time::Duration::from_millis(20));
        let original_index = rcf::inspect(&original_archive).expect("original index");
        let original_unchanged = original_index
            .entries
            .iter()
            .find(|entry| entry.name == r"script\unchanged.lua")
            .expect("original unchanged entry");
        let mut original_file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&original_archive)
            .expect("open original for external change");
        original_file
            .seek(SeekFrom::Start(original_unchanged.offset))
            .expect("seek original unchanged");
        original_file
            .write_all(b"P")
            .expect("external source modification");
        original_file
            .sync_all()
            .expect("flush external modification");
        drop(original_file);

        let externally_modified =
            fs::read(&original_archive).expect("externally modified original bytes");
        assert_ne!(externally_modified, original_bytes);
        assert!(!status(&original_archive, &game_root, PT_BR_PROFILE).overlay_ready);

        let mut rebuild_progress_calls = 0usize;
        let rebuilt_after_source_change =
            prepare_overlay(&original_archive, &game_root, PT_BR_PROFILE, |_, _| {
                rebuild_progress_calls += 1;
                Ok(())
            })
            .expect("rebuild after source change");
        assert_eq!(rebuilt_after_source_change, derived);
        assert!(rebuild_progress_calls > 0);
        assert_eq!(
            fs::read(&original_archive).expect("original after source-triggered rebuild"),
            externally_modified
        );

        let rebuilt_index = rcf::inspect(&rebuilt_after_source_change).expect("rebuilt index");
        let rebuilt_unchanged = rebuilt_index
            .entries
            .iter()
            .find(|entry| entry.name == r"script\unchanged.lua")
            .expect("rebuilt unchanged entry");
        let mut rebuilt_file =
            File::open(&rebuilt_after_source_change).expect("open rebuilt after source change");
        rebuilt_file
            .seek(SeekFrom::Start(rebuilt_unchanged.offset))
            .expect("seek rebuilt unchanged");
        let mut rebuilt_unchanged_payload = vec![0u8; rebuilt_unchanged.size as usize];
        rebuilt_file
            .read_exact(&mut rebuilt_unchanged_payload)
            .expect("read rebuilt unchanged");
        assert_eq!(rebuilt_unchanged_payload[0], b'P');

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn cancelled_prepare_removes_temp_and_preserves_original_archive() {
        let root = test_root("cancel");
        let game_root = root.join("game");
        let selected = root.join("selected");
        fs::create_dir_all(&selected).expect("selected");
        fs::create_dir_all(&game_root).expect("game root");
        let original_archive = game_root.join("default.rcf");
        create_synthetic_cot_archive(&original_archive);
        let original_bytes = fs::read(&original_archive).expect("original bytes");

        for (index, target) in COT_LEGACY_LOCALIZATION_TARGETS.iter().enumerate() {
            let directory = selected.join(format!("patch-{index}"));
            fs::create_dir_all(&directory).expect("patch directory");
            let mut payload = b"P3D\xFFtranslated-resource-".to_vec();
            payload.extend_from_slice(index.to_string().as_bytes());
            fs::write(directory.join(expected_basename(target)), payload).expect("patch file");
        }
        import_patch(&selected, &game_root).expect("import");
        let existing = prepare_overlay(&original_archive, &game_root, PT_BR_PROFILE, |_, _| Ok(()))
            .expect("initial cached archive");
        let existing_bytes = fs::read(&existing).expect("existing derived bytes");
        let changed_patch = archive_relative_path(
            &source_root(&game_root, PT_BR_PROFILE),
            COT_LEGACY_LOCALIZATION_TARGETS[0],
        );
        fs::write(&changed_patch, b"P3D\xFFchanged-after-cache").expect("change managed patch");
        assert!(!status(&original_archive, &game_root, PT_BR_PROFILE).overlay_ready);

        let result = prepare_overlay(&original_archive, &game_root, PT_BR_PROFILE, |done, _| {
            if done > 0 {
                return Err("cancelled by test".into());
            }
            Ok(())
        });
        assert!(result.is_err());
        assert_eq!(
            fs::read(overlay_archive(&game_root, PT_BR_PROFILE))
                .expect("existing derived after cancel"),
            existing_bytes
        );
        assert!(
            !overlay_root(&game_root, PT_BR_PROFILE)
                .join(".default.rcf.building.tmp")
                .exists()
        );
        assert_eq!(
            fs::read(&original_archive).expect("original after cancel"),
            original_bytes
        );

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn interrupted_source_promotion_restores_previous_directory_on_failure() {
        let root = test_root("source-rollback");
        let parent = root.join("game").join("localization").join(PT_BR_PROFILE);
        let destination = parent.join("source");
        let backup = parent.join(".source-previous");
        fs::create_dir_all(&backup).expect("backup");
        fs::write(backup.join("marker.txt"), b"previous").expect("marker");
        let missing_staging = parent.join("missing-staging");

        let result = promote_directory(&missing_staging, &destination);
        assert!(result.is_err());
        assert_eq!(
            fs::read(destination.join("marker.txt")).expect("restored marker"),
            b"previous"
        );
        assert!(!backup.exists());

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn interrupted_archive_promotion_restores_previous_archive_on_failure() {
        let root = test_root("archive-rollback");
        fs::create_dir_all(&root).expect("root");
        let destination = root.join("default.rcf");
        let backup = root.join(".default.rcf.previous");
        fs::write(&backup, b"previous-archive").expect("backup");
        let missing_temp = root.join("missing-build.tmp");

        let result = activate_archive(&missing_temp, &destination);
        assert!(result.is_err());
        assert_eq!(
            fs::read(&destination).expect("restored archive"),
            b"previous-archive"
        );
        assert!(!backup.exists());

        fs::remove_dir_all(root).expect("cleanup");
    }
}
