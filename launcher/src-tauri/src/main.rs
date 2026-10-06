#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod localization;
mod presence;
mod rcf;
mod storage;
mod updates;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
#[cfg(windows)]
use std::os::windows::io::AsRawHandle;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use storage::{
    LauncherSettings, StorageLayout, execute_library_migration, initialize_library,
    library_has_persistent_data, move_file_verified, plan_library_migration,
    rollback_library_migration,
};
use tauri::{Emitter, Manager};
#[cfg(windows)]
use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, HANDLE};
#[cfg(windows)]
use windows_sys::Win32::System::JobObjects::{
    AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation,
    SetInformationJobObject,
};
#[cfg(windows)]
use windows_sys::Win32::System::Threading::{OpenProcess, WaitForSingleObject};
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, ZipWriter};

const GAME_MANIFESTS: [&str; 2] = [
    include_str!("../../resources/games/cot.toml"),
    include_str!("../../resources/games/mom.toml"),
];
const SUITE_MANIFEST: &str = include_str!("../../resources/suite.toml");
const RUNTIME_HISTORY_MANIFEST: &str = include_str!("../../resources/runtime-history.toml");
const DISK_MINIMUM_MARGIN_BYTES: u64 = 1024 * 1024 * 1024;
const OBS_VULKAN_CAPTURE_DISABLE_ENV: &str = "DISABLE_VULKAN_OBS_CAPTURE";
include!(concat!(env!("OUT_DIR"), "/third_party_licenses.rs"));
// Production builds embed the verified runtime/tool payload prepared by
// scripts/prepare-bundle.mjs. Unit tests exercise launcher logic only and must
// remain runnable from a clean source tree before packaging output exists.
#[cfg(not(test))]
const EMBEDDED_COT_RUNTIME: &[u8] =
    include_bytes!("../binaries/cot-runtime-x86_64-pc-windows-msvc.exe");
#[cfg(not(test))]
const EMBEDDED_DXCOMPILER: &[u8] = include_bytes!("../../bundle/lib/dxcompiler.dll");
#[cfg(not(test))]
const EMBEDDED_DXIL: &[u8] = include_bytes!("../../bundle/lib/dxil.dll");
#[cfg(not(test))]
const EMBEDDED_FFMPEG: &[u8] = include_bytes!("../../bundle/lib/mojorecomp-ffmpeg.dll");
#[cfg(not(test))]
const EMBEDDED_LZX: &[u8] = include_bytes!("../../bundle/lib/mojorecomp-lzx.dll");
#[cfg(not(test))]
const EMBEDDED_EXTRACT_XISO: &[u8] = include_bytes!("../../bundle/tools/extract-xiso.exe");

#[cfg(test)]
const EMBEDDED_COT_RUNTIME: &[u8] = &[];
#[cfg(test)]
const EMBEDDED_DXCOMPILER: &[u8] = &[];
#[cfg(test)]
const EMBEDDED_DXIL: &[u8] = &[];
#[cfg(test)]
const EMBEDDED_FFMPEG: &[u8] = &[];
#[cfg(test)]
const EMBEDDED_LZX: &[u8] = &[];
#[cfg(test)]
const EMBEDDED_EXTRACT_XISO: &[u8] = &[];

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;
#[cfg(windows)]
const PROCESS_SYNCHRONIZE_ACCESS: u32 = 0x00100000;
#[cfg(windows)]
const WAIT_OBJECT_0_RESULT: u32 = 0;

fn hide_child_console(command: &mut Command) {
    #[cfg(windows)]
    {
        command.creation_flags(CREATE_NO_WINDOW);
    }
}

fn apply_runtime_compatibility_environment(command: &mut Command) {
    // OBS and applications built on its capture stack install an implicit
    // Vulkan layer system-wide. Some hook versions corrupt the runtime process
    // during startup. OBS publishes this variable as the supported, per-process
    // way to keep its Vulkan capture layer out of an application.
    command.env(OBS_VULKAN_CAPTURE_DISABLE_ENV, "1");
}

#[cfg(windows)]
struct RuntimeJob {
    handle: usize,
}

#[cfg(windows)]
impl RuntimeJob {
    fn new() -> Result<Self, String> {
        unsafe {
            let handle = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if handle.is_null() {
                return Err(format!(
                    "Could not create runtime Job Object (Win32 error {})",
                    GetLastError()
                ));
            }

            let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                &info as *const _ as *const std::ffi::c_void,
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            ) == 0
            {
                let error = GetLastError();
                CloseHandle(handle);
                return Err(format!(
                    "Could not configure runtime Job Object (Win32 error {error})"
                ));
            }
            Ok(Self {
                handle: handle as usize,
            })
        }
    }

    fn assign(&self, child: &Child) -> Result<(), String> {
        unsafe {
            let process = child.as_raw_handle() as HANDLE;
            if AssignProcessToJobObject(self.handle as HANDLE, process) == 0 {
                return Err(format!(
                    "Could not attach runtime to launcher Job Object (Win32 error {})",
                    GetLastError()
                ));
            }
        }
        Ok(())
    }
}

#[cfg(windows)]
impl Drop for RuntimeJob {
    fn drop(&mut self) {
        unsafe {
            if self.handle != 0 {
                CloseHandle(self.handle as HANDLE);
                self.handle = 0;
            }
        }
    }
}

#[cfg(not(windows))]
struct RuntimeJob;

#[cfg(not(windows))]
impl RuntimeJob {
    fn new() -> Result<Self, String> {
        Ok(Self)
    }
    fn assign(&self, _child: &Child) -> Result<(), String> {
        Ok(())
    }
}

#[derive(Clone, Deserialize)]
struct FileRequirements {
    required: Vec<String>,
    #[serde(default)]
    integrity: Vec<FileIntegrity>,
}

#[derive(Clone, Deserialize)]
struct FileIntegrity {
    path: String,
    size: u64,
    sha256: String,
    #[serde(default)]
    verify_on_launch: bool,
}

#[derive(Clone, Deserialize, Serialize)]
struct Capabilities {
    resolution_scale: bool,
    aspect_ratio: bool,
    fxaa: bool,
    anisotropic_filtering: bool,
    debug_mode: bool,
    #[serde(default)]
    localization: bool,
}

#[derive(Clone, Deserialize)]
struct GameManifest {
    id: String,
    name: String,
    runtime: String,
    runtime_version: String,
    status: String,
    playable: bool,
    files: FileRequirements,
    capabilities: Capabilities,
}

#[derive(Clone, Deserialize)]
struct SuiteManifest {
    version: String,
    release_channel: String,
    update_catalog: String,
    #[serde(default)]
    localization_catalog: String,
    #[serde(default)]
    discord_application_id: String,
}

#[derive(Clone, Serialize)]
struct GameInfo {
    id: String,
    name: String,
    status: String,
    runtime_version: String,
    installed: bool,
    game_root: Option<String>,
    managed: bool,
    playable: bool,
    capabilities: Capabilities,
}

#[derive(Clone, Serialize, Deserialize)]
struct RuntimeSettings {
    schema_version: u32,
    #[serde(default)]
    localization: LocalizationSettings,
    display: DisplaySettings,
    graphics: GraphicsSettings,
    advanced: AdvancedSettings,
}

#[derive(Clone, Serialize, Deserialize)]
struct LocalizationSettings {
    #[serde(default = "default_localization_profile")]
    profile: String,
    #[serde(default = "default_xbox_language")]
    xbox_language: u32,
}

fn default_localization_profile() -> String {
    "en".into()
}

fn default_xbox_language() -> u32 {
    1
}

impl Default for LocalizationSettings {
    fn default() -> Self {
        Self {
            profile: default_localization_profile(),
            xbox_language: default_xbox_language(),
        }
    }
}

#[derive(Clone, Serialize, Deserialize)]
struct DisplaySettings {
    mode: String,
    monitor: String,
    output_resolution: String,
    resolution_scale: u32,
    aspect_ratio: String,
    vsync: bool,
}

#[derive(Clone, Serialize, Deserialize)]
struct GraphicsSettings {
    anti_aliasing: String,
    texture_filtering: String,
    #[serde(default = "default_frame_rate")]
    frame_rate: String,
}

#[cfg(windows)]
fn wait_for_process_exit(pid: u32, timeout_ms: u32) -> Result<(), String> {
    unsafe {
        let handle = OpenProcess(PROCESS_SYNCHRONIZE_ACCESS, 0, pid);
        if handle.is_null() {
            let error = std::io::Error::last_os_error();
            if error.raw_os_error() == Some(87) {
                return Ok(());
            }
            return Err(format!(
                "Could not wait for the previous launcher process: {error}"
            ));
        }
        let result = WaitForSingleObject(handle, timeout_ms);
        CloseHandle(handle);
        if result != WAIT_OBJECT_0_RESULT {
            return Err(format!(
                "Timed out waiting for the previous launcher process to exit ({result})"
            ));
        }
        Ok(())
    }
}

#[cfg(not(windows))]
fn wait_for_process_exit(_pid: u32, _timeout_ms: u32) -> Result<(), String> {
    Ok(())
}

#[cfg(windows)]
fn wait_for_runtime_process_exit(pid: u32) -> bool {
    unsafe {
        let handle = OpenProcess(PROCESS_SYNCHRONIZE_ACCESS, 0, pid);
        if handle.is_null() {
            return false;
        }
        let result = WaitForSingleObject(handle, u32::MAX);
        CloseHandle(handle);
        result == WAIT_OBJECT_0_RESULT
    }
}

fn restore_and_restart_previous_launcher(
    target_exe: &Path,
    target_root: &Path,
    backup_root: &Path,
) -> Result<(), String> {
    updates::restore_launcher_replacement_tree(target_root, backup_root)?;
    let _ = fs::remove_dir_all(backup_root);
    Command::new(target_exe)
        .spawn()
        .map_err(|error| format!("Could not restart the restored launcher: {error}"))?;
    Ok(())
}

fn record_launcher_update_action(state: &str, detail: &str) {
    if let Ok(store) = launcher_component_store() {
        let _ = store.record_action("launcher", state, detail);
    }
}

fn run_launcher_update_helper_from_args() -> Option<i32> {
    let args = std::env::args_os().collect::<Vec<_>>();
    if args.get(1).and_then(|value| value.to_str()) != Some("--apply-launcher-update") {
        return None;
    }
    let result = (|| -> Result<(), String> {
        if args.len() != 5 {
            return Err("Launcher update helper received invalid arguments".into());
        }
        let pid = args[2]
            .to_string_lossy()
            .parse::<u32>()
            .map_err(|_| "Launcher update helper received an invalid process ID".to_string())?;
        let source_root = PathBuf::from(&args[3]);
        let target_exe = PathBuf::from(&args[4]);
        let target_root = target_exe
            .parent()
            .ok_or_else(|| "Launcher update target has no parent directory".to_string())?;
        wait_for_process_exit(pid, 60_000)?;
        let version = updates::signed_launcher_executable_version(
            &source_root.join("mojorecomp-launcher.exe"),
        )?;
        let backup_root = std::env::temp_dir().join(format!(
            "mojorecomp-launcher-backup-{}-{}",
            std::process::id(),
            timestamp_seconds()
        ));
        updates::backup_launcher_replacement_tree(target_root, &backup_root)?;
        if let Err(error) = updates::apply_launcher_replacement_tree(&source_root, target_root) {
            record_launcher_update_action(
                "apply_failed",
                &format!("Launcher update {version} could not be applied: {error}"),
            );
            let rollback =
                restore_and_restart_previous_launcher(&target_exe, target_root, &backup_root);
            return Err(match rollback {
                Ok(()) => format!("Launcher update {version} failed and was rolled back: {error}"),
                Err(restore_error) => format!(
                    "Launcher update {version} failed ({error}) and recovery also failed: {restore_error}"
                ),
            });
        }
        record_launcher_update_action(
            "applying",
            &format!("Launcher update {version} is being verified after restart"),
        );
        let mut updated = match Command::new(&target_exe).spawn() {
            Ok(child) => child,
            Err(error) => {
                record_launcher_update_action(
                    "apply_failed",
                    &format!("Launcher update {version} could not restart: {error}"),
                );
                restore_and_restart_previous_launcher(&target_exe, target_root, &backup_root)?;
                return Err(format!(
                    "Could not restart launcher {version}; the previous version was restored: {error}"
                ));
            }
        };
        std::thread::sleep(std::time::Duration::from_secs(2));
        match updated.try_wait() {
            Ok(Some(status)) => {
                record_launcher_update_action(
                    "apply_failed",
                    &format!("Launcher update {version} exited during startup with {status}"),
                );
                restore_and_restart_previous_launcher(&target_exe, target_root, &backup_root)?;
                return Err(format!(
                    "Updated launcher {version} exited during startup with {status}; the previous version was restored"
                ));
            }
            Err(error) => {
                record_launcher_update_action(
                    "apply_failed",
                    &format!("Launcher update {version} startup could not be monitored: {error}"),
                );
                restore_and_restart_previous_launcher(&target_exe, target_root, &backup_root)?;
                return Err(format!(
                    "Could not monitor launcher {version} startup; the previous version was restored: {error}"
                ));
            }
            Ok(None) => {}
        }
        record_launcher_update_action(
            "installed",
            &format!("Launcher update {version} applied successfully"),
        );
        let _ = fs::remove_dir_all(&backup_root);
        Ok(())
    })();
    Some(match result {
        Ok(()) => {
            append_launcher_log("Launcher update helper completed successfully");
            0
        }
        Err(error) => {
            record_launcher_update_action("apply_failed", &error);
            append_launcher_log(&format!("Launcher update helper failed: {error}"));
            1
        }
    })
}

fn cleanup_launcher_update_helpers() {
    let Ok(current_exe) = std::env::current_exe() else {
        return;
    };
    let Ok(entries) = fs::read_dir(std::env::temp_dir()) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let is_helper = path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| {
                name.starts_with("mojorecomp-launcher-update-") && name.ends_with(".exe")
            });
        if is_helper && path != current_exe {
            let _ = fs::remove_file(path);
        }
    }
}

#[derive(Clone, Serialize, Deserialize)]
struct AdvancedSettings {
    #[serde(default = "default_true")]
    logging_enabled: bool,
}

fn default_true() -> bool {
    true
}

fn default_frame_rate() -> String {
    "30".into()
}

impl Default for RuntimeSettings {
    fn default() -> Self {
        Self {
            schema_version: 1,
            localization: LocalizationSettings::default(),
            display: DisplaySettings {
                mode: "windowed".into(),
                monitor: "primary".into(),
                output_resolution: "desktop".into(),
                resolution_scale: 1,
                aspect_ratio: "16:9".into(),
                // Keep COT's current timing-safe presentation default.
                vsync: false,
            },
            graphics: GraphicsSettings {
                anti_aliasing: "fxaa_extreme".into(),
                texture_filtering: "8x".into(),
                frame_rate: default_frame_rate(),
            },
            advanced: AdvancedSettings {
                logging_enabled: true,
            },
        }
    }
}

fn localization_xbox_language(game_id: &str, profile: &str) -> Option<u32> {
    match (game_id, profile) {
        (_, "en") => Some(1),
        ("cot", "de") => Some(3),
        ("cot", "fr") => Some(4),
        ("cot", "es") => Some(5),
        ("cot", "it") => Some(6),
        ("cot", "nl") => Some(16),
        ("cot", "pt-BR") => Some(1),
        _ => None,
    }
}

fn native_localization_profile(game_id: &str, profile: &str) -> bool {
    match game_id {
        "cot" => matches!(profile, "en" | "de" | "fr" | "es" | "it" | "nl"),
        _ => profile == "en",
    }
}

fn language_component_id(game_id: &str, profile: &str) -> String {
    format!("language.{game_id}.{}", profile.to_ascii_lowercase())
}

fn valid_dynamic_localization_profile(profile: &str) -> bool {
    let bytes = profile.as_bytes();
    (2..=35).contains(&bytes.len())
        && bytes[0].is_ascii_alphanumeric()
        && bytes[bytes.len() - 1].is_ascii_alphanumeric()
        && bytes
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || *byte == b'-')
}

fn validate_localization_settings(
    game_id: &str,
    localization: &LocalizationSettings,
) -> Result<(), String> {
    if let Some(expected) = localization_xbox_language(game_id, &localization.profile) {
        if localization.xbox_language != expected {
            return Err("Localization profile and Xbox language do not match".into());
        }
        return Ok(());
    }
    if game_id != "cot"
        || !valid_dynamic_localization_profile(&localization.profile)
        || localization.xbox_language == 0
        || localization.xbox_language > 255
    {
        return Err(format!(
            "Unsupported localization profile: {}",
            localization.profile
        ));
    }
    Ok(())
}

#[derive(Serialize)]
struct ProcessStatus {
    running: bool,
    pid: Option<u32>,
    exit_code: Option<i32>,
}

#[derive(Serialize)]
struct ValidationResult {
    valid: bool,
    missing: Vec<String>,
}

#[derive(Clone, Serialize)]
struct GameSetupProgress {
    game_id: String,
    stage: String,
    progress: u8,
    detail: String,
    files_done: u64,
    files_total: u64,
    bytes_done: u64,
    bytes_total: u64,
}

#[derive(Serialize)]
struct GameSetupResult {
    game_root: String,
    files: u64,
    bytes: u64,
}

#[derive(Serialize)]
struct LauncherStorageStatus {
    configured: bool,
    library_path: String,
    default_library_path: String,
    existing_library_detected: bool,
    available_bytes: u64,
    discord_activity_enabled: bool,
    language_setup_completed_games: Vec<String>,
    notice: Option<String>,
}

#[derive(Serialize)]
struct LibraryPathStatus {
    path: String,
    available_bytes: u64,
    required_free_bytes: u64,
    valid: bool,
    enough_space: bool,
    error: Option<String>,
}

#[derive(Clone, Serialize)]
struct LibraryMigrationProgress {
    stage: String,
    progress: u8,
    detail: String,
    bytes_done: u64,
    bytes_total: u64,
}

#[derive(Clone, Serialize)]
struct LocalizationProgress {
    game_id: String,
    profile: String,
    stage: String,
    progress: u8,
    detail: String,
    bytes_done: u64,
    bytes_total: u64,
}

#[derive(Clone, Serialize)]
struct ComponentUpdateProgress {
    component_id: String,
    stage: String,
    progress: u8,
    detail: String,
    bytes_done: u64,
    bytes_total: u64,
}

fn progress_percent(done: u64, total: u64, when_empty: u8, cap: u8) -> u8 {
    done.saturating_mul(100)
        .checked_div(total)
        .map(|percent| percent.min(u64::from(cap)) as u8)
        .unwrap_or(when_empty)
}

#[derive(Clone, Serialize)]
struct ComponentReleaseStatus {
    version: String,
    published: String,
    notes_url: String,
    size: u64,
    downloadable: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RuntimeHistoryManifest {
    schema_version: u32,
    #[serde(rename = "runtime")]
    runtimes: Vec<RuntimeHistoryEntry>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RuntimeHistoryEntry {
    id: String,
    version: String,
    published: String,
    notes_url: String,
}

fn archived_runtime_releases(component_id: &str) -> Result<Vec<ComponentReleaseStatus>, String> {
    let manifest: RuntimeHistoryManifest = toml::from_str(RUNTIME_HISTORY_MANIFEST)
        .map_err(|error| format!("Runtime history metadata is invalid: {error}"))?;
    if manifest.schema_version != 1 {
        return Err(format!(
            "Unsupported runtime history schema version: {}",
            manifest.schema_version
        ));
    }

    let mut seen = HashSet::new();
    let mut releases = Vec::new();
    for entry in manifest
        .runtimes
        .into_iter()
        .filter(|entry| entry.id == component_id)
    {
        semver::Version::parse(&entry.version)
            .map_err(|_| format!("Archived runtime {} has an invalid version", entry.version))?;
        let published = entry
            .published
            .split('-')
            .collect::<Vec<_>>();
        if published.len() != 3
            || published[0].len() != 4
            || published[1].len() != 2
            || published[2].len() != 2
            || published.iter().any(|part| !part.chars().all(|ch| ch.is_ascii_digit()))
        {
            return Err(format!(
                "Archived runtime {} has an invalid release date",
                entry.version
            ));
        }
        let notes_url = reqwest::Url::parse(&entry.notes_url)
            .map_err(|_| format!("Archived runtime {} has an invalid notes URL", entry.version))?;
        if notes_url.scheme() != "https" || notes_url.host_str() != Some("github.com") {
            return Err(format!(
                "Archived runtime {} must use an HTTPS GitHub notes URL",
                entry.version
            ));
        }
        if !seen.insert(entry.version.clone()) {
            return Err(format!("Duplicate archived runtime version: {}", entry.version));
        }
        releases.push(ComponentReleaseStatus {
            version: entry.version,
            published: entry.published,
            notes_url: entry.notes_url,
            size: 0,
            downloadable: false,
        });
    }
    releases.sort_by(|left, right| {
        semver::Version::parse(&right.version)
            .ok()
            .cmp(&semver::Version::parse(&left.version).ok())
    });
    Ok(releases)
}

fn merge_archived_runtime_releases(
    releases: &mut Vec<ComponentReleaseStatus>,
    component_id: &str,
) -> Result<(), String> {
    for archived in archived_runtime_releases(component_id)? {
        if releases.iter().any(|release| release.version == archived.version) {
            continue;
        }
        releases.push(archived);
    }
    releases.sort_by(|left, right| {
        semver::Version::parse(&right.version)
            .ok()
            .cmp(&semver::Version::parse(&left.version).ok())
    });
    Ok(())
}

fn merge_manifest_runtime_release(
    releases: &mut Vec<ComponentReleaseStatus>,
    game: &GameManifest,
) -> Result<(), String> {
    semver::Version::parse(&game.runtime_version).map_err(|_| {
        format!(
            "Embedded {} runtime version is invalid: {}",
            game.name, game.runtime_version
        )
    })?;
    if releases
        .iter()
        .any(|release| release.version == game.runtime_version)
    {
        return Ok(());
    }
    releases.push(ComponentReleaseStatus {
        version: game.runtime_version.clone(),
        published: String::new(),
        notes_url: String::new(),
        size: 0,
        downloadable: false,
    });
    releases.sort_by(|left, right| {
        semver::Version::parse(&right.version)
            .ok()
            .cmp(&semver::Version::parse(&left.version).ok())
    });
    Ok(())
}

fn local_runtime_releases(
    game: &GameManifest,
    component_id: &str,
) -> Result<Vec<ComponentReleaseStatus>, String> {
    let mut releases = archived_runtime_releases(component_id)?;
    merge_manifest_runtime_release(&mut releases, game)?;
    Ok(releases)
}

#[derive(Clone, Serialize)]
struct InstalledComponentVersionStatus {
    version: String,
    healthy: bool,
}

#[derive(Clone, Serialize)]
struct ComponentUpdateStatus {
    id: String,
    kind: String,
    game_id: Option<String>,
    locale: Option<String>,
    display_name: Option<String>,
    xbox_language: Option<u32>,
    translation_version: Option<String>,
    installed_version: Option<String>,
    latest_version: Option<String>,
    state: String,
    download_url: Option<String>,
    size: Option<u64>,
    published: Option<String>,
    notes_url: Option<String>,
    last_action: Option<String>,
    can_rollback: bool,
    releases: Vec<ComponentReleaseStatus>,
    installed_versions: Vec<InstalledComponentVersionStatus>,
}

#[derive(Serialize)]
struct UpdateOverview {
    configured: bool,
    components: Vec<ComponentUpdateStatus>,
    error: Option<String>,
}

#[derive(Serialize)]
struct ComponentUpdateResult {
    component_id: String,
    state: String,
    restart_required: bool,
}

#[derive(Clone, Serialize)]
struct RuntimeAdditionalLanguageStatus {
    id: String,
    locale: String,
    display_name: String,
    version: String,
    translation_version: Option<String>,
    installed_version: Option<String>,
}

#[derive(Clone, Serialize)]
struct RuntimeAdditionalContentStatus {
    game_id: String,
    runtime_version: String,
    pack_version: String,
    pack_size: u64,
    languages: Vec<RuntimeAdditionalLanguageStatus>,
}

#[derive(Serialize)]
struct LocalizationPackInstallResult {
    game_id: String,
    version: String,
    installed_languages: Vec<String>,
}

struct AppState {
    manifests: Vec<GameManifest>,
    processes: Mutex<HashMap<String, Child>>,
    setup_jobs: Mutex<HashMap<String, bool>>,
    localization_jobs: Mutex<HashMap<String, Arc<AtomicBool>>>,
    library_job: Mutex<bool>,
    component_update_job: Mutex<bool>,
    operation_gate: Mutex<()>,
    runtime_job: RuntimeJob,
    presence: presence::PresenceController,
}

fn terminate_all_processes(processes: &Mutex<HashMap<String, Child>>) {
    if let Ok(mut processes) = processes.lock() {
        for (_, child) in processes.iter_mut() {
            let _ = child.kill();
            let _ = child.wait();
        }
        processes.clear();
    }
}

fn app_root() -> Result<PathBuf, String> {
    Ok(storage_layout()?.local_root().to_path_buf())
}

fn local_app_root() -> Result<PathBuf, String> {
    app_root()
}

fn launcher_root() -> Result<PathBuf, String> {
    local_app_root()
}

fn title_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.title_data_root(game_id))
}

fn settings_path(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.settings_path(game_id))
}

fn save_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.save_root(game_id))
}

fn content_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.content_root(game_id))
}

fn cache_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.cache_root(game_id))
}

fn utility_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.utility_root(game_id))
}

fn launcher_log_path() -> Result<PathBuf, String> {
    Ok(launcher_root()?.join("logs").join("launcher.log"))
}

fn game_log_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(storage_layout()?.game_logs_root(game_id))
}

fn move_file_preserving_existing(source: &Path, destination: &Path) {
    if !source.is_file() {
        return;
    }
    if let Some(parent) = destination.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let target = if destination.exists() {
        let stem = destination
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or("legacy");
        let extension = destination
            .extension()
            .and_then(|value| value.to_str())
            .map(|value| format!(".{value}"))
            .unwrap_or_default();
        let timestamp = timestamp_seconds();
        let mut sequence = 0_u32;
        loop {
            let suffix = if sequence == 0 {
                timestamp.to_string()
            } else {
                format!("{timestamp}-{sequence}")
            };
            let candidate =
                destination.with_file_name(format!("{stem}-legacy-{suffix}{extension}"));
            if !candidate.exists() {
                break candidate;
            }
            sequence = sequence.saturating_add(1);
        }
    } else {
        destination.to_path_buf()
    };

    if let Err(error) = move_file_verified(source, &target) {
        append_launcher_log(&format!(
            "Legacy data migration retained {} after an error: {error}",
            source.display()
        ));
    }
}

fn move_directory_contents_preserving_existing(source: &Path, destination: &Path) {
    let Ok(entries) = fs::read_dir(source) else {
        return;
    };
    for entry in entries.flatten() {
        let source_path = entry.path();
        let destination_path = destination.join(entry.file_name());
        let Ok(file_type) = entry.file_type() else {
            continue;
        };
        if file_type.is_symlink() {
            continue;
        }
        if file_type.is_dir() {
            move_directory_contents_preserving_existing(&source_path, &destination_path);
            remove_directory_if_empty(&source_path);
        } else if file_type.is_file() {
            move_file_preserving_existing(&source_path, &destination_path);
        }
    }
    remove_directory_if_empty(source);
}

fn migrate_legacy_diagnostics() {
    let Ok(layout) = storage_layout() else {
        return;
    };
    let portable_root = layout.legacy_portable_root();
    let old_launcher = portable_root.join("launcher");
    move_file_preserving_existing(
        &old_launcher.join("launcher.log"),
        &launcher_log_path().unwrap_or_else(|_| old_launcher.join("launcher.log")),
    );
    let old_support = old_launcher.join("support");
    if let Ok(entries) = fs::read_dir(&old_support) {
        let destination = launcher_root().ok().map(|root| root.join("support"));
        if let Some(destination) = destination {
            for entry in entries.flatten() {
                let source = entry.path();
                if source.is_file() {
                    move_file_preserving_existing(&source, &destination.join(entry.file_name()));
                }
            }
        }
    }
    remove_directory_if_empty(&old_support);
    remove_directory_if_empty(&old_launcher);

    for game_id in ["cot", "mom"] {
        let old_title = portable_root.join(game_id);
        move_file_preserving_existing(
            &old_title.join("settings.toml"),
            &layout.settings_path(game_id),
        );
        move_directory_contents_preserving_existing(
            &old_title.join("save"),
            &layout.save_root(game_id),
        );
        move_directory_contents_preserving_existing(
            &old_title.join("content"),
            &layout.content_root(game_id),
        );
        move_directory_contents_preserving_existing(
            &old_title.join("cache"),
            &layout.cache_root(game_id),
        );
        move_directory_contents_preserving_existing(
            &old_title.join("utility"),
            &layout.utility_root(game_id),
        );
        let old_logs = portable_root.join(game_id).join("logs");
        let Ok(new_logs) = game_log_root(game_id) else {
            continue;
        };
        if let Ok(entries) = fs::read_dir(&old_logs) {
            for entry in entries.flatten() {
                let source = entry.path();
                if source.is_file() {
                    move_file_preserving_existing(&source, &new_logs.join(entry.file_name()));
                }
            }
        }
        remove_directory_if_empty(&old_logs);
        remove_directory_if_empty(&old_title);
    }
    remove_directory_if_empty(&portable_root);

    if let Some(local_base) = std::env::var_os("LOCALAPPDATA").map(PathBuf::from) {
        let old_launcher_root = local_base.join("MojoRecomp-Launcher");
        move_directory_contents_preserving_existing(
            &old_launcher_root.join("logs"),
            &layout.logs_root(),
        );
        move_directory_contents_preserving_existing(
            &old_launcher_root.join("support"),
            &layout.support_root(),
        );
        remove_directory_if_empty(&old_launcher_root);
    }
}

fn executable_root() -> Result<PathBuf, String> {
    let exe = std::env::current_exe()
        .map_err(|e| format!("Could not resolve launcher executable: {e}"))?;
    exe.parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| "Launcher executable has no parent directory".to_string())
}

fn storage_layout() -> Result<StorageLayout, String> {
    StorageLayout::discover(executable_root()?)
}

fn launcher_component_store() -> Result<updates::ComponentStore, String> {
    Ok(updates::ComponentStore::new(
        storage_layout()?.components_root(),
    ))
}

fn game_component_store() -> Result<updates::ComponentStore, String> {
    let managed_root = managed_games_root()?.join(".mojorecomp");
    Ok(updates::ComponentStore::with_roots(
        managed_root.join("components"),
        managed_root.join("downloads"),
        managed_root.join("staging").join("components"),
    ))
}

fn runtime_component_is_ready(
    store: &updates::ComponentStore,
    component_id: &str,
) -> Result<bool, String> {
    if component_id == "runtime.cot" {
        return store.active_runtime_is_ready(
            component_id,
            updates::COT_RUNTIME_ENTRYPOINT,
            &updates::COT_RUNTIME_REQUIRED_FILES,
        );
    }
    Ok(store
        .active_status(component_id)?
        .is_some_and(|status| status.healthy))
}

#[derive(Default)]
struct LegacyComponentMigration {
    migrated: Vec<String>,
    deferred: Vec<(String, String)>,
}

fn migrate_known_game_components(
    legacy: &updates::ComponentStore,
    destination: &updates::ComponentStore,
) -> LegacyComponentMigration {
    let mut report = LegacyComponentMigration::default();
    let components: [(&str, &[&str]); 3] = [
        ("runtime.cot", &updates::COT_RUNTIME_REQUIRED_FILES),
        ("runtime.mom", &[]),
        ("language.cot.pt-br", &[]),
    ];
    for (id, required_files) in components {
        match destination.migrate_component_from(legacy, id, required_files) {
            Ok(true) => report.migrated.push(id.to_string()),
            Ok(false) => {}
            Err(error) => report.deferred.push((id.to_string(), error)),
        }
    }
    report
}

fn migrate_legacy_game_components() -> Result<LegacyComponentMigration, String> {
    let layout = storage_layout()?;
    let legacy = updates::ComponentStore::new(layout.components_root());
    let destination = game_component_store()?;
    Ok(migrate_known_game_components(&legacy, &destination))
}

fn migrate_legacy_embedded_runtime_cache(runtime_version: &str) -> Result<bool, String> {
    let store = game_component_store()?;
    if runtime_component_is_ready(&store, "runtime.cot")? {
        return Ok(false);
    }
    let legacy_base = storage_layout()?.local_root().join("runtime");
    if !legacy_base.is_dir() {
        return Ok(false);
    }
    let embedded_payloads: [&[u8]; 6] = [
        EMBEDDED_COT_RUNTIME,
        EMBEDDED_DXCOMPILER,
        EMBEDDED_DXIL,
        EMBEDDED_FFMPEG,
        EMBEDDED_LZX,
        EMBEDDED_EXTRACT_XISO,
    ];
    let expected = updates::COT_RUNTIME_REQUIRED_FILES
        .into_iter()
        .zip(embedded_payloads);
    for entry in fs::read_dir(&legacy_base)
        .map_err(|error| format!("Could not inspect legacy runtime cache: {error}"))?
    {
        let entry =
            entry.map_err(|error| format!("Could not inspect legacy runtime cache: {error}"))?;
        if !entry
            .file_type()
            .map_err(|error| format!("Could not inspect legacy runtime cache entry: {error}"))?
            .is_dir()
        {
            continue;
        }
        let root = entry.path();
        let matches = expected.clone().all(|(name, bytes)| {
            fs::read(root.join(name))
                .map(|current| current.as_slice() == bytes)
                .unwrap_or(false)
        });
        if !matches {
            continue;
        }
        return store.import_migrated_runtime(
            "runtime.cot",
            runtime_version,
            updates::COT_RUNTIME_ENTRYPOINT,
            &root,
            &updates::COT_RUNTIME_REQUIRED_FILES,
        );
    }
    Ok(false)
}

fn cleanup_legacy_runtime_cache_if_ready() -> Result<bool, String> {
    let store = game_component_store()?;
    if !runtime_component_is_ready(&store, "runtime.cot")? {
        return Ok(false);
    }
    let legacy_root = storage_layout()?.local_root().join("runtime");
    if !legacy_root.is_dir() {
        return Ok(false);
    }
    fs::remove_dir_all(&legacy_root)
        .map_err(|error| format!("Could not remove validated legacy runtime cache: {error}"))?;
    Ok(true)
}

fn cleanup_legacy_component_storage_if_ready() -> Result<Vec<String>, String> {
    let layout = storage_layout()?;
    let destination = game_component_store()?;
    let legacy_root = layout.components_root();
    let mut removed = Vec::new();
    for id in ["runtime.cot", "runtime.mom", "language.cot.pt-br"] {
        let ready = if id == "runtime.cot" {
            runtime_component_is_ready(&destination, id)?
        } else {
            destination
                .active_payload(id)?
                .is_some_and(|payload| match id {
                    "runtime.mom" => payload.kind == updates::ComponentKind::Runtime,
                    "language.cot.pt-br" => payload.kind == updates::ComponentKind::Language,
                    _ => false,
                })
        };
        if !ready {
            continue;
        }
        let legacy = legacy_root.join(id);
        if legacy.is_dir() {
            fs::remove_dir_all(&legacy).map_err(|error| {
                format!("Could not remove replaced legacy component {id}: {error}")
            })?;
            removed.push(id.to_string());
        }
    }
    Ok(removed)
}

fn directory_has_entries(path: &Path) -> bool {
    fs::read_dir(path)
        .map(|mut entries| entries.next().is_some())
        .unwrap_or(false)
}

fn effective_library_root(layout: &StorageLayout) -> Result<PathBuf, String> {
    if let Some(settings) = layout.load_launcher_settings()? {
        return Ok(settings.game_library);
    }
    let existing = layout.legacy_game_library();
    if directory_has_entries(&existing) {
        return Ok(existing);
    }
    Ok(layout.default_library_root())
}

fn managed_games_root() -> Result<PathBuf, String> {
    let layout = storage_layout()?;
    effective_library_root(&layout)
}

fn managed_game_root(game_id: &str) -> Result<PathBuf, String> {
    Ok(managed_games_root()?.join(game_id))
}

fn embedded_payload_root() -> Result<PathBuf, String> {
    Ok(storage_layout()?.runtime_root(env!("CARGO_PKG_VERSION")))
}

fn materialize_embedded_file(name: &str, bytes: &[u8]) -> Result<PathBuf, String> {
    let root = embedded_payload_root()?;
    materialize_embedded_file_at(&root, name, bytes)
}

fn materialize_embedded_file_at(root: &Path, name: &str, bytes: &[u8]) -> Result<PathBuf, String> {
    fs::create_dir_all(root)
        .map_err(|e| format!("Could not create embedded payload cache: {e}"))?;
    let path = root.join(name);
    let current_matches = fs::read(&path)
        .map(|current| current == bytes)
        .unwrap_or(false);
    if current_matches {
        return Ok(path);
    }

    let temp = root.join(format!(".{name}.{}.tmp", std::process::id()));
    fs::write(&temp, bytes)
        .map_err(|e| format!("Could not materialize embedded payload {name}: {e}"))?;
    if path.exists() {
        let _ = fs::remove_file(&path);
    }
    fs::rename(&temp, &path)
        .map_err(|e| format!("Could not activate embedded payload {name}: {e}"))?;
    Ok(path)
}

fn materialize_replaceable_library(name: &str, embedded: &[u8]) -> Result<PathBuf, String> {
    let layout = storage_layout()?;
    materialize_replaceable_library_at(
        &embedded_payload_root()?,
        &layout.local_root().join("lgpl-overrides"),
        name,
        embedded,
    )
}

fn materialize_replaceable_library_at(
    payload_root: &Path,
    override_root: &Path,
    name: &str,
    embedded: &[u8],
) -> Result<PathBuf, String> {
    let override_path = override_root.join(name);
    let bytes = match fs::read(&override_path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => embedded.to_vec(),
        Err(error) => {
            return Err(format!(
                "Could not read LGPL library override {}: {error}",
                override_path.display()
            ));
        }
    };
    materialize_embedded_file_at(payload_root, name, &bytes)
}

fn write_text_if_changed(path: &Path, text: &str) -> Result<(), String> {
    if fs::read_to_string(path)
        .map(|current| current == text)
        .unwrap_or(false)
    {
        return Ok(());
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Could not create notice directory: {e}"))?;
    }
    fs::write(path, text).map_err(|e| format!("Could not write {}: {e}", path.display()))
}

fn materialize_license_notices(root: &Path) -> Result<PathBuf, String> {
    fs::create_dir_all(root)
        .map_err(|e| format!("Could not create license/notice directory: {e}"))?;
    write_text_if_changed(&root.join("LICENSE"), PROJECT_LICENSE)?;
    let summary = root.join("THIRD_PARTY_NOTICES.md");
    write_text_if_changed(&summary, THIRD_PARTY_NOTICE_SUMMARY)?;
    for (name, text) in THIRD_PARTY_LICENSES {
        write_text_if_changed(&root.join(name), text)?;
    }
    Ok(summary)
}

fn ensure_embedded_runtime_payloads() -> Result<(PathBuf, PathBuf, PathBuf), String> {
    let runtime = materialize_embedded_file("cot-runtime.exe", EMBEDDED_COT_RUNTIME)?;
    let dxc = materialize_embedded_file("dxcompiler.dll", EMBEDDED_DXCOMPILER)?;
    let dxil = materialize_embedded_file("dxil.dll", EMBEDDED_DXIL)?;
    materialize_replaceable_library("mojorecomp-ffmpeg.dll", EMBEDDED_FFMPEG)?;
    materialize_replaceable_library("mojorecomp-lzx.dll", EMBEDDED_LZX)?;
    Ok((runtime, dxc, dxil))
}

fn embedded_cot_bootstrap_available() -> bool {
    [
        EMBEDDED_COT_RUNTIME,
        EMBEDDED_DXCOMPILER,
        EMBEDDED_DXIL,
        EMBEDDED_FFMPEG,
        EMBEDDED_LZX,
        EMBEDDED_EXTRACT_XISO,
    ]
    .iter()
    .all(|payload| !payload.is_empty())
}

fn prepared_cot_runtime_component() -> Result<Option<PathBuf>, String> {
    let store = game_component_store()?;
    let Some(status) = store.active_status("runtime.cot")? else {
        return Ok(None);
    };
    if !runtime_component_is_ready(&store, "runtime.cot")? {
        return Err(format!(
            "Installed runtime.cot {} is incomplete or corrupted",
            status.version
        ));
    }
    let layout = storage_layout()?;
    store.prepare_runtime_launch("runtime.cot", &layout.local_root().join("lgpl-overrides"))
}

fn extract_xiso_path() -> Result<PathBuf, String> {
    match prepared_cot_runtime_component() {
        Ok(Some(runtime)) => {
            if let Some(root) = runtime.parent() {
                let tool = root.join("extract-xiso.exe");
                if tool.is_file() {
                    return Ok(tool);
                }
            }
        }
        Ok(None) => {}
        Err(error) if embedded_cot_bootstrap_available() => {
            append_launcher_log(&format!(
                "COT runtime component is unusable during setup; using the transitional embedded bootstrap: {error}"
            ));
        }
        Err(error) => return Err(error),
    }
    if !embedded_cot_bootstrap_available() {
        return Err("The required COT runtime component is not installed or is corrupted".into());
    }
    materialize_embedded_file("extract-xiso.exe", EMBEDDED_EXTRACT_XISO)
}

fn emit_setup_progress(app: &tauri::AppHandle, progress: GameSetupProgress) {
    let _ = app.emit("game-setup-progress", progress);
}

fn emit_library_progress(app: &tauri::AppHandle, progress: LibraryMigrationProgress) {
    let _ = app.emit("library-migration-progress", progress);
}

fn emit_localization_progress(app: &tauri::AppHandle, progress: LocalizationProgress) {
    let _ = app.emit("localization-progress", progress);
}

fn emit_component_update_progress(app: &tauri::AppHandle, progress: ComponentUpdateProgress) {
    let _ = app.emit("component-update-progress", progress);
}

fn parse_iso_listing(text: &str) -> (u64, u64) {
    let mut files = 0u64;
    let mut bytes = 0u64;
    for raw in text.lines() {
        let line = raw.trim();
        let Some((path, size_text)) = line.rsplit_once(" (") else {
            continue;
        };
        let Some(size_text) = size_text.strip_suffix(" bytes)") else {
            continue;
        };
        let Ok(size) = size_text.parse::<u64>() else {
            continue;
        };
        if !path.ends_with('\\') {
            files += 1;
            bytes = bytes.saturating_add(size);
        }
    }
    (files, bytes)
}

fn directory_stats(root: &Path) -> Result<(u64, u64), String> {
    fn walk(path: &Path, files: &mut u64, bytes: &mut u64) -> Result<(), String> {
        if !path.exists() {
            return Ok(());
        }
        for entry in fs::read_dir(path)
            .map_err(|e| format!("Could not inspect extraction directory: {e}"))?
        {
            let entry = entry.map_err(|e| format!("Could not inspect extracted file: {e}"))?;
            let kind = entry.file_type().map_err(|e| e.to_string())?;
            if kind.is_symlink() {
                return Err(format!(
                    "Extraction produced a symbolic link: {}",
                    entry.path().display()
                ));
            }
            if kind.is_dir() {
                walk(&entry.path(), files, bytes)?;
            } else if kind.is_file() {
                let metadata = entry.metadata().map_err(|e| e.to_string())?;
                *files += 1;
                *bytes = bytes.saturating_add(metadata.len());
            }
        }
        Ok(())
    }

    let mut files = 0;
    let mut bytes = 0;
    walk(root, &mut files, &mut bytes)?;
    Ok((files, bytes))
}

fn remove_managed_directory(path: &Path, managed_root: &Path) -> Result<(), String> {
    if !path.starts_with(managed_root) {
        return Err(format!(
            "Refusing to remove path outside managed game library: {}",
            path.display()
        ));
    }
    if path.exists() {
        fs::remove_dir_all(path)
            .map_err(|e| format!("Could not clean managed directory {}: {e}", path.display()))?;
    }
    Ok(())
}

fn remove_directory_if_empty(path: &Path) {
    let is_empty = fs::read_dir(path)
        .map(|mut entries| entries.next().is_none())
        .unwrap_or(false);
    if is_empty {
        let _ = fs::remove_dir(path);
    }
}

fn cleanup_setup_staging(
    staging: &Path,
    staging_root: &Path,
    extraction_log: &Path,
    games_root: &Path,
) {
    let _ = remove_managed_directory(staging, games_root);
    let _ = fs::remove_file(extraction_log);
    remove_directory_if_empty(staging_root);
}

fn cleanup_library_transients(root: &Path) -> Result<(), String> {
    for name in [".staging", ".backup"] {
        let path = root.join(name);
        if path.exists() {
            remove_managed_directory(&path, root)?;
        }
    }
    let managed_staging = root.join(".mojorecomp").join("staging").join("games");
    if managed_staging.exists() {
        remove_managed_directory(&managed_staging, root)?;
    }
    for path in [
        root.join(".mojorecomp").join("downloads"),
        root.join(".mojorecomp").join("staging").join("components"),
    ] {
        if path.exists() {
            remove_managed_directory(&path, root)?;
        }
    }
    let managed_root = root.join(".mojorecomp");
    remove_directory_if_empty(&managed_root.join("staging"));
    remove_directory_if_empty(&managed_root.join("components"));
    remove_directory_if_empty(&managed_root);
    Ok(())
}

fn cleanup_game_transients(root: &Path, game_id: &str) -> Result<(), String> {
    let staging_root = root.join(".mojorecomp").join("staging").join("games");
    remove_managed_directory(&staging_root.join(game_id), root)?;
    let _ = fs::remove_file(staging_root.join(format!("{game_id}-extract-xiso.log")));
    remove_directory_if_empty(&staging_root);

    let backup_root = root.join(".backup");
    if backup_root.is_dir() {
        let prefix = format!("{game_id}-");
        for entry in fs::read_dir(&backup_root)
            .map_err(|error| format!("Could not inspect managed game backups: {error}"))?
        {
            let entry =
                entry.map_err(|error| format!("Could not inspect managed game backup: {error}"))?;
            if entry.file_name().to_string_lossy().starts_with(&prefix) {
                remove_managed_directory(&entry.path(), root)?;
            }
        }
    }
    remove_directory_if_empty(&backup_root);
    Ok(())
}

fn timestamp_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0)
}

fn utc_timestamp_iso8601() -> String {
    let seconds = timestamp_seconds() as i64;
    let days = seconds.div_euclid(86_400);
    let day_seconds = seconds.rem_euclid(86_400);
    let hour = day_seconds / 3_600;
    let minute = (day_seconds % 3_600) / 60;
    let second = day_seconds % 60;

    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let day_of_era = z - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096)
            / 365;
    let mut year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_prime = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    let month = month_prime + if month_prime < 10 { 3 } else { -9 };
    if month <= 2 {
        year += 1;
    }
    format!(
        "{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}:{second:02}Z"
    )
}

fn configured_discord_application_id(suite: &SuiteManifest) -> Option<String> {
    match std::env::var("MOJORECOMP_DISCORD_APPLICATION_ID") {
        Ok(value) => presence::valid_application_id(&value),
        Err(_) => presence::valid_application_id(&suite.discord_application_id),
    }
}

fn append_launcher_log(message: &str) {
    let Ok(path) = launcher_log_path() else {
        return;
    };
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(file, "[{}] {}", timestamp_seconds(), message);
    }
}

fn sanitize_text(mut text: String, game_root: Option<&Path>) -> String {
    if let Some(root) = game_root {
        text = text.replace(&root.to_string_lossy().to_string(), "<GAME_ROOT>");
    }
    if let Ok(root) = app_root() {
        text = text.replace(&root.to_string_lossy().to_string(), "<APP_DATA>");
    }
    if let Ok(root) = local_app_root() {
        text = text.replace(&root.to_string_lossy().to_string(), "<LOCAL_APP_DATA>");
    }
    if let Ok(layout) = storage_layout() {
        text = text.replace(
            &layout.saved_games_root().to_string_lossy().to_string(),
            "<SAVED_GAMES>",
        );
    }
    if let Some(profile) = std::env::var_os("USERPROFILE") {
        text = text.replace(
            &PathBuf::from(profile).to_string_lossy().to_string(),
            "<USER_PROFILE>",
        );
    }
    text
}

fn add_text_to_zip(zip: &mut ZipWriter<File>, name: &str, text: &str) -> Result<(), String> {
    let options = SimpleFileOptions::default()
        .compression_method(CompressionMethod::Deflated)
        .unix_permissions(0o644);
    zip.start_file(name, options)
        .map_err(|e| format!("Could not create ZIP entry {name}: {e}"))?;
    zip.write_all(text.as_bytes())
        .map_err(|e| format!("Could not write ZIP entry {name}: {e}"))
}

const SUPPORT_BINARY_FILE_LIMIT: u64 = 256 * 1024 * 1024;

fn add_binary_file_to_zip(
    zip: &mut ZipWriter<File>,
    name: &str,
    path: &Path,
    max_bytes: u64,
) -> Result<(), String> {
    let metadata = fs::symlink_metadata(path)
        .map_err(|error| format!("Could not inspect support artifact {name}: {error}"))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(format!("Support artifact {name} is not a regular file"));
    }
    if metadata.len() == 0 || metadata.len() > max_bytes {
        return Err(format!(
            "Support artifact {name} has an unsafe size: {} bytes",
            metadata.len()
        ));
    }
    let options = SimpleFileOptions::default()
        .compression_method(CompressionMethod::Deflated)
        .unix_permissions(0o644);
    zip.start_file(name, options)
        .map_err(|error| format!("Could not create ZIP entry {name}: {error}"))?;
    let mut source = File::open(path)
        .map_err(|error| format!("Could not open support artifact {name}: {error}"))?;
    std::io::copy(&mut source, zip)
        .map_err(|error| format!("Could not stream support artifact {name}: {error}"))?;
    Ok(())
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = File::open(path)
        .map_err(|error| format!("Could not open file for SHA-256: {error}"))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|error| format!("Could not hash file: {error}"))?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct CrashIncident {
    stem: String,
    log: PathBuf,
    json: Option<PathBuf>,
    dump: Option<PathBuf>,
}

fn newest_crash_incident(logs: &Path) -> Option<CrashIncident> {
    let mut candidates = fs::read_dir(logs)
        .ok()?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| {
            path.is_file()
                && path
                    .file_name()
                    .and_then(|name| name.to_str())
                    .map(|name| name.starts_with("crash-") && name.ends_with(".log"))
                    .unwrap_or(false)
        })
        .collect::<Vec<_>>();
    candidates.sort_by_key(|path| {
        fs::metadata(path)
            .and_then(|metadata| metadata.modified())
            .unwrap_or(UNIX_EPOCH)
    });
    let log = candidates.pop()?;
    let stem = log.file_stem()?.to_str()?.to_string();
    let json = log.with_extension("json");
    let dump = log.with_extension("dmp");
    Some(CrashIncident {
        stem,
        log,
        json: json.is_file().then_some(json),
        dump: dump.is_file().then_some(dump),
    })
}

fn manifest<'a>(state: &'a AppState, game_id: &str) -> Result<&'a GameManifest, String> {
    state
        .manifests
        .iter()
        .find(|m| m.id == game_id)
        .ok_or_else(|| format!("Unknown game profile: {game_id}"))
}

fn validate_root(manifest: &GameManifest, root: &Path) -> ValidationResult {
    let missing = manifest
        .files
        .required
        .iter()
        .filter(|name| !root.join(name).is_file())
        .cloned()
        .collect::<Vec<_>>();
    ValidationResult {
        valid: missing.is_empty(),
        missing,
    }
}

fn validate_integrity_entry(root: &Path, expected: &FileIntegrity) -> Result<(), String> {
    let relative = Path::new(&expected.path);
    if relative.is_absolute()
        || relative
            .components()
            .any(|part| !matches!(part, std::path::Component::Normal(_)))
    {
        return Err(format!(
            "Invalid integrity path in the game profile: {}",
            expected.path
        ));
    }
    if expected.sha256.len() != 64 || !expected.sha256.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return Err(format!(
            "Invalid SHA-256 value in the game profile: {}",
            expected.path
        ));
    }

    let path = root.join(relative);
    let metadata = fs::metadata(&path)
        .map_err(|_| format!("Required game file is missing: {}", expected.path))?;
    if !metadata.is_file() {
        return Err(format!("Required game file is missing: {}", expected.path));
    }
    if metadata.len() != expected.size {
        return Err(format!(
            "Unsupported or damaged game file: {} has the wrong size",
            expected.path
        ));
    }

    let mut file =
        File::open(&path).map_err(|error| format!("Could not read {}: {error}", expected.path))?;
    let mut digest = Sha256::new();
    let mut buffer = vec![0u8; 1024 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|error| format!("Could not read {}: {error}", expected.path))?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    let actual = format!("{:x}", digest.finalize());
    if !actual.eq_ignore_ascii_case(&expected.sha256) {
        return Err(format!(
            "Unsupported or damaged game file: {} does not match the supported release",
            expected.path
        ));
    }
    Ok(())
}

fn validate_install_integrity(manifest: &GameManifest, root: &Path) -> Result<(), String> {
    for expected in &manifest.files.integrity {
        validate_integrity_entry(root, expected)?;
    }
    Ok(())
}

fn validate_launch_integrity(manifest: &GameManifest, root: &Path) -> Result<(), String> {
    for expected in manifest
        .files
        .integrity
        .iter()
        .filter(|entry| entry.verify_on_launch)
    {
        validate_integrity_entry(root, expected)?;
    }
    Ok(())
}

fn resolved_game_root(manifest: &GameManifest) -> Option<PathBuf> {
    managed_game_root(&manifest.id)
        .ok()
        .filter(|path| validate_root(manifest, path).valid)
}

fn available_space_for(path: &Path) -> u64 {
    let mut candidate = path;
    loop {
        if candidate.exists() {
            return fs2::available_space(candidate).unwrap_or(0);
        }
        let Some(parent) = candidate.parent() else {
            return 0;
        };
        candidate = parent;
    }
}

fn minimum_library_free_bytes(manifests: &[GameManifest]) -> u64 {
    let known_game_bytes = manifests
        .iter()
        .filter(|manifest| manifest.playable)
        .map(|manifest| {
            manifest
                .files
                .integrity
                .iter()
                .fold(0u64, |total, entry| total.saturating_add(entry.size))
        })
        .max()
        .unwrap_or(0);
    known_game_bytes.saturating_add(DISK_MINIMUM_MARGIN_BYTES)
}

fn inspect_library_path(
    path: &str,
    manifests: &[GameManifest],
) -> Result<LibraryPathStatus, String> {
    let required_free_bytes = minimum_library_free_bytes(manifests);
    if path.trim().is_empty() {
        return Ok(LibraryPathStatus {
            path: String::new(),
            available_bytes: 0,
            required_free_bytes,
            valid: false,
            enough_space: false,
            error: Some("Choose a game library location".into()),
        });
    }

    let layout = storage_layout()?;
    match layout.normalize_game_library(Path::new(path.trim())) {
        Ok(destination) => {
            let available_bytes = available_space_for(&destination);
            Ok(LibraryPathStatus {
                path: destination.to_string_lossy().to_string(),
                available_bytes,
                required_free_bytes,
                valid: true,
                enough_space: available_bytes >= required_free_bytes,
                error: None,
            })
        }
        Err(error) => Ok(LibraryPathStatus {
            path: path.trim().to_string(),
            available_bytes: 0,
            required_free_bytes,
            valid: false,
            enough_space: false,
            error: Some(error),
        }),
    }
}

fn launcher_storage_status() -> Result<LauncherStorageStatus, String> {
    let layout = storage_layout()?;
    let launcher_settings = layout.load_launcher_settings()?;
    let configured = launcher_settings.is_some();
    let discord_activity_enabled = launcher_settings
        .as_ref()
        .map(|settings| settings.discord_activity_enabled)
        .unwrap_or(true);
    let language_setup_completed_games = launcher_settings
        .as_ref()
        .map(|settings| settings.language_setup_completed_games.clone())
        .unwrap_or_default();
    let library = effective_library_root(&layout)?;
    let existing_library_detected =
        !configured && directory_has_entries(&layout.legacy_game_library());
    Ok(LauncherStorageStatus {
        configured,
        library_path: library.to_string_lossy().to_string(),
        default_library_path: layout.default_library_root().to_string_lossy().to_string(),
        existing_library_detected,
        available_bytes: available_space_for(&library),
        discord_activity_enabled,
        language_setup_completed_games,
        notice: None,
    })
}

#[tauri::command]
fn get_launcher_storage() -> Result<LauncherStorageStatus, String> {
    launcher_storage_status()
}

#[tauri::command]
fn complete_game_language_setup(
    game_id: String,
    profile: String,
    xbox_language: u32,
    state: tauri::State<'_, AppState>,
) -> Result<LauncherStorageStatus, String> {
    manifest(&state, &game_id)?;
    let localization = LocalizationSettings {
        profile,
        xbox_language,
    };
    validate_localization_settings(&game_id, &localization)?;

    let path = settings_path(&game_id)?;
    let mut settings = if path.is_file() {
        let text = fs::read_to_string(&path)
            .map_err(|error| format!("Could not read settings: {error}"))?;
        toml::from_str::<RuntimeSettings>(&text)
            .map_err(|error| format!("Could not parse settings: {error}"))?
    } else {
        RuntimeSettings::default()
    };
    settings.localization = localization;
    validate_localization_settings(&game_id, &settings.localization)?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Could not create title data directory: {error}"))?;
    }
    let text = toml::to_string_pretty(&settings)
        .map_err(|error| format!("Could not serialize settings: {error}"))?;
    fs::write(&path, text).map_err(|error| format!("Could not save settings: {error}"))?;

    let layout = storage_layout()?;
    let mut launcher_settings = layout
        .load_launcher_settings()?
        .ok_or_else(|| "Finish launcher setup before choosing a game language".to_string())?;
    if !launcher_settings
        .language_setup_completed_games
        .iter()
        .any(|value| value == &game_id)
    {
        launcher_settings
            .language_setup_completed_games
            .push(game_id.clone());
        launcher_settings.language_setup_completed_games.sort();
        launcher_settings.language_setup_completed_games.dedup();
        layout.save_launcher_settings(&launcher_settings)?;
    }
    append_launcher_log(&format!(
        "Completed initial language selection for {game_id}: {}",
        settings.localization.profile
    ));
    launcher_storage_status()
}

#[tauri::command]
fn set_discord_activity_enabled(
    enabled: bool,
    state: tauri::State<'_, AppState>,
) -> Result<bool, String> {
    let layout = storage_layout()?;
    let mut settings = layout
        .load_launcher_settings()?
        .ok_or_else(|| "Finish launcher setup before changing Discord activity".to_string())?;
    settings.discord_activity_enabled = enabled;
    layout.save_launcher_settings(&settings)?;
    state.presence.set_enabled(enabled);
    Ok(enabled)
}

#[tauri::command]
fn inspect_game_library(
    path: String,
    state: tauri::State<'_, AppState>,
) -> Result<LibraryPathStatus, String> {
    inspect_library_path(&path, &state.manifests)
}

fn ensure_library_change_is_idle(state: &AppState) -> Result<(), String> {
    {
        let mut processes = state
            .processes
            .lock()
            .map_err(|_| "Process state lock failed")?;
        for child in processes.values_mut() {
            if child
                .try_wait()
                .map_err(|error| error.to_string())?
                .is_none()
            {
                return Err("Close every running game before changing the game library".into());
            }
        }
        processes.clear();
    }
    if state
        .setup_jobs
        .lock()
        .map_err(|_| "Game setup state lock failed")?
        .values()
        .any(|running| *running)
    {
        return Err("Wait for game setup to finish before changing the game library".into());
    }
    if !state
        .localization_jobs
        .lock()
        .map_err(|_| "Localization state lock failed")?
        .is_empty()
    {
        return Err("Wait for localization work to finish before changing the game library".into());
    }
    Ok(())
}

fn ensure_library_idle(state: &AppState) -> Result<(), String> {
    if *state
        .library_job
        .lock()
        .map_err(|_| "Library migration state lock failed")?
    {
        return Err("Wait for the game library operation to finish".into());
    }
    Ok(())
}

fn ensure_component_update_idle(state: &AppState) -> Result<(), String> {
    if *state
        .component_update_job
        .lock()
        .map_err(|_| "Component update state lock failed")?
    {
        return Err("Wait for the component update to finish".into());
    }
    Ok(())
}

#[tauri::command]
async fn set_game_library(
    path: String,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<LauncherStorageStatus, String> {
    let operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_component_update_idle(&state)?;
    ensure_library_change_is_idle(&state)?;
    let layout = storage_layout()?;
    let source = effective_library_root(&layout)?;
    let destination = layout.normalize_game_library(Path::new(&path))?;
    let required_free_bytes = minimum_library_free_bytes(&state.manifests);
    let available_bytes = available_space_for(&destination);
    if available_bytes < required_free_bytes {
        return Err(format!(
            "Not enough free space for the game library: {} bytes available, {} bytes required",
            available_bytes, required_free_bytes
        ));
    }
    {
        let mut running = state
            .library_job
            .lock()
            .map_err(|_| "Library migration state lock failed")?;
        if *running {
            return Err("A game library operation is already running".into());
        }
        *running = true;
    }
    drop(operation_gate);
    let worker_app = app.clone();

    let worker = tauri::async_runtime::spawn_blocking(move || {
        let mut source_to_clean = None;
        let mut completed_plan = None;
        let mut migration_performed = false;
        if source == destination {
            initialize_library(&destination)?;
        } else {
            cleanup_library_transients(&source)?;
            if library_has_persistent_data(&source)? {
                emit_library_progress(
                    &worker_app,
                    LibraryMigrationProgress {
                        stage: "planning".into(),
                        progress: 0,
                        detail: "Checking the installed game library and available disk space...".into(),
                        bytes_done: 0,
                        bytes_total: 0,
                    },
                );
                let plan = plan_library_migration(&source, &destination)?;
                let total = plan.bytes;
                emit_library_progress(
                    &worker_app,
                    LibraryMigrationProgress {
                        stage: "moving".into(),
                        progress: 1,
                        detail: "Copying and verifying the game library...".into(),
                        bytes_done: 0,
                        bytes_total: total,
                    },
                );
                layout.begin_library_migration(&plan)?;
                let outcome = match execute_library_migration(&plan, |done, total| {
                    let progress = progress_percent(done, total, 100, 99);
                    emit_library_progress(
                        &worker_app,
                        LibraryMigrationProgress {
                            stage: "moving".into(),
                            progress,
                            detail: "Copying and verifying game files...".into(),
                            bytes_done: done,
                            bytes_total: total,
                        },
                    );
                }) {
                    Ok(outcome) => outcome,
                    Err(error) => {
                        match rollback_library_migration(&plan) {
                            Ok(()) => layout.clear_library_migration_journal(),
                            Err(cleanup_error) => {
                                return Err(format!(
                                    "{error}. Migration cleanup will be retried next launch: {cleanup_error}"
                                ));
                            }
                        }
                        return Err(error);
                    }
                };
                source_to_clean = outcome.source_to_clean;
                completed_plan = Some(plan);
                migration_performed = true;
            } else {
                if destination.exists()
                    && library_has_persistent_data(&destination)?
                    && !destination.join(storage::LIBRARY_MARKER).is_file()
                {
                    return Err(
                        "Select an empty folder or an existing MojoRecomp game library".into(),
                    );
                }
                initialize_library(&destination)?;
                if source.exists() {
                    source_to_clean = Some(source.clone());
                }
            }
        }
        let mut launcher_settings = layout
            .load_launcher_settings()?
            .unwrap_or_else(|| LauncherSettings::new(destination.clone()));
        launcher_settings.game_library = destination.clone();
        if let Err(settings_error) = layout.save_launcher_settings(&launcher_settings) {
            if let Some(plan) = completed_plan {
                if let Err(rollback_error) = rollback_library_migration(&plan) {
                    return Err(format!(
                        "Could not save the new library location ({settings_error}). Rollback also failed ({rollback_error}); verified data remains at {}",
                        plan.destination.display()
                    ));
                }
                layout.clear_library_migration_journal();
            }
            return Err(format!(
                "Could not save the new library location; the move was rolled back: {settings_error}"
            ));
        }
        let notice = source_to_clean.and_then(|source| {
            fs::remove_dir_all(&source).err().map(|error| {
                format!(
                    "The verified new library is active, but some files could not be removed from {}: {error}. Close any program using the old library; cleanup will be retried on the next launcher start",
                    source.display()
                )
            })
        });
        if notice.is_none() {
            layout.clear_library_migration_journal();
        }
        Ok::<_, String>((notice, migration_performed))
    })
    .await;

    if let Ok(mut running) = state.library_job.lock() {
        *running = false;
    }
    let result = worker.map_err(|error| format!("Game library worker failed: {error}"))?;
    match result {
        Ok((notice, migration_performed)) => {
            if migration_performed {
                emit_library_progress(
                    &app,
                    LibraryMigrationProgress {
                        stage: "complete".into(),
                        progress: 100,
                        detail: notice
                            .clone()
                            .unwrap_or_else(|| "Game library moved and verified.".into()),
                        bytes_done: 0,
                        bytes_total: 0,
                    },
                );
            }
            append_launcher_log("Game library location updated");
            if let Some(detail) = notice.as_deref() {
                append_launcher_log(detail);
            }
            let mut status = launcher_storage_status()?;
            status.notice = notice;
            Ok(status)
        }
        Err(error) => {
            emit_library_progress(
                &app,
                LibraryMigrationProgress {
                    stage: "failed".into(),
                    progress: 0,
                    detail: error.clone(),
                    bytes_done: 0,
                    bytes_total: 0,
                },
            );
            Err(error)
        }
    }
}

#[tauri::command]
fn open_game_library() -> Result<(), String> {
    let path = managed_games_root()?;
    initialize_library(&path)?;
    Command::new("explorer.exe")
        .arg(path)
        .spawn()
        .map_err(|error| format!("Could not open game library: {error}"))?;
    Ok(())
}

fn runtime_path(manifest: &GameManifest) -> Result<PathBuf, String> {
    if manifest.id == "cot" {
        match prepared_cot_runtime_component() {
            Ok(Some(runtime)) => return Ok(runtime),
            Ok(None) => {}
            Err(error) if embedded_cot_bootstrap_available() => {
                append_launcher_log(&format!(
                    "COT runtime component is unusable; using the transitional embedded bootstrap: {error}"
                ));
            }
            Err(error) => return Err(error),
        }
        if !embedded_cot_bootstrap_available() {
            return Err(
                "The required COT runtime component is not installed or is corrupted".into(),
            );
        }
        return ensure_embedded_runtime_payloads().map(|(runtime, _, _)| runtime);
    }
    Err(format!(
        "Runtime executable not found for {}",
        manifest.name
    ))
}

fn run_hardware_probe(game: &GameManifest) -> Result<serde_json::Value, String> {
    let runtime = runtime_path(game)?;
    let mut command = Command::new(runtime);
    apply_runtime_compatibility_environment(&mut command);
    command
        .args(["--probe-hardware", "--json"])
        .stdin(Stdio::null());
    hide_child_console(&mut command);
    let output = command
        .output()
        .map_err(|e| format!("Could not start hardware probe: {e}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_string());
    }
    serde_json::from_slice(&output.stdout)
        .map_err(|e| format!("Hardware probe returned invalid JSON: {e}"))
}

#[tauri::command]
fn list_games(state: tauri::State<'_, AppState>) -> Result<Vec<GameInfo>, String> {
    let store = game_component_store().ok();
    Ok(state
        .manifests
        .iter()
        .map(|m| {
            let component_id = format!("runtime.{}", m.id);
            let runtime_version = store
                .as_ref()
                .and_then(|store| store.active_status(&component_id).ok().flatten())
                .map(|status| status.version)
                .unwrap_or_else(|| m.runtime_version.clone());
            let resolved_root = resolved_game_root(m);
            let root = resolved_root
                .as_ref()
                .map(|path| path.to_string_lossy().to_string());
            let installed = root
                .as_deref()
                .map(Path::new)
                .map(|path| validate_root(m, path).valid)
                .unwrap_or(false);
            let managed = root
                .as_deref()
                .map(Path::new)
                .zip(managed_game_root(&m.id).ok())
                .map(|(root, managed_root)| root == managed_root)
                .unwrap_or(false);
            GameInfo {
                id: m.id.clone(),
                name: m.name.clone(),
                status: m.status.clone(),
                runtime_version,
                installed,
                game_root: root,
                managed,
                playable: m.playable,
                capabilities: m.capabilities.clone(),
            }
        })
        .collect())
}

fn import_game_iso_blocking(
    game: GameManifest,
    iso_path: PathBuf,
    app: tauri::AppHandle,
) -> Result<GameSetupResult, String> {
    if !game.playable {
        return Err(format!("{} is not playable yet", game.name));
    }
    if !iso_path.is_file() {
        return Err(format!("ISO file does not exist: {}", iso_path.display()));
    }
    if iso_path
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| !value.eq_ignore_ascii_case("iso"))
        .unwrap_or(true)
    {
        return Err("Selected file is not an .iso image".into());
    }

    let tool = extract_xiso_path()?;
    let games_root = managed_games_root()?;
    let staging_root = games_root.join(".mojorecomp").join("staging").join("games");
    let staging = staging_root.join(&game.id);
    let extraction_log = staging_root.join(format!("{}-extract-xiso.log", game.id));
    let destination = games_root.join(&game.id);
    fs::create_dir_all(&staging_root)
        .map_err(|e| format!("Could not create managed staging directory: {e}"))?;
    remove_managed_directory(&staging, &games_root)?;
    fs::create_dir_all(&staging)
        .map_err(|e| format!("Could not create extraction staging directory: {e}"))?;

    emit_setup_progress(
        &app,
        GameSetupProgress {
            game_id: game.id.clone(),
            stage: "analyzing".into(),
            progress: 2,
            detail: "Analyzing ISO contents...".into(),
            files_done: 0,
            files_total: 0,
            bytes_done: 0,
            bytes_total: 0,
        },
    );

    let mut listing_command = Command::new(&tool);
    listing_command
        .arg("-l")
        .arg(&iso_path)
        .stdin(Stdio::null());
    hide_child_console(&mut listing_command);
    let listing = listing_command
        .output()
        .map_err(|e| format!("Could not analyze ISO with extract-xiso: {e}"))?;
    if !listing.status.success() {
        let detail = String::from_utf8_lossy(&listing.stderr).trim().to_string();
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err(if detail.is_empty() {
            "extract-xiso could not read the selected ISO".into()
        } else {
            format!("extract-xiso could not read the selected ISO: {detail}")
        });
    }

    let mut listing_text = String::from_utf8_lossy(&listing.stdout).to_string();
    if !listing.stderr.is_empty() {
        listing_text.push_str(&String::from_utf8_lossy(&listing.stderr));
    }
    let (expected_files, expected_bytes) = parse_iso_listing(&listing_text);
    if expected_files == 0 || expected_bytes == 0 {
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err("The selected ISO does not contain a readable Xbox filesystem".into());
    }
    let disk_margin_bytes = (expected_bytes / 10).max(DISK_MINIMUM_MARGIN_BYTES);
    let available_bytes = fs2::available_space(&staging_root)
        .map_err(|e| format!("Could not query free disk space for managed game library: {e}"))?;
    let required_bytes = expected_bytes.saturating_add(disk_margin_bytes);
    if available_bytes < required_bytes {
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err(format!(
            "Not enough free disk space for managed extraction: {} bytes available, {} bytes required",
            available_bytes, required_bytes
        ));
    }
    let normalized_listing = listing_text.replace('/', "\\").to_ascii_lowercase();
    let missing_from_iso = game
        .files
        .required
        .iter()
        .filter(|name| {
            let normalized = name.replace('/', "\\").to_ascii_lowercase();
            let needle = format!("\\{normalized} (");
            !normalized_listing.contains(&needle)
        })
        .cloned()
        .collect::<Vec<_>>();
    if !missing_from_iso.is_empty() {
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err(format!(
            "The selected ISO does not match the required game profile. Missing: {}",
            missing_from_iso.join(", ")
        ));
    }

    emit_setup_progress(
        &app,
        GameSetupProgress {
            game_id: game.id.clone(),
            stage: "extracting".into(),
            progress: 5,
            detail: format!("Extracting {expected_files} files..."),
            files_done: 0,
            files_total: expected_files,
            bytes_done: 0,
            bytes_total: expected_bytes,
        },
    );

    let log = File::create(&extraction_log)
        .map_err(|e| format!("Could not create extract-xiso log: {e}"))?;
    let log_err = log
        .try_clone()
        .map_err(|e| format!("Could not duplicate extract-xiso log handle: {e}"))?;
    let mut extract_command = Command::new(&tool);
    extract_command
        .arg("-d")
        .arg(&staging)
        .arg(&iso_path)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log))
        .stderr(Stdio::from(log_err));
    hide_child_console(&mut extract_command);
    let mut child = extract_command
        .spawn()
        .map_err(|e| format!("Could not start extract-xiso: {e}"))?;

    loop {
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            if !status.success() {
                let detail = fs::read_to_string(&extraction_log)
                    .unwrap_or_default()
                    .lines()
                    .rev()
                    .find(|line| !line.trim().is_empty())
                    .unwrap_or("extract-xiso failed")
                    .to_string();
                cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
                emit_setup_progress(
                    &app,
                    GameSetupProgress {
                        game_id: game.id.clone(),
                        stage: "failed".into(),
                        progress: 0,
                        detail: detail.clone(),
                        files_done: 0,
                        files_total: expected_files,
                        bytes_done: 0,
                        bytes_total: expected_bytes,
                    },
                );
                return Err(format!("Game extraction failed: {detail}"));
            }
            break;
        }

        let (files_done, bytes_done) = match directory_stats(&staging) {
            Ok(stats) => stats,
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
                return Err(error);
            }
        };
        let remaining_bytes = expected_bytes
            .saturating_sub(bytes_done)
            .saturating_add(disk_margin_bytes);
        if fs2::available_space(&staging_root).unwrap_or(0) < remaining_bytes {
            let _ = child.kill();
            let _ = child.wait();
            cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
            return Err(
                "Game setup stopped because the destination drive ran out of safe working space"
                    .into(),
            );
        }
        let ratio = if expected_bytes == 0 {
            0.0
        } else {
            (bytes_done as f64 / expected_bytes as f64).clamp(0.0, 1.0)
        };
        let progress = 5u8.saturating_add((ratio * 85.0).round() as u8).min(90);
        emit_setup_progress(
            &app,
            GameSetupProgress {
                game_id: game.id.clone(),
                stage: "extracting".into(),
                progress,
                detail: format!("Extracting files... {files_done}/{expected_files}"),
                files_done,
                files_total: expected_files,
                bytes_done,
                bytes_total: expected_bytes,
            },
        );
        thread::sleep(Duration::from_millis(250));
    }

    emit_setup_progress(
        &app,
        GameSetupProgress {
            game_id: game.id.clone(),
            stage: "validating".into(),
            progress: 94,
            detail: "Validating extracted game files...".into(),
            files_done: expected_files,
            files_total: expected_files,
            bytes_done: expected_bytes,
            bytes_total: expected_bytes,
        },
    );

    let validation = validate_root(&game, &staging);
    if !validation.valid {
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err(format!(
            "Extracted game data is incomplete: {}",
            validation.missing.join(", ")
        ));
    }
    let (actual_files, actual_bytes) = directory_stats(&staging)?;
    if actual_files != expected_files || actual_bytes != expected_bytes {
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err(format!(
            "Extraction verification failed: expected {expected_files} files / {expected_bytes} bytes, got {actual_files} files / {actual_bytes} bytes"
        ));
    }
    if let Err(error) = validate_install_integrity(&game, &staging) {
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        return Err(error);
    }

    emit_setup_progress(
        &app,
        GameSetupProgress {
            game_id: game.id.clone(),
            stage: "installing".into(),
            progress: 97,
            detail: "Promoting validated game files...".into(),
            files_done: actual_files,
            files_total: expected_files,
            bytes_done: actual_bytes,
            bytes_total: expected_bytes,
        },
    );

    let backup_root = games_root.join(".backup");
    fs::create_dir_all(&backup_root)
        .map_err(|e| format!("Could not create managed game backup directory: {e}"))?;
    let backup = backup_root.join(format!("{}-{}", game.id, timestamp_seconds()));
    let had_existing = destination.exists();
    if had_existing {
        fs::rename(&destination, &backup)
            .map_err(|e| format!("Could not back up previous managed game installation: {e}"))?;
    }
    if let Err(error) = fs::rename(&staging, &destination) {
        if had_existing && backup.exists() && !destination.exists() {
            let _ = fs::rename(&backup, &destination);
        }
        cleanup_setup_staging(&staging, &staging_root, &extraction_log, &games_root);
        remove_directory_if_empty(&backup_root);
        return Err(format!(
            "Could not promote validated game installation: {error}"
        ));
    }

    if had_existing && backup.exists() {
        let _ = fs::remove_dir_all(&backup);
    }
    let _ = fs::remove_file(&extraction_log);
    remove_directory_if_empty(&staging_root);
    remove_directory_if_empty(&backup_root);
    append_launcher_log(&format!(
        "Imported {} from ISO into managed library ({} files, {} bytes)",
        game.id, actual_files, actual_bytes
    ));

    emit_setup_progress(
        &app,
        GameSetupProgress {
            game_id: game.id.clone(),
            stage: "complete".into(),
            progress: 100,
            detail: "Game setup complete. Ready to play.".into(),
            files_done: actual_files,
            files_total: expected_files,
            bytes_done: actual_bytes,
            bytes_total: expected_bytes,
        },
    );

    Ok(GameSetupResult {
        game_root: destination.to_string_lossy().to_string(),
        files: actual_files,
        bytes: actual_bytes,
    })
}

#[tauri::command]
async fn import_game_iso(
    game_id: String,
    iso_path: String,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<GameSetupResult, String> {
    let game = manifest(&state, &game_id)?.clone();
    if game.playable {
        let component_id = format!("runtime.{}", game.id);
        let store = game_component_store()?;
        if !runtime_component_is_ready(&store, &component_id)? {
            return Err(
                "The required game runtime is not installed. Open Versions and install a runtime before setting up the game."
                    .into(),
            );
        }
    }

    let operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_component_update_idle(&state)?;
    ensure_library_idle(&state)?;
    {
        let jobs = state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?;
        if jobs.contains_key(&game_id) {
            return Err("Wait for localization work to finish before setting up the game".into());
        }
    }
    {
        let mut jobs = state
            .setup_jobs
            .lock()
            .map_err(|_| "Game setup state lock failed")?;
        if jobs.values().copied().any(|running| running) {
            return Err("Another game setup operation is already running".into());
        }
        jobs.insert(game_id.clone(), true);
    }
    drop(operation_gate);

    let job_game_id = game_id.clone();
    let failure_app = app.clone();
    let worker = tauri::async_runtime::spawn_blocking(move || {
        import_game_iso_blocking(game, PathBuf::from(iso_path), app)
    })
    .await;

    if let Ok(mut jobs) = state.setup_jobs.lock() {
        jobs.remove(&job_game_id);
    }
    let result = worker.map_err(|e| format!("Game setup worker failed: {e}"))?;
    if let Err(error) = &result {
        emit_setup_progress(
            &failure_app,
            GameSetupProgress {
                game_id: job_game_id,
                stage: "failed".into(),
                progress: 0,
                detail: error.clone(),
                files_done: 0,
                files_total: 0,
                bytes_done: 0,
                bytes_total: 0,
            },
        );
    }
    result
}

#[tauri::command]
fn uninstall_game(game_id: String, state: tauri::State<'_, AppState>) -> Result<(), String> {
    let _operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_component_update_idle(&state)?;
    ensure_library_idle(&state)?;
    let game = manifest(&state, &game_id)?.clone();
    if !game.playable {
        return Err(format!("{} is not playable yet", game.name));
    }

    let status = inspect_process_status(&game_id, &state.processes)?;
    if status.running {
        return Err("Close the game before uninstalling it".into());
    }

    let jobs = state
        .setup_jobs
        .lock()
        .map_err(|_| "Game setup state lock failed")?;
    if jobs.get(&game_id).copied().unwrap_or(false) {
        return Err("Wait for game setup to finish before uninstalling".into());
    }
    drop(jobs);
    {
        let jobs = state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?;
        if jobs.contains_key(&game_id) {
            return Err("Wait for localization work to finish before uninstalling".into());
        }
    }

    let games_root = managed_games_root()?;
    let game_root = managed_game_root(&game_id)?;
    if game_root.parent() != Some(games_root.as_path()) {
        return Err("Refusing to uninstall a path outside the managed game library".into());
    }

    remove_managed_directory(&game_root, &games_root)?;
    cleanup_game_transients(&games_root, &game_id)?;
    append_launcher_log(&format!(
        "Uninstalled {} from the game library; saves and local user data were preserved",
        game_id
    ));
    Ok(())
}

#[tauri::command]
fn load_settings(
    game_id: String,
    state: tauri::State<'_, AppState>,
) -> Result<RuntimeSettings, String> {
    manifest(&state, &game_id)?;
    let path = settings_path(&game_id)?;
    if !path.is_file() {
        return Ok(RuntimeSettings::default());
    }
    let text = fs::read_to_string(path).map_err(|e| format!("Could not read settings: {e}"))?;
    let settings: RuntimeSettings =
        toml::from_str(&text).map_err(|e| format!("Could not parse settings: {e}"))?;
    validate_localization_settings(&game_id, &settings.localization)?;
    Ok(settings)
}

#[tauri::command]
fn save_settings(
    game_id: String,
    settings: RuntimeSettings,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    manifest(&state, &game_id)?;
    if settings.schema_version != 1 || !(1..=3).contains(&settings.display.resolution_scale) {
        return Err("Unsupported settings schema or resolution scale".into());
    }
    if !matches!(settings.graphics.frame_rate.as_str(), "30" | "60") {
        return Err("Frame rate must be 30 or 60".into());
    }
    validate_localization_settings(&game_id, &settings.localization)?;
    let path = settings_path(&game_id)?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Could not create title data directory: {e}"))?;
    }
    let text = toml::to_string_pretty(&settings)
        .map_err(|e| format!("Could not serialize settings: {e}"))?;
    fs::write(path, text).map_err(|e| format!("Could not save settings: {e}"))
}

fn localization_context(
    game_id: &str,
    state: &tauri::State<'_, AppState>,
) -> Result<(PathBuf, PathBuf), String> {
    if game_id != "cot" {
        return Err("Localization profiles are not available for this title".into());
    }
    let game = manifest(state, game_id)?;
    let root = resolved_game_root(game)
        .ok_or_else(|| "Game must be set up before localization can be used".to_string())?;
    let validation = validate_root(game, &root);
    if !validation.valid {
        return Err(format!(
            "Game data is incomplete: {}",
            validation.missing.join(", ")
        ));
    }
    Ok((root.join("default.rcf"), root))
}

fn active_language_component(
    game_id: &str,
    profile: &str,
) -> Result<Option<updates::ActiveComponentPayload>, String> {
    let component_id = language_component_id(game_id, profile);
    let Some(component) = game_component_store()?.active_payload(&component_id)? else {
        return Ok(None);
    };
    if component.kind != updates::ComponentKind::Language {
        return Err(format!(
            "The active {profile} component is not a language component"
        ));
    }
    Ok(Some(component))
}

fn localization_status_with_component(
    game_id: &str,
    profile: &str,
    archive: &Path,
    game_root: &Path,
) -> Result<localization::LocalizationStatus, String> {
    let mut status = localization::status(archive, game_root, profile);
    let active = active_language_component(game_id, profile)?;
    if let Some(component) = active {
        let component_source_current = localization::uses_component_source(game_root, profile)
            && localization::component_source_version(game_root, profile).as_deref()
                == Some(component.version.as_str());
        let manual_source = status.source_installed
            && !(profile == localization::PT_BR_PROFILE
                && localization::uses_bundled_source(game_root))
            && !localization::uses_component_source(game_root, profile);
        if !manual_source && !component_source_current {
            status.overlay_ready = false;
            status.detail = "A downloaded Localization Pack component is ready to install.".into();
        }
    } else if localization::uses_component_source(game_root, profile) {
        status.overlay_ready = false;
        status.detail =
            "The downloaded Localization Pack component was rolled back and needs to be restored."
                .into();
    }
    Ok(status)
}

#[tauri::command]
fn localization_status(
    game_id: String,
    profile: Option<String>,
    state: tauri::State<'_, AppState>,
) -> Result<localization::LocalizationStatus, String> {
    let profile = match profile {
        Some(profile) => profile,
        None => {
            load_settings(game_id.clone(), state.clone())?
                .localization
                .profile
        }
    };
    if !native_localization_profile(&game_id, &profile)
        && !valid_dynamic_localization_profile(&profile)
    {
        return Err(format!("Unsupported localization profile: {profile}"));
    }
    if native_localization_profile(&game_id, &profile) {
        return Ok(localization::LocalizationStatus {
            profile,
            source_installed: true,
            overlay_ready: true,
            detail: "Original game language selected.".into(),
        });
    }
    let (archive, game_root) = localization_context(&game_id, &state)?;
    localization_status_with_component(&game_id, &profile, &archive, &game_root)
}

#[tauri::command]
async fn prepare_localization(
    game_id: String,
    profile: String,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<localization::LocalizationStatus, String> {
    let operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_component_update_idle(&state)?;
    ensure_library_idle(&state)?;
    if native_localization_profile(&game_id, &profile) {
        return Err("Original game languages do not require Localization Pack preparation".into());
    }
    let process = inspect_process_status(&game_id, &state.processes)?;
    if process.running {
        return Err("Close the game before preparing localization resources".into());
    }
    {
        let jobs = state
            .setup_jobs
            .lock()
            .map_err(|_| "Game setup state lock failed")?;
        if jobs.get(&game_id).copied().unwrap_or(false) {
            return Err(
                "Wait for game setup to finish before preparing localization resources".into(),
            );
        }
    }
    let (archive, game_root) = localization_context(&game_id, &state)?;
    let current_status = localization::status(&archive, &game_root, &profile);
    let active_language_component = active_language_component(&game_id, &profile)?;
    let needs_component_source = active_language_component.as_ref().is_some_and(|component| {
        !current_status.source_installed
            || (profile == localization::PT_BR_PROFILE
                && localization::uses_bundled_source(&game_root))
            || (localization::uses_component_source(&game_root, &profile)
                && localization::component_source_version(&game_root, &profile).as_deref()
                    != Some(component.version.as_str()))
    });
    let cancel = Arc::new(AtomicBool::new(false));
    {
        let mut jobs = state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?;
        if jobs.contains_key(&game_id) {
            return Err("Localization resources are already being prepared for this title".into());
        }
        jobs.insert(game_id.clone(), cancel.clone());
    }
    drop(operation_gate);

    emit_localization_progress(
        &app,
        LocalizationProgress {
            game_id: game_id.clone(),
            profile: profile.clone(),
            stage: "preparing".into(),
            progress: 0,
            detail: "Preparing Localization Pack...".into(),
            bytes_done: 0,
            bytes_total: 0,
        },
    );

    let worker_game_id = game_id.clone();
    let worker_profile = profile.clone();
    let worker_app = app.clone();
    let worker_game_root = game_root.clone();
    let worker_language_component = active_language_component.clone();
    let worker = tauri::async_runtime::spawn_blocking(move || {
        if needs_component_source {
            let component = worker_language_component
                .as_ref()
                .ok_or_else(|| "The selected language component is no longer active".to_string())?;
            emit_localization_progress(
                &worker_app,
                LocalizationProgress {
                    game_id: worker_game_id.clone(),
                    profile: worker_profile.clone(),
                    stage: "importing".into(),
                    progress: 0,
                    detail: "Installing downloaded Localization Pack resources...".into(),
                    bytes_done: 0,
                    bytes_total: 0,
                },
            );
            localization::install_component_patch(
                &component.root,
                &language_component_id(&worker_game_id, &worker_profile),
                &component.version,
                &worker_profile,
                &worker_game_root,
            )?;
        }
        let overlay = localization::prepare_overlay(
            &archive,
            &worker_game_root,
            &worker_profile,
            |done, total| {
                if cancel.load(Ordering::Relaxed) {
                    return Err("Localization preparation cancelled".into());
                }
                let progress = progress_percent(done, total, 0, 99);
                emit_localization_progress(
                    &worker_app,
                    LocalizationProgress {
                        game_id: worker_game_id.clone(),
                        profile: worker_profile.clone(),
                        stage: "building".into(),
                        progress,
                        detail: "Building localization archive...".into(),
                        bytes_done: done,
                        bytes_total: total,
                    },
                );
                Ok(())
            },
        )?;
        if !overlay.is_file() {
            return Err("Localization preparation completed without a derived archive".into());
        }
        Ok::<_, String>(localization::status(
            &archive,
            &worker_game_root,
            &worker_profile,
        ))
    })
    .await;

    if let Ok(mut jobs) = state.localization_jobs.lock() {
        jobs.remove(&game_id);
    }
    let result =
        worker.map_err(|error| format!("Localization preparation worker failed: {error}"))?;
    match &result {
        Ok(status) => emit_localization_progress(
            &app,
            LocalizationProgress {
                game_id,
                profile,
                stage: "complete".into(),
                progress: 100,
                detail: status.detail.clone(),
                bytes_done: 0,
                bytes_total: 0,
            },
        ),
        Err(error) => emit_localization_progress(
            &app,
            LocalizationProgress {
                game_id,
                profile,
                stage: if error == "Localization preparation cancelled" {
                    "cancelled".into()
                } else {
                    "failed".into()
                },
                progress: 0,
                detail: error.clone(),
                bytes_done: 0,
                bytes_total: 0,
            },
        ),
    }
    result
}

#[tauri::command]
fn cancel_localization(game_id: String, state: tauri::State<'_, AppState>) -> Result<(), String> {
    let jobs = state
        .localization_jobs
        .lock()
        .map_err(|_| "Localization state lock failed")?;
    if let Some(cancel) = jobs.get(&game_id) {
        cancel.store(true, Ordering::Relaxed);
    }
    Ok(())
}

#[tauri::command]
fn probe_hardware(
    game_id: String,
    state: tauri::State<'_, AppState>,
) -> Result<serde_json::Value, String> {
    let game = manifest(&state, &game_id)?;
    run_hardware_probe(game)
}

#[tauri::command]
fn launch_game(
    game_id: String,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<ProcessStatus, String> {
    let _operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_library_idle(&state)?;
    ensure_component_update_idle(&state)?;
    let game = manifest(&state, &game_id)?.clone();
    if !game.playable {
        return Err(format!("{} is not playable yet", game.name));
    }
    {
        let jobs = state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?;
        if jobs.contains_key(&game_id) {
            return Err("Wait for localization work to finish before launching the game".into());
        }
    }
    let root =
        resolved_game_root(&game).ok_or_else(|| "Game has not been imported yet".to_string())?;
    let validation = validate_root(&game, &root);
    if !validation.valid {
        return Err(format!(
            "Game data is incomplete: {}",
            validation.missing.join(", ")
        ));
    }
    validate_launch_integrity(&game, &root)?;

    let settings = settings_path(&game_id)?;
    if !settings.is_file() {
        save_settings(game_id.clone(), RuntimeSettings::default(), state.clone())?;
    }
    let runtime_settings = load_settings(game_id.clone(), state.clone())?;
    let localization_overlay =
        if !native_localization_profile(&game_id, &runtime_settings.localization.profile) {
            let original_archive = root.join("default.rcf");
            let status = localization_status_with_component(
                &game_id,
                &runtime_settings.localization.profile,
                &original_archive,
                &root,
            )?;
            if !status.overlay_ready {
                return Err(format!(
                    "The selected Localization Pack is not installed: {}",
                    status.detail
                ));
            }
            Some(localization::overlay_root(
                &root,
                &runtime_settings.localization.profile,
            ))
        } else {
            None
        };

    let mut processes = state
        .processes
        .lock()
        .map_err(|_| "Process state lock failed")?;
    if let Some(child) = processes.get_mut(&game_id)
        && child.try_wait().map_err(|e| e.to_string())?.is_none()
    {
        return Ok(ProcessStatus {
            running: true,
            pid: Some(child.id()),
            exit_code: None,
        });
    }
    processes.remove(&game_id);

    let runtime = runtime_path(&game)?;
    let runtime_sha256 = sha256_file(&runtime)?;
    let runtime_payload_dir = runtime
        .parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| "Embedded runtime path has no parent directory".to_string())?;
    let dxc = runtime_payload_dir.join("dxcompiler.dll");
    let mut command = Command::new(&runtime);
    apply_runtime_compatibility_environment(&mut command);
    command
        .arg("--cpu")
        .arg("--config")
        .arg(&settings)
        .arg("--game-root")
        .arg(&root)
        .env("MOJORECOMP_VULKAN_PRESENT", "1")
        .env("MOJORECOMP_SAVE_ROOT", save_root(&game_id)?)
        .env("MOJORECOMP_CONTENT_ROOT", content_root(&game_id)?)
        .env("MOJORECOMP_CACHE_ROOT", cache_root(&game_id)?)
        .env("MOJORECOMP_UTILITY_ROOT", utility_root(&game_id)?)
        .env("MOJORECOMP_LOG_ROOT", game_log_root(&game_id)?)
        .env("MOJORECOMP_RUNTIME_SHA256", &runtime_sha256)
        .env(
            "MOJORECOMP_FRAME_RATE",
            &runtime_settings.graphics.frame_rate,
        )
        .stdin(Stdio::null());
    if let Some(overlay) = localization_overlay {
        command.arg("--game-overlay").arg(overlay);
    }

    if dxc.is_file() {
        command.env("MOJORECOMP_DXC_LIB", &dxc);
        let existing_path = std::env::var_os("PATH").unwrap_or_default();
        let mut child_path = runtime_payload_dir.into_os_string();
        child_path.push(";");
        child_path.push(existing_path);
        command.env("PATH", child_path);
    }

    if runtime_settings.advanced.logging_enabled {
        let logs_dir = game_log_root(&game_id)?;
        fs::create_dir_all(&logs_dir)
            .map_err(|e| format!("Could not create runtime log directory: {e}"))?;
        let runtime_log_path = logs_dir.join("runtime.log");
        let mut runtime_log = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&runtime_log_path)
            .map_err(|e| format!("Could not open runtime log: {e}"))?;
        writeln!(runtime_log, "=== MojoRecomp Runtime Log v1 ===")
            .and_then(|_| writeln!(runtime_log, "session_unix={}", timestamp_seconds()))
            .and_then(|_| writeln!(runtime_log, "game_id={}", game.id))
            .and_then(|_| writeln!(runtime_log, "game_version={}", game.runtime_version))
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "launcher_version={}",
                    env!("CARGO_PKG_VERSION")
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "settings_schema={}",
                    runtime_settings.schema_version
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "localization_profile={}",
                    runtime_settings.localization.profile
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "xbox_language={}",
                    runtime_settings.localization.xbox_language
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "display_mode={}",
                    runtime_settings.display.mode
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "resolution_scale={}x",
                    runtime_settings.display.resolution_scale
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "aspect_ratio={}",
                    runtime_settings.display.aspect_ratio
                )
            })
            .and_then(|_| writeln!(runtime_log, "vsync={}", runtime_settings.display.vsync))
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "anti_aliasing={}",
                    runtime_settings.graphics.anti_aliasing
                )
            })
            .and_then(|_| {
                writeln!(
                    runtime_log,
                    "texture_filtering={}",
                    runtime_settings.graphics.texture_filtering
                )
            })
            .and_then(|_| writeln!(runtime_log, "--- runtime output ---"))
            .map_err(|e| format!("Could not write runtime log header: {e}"))?;
        let runtime_stdout = runtime_log
            .try_clone()
            .map_err(|e| format!("Could not duplicate runtime log handle: {e}"))?;
        command
            .stdout(Stdio::from(runtime_stdout))
            .stderr(Stdio::from(runtime_log));
    } else {
        command.stdout(Stdio::null()).stderr(Stdio::null());
    }
    hide_child_console(&mut command);
    if let Some(parent) = runtime.parent() {
        command.current_dir(parent);
    }
    let mut child = command
        .spawn()
        .map_err(|error| format!("Could not launch {}: {error}", game.name))?;
    if let Err(error) = state.runtime_job.assign(&child) {
        let _ = child.kill();
        let _ = child.wait();
        return Err(error);
    }
    let pid = child.id();
    append_launcher_log(&format!("Launched {} runtime with PID {}", game.id, pid));
    let is_cot = game_id == "cot";
    if is_cot {
        state.presence.cot_started(pid);
    }
    processes.insert(game_id, child);
    #[cfg(windows)]
    if is_cot {
        thread::spawn(move || {
            if wait_for_runtime_process_exit(pid) {
                let state = app.state::<AppState>();
                state.presence.cot_stopped(pid);
            }
        });
    }
    Ok(ProcessStatus {
        running: true,
        pid: Some(pid),
        exit_code: None,
    })
}

#[tauri::command]
fn process_status(
    game_id: String,
    state: tauri::State<'_, AppState>,
) -> Result<ProcessStatus, String> {
    let status = inspect_process_status(&game_id, &state.processes)?;
    if let Some(code) = status.exit_code {
        append_launcher_log(&format!("{} runtime exited with code {}", game_id, code));
    }
    Ok(status)
}

fn inspect_process_status(
    game_id: &str,
    processes: &Mutex<HashMap<String, Child>>,
) -> Result<ProcessStatus, String> {
    let mut processes = processes.lock().map_err(|_| "Process state lock failed")?;
    let mut finished = None;
    if let Some(child) = processes.get_mut(game_id) {
        match child.try_wait().map_err(|e| e.to_string())? {
            None => {
                return Ok(ProcessStatus {
                    running: true,
                    pid: Some(child.id()),
                    exit_code: None,
                });
            }
            Some(status) => finished = Some(status.code().unwrap_or(-1)),
        }
    }
    processes.remove(game_id);
    Ok(ProcessStatus {
        running: false,
        pid: None,
        exit_code: finished,
    })
}

#[tauri::command]
fn create_support_package(
    game_id: String,
    include_minidump: bool,
    state: tauri::State<'_, AppState>,
) -> Result<String, String> {
    let game = manifest(&state, &game_id)?.clone();
    let game_root = resolved_game_root(&game);
    let suite: SuiteManifest = toml::from_str(SUITE_MANIFEST)
        .map_err(|e| format!("Could not parse suite manifest: {e}"))?;

    let logs_dir = game_log_root(&game_id)?;
    let incident = newest_crash_incident(&logs_dir);
    let crash_json_text = incident
        .as_ref()
        .and_then(|incident| incident.json.as_ref())
        .and_then(|path| fs::read_to_string(path).ok());
    let crash_data = crash_json_text
        .as_deref()
        .and_then(|text| serde_json::from_str::<serde_json::Value>(text).ok());
    let dump_available = incident
        .as_ref()
        .and_then(|incident| incident.dump.as_ref())
        .is_some_and(|path| {
            fs::symlink_metadata(path).ok().is_some_and(|metadata| {
                metadata.is_file()
                    && !metadata.file_type().is_symlink()
                    && metadata.len() > 0
                    && metadata.len() <= SUPPORT_BINARY_FILE_LIMIT
            })
        });
    let dump_included = include_minidump && dump_available;

    let settings_file = settings_path(&game_id)?;
    let settings_text = fs::read_to_string(&settings_file).ok();
    let settings_value = settings_text
        .as_deref()
        .and_then(|text| toml::from_str::<RuntimeSettings>(text).ok())
        .and_then(|settings| serde_json::to_value(settings).ok());

    let hardware_result = run_hardware_probe(&game);
    let hardware_value = hardware_result.as_ref().ok();

    let mut os_version_command = Command::new("cmd.exe");
    os_version_command.args(["/C", "ver"]);
    hide_child_console(&mut os_version_command);
    let os_version = os_version_command
        .output()
        .ok()
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "Windows version unavailable".to_string());

    let runtime_file = runtime_path(&game).ok();
    let runtime_sha256 = runtime_file
        .as_deref()
        .and_then(|path| sha256_file(path).ok());
    let active_runtime_version = if game.id == "cot" {
        game_component_store()
            .ok()
            .and_then(|store| store.active_status("runtime.cot").ok().flatten())
            .map(|status| status.version)
            .unwrap_or_else(|| game.runtime_version.clone())
    } else {
        game.runtime_version.clone()
    };
    let build_id = crash_data
        .as_ref()
        .and_then(|value| value.get("build_id"))
        .cloned()
        .unwrap_or(serde_json::Value::Null);
    let crash_runtime_sha256 = crash_data
        .as_ref()
        .and_then(|value| value.get("runtime_sha256"))
        .cloned()
        .filter(|value| !value.is_null())
        .or_else(|| runtime_sha256.clone().map(serde_json::Value::String))
        .unwrap_or(serde_json::Value::Null);
    let last_vulkan_call = crash_data
        .as_ref()
        .and_then(|value| value.get("last_vulkan_call"))
        .cloned()
        .unwrap_or(serde_json::Value::Null);
    let classification = match last_vulkan_call.as_str() {
        Some("vkQueuePresentKHR") => serde_json::Value::String("renderer_presentation_failure".into()),
        Some(_) => serde_json::Value::String("renderer_vulkan_failure".into()),
        None => serde_json::Value::Null,
    };
    let incident_id = incident.as_ref().map(|incident| {
        format!("{}-{}", game.id, incident.stem.trim_start_matches("crash-"))
    });

    let hardware_field = |name: &str| {
        hardware_value
            .and_then(|value| value.get(name))
            .cloned()
            .unwrap_or(serde_json::Value::Null)
    };
    let settings_field = |pointer: &str| {
        settings_value
            .as_ref()
            .and_then(|value| value.pointer(pointer))
            .cloned()
            .unwrap_or(serde_json::Value::Null)
    };
    let crash_field = |name: &str| {
        crash_data
            .as_ref()
            .and_then(|value| value.get(name))
            .cloned()
            .unwrap_or(serde_json::Value::Null)
    };

    let summary = serde_json::json!({
        "schema_version": 2,
        "incident_id": incident_id,
        "created_at": utc_timestamp_iso8601(),
        "result": if incident.is_some() { "crash" } else { "manual_support_request" },
        "classification": classification,
        "game": {
            "id": game.id,
            "name": game.name,
        },
        "versions": {
            "suite": suite.version,
            "launcher": env!("CARGO_PKG_VERSION"),
            "runtime": active_runtime_version,
            "build_id": build_id,
            "runtime_sha256": crash_runtime_sha256,
        },
        "system": {
            "os": os_version,
            "os_build": serde_json::Value::Null,
            "architecture": std::env::consts::ARCH,
            "gpu": hardware_field("adapter"),
            "gpu_vendor_id": hardware_field("vendor_id"),
            "gpu_device_id": hardware_field("device_id"),
            "vulkan_api": hardware_field("vulkan_api"),
            "driver_version": serde_json::Value::Null,
        },
        "settings": {
            "display_mode": settings_field("/display/mode"),
            "output_resolution": settings_field("/display/output_resolution"),
            "resolution_scale": settings_field("/display/resolution_scale"),
            "aspect_ratio": settings_field("/display/aspect_ratio"),
            "vsync": settings_field("/display/vsync"),
            "anti_aliasing": settings_field("/graphics/anti_aliasing"),
            "texture_filtering": settings_field("/graphics/texture_filtering"),
        },
        "failure": {
            "exception_code": crash_field("exception_code"),
            "access": crash_field("access"),
            "invalid_address": crash_field("invalid_address"),
            "frame": crash_field("frame"),
            "thread_id": crash_field("thread_id"),
            "last_vulkan_call": last_vulkan_call,
            "vulkan_result": crash_field("vulkan_result_name"),
            "vulkan_result_code": crash_field("vulkan_result"),
            "vulkan_stage": crash_field("vulkan_stage"),
            "gpu_sequence": crash_field("gpu_sequence"),
            "faulting_module": crash_field("faulting_module"),
        },
        "artifacts": {
            "runtime_log": logs_dir.join("runtime.log").is_file(),
            "stutter_log": logs_dir.join("stutter.log").is_file(),
            "crash_log": incident.is_some(),
            "crash_json": incident.as_ref().is_some_and(|incident| incident.json.is_some()),
            "minidump": dump_included,
            "minidump_available": dump_available,
            "gpu_breadcrumbs": false,
        },
        "privacy": {
            "game_files_included": false,
            "save_contents_included": false,
            "personal_paths_sanitized": true,
            "minidump_requires_consent": true,
        }
    });

    let support_dir = storage_layout()?.support_root();
    fs::create_dir_all(&support_dir)
        .map_err(|e| format!("Could not create support directory: {e}"))?;
    let output_path = support_dir.join(format!(
        "mojorecomp-support-{}-{}.zip",
        game.id,
        timestamp_seconds()
    ));
    let file =
        File::create(&output_path).map_err(|e| format!("Could not create support package: {e}"))?;
    let mut zip = ZipWriter::new(file);

    add_text_to_zip(
        &mut zip,
        "summary.json",
        &serde_json::to_string_pretty(&summary).map_err(|error| error.to_string())?,
    )?;

    if let Ok(path) = launcher_log_path()
        && let Ok(text) = fs::read_to_string(path)
    {
        add_text_to_zip(
            &mut zip,
            "logs/launcher.log",
            &sanitize_text(text, game_root.as_deref()),
        )?;
    }

    let runtime_log = logs_dir.join("runtime.log");
    if let Ok(text) = fs::read_to_string(&runtime_log) {
        add_text_to_zip(
            &mut zip,
            "logs/runtime.log",
            &sanitize_text(text, game_root.as_deref()),
        )?;
    }
    let stutter_log = logs_dir.join("stutter.log");
    if let Ok(text) = fs::read_to_string(&stutter_log) {
        add_text_to_zip(
            &mut zip,
            "logs/stutter.log",
            &sanitize_text(text, game_root.as_deref()),
        )?;
    }

    if let Some(incident) = &incident {
        if let Ok(text) = fs::read_to_string(&incident.log) {
            add_text_to_zip(
                &mut zip,
                &format!("crash/{}.log", incident.stem),
                &sanitize_text(text, game_root.as_deref()),
            )?;
        }
        if let Some(json) = &incident.json
            && let Ok(text) = fs::read_to_string(json)
        {
            add_text_to_zip(
                &mut zip,
                &format!("crash/{}.json", incident.stem),
                &sanitize_text(text, game_root.as_deref()),
            )?;
        }
        if dump_included
            && let Some(dump) = &incident.dump
        {
            add_binary_file_to_zip(
                &mut zip,
                &format!("crash/{}.dmp", incident.stem),
                dump,
                SUPPORT_BINARY_FILE_LIMIT,
            )?;
        }
    }

    let versions = serde_json::json!({
        "suite": suite.version,
        "release_channel": suite.release_channel,
        "launcher": env!("CARGO_PKG_VERSION"),
        "game_id": game.id,
        "game_name": game.name,
        "runtime": game.runtime,
        "runtime_version": active_runtime_version,
        "build_id": build_id,
        "runtime_sha256": runtime_sha256,
        "profile_status": game.status,
    });
    add_text_to_zip(
        &mut zip,
        "versions.json",
        &serde_json::to_string_pretty(&versions).map_err(|e| e.to_string())?,
    )?;

    if let Some(text) = settings_text {
        add_text_to_zip(
            &mut zip,
            "settings.toml",
            &sanitize_text(text, game_root.as_deref()),
        )?;
    }

    match hardware_result {
        Ok(hardware) => add_text_to_zip(
            &mut zip,
            "hardware.json",
            &serde_json::to_string_pretty(&hardware).map_err(|e| e.to_string())?,
        )?,
        Err(error) => add_text_to_zip(
            &mut zip,
            "hardware-error.txt",
            &sanitize_text(error, game_root.as_deref()),
        )?,
    }

    add_text_to_zip(
        &mut zip,
        "os.txt",
        &format!("{}\narchitecture={}\n", os_version, std::env::consts::ARCH),
    )?;

    add_text_to_zip(
        &mut zip,
        "README.txt",
        &format!(
            "This support package contains technical diagnostics only. It does not include game files or save-file contents.\n\nMinidump included: {}\nMinidumps may contain small fragments of memory used by the runtime and are never uploaded automatically.\n",
            if dump_included { "yes (user consented)" } else { "no" }
        ),
    )?;
    zip.finish()
        .map_err(|e| format!("Could not finalize support package: {e}"))?;

    append_launcher_log(&format!(
        "Created support package for {} (minidump_included={})",
        game_id, dump_included
    ));
    if let Some(parent) = output_path.parent() {
        let _ = Command::new("explorer.exe").arg(parent).spawn();
    }
    Ok(output_path.to_string_lossy().to_string())
}

#[tauri::command]
fn open_title_folder(
    game_id: String,
    kind: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    manifest(&state, &game_id)?;
    let path = match kind.as_str() {
        "save" => save_root(&game_id)?,
        "logs" => game_log_root(&game_id)?,
        "all_logs" => launcher_root()?.join("logs"),
        "userdata" => title_root(&game_id)?,
        _ => title_root(&game_id)?,
    };
    fs::create_dir_all(&path).map_err(|e| format!("Could not create folder: {e}"))?;
    Command::new("explorer.exe")
        .arg(path)
        .spawn()
        .map_err(|e| format!("Could not open folder: {e}"))?;
    Ok(())
}

#[tauri::command]
fn open_license_notices() -> Result<(), String> {
    let root = storage_layout()?.licenses_root();
    materialize_license_notices(&root)?;
    Command::new("explorer.exe")
        .arg(root)
        .spawn()
        .map_err(|e| format!("Could not open licenses and notices: {e}"))?;
    Ok(())
}

fn parsed_suite_manifest() -> Result<SuiteManifest, String> {
    toml::from_str(SUITE_MANIFEST)
        .map_err(|error| format!("Could not parse suite manifest: {error}"))
}

fn fetch_game_component_catalog(
    update_catalog_url: &str,
    localization_catalog_url: &str,
) -> Result<updates::UpdateCatalog, String> {
    let mut catalog = updates::fetch_catalog(update_catalog_url)?;
    updates::apply_builtin_language_metadata(&mut catalog);
    if !localization_catalog_url.trim().is_empty() {
        match updates::fetch_localization_catalog(localization_catalog_url) {
            Ok(localization_catalog) => {
                if let Err(error) =
                    updates::apply_localization_catalog_metadata(&mut catalog, &localization_catalog)
                {
                    append_launcher_log(&format!(
                        "Localization catalog could not be applied; continuing without dynamic language components: {error}"
                    ));
                }
            }
            Err(error) => append_launcher_log(&format!(
                "Localization catalog unavailable; continuing without dynamic language metadata: {error}"
            )),
        }
    }
    Ok(catalog)
}

async fn fetch_runtime_localization_catalog(
    component_id: &str,
    version: &str,
) -> Result<Option<(updates::ComponentRelease, updates::LocalizationCatalog)>, String> {
    let suite = parsed_suite_manifest()?;
    if suite.update_catalog.trim().is_empty() {
        return Ok(None);
    }
    let catalog_url = suite.update_catalog.clone();
    let expected_channel = suite.release_channel.clone();
    let component_id = component_id.to_string();
    let version = version.to_string();
    let catalog = tauri::async_runtime::spawn_blocking(move || updates::fetch_catalog(&catalog_url))
        .await
        .map_err(|error| format!("Update catalog worker failed: {error}"))??;
    if catalog.channel != expected_channel {
        return Err(format!(
            "Update catalog channel mismatch: expected {expected_channel}, got {}",
            catalog.channel
        ));
    }
    let release = catalog
        .releases
        .into_iter()
        .find(|release| release.id == component_id && release.version == version)
        .ok_or_else(|| format!("Runtime {component_id} {version} is not available"))?;
    if release.kind != updates::ComponentKind::Runtime {
        return Err(format!("Component {} is not a runtime", release.id));
    }
    let Some(localization_catalog_url) = release.localization_catalog_url.clone() else {
        return Ok(None);
    };
    let localization_catalog = tauri::async_runtime::spawn_blocking(move || {
        updates::fetch_localization_catalog(&localization_catalog_url)
    })
    .await
    .map_err(|error| format!("Localization catalog worker failed: {error}"))??;
    let game_id = release
        .game_id
        .as_deref()
        .ok_or_else(|| "Runtime release is missing game_id".to_string())?;
    if localization_catalog.game_id != game_id
        || localization_catalog.runtime_version != release.version
    {
        return Err(format!(
            "Localization catalog does not belong to {} {}",
            release.id, release.version
        ));
    }
    Ok(Some((release, localization_catalog)))
}

fn component_kind_name(kind: &updates::ComponentKind) -> &'static str {
    match kind {
        updates::ComponentKind::Launcher => "launcher",
        updates::ComponentKind::Runtime => "runtime",
        updates::ComponentKind::Language => "language",
    }
}

fn plan_state_name(state: &updates::PlanState) -> &'static str {
    match state {
        updates::PlanState::UpToDate => "up_to_date",
        updates::PlanState::UpdateAvailable => "update_available",
        updates::PlanState::Available => "available",
        updates::PlanState::Corrupted => "corrupted",
        updates::PlanState::RepairUnavailable => "repair_unavailable",
        updates::PlanState::Incompatible => "incompatible",
    }
}

fn local_runtime_install_state(
    active: Option<&updates::ActiveComponentStatus>,
    ready: bool,
) -> (Option<String>, &'static str) {
    let Some(active) = active else {
        return (None, "not_installed");
    };
    let state = if ready { "up_to_date" } else { "corrupted" };
    (Some(active.version.clone()), state)
}

fn local_component_statuses(
    manifests: &[GameManifest],
) -> Result<Vec<ComponentUpdateStatus>, String> {
    let launcher_store = launcher_component_store()?;
    let game_store = game_component_store()?;
    let mut statuses = vec![ComponentUpdateStatus {
        id: "launcher".into(),
        kind: "launcher".into(),
        game_id: None,
        locale: None,
        display_name: None,
        xbox_language: None,
        translation_version: None,
        installed_version: Some(env!("CARGO_PKG_VERSION").into()),
        latest_version: None,
        state: "up_to_date".into(),
        download_url: None,
        size: None,
        published: None,
        notes_url: None,
        last_action: launcher_store.last_action_state("launcher"),
        can_rollback: false,
        releases: Vec::new(),
        installed_versions: Vec::new(),
    }];
    for game in manifests {
        let id = format!("runtime.{}", game.id);
        let active = game_store.active_status(&id)?;
        let active_healthy = if active.is_some() {
            runtime_component_is_ready(&game_store, &id)?
        } else {
            false
        };
        let (installed_version, state) =
            local_runtime_install_state(active.as_ref(), active_healthy);
        statuses.push(ComponentUpdateStatus {
            id: id.clone(),
            kind: "runtime".into(),
            game_id: Some(game.id.clone()),
            locale: None,
            display_name: None,
            xbox_language: None,
            translation_version: None,
            installed_version,
            latest_version: Some(game.runtime_version.clone()),
            state: state.into(),
            download_url: None,
            size: None,
            published: None,
            notes_url: None,
            can_rollback: active.as_ref().is_some_and(|status| status.can_rollback),
            last_action: active.and_then(|status| status.last_action),
            releases: local_runtime_releases(game, &id)?,
            installed_versions: game_store
                .installed_versions(&id)?
                .into_iter()
                .map(|version| InstalledComponentVersionStatus {
                    version: version.version,
                    healthy: version.healthy,
                })
                .collect(),
        });
    }
    for game in manifests {
        for language in game_store.active_languages(&game.id)? {
            let active = game_store.active_status(&language.id)?;
            statuses.push(ComponentUpdateStatus {
                id: language.id.clone(),
                kind: "language".into(),
                game_id: Some(language.game_id),
                locale: Some(language.locale),
                display_name: Some(language.display_name),
                xbox_language: Some(language.xbox_language),
                translation_version: language.translation_version,
                installed_version: Some(language.version),
                latest_version: None,
                state: if language.healthy {
                    "up_to_date".into()
                } else {
                    "corrupted".into()
                },
                download_url: None,
                size: None,
                published: None,
                notes_url: None,
                can_rollback: active.as_ref().is_some_and(|status| status.can_rollback),
                last_action: active
                    .and_then(|status| status.last_action)
                    .or_else(|| game_store.last_action_state(&language.id)),
                releases: Vec::new(),
                installed_versions: Vec::new(),
            });
        }
    }

    // Compatibility for PT-BR components installed by launcher versions that
    // predate generic language metadata in installation.toml.
    let legacy_ptbr_id = "language.cot.pt-br";
    if !statuses.iter().any(|status| status.id == legacy_ptbr_id)
        && let Some(active) = game_store.active_status(legacy_ptbr_id)?
    {
        statuses.push(ComponentUpdateStatus {
            id: legacy_ptbr_id.into(),
            kind: "language".into(),
            game_id: Some("cot".into()),
            locale: Some("pt-BR".into()),
            display_name: Some("Brazilian Portuguese".into()),
            xbox_language: Some(1),
            translation_version: None,
            installed_version: Some(active.version),
            latest_version: None,
            state: if active.healthy {
                "up_to_date".into()
            } else {
                "corrupted".into()
            },
            download_url: None,
            size: None,
            published: None,
            notes_url: None,
            can_rollback: active.can_rollback,
            last_action: active
                .last_action
                .or_else(|| game_store.last_action_state(legacy_ptbr_id)),
            releases: Vec::new(),
            installed_versions: Vec::new(),
        });
    }
    Ok(statuses)
}

fn installed_update_components(
    manifests: &[GameManifest],
    catalog: &updates::UpdateCatalog,
) -> Result<Vec<updates::InstalledComponent>, String> {
    let store = game_component_store()?;
    let mut installed = vec![updates::InstalledComponent {
        id: "launcher".into(),
        version: env!("CARGO_PKG_VERSION").into(),
        healthy: true,
    }];
    let mut seen = HashSet::from(["launcher".to_string()]);

    for game in manifests {
        let id = format!("runtime.{}", game.id);
        if let Some(status) = store.active_status(&id)? {
            let healthy = status.healthy && runtime_component_is_ready(&store, &id)?;
            installed.push(updates::InstalledComponent {
                id: id.clone(),
                version: status.version,
                healthy,
            });
            seen.insert(id);
        }
    }

    for release in &catalog.releases {
        if seen.contains(&release.id) {
            continue;
        }
        if let Some(status) = store.active_status(&release.id)? {
            installed.push(updates::InstalledComponent {
                id: release.id.clone(),
                version: status.version,
                healthy: status.healthy,
            });
            seen.insert(release.id.clone());
        }
    }
    Ok(installed)
}

fn installed_components_for_compatibility(
    manifests: &[GameManifest],
    release: &updates::ComponentRelease,
) -> Result<Vec<updates::InstalledComponent>, String> {
    let store = game_component_store()?;
    let mut installed = vec![updates::InstalledComponent {
        id: "launcher".into(),
        version: env!("CARGO_PKG_VERSION").into(),
        healthy: true,
    }];
    let mut seen = HashSet::from(["launcher".to_string()]);

    for game in manifests {
        let id = format!("runtime.{}", game.id);
        if let Some(status) = store.active_status(&id)? {
            let healthy = status.healthy && runtime_component_is_ready(&store, &id)?;
            installed.push(updates::InstalledComponent {
                id: id.clone(),
                version: status.version,
                healthy,
            });
            seen.insert(id);
        }
    }

    for requirement in &release.compatibility.requirements {
        if seen.contains(&requirement.id) {
            continue;
        }
        if let Some(status) = store.active_status(&requirement.id)? {
            installed.push(updates::InstalledComponent {
                id: requirement.id.clone(),
                version: status.version,
                healthy: status.healthy,
            });
            seen.insert(requirement.id.clone());
        }
    }
    Ok(installed)
}

fn component_statuses_from_catalog(
    manifests: &[GameManifest],
    catalog: &updates::UpdateCatalog,
) -> Result<Vec<ComponentUpdateStatus>, String> {
    let launcher_store = launcher_component_store()?;
    let game_store = game_component_store()?;
    let installed = installed_update_components(manifests, catalog)?;
    let plans = updates::plan_updates(catalog, &installed, env!("CARGO_PKG_VERSION"))?;
    let mut statuses = Vec::with_capacity(plans.len());
    for plan in plans {
        if plan.kind == updates::ComponentKind::Language
            && (plan.display_name.is_none() || plan.xbox_language.is_none())
        {
            continue;
        }
        let launcher_incompatible = plan.kind == updates::ComponentKind::Launcher
            && plan.latest_version.as_deref().is_some_and(|version| {
                game_store
                    .ensure_launcher_version_compatible(version)
                    .is_err()
            });
        let last_action = match &plan.kind {
            updates::ComponentKind::Launcher => launcher_store.last_action_state(&plan.id),
            updates::ComponentKind::Runtime | updates::ComponentKind::Language
                if plan.installed_version.is_some() =>
            {
                game_store.last_action_state(&plan.id)
            }
            updates::ComponentKind::Runtime | updates::ComponentKind::Language => None,
        };
        let can_rollback = match &plan.kind {
            updates::ComponentKind::Launcher => false,
            updates::ComponentKind::Runtime | updates::ComponentKind::Language => game_store
                .active_status(&plan.id)?
                .is_some_and(|status| status.can_rollback),
        };
        let mut releases = updates::compatible_releases_for_component(
            catalog,
            &installed,
            env!("CARGO_PKG_VERSION"),
            &plan.id,
        )?
        .into_iter()
        .map(|release| ComponentReleaseStatus {
            version: release.version,
            published: release.published,
            notes_url: release.notes_url,
            size: release.size,
            downloadable: true,
        })
        .collect();
        if plan.kind == updates::ComponentKind::Runtime {
            merge_archived_runtime_releases(&mut releases, &plan.id)?;
            if let Some(game) = plan
                .game_id
                .as_deref()
                .and_then(|game_id| manifests.iter().find(|game| game.id == game_id))
            {
                merge_manifest_runtime_release(&mut releases, game)?;
            }
        }
        let installed_versions = if plan.kind == updates::ComponentKind::Runtime {
            game_store
                .installed_versions(&plan.id)?
                .into_iter()
                .map(|version| InstalledComponentVersionStatus {
                    version: version.version,
                    healthy: version.healthy,
                })
                .collect()
        } else {
            Vec::new()
        };
        statuses.push(ComponentUpdateStatus {
            id: plan.id.clone(),
            kind: component_kind_name(&plan.kind).into(),
            game_id: plan.game_id,
            locale: plan.locale,
            display_name: plan.display_name,
            xbox_language: plan.xbox_language,
            translation_version: plan.translation_version,
            installed_version: plan.installed_version,
            latest_version: plan.latest_version,
            state: if launcher_incompatible {
                "incompatible".into()
            } else {
                plan_state_name(&plan.state).into()
            },
            download_url: if launcher_incompatible {
                None
            } else {
                plan.download_url
            },
            size: plan.size,
            published: plan.published,
            notes_url: plan.notes_url,
            last_action,
            can_rollback,
            releases,
            installed_versions,
        });
    }

    let local = local_component_statuses(manifests)?;
    for status in local {
        if !statuses.iter().any(|entry| entry.id == status.id) {
            statuses.push(status);
        }
    }
    statuses.sort_by(|left, right| left.id.cmp(&right.id));
    Ok(statuses)
}

fn launcher_component_status_from_catalog(
    catalog: &updates::UpdateCatalog,
) -> Result<ComponentUpdateStatus, String> {
    let launcher_store = launcher_component_store()?;
    let game_store = game_component_store()?;
    let launcher_catalog = updates::UpdateCatalog {
        schema_version: catalog.schema_version,
        channel: catalog.channel.clone(),
        releases: catalog
            .releases
            .iter()
            .filter(|release| {
                release.id == "launcher" && release.package == updates::PackageFormat::PortableExe
            })
            .cloned()
            .collect(),
    };
    if launcher_catalog.releases.is_empty() {
        return local_component_statuses(&[])?
            .into_iter()
            .find(|component| component.id == "launcher")
            .ok_or_else(|| "Launcher status is unavailable".to_string());
    }

    let mut installed = vec![updates::InstalledComponent {
        id: "launcher".into(),
        version: env!("CARGO_PKG_VERSION").into(),
        healthy: true,
    }];
    let mut seen = HashSet::from(["launcher".to_string()]);
    for requirement in launcher_catalog
        .releases
        .iter()
        .flat_map(|release| release.compatibility.requirements.iter())
    {
        if seen.contains(&requirement.id) {
            continue;
        }
        if let Some(status) = game_store.active_status(&requirement.id)? {
            installed.push(updates::InstalledComponent {
                id: requirement.id.clone(),
                version: status.version,
                healthy: status.healthy,
            });
            seen.insert(requirement.id.clone());
        }
    }

    let plan = updates::plan_updates(&launcher_catalog, &installed, env!("CARGO_PKG_VERSION"))?
        .into_iter()
        .find(|plan| plan.id == "launcher")
        .ok_or_else(|| "Launcher update plan is unavailable".to_string())?;
    let launcher_incompatible = plan.latest_version.as_deref().is_some_and(|version| {
        game_store
            .ensure_launcher_version_compatible(version)
            .is_err()
    });
    let releases = updates::compatible_releases_for_component(
        &launcher_catalog,
        &installed,
        env!("CARGO_PKG_VERSION"),
        "launcher",
    )?
    .into_iter()
    .map(|release| ComponentReleaseStatus {
        version: release.version,
        published: release.published,
        notes_url: release.notes_url,
        size: release.size,
        downloadable: true,
    })
    .collect();

    Ok(ComponentUpdateStatus {
        id: plan.id,
        kind: "launcher".into(),
        game_id: None,
        locale: None,
        display_name: None,
        xbox_language: None,
        translation_version: None,
        installed_version: plan.installed_version,
        latest_version: plan.latest_version,
        state: if launcher_incompatible {
            "incompatible".into()
        } else {
            plan_state_name(&plan.state).into()
        },
        download_url: if launcher_incompatible {
            None
        } else {
            plan.download_url
        },
        size: plan.size,
        published: plan.published,
        notes_url: plan.notes_url,
        last_action: launcher_store.last_action_state("launcher"),
        can_rollback: false,
        releases,
        installed_versions: Vec::new(),
    })
}

fn select_component_release(
    manifests: &[GameManifest],
    catalog: &updates::UpdateCatalog,
    component_id: &str,
    requested_version: Option<&str>,
    allow_current: bool,
) -> Result<updates::ComponentRelease, String> {
    let installed = installed_update_components(manifests, catalog)?;
    let compatible = updates::compatible_releases_for_component(
        catalog,
        &installed,
        env!("CARGO_PKG_VERSION"),
        component_id,
    )?
    .into_iter()
    .filter(|release| {
        component_id != "launcher" || release.package == updates::PackageFormat::PortableExe
    })
    .collect::<Vec<_>>();
    let release = match requested_version {
        Some(version) => compatible
            .iter()
            .find(|release| release.version == version)
            .cloned()
            .ok_or_else(|| {
                format!("Component {component_id} version {version} is not available or compatible")
            })?,
        None => compatible
            .first()
            .cloned()
            .ok_or_else(|| format!("Component {component_id} has no compatible release"))?,
    };
    let installed_component = installed
        .iter()
        .find(|component| component.id == component_id);
    let installing_current = installed_component
        .is_some_and(|component| component.healthy && component.version == release.version);
    if installing_current && !(allow_current && release.kind == updates::ComponentKind::Runtime) {
        return Err(format!(
            "Component {component_id} version {} is already active",
            release.version
        ));
    }
    if release.kind == updates::ComponentKind::Launcher {
        game_component_store()?.ensure_launcher_version_compatible(&release.version)?;
    }
    if release.kind == updates::ComponentKind::Language
        && (release.display_name.is_none() || release.xbox_language.is_none())
    {
        return Err(format!(
            "Localization metadata is unavailable for {component_id}; check for updates again"
        ));
    }
    Ok(release)
}

fn ensure_component_game_stopped(
    release: &updates::ComponentRelease,
    state: &AppState,
) -> Result<(), String> {
    let Some(game_id) = release.game_id.as_deref() else {
        return Ok(());
    };
    ensure_game_stopped(game_id, state)
}

fn ensure_game_stopped(game_id: &str, state: &AppState) -> Result<(), String> {
    let mut processes = state
        .processes
        .lock()
        .map_err(|_| "Process state lock failed")?;
    if let Some(child) = processes.get_mut(game_id) {
        if child
            .try_wait()
            .map_err(|error| error.to_string())?
            .is_none()
        {
            return Err(format!(
                "Close {} before updating its components",
                manifest(state, game_id)?.name
            ));
        }
        processes.remove(game_id);
    }
    Ok(())
}

fn ensure_runtime_version_operation_idle(
    component_id: &str,
    state: &AppState,
) -> Result<String, String> {
    ensure_component_update_idle(state)?;
    ensure_library_idle(state)?;
    let game_id = component_id
        .strip_prefix("runtime.")
        .ok_or_else(|| "Only game runtime versions can be managed here".to_string())?;
    let game = manifest(state, game_id)?;
    if !game.playable {
        return Err(format!(
            "{} does not have a selectable runtime yet",
            game.name
        ));
    }
    ensure_game_stopped(game_id, state).map_err(|error| {
        if error.starts_with("Close ") {
            format!("Close {} before changing runtime versions", game.name)
        } else {
            error
        }
    })?;
    if state
        .setup_jobs
        .lock()
        .map_err(|_| "Game setup state lock failed")?
        .values()
        .any(|running| *running)
    {
        return Err("Wait for game setup to finish before changing runtime versions".into());
    }
    if !state
        .localization_jobs
        .lock()
        .map_err(|_| "Localization state lock failed")?
        .is_empty()
    {
        return Err("Wait for localization work to finish before changing runtime versions".into());
    }
    Ok(game_id.to_string())
}

#[tauri::command]
async fn check_component_updates(
    offline_only: Option<bool>,
    state: tauri::State<'_, AppState>,
) -> Result<UpdateOverview, String> {
    let suite = parsed_suite_manifest()?;
    let manifests = state.manifests.clone();
    let game_components_only = |mut components: Vec<ComponentUpdateStatus>| {
        components.retain(|component| component.id != "launcher");
        components
    };
    if offline_only.unwrap_or(false) || suite.update_catalog.trim().is_empty() {
        return Ok(UpdateOverview {
            configured: !suite.update_catalog.trim().is_empty(),
            components: game_components_only(local_component_statuses(&manifests)?),
            error: None,
        });
    }
    let catalog_url = suite.update_catalog;
    let localization_catalog_url = suite.localization_catalog;
    let expected_channel = suite.release_channel;
    match tauri::async_runtime::spawn_blocking(move || {
        fetch_game_component_catalog(&catalog_url, &localization_catalog_url)
    })
    .await
    {
        Ok(Ok(catalog)) if catalog.channel == expected_channel => Ok(UpdateOverview {
            configured: true,
            components: game_components_only(component_statuses_from_catalog(
                &manifests, &catalog,
            )?),
            error: None,
        }),
        Ok(Ok(catalog)) => Ok(UpdateOverview {
            configured: true,
            components: game_components_only(local_component_statuses(&manifests)?),
            error: Some(format!(
                "Update catalog channel mismatch: expected {expected_channel}, got {}",
                catalog.channel
            )),
        }),
        Ok(Err(error)) => Ok(UpdateOverview {
            configured: true,
            components: game_components_only(local_component_statuses(&manifests)?),
            error: Some(error),
        }),
        Err(error) => Ok(UpdateOverview {
            configured: true,
            components: game_components_only(local_component_statuses(&manifests)?),
            error: Some(format!("Update catalog worker failed: {error}")),
        }),
    }
}

#[tauri::command]
async fn runtime_additional_content(
    component_id: String,
    version: String,
) -> Result<Option<RuntimeAdditionalContentStatus>, String> {
    let Some((_runtime, localization_catalog)) =
        fetch_runtime_localization_catalog(&component_id, &version).await?
    else {
        return Ok(None);
    };
    let store = game_component_store()?;
    let languages = localization_catalog
        .languages
        .iter()
        .map(|language| {
            let installed_version = store
                .active_status(&language.id)
                .ok()
                .flatten()
                .filter(|status| status.healthy)
                .map(|status| status.version);
            RuntimeAdditionalLanguageStatus {
                id: language.id.clone(),
                locale: language.locale.clone(),
                display_name: language.display_name.clone(),
                version: language.version.clone(),
                translation_version: language.translation_version.clone(),
                installed_version,
            }
        })
        .collect();
    Ok(Some(RuntimeAdditionalContentStatus {
        game_id: localization_catalog.game_id,
        runtime_version: localization_catalog.runtime_version,
        pack_version: localization_catalog.pack_version,
        pack_size: localization_catalog.size,
        languages,
    }))
}

#[tauri::command]
async fn check_launcher_update() -> Result<UpdateOverview, String> {
    let suite = parsed_suite_manifest()?;
    let local_launcher = || -> Result<Vec<ComponentUpdateStatus>, String> {
        Ok(local_component_statuses(&[])?
            .into_iter()
            .filter(|component| component.id == "launcher")
            .collect())
    };
    if suite.update_catalog.trim().is_empty() {
        return Ok(UpdateOverview {
            configured: false,
            components: local_launcher()?,
            error: None,
        });
    }
    let catalog_url = suite.update_catalog;
    let expected_channel = suite.release_channel;
    match tauri::async_runtime::spawn_blocking(move || updates::fetch_catalog(&catalog_url)).await {
        Ok(Ok(catalog)) if catalog.channel == expected_channel => Ok(UpdateOverview {
            configured: true,
            components: vec![launcher_component_status_from_catalog(&catalog)?],
            error: None,
        }),
        Ok(Ok(catalog)) => Ok(UpdateOverview {
            configured: true,
            components: local_launcher()?,
            error: Some(format!(
                "Update catalog channel mismatch: expected {expected_channel}, got {}",
                catalog.channel
            )),
        }),
        Ok(Err(error)) => Ok(UpdateOverview {
            configured: true,
            components: local_launcher()?,
            error: Some(error),
        }),
        Err(error) => Ok(UpdateOverview {
            configured: true,
            components: local_launcher()?,
            error: Some(format!("Update catalog worker failed: {error}")),
        }),
    }
}

#[tauri::command]
async fn install_component_update(
    component_id: String,
    version: Option<String>,
    reinstall: bool,
    activate: bool,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<ComponentUpdateResult, String> {
    let suite = parsed_suite_manifest()?;
    let catalog_url = suite.update_catalog.clone();
    let localization_catalog_url = suite.localization_catalog.clone();
    if catalog_url.trim().is_empty() {
        return Err("No update catalog is configured".into());
    }
    let expected_channel = suite.release_channel.clone();
    let catalog = tauri::async_runtime::spawn_blocking(move || {
        fetch_game_component_catalog(&catalog_url, &localization_catalog_url)
    })
    .await
    .map_err(|error| format!("Update catalog worker failed: {error}"))??;
    if catalog.channel != expected_channel {
        return Err(format!(
            "Update catalog channel mismatch: expected {expected_channel}, got {}",
            catalog.channel
        ));
    }
    let release = select_component_release(
        &state.manifests,
        &catalog,
        &component_id,
        version.as_deref(),
        reinstall,
    )?;
    let store = match &release.kind {
        updates::ComponentKind::Launcher => launcher_component_store()?,
        updates::ComponentKind::Runtime | updates::ComponentKind::Language => {
            game_component_store()?
        }
    };
    if !activate && release.kind != updates::ComponentKind::Runtime {
        return Err("Only runtime versions can be downloaded without activation".into());
    }
    {
        let _operation_gate = state
            .operation_gate
            .lock()
            .map_err(|_| "Operation gate lock failed")?;
        ensure_library_idle(&state)?;
        ensure_component_game_stopped(&release, &state)?;
        if state
            .setup_jobs
            .lock()
            .map_err(|_| "Game setup state lock failed")?
            .values()
            .any(|running| *running)
        {
            return Err("Wait for game setup to finish before updating components".into());
        }
        if !state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?
            .is_empty()
        {
            return Err("Wait for localization work to finish before updating components".into());
        }
        let mut update_running = state
            .component_update_job
            .lock()
            .map_err(|_| "Component update state lock failed")?;
        if *update_running {
            return Err("Another component update is already running".into());
        }
        *update_running = true;
    }

    let worker_store = store.clone();
    let worker_release = release.clone();
    let worker_app = app.clone();
    let worker_result = tauri::async_runtime::spawn_blocking(move || {
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "downloading".into(),
                progress: 0,
                detail: "Downloading verified component package...".into(),
                bytes_done: 0,
                bytes_total: worker_release.size,
            },
        );
        let mut last_download_percent = 0u8;
        let download = updates::download_release(&worker_store, &worker_release, |done, total| {
            let percent = progress_percent(done, total, 0, 100);
            if percent == last_download_percent {
                return;
            }
            last_download_percent = percent;
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: worker_release.id.clone(),
                    stage: "downloading".into(),
                    progress: percent,
                    detail: "Downloading and verifying SHA-256...".into(),
                    bytes_done: done,
                    bytes_total: total,
                },
            );
        });
        if let Err(error) = download {
            let _ = worker_store.record_action(&worker_release.id, "failed", &error);
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: worker_release.id.clone(),
                    stage: "failed".into(),
                    progress: 0,
                    detail: error.clone(),
                    bytes_done: 0,
                    bytes_total: worker_release.size,
                },
            );
            return Err(error);
        }
        let worker_release = if worker_release.kind == updates::ComponentKind::Language {
            if let Some(pack_reference) = worker_release.localization_pack.clone() {
                let artifact = worker_store.staged_artifact_path(&worker_release);
                let pack_staging = std::env::temp_dir().join(format!(
                    "mojorecomp-online-localization-pack-{}-{}",
                    std::process::id(),
                    timestamp_seconds()
                ));
                let prepared = (|| -> Result<updates::ComponentRelease, String> {
                    let pack = updates::extract_offline_localization_pack(&artifact, &pack_staging)?;
                    if pack.version != pack_reference.version {
                        return Err(format!(
                            "Localization Pack version mismatch: expected {}, got {}",
                            pack_reference.version, pack.version
                        ));
                    }
                    if worker_release.game_id.as_deref() != Some(pack.game_id.as_str()) {
                        return Err("Localization Pack game does not match the selected language component".into());
                    }
                    let language = pack
                        .languages
                        .iter()
                        .find(|language| language.id == worker_release.id)
                        .ok_or_else(|| {
                            format!(
                                "Localization Pack does not contain component {}",
                                worker_release.id
                            )
                        })?;
                    if language.version != worker_release.version
                        || worker_release.locale.as_deref() != Some(language.locale.as_str())
                        || worker_release.display_name.as_deref()
                            != Some(language.display_name.as_str())
                        || worker_release.xbox_language != Some(language.xbox_language)
                    {
                        return Err(
                            "Localization Pack language metadata does not match the update catalog"
                                .into(),
                        );
                    }
                    let mut component_release = worker_release.clone();
                    component_release.size = pack_reference.component_size;
                    component_release.sha256 = pack_reference.component_sha256;
                    component_release.localization_pack = None;
                    let signed = updates::inspect_signed_release_artifact(
                        &language.package_path,
                        env!("CARGO_PKG_VERSION"),
                        &component_release,
                    )?;
                    worker_store.stage_local_artifact(&signed, &language.package_path)?;
                    Ok(signed)
                })();
                let _ = fs::remove_dir_all(&pack_staging);
                match prepared {
                    Ok(release) => release,
                    Err(error) => {
                        let _ = worker_store.record_action(&worker_release.id, "failed", &error);
                        emit_component_update_progress(
                            &worker_app,
                            ComponentUpdateProgress {
                                component_id: worker_release.id.clone(),
                                stage: "failed".into(),
                                progress: 0,
                                detail: error.clone(),
                                bytes_done: worker_release.size,
                                bytes_total: worker_release.size,
                            },
                        );
                        return Err(error);
                    }
                }
            } else {
                worker_release
            }
        } else {
            worker_release
        };
        let worker_release = match worker_release.kind {
            updates::ComponentKind::Launcher => {
                let artifact = worker_store.staged_artifact_path(&worker_release);
                if let Err(error) =
                    updates::verify_signed_launcher_executable(&artifact, &worker_release.version)
                {
                    let _ = worker_store.record_action(&worker_release.id, "failed", &error);
                    emit_component_update_progress(
                        &worker_app,
                        ComponentUpdateProgress {
                            component_id: worker_release.id.clone(),
                            stage: "failed".into(),
                            progress: 0,
                            detail: error.clone(),
                            bytes_done: worker_release.size,
                            bytes_total: worker_release.size,
                        },
                    );
                    return Err(error);
                }
                worker_release
            }
            updates::ComponentKind::Runtime | updates::ComponentKind::Language => {
                let artifact = worker_store.staged_artifact_path(&worker_release);
                match updates::inspect_signed_release_artifact(
                    &artifact,
                    env!("CARGO_PKG_VERSION"),
                    &worker_release,
                ) {
                    Ok(signed) => signed,
                    Err(error) => {
                        let _ = worker_store.record_action(&worker_release.id, "failed", &error);
                        emit_component_update_progress(
                            &worker_app,
                            ComponentUpdateProgress {
                                component_id: worker_release.id.clone(),
                                stage: "failed".into(),
                                progress: 0,
                                detail: error.clone(),
                                bytes_done: worker_release.size,
                                bytes_total: worker_release.size,
                            },
                        );
                        return Err(error);
                    }
                }
            }
        };
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "validating".into(),
                progress: 100,
                detail: if worker_release.kind == updates::ComponentKind::Launcher {
                    "Launcher executable signature, size, and SHA-256 verified.".into()
                } else {
                    "Package signature, manifest, size, and SHA-256 verified.".into()
                },
                bytes_done: worker_release.size,
                bytes_total: worker_release.size,
            },
        );
        if worker_release.kind == updates::ComponentKind::Launcher {
            if let Err(error) = worker_store.stage_launcher_ready(&worker_release) {
                let _ = worker_store.record_action(&worker_release.id, "failed", &error);
                emit_component_update_progress(
                    &worker_app,
                    ComponentUpdateProgress {
                        component_id: worker_release.id.clone(),
                        stage: "failed".into(),
                        progress: 0,
                        detail: error.clone(),
                        bytes_done: worker_release.size,
                        bytes_total: worker_release.size,
                    },
                );
                return Err(error);
            }
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: worker_release.id.clone(),
                    stage: "ready_restart".into(),
                    progress: 100,
                    detail: "Verified launcher package is ready to install on restart.".into(),
                    bytes_done: worker_release.size,
                    bytes_total: worker_release.size,
                },
            );
            return Ok(ComponentUpdateResult {
                component_id: worker_release.id,
                state: "ready_restart".into(),
                restart_required: true,
            });
        }

        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "installing".into(),
                progress: 100,
                detail: "Activating verified component version...".into(),
                bytes_done: worker_release.size,
                bytes_total: worker_release.size,
            },
        );
        if let Err(error) = worker_store.prepare_repair(&worker_release.id, &worker_release.version)
        {
            let _ = worker_store.record_action(&worker_release.id, "failed", &error);
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: worker_release.id.clone(),
                    stage: "failed".into(),
                    progress: 0,
                    detail: error.clone(),
                    bytes_done: worker_release.size,
                    bytes_total: worker_release.size,
                },
            );
            return Err(error);
        }
        let install_result = if activate {
            worker_store.install_staged(&worker_release)
        } else {
            worker_store.install_staged_inactive(&worker_release)
        };
        if let Err(error) = install_result {
            if matches!(
                worker_store.recover_component(&worker_release.id),
                Ok(Some(updates::RecoveryResult::Completed(_)))
            ) && activate
            {
                let _ = worker_store.record_action(
                    &worker_release.id,
                    "installed",
                    "Component update installed successfully after journal recovery",
                );
            } else {
                let _ = worker_store.record_action(&worker_release.id, "failed", &error);
                emit_component_update_progress(
                    &worker_app,
                    ComponentUpdateProgress {
                        component_id: worker_release.id.clone(),
                        stage: "failed".into(),
                        progress: 0,
                        detail: error.clone(),
                        bytes_done: worker_release.size,
                        bytes_total: worker_release.size,
                    },
                );
                return Err(error);
            }
        }
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "complete".into(),
                progress: 100,
                detail: if activate {
                    "Component update installed successfully.".into()
                } else {
                    "Runtime version downloaded and verified.".into()
                },
                bytes_done: worker_release.size,
                bytes_total: worker_release.size,
            },
        );
        Ok(ComponentUpdateResult {
            component_id: worker_release.id,
            state: if activate {
                "installed".into()
            } else {
                "downloaded".into()
            },
            restart_required: false,
        })
    })
    .await;

    if let Ok(mut running) = state.component_update_job.lock() {
        *running = false;
    }
    let result = match worker_result {
        Ok(result) => result,
        Err(error) => Err(format!("Component update worker failed: {error}")),
    };
    if component_id == "runtime.cot" && result.is_ok() {
        match cleanup_legacy_runtime_cache_if_ready() {
            Ok(true) => append_launcher_log(
                "Removed legacy Local AppData runtime cache after validating runtime.cot",
            ),
            Ok(false) => {}
            Err(error) => append_launcher_log(&format!(
                "Legacy runtime cache cleanup was deferred: {error}"
            )),
        }
    }
    result
}

#[tauri::command]
async fn install_runtime_additional_content(
    component_id: String,
    version: String,
    language_ids: Vec<String>,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<LocalizationPackInstallResult, String> {
    let Some((runtime_release, localization_catalog)) =
        fetch_runtime_localization_catalog(&component_id, &version).await?
    else {
        return Err(format!(
            "Runtime {component_id} {version} has no published additional content"
        ));
    };
    let game_id = runtime_release
        .game_id
        .clone()
        .ok_or_else(|| "Runtime release is missing game_id".to_string())?;
    let store = game_component_store()?;
    let active_runtime = store
        .active_status(&component_id)?
        .filter(|status| status.healthy)
        .ok_or_else(|| format!("Activate runtime {version} before installing additional content"))?;
    if active_runtime.version != version || !runtime_component_is_ready(&store, &component_id)? {
        return Err(format!(
            "Activate runtime {version} before installing its additional content"
        ));
    }

    let requested = language_ids.into_iter().collect::<HashSet<_>>();
    if requested.is_empty() {
        return Ok(LocalizationPackInstallResult {
            game_id,
            version: localization_catalog.pack_version,
            installed_languages: Vec::new(),
        });
    }
    if requested.len() > localization_catalog.languages.len() {
        return Err("Additional content selection contains unknown languages".into());
    }
    for id in &requested {
        if !localization_catalog
            .languages
            .iter()
            .any(|language| &language.id == id)
        {
            return Err(format!("Additional content is not part of runtime {version}: {id}"));
        }
    }

    let suite = parsed_suite_manifest()?;
    let mut language_catalog = updates::UpdateCatalog {
        schema_version: 1,
        channel: suite.release_channel,
        releases: Vec::new(),
    };
    updates::apply_localization_catalog_metadata(&mut language_catalog, &localization_catalog)?;
    let mut releases = Vec::new();
    for release in language_catalog.releases {
        if !requested.contains(&release.id) {
            continue;
        }
        let already_installed = store.active_status(&release.id)?.is_some_and(|status| {
            status.healthy && status.version == release.version
        });
        if already_installed {
            continue;
        }
        let installed = installed_components_for_compatibility(&state.manifests, &release)?;
        updates::validate_release_compatibility(
            &release,
            env!("CARGO_PKG_VERSION"),
            &installed,
        )?;
        releases.push(release);
    }
    if releases.is_empty() {
        let installed_languages = localization_catalog
            .languages
            .iter()
            .filter(|language| requested.contains(&language.id))
            .map(|language| language.locale.clone())
            .collect();
        return Ok(LocalizationPackInstallResult {
            game_id,
            version: localization_catalog.pack_version,
            installed_languages,
        });
    }

    {
        let _operation_gate = state
            .operation_gate
            .lock()
            .map_err(|_| "Operation gate lock failed")?;
        ensure_library_idle(&state)?;
        ensure_game_stopped(&game_id, &state)?;
        if state
            .setup_jobs
            .lock()
            .map_err(|_| "Game setup state lock failed")?
            .values()
            .any(|running| *running)
        {
            return Err("Wait for game setup to finish before installing additional content".into());
        }
        if !state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?
            .is_empty()
        {
            return Err("Wait for localization work to finish before installing additional content".into());
        }
        let mut update_running = state
            .component_update_job
            .lock()
            .map_err(|_| "Component update state lock failed")?;
        if *update_running {
            return Err("Another component update is already running".into());
        }
        *update_running = true;
    }

    let worker_store = store.clone();
    let worker_app = app.clone();
    let worker_catalog = localization_catalog.clone();
    let worker_game_id = game_id.clone();
    let worker_result = tauri::async_runtime::spawn_blocking(move || {
        let first = releases
            .first()
            .ok_or_else(|| "No additional language component was selected".to_string())?;
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: first.id.clone(),
                stage: "downloading".into(),
                progress: 0,
                detail: format!(
                    "Downloading Localization Pack {} once for {} selected language(s)...",
                    worker_catalog.pack_version,
                    releases.len()
                ),
                bytes_done: 0,
                bytes_total: first.size,
            },
        );
        let mut last_percent = 0u8;
        let artifact = updates::download_release(&worker_store, first, |done, total| {
            let percent = progress_percent(done, total, 0, 100);
            if percent == last_percent {
                return;
            }
            last_percent = percent;
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: first.id.clone(),
                    stage: "downloading".into(),
                    progress: percent,
                    detail: "Downloading and verifying Localization Pack SHA-256...".into(),
                    bytes_done: done,
                    bytes_total: total,
                },
            );
        })?;
        let pack_staging = std::env::temp_dir().join(format!(
            "mojorecomp-runtime-additional-content-{}-{}",
            std::process::id(),
            timestamp_seconds()
        ));
        let result = (|| -> Result<Vec<String>, String> {
            let pack = updates::extract_offline_localization_pack(&artifact, &pack_staging)?;
            if pack.game_id != worker_game_id || pack.version != worker_catalog.pack_version {
                return Err("Downloaded Localization Pack does not match this runtime release".into());
            }
            let mut prepared_languages = Vec::with_capacity(releases.len());
            for release in &releases {
                let language = pack
                    .languages
                    .iter()
                    .find(|language| language.id == release.id)
                    .ok_or_else(|| format!("Localization Pack does not contain {}", release.id))?;
                if release.locale.as_deref() != Some(language.locale.as_str())
                    || release.display_name.as_deref() != Some(language.display_name.as_str())
                    || release.xbox_language != Some(language.xbox_language)
                    || release.version != language.version
                {
                    return Err("Localization Pack language metadata does not match its catalog".into());
                }
                let mut component_release = release.clone();
                let pack_reference = component_release
                    .localization_pack
                    .clone()
                    .ok_or_else(|| "Localization component is missing pack metadata".to_string())?;
                component_release.size = pack_reference.component_size;
                component_release.sha256 = pack_reference.component_sha256;
                component_release.localization_pack = None;
                let signed = updates::inspect_signed_release_artifact(
                    &language.package_path,
                    env!("CARGO_PKG_VERSION"),
                    &component_release,
                )?;
                prepared_languages.push((signed, language.clone(), component_release.size));
            }

            let mut installed_languages = Vec::new();
            for (index, (signed, language, component_size)) in
                prepared_languages.iter().enumerate()
            {
                let ordinal = index + 1;
                let total = prepared_languages.len();
                emit_component_update_progress(
                    &worker_app,
                    ComponentUpdateProgress {
                        component_id: signed.id.clone(),
                        stage: "installing".into(),
                        progress: 100,
                        detail: format!("Installing selected language {ordinal} of {total}..."),
                        bytes_done: *component_size,
                        bytes_total: *component_size,
                    },
                );
                worker_store.stage_local_artifact(&signed, &language.package_path)?;
                worker_store.prepare_repair(&signed.id, &signed.version)?;
                worker_store.install_staged(&signed)?;
                installed_languages.push(language.locale.clone());
                emit_component_update_progress(
                    &worker_app,
                    ComponentUpdateProgress {
                        component_id: signed.id.clone(),
                        stage: "complete".into(),
                        progress: 100,
                        detail: format!("Installed {}.", language.display_name),
                        bytes_done: *component_size,
                        bytes_total: *component_size,
                    },
                );
            }
            Ok(installed_languages)
        })();
        let _ = fs::remove_dir_all(&pack_staging);
        result
    })
    .await;

    if let Ok(mut running) = state.component_update_job.lock() {
        *running = false;
    }
    let installed_languages = worker_result
        .map_err(|error| format!("Additional content worker failed: {error}"))??;
    append_launcher_log(&format!(
        "Installed Localization Pack {} for {} runtime {} ({} selected language component(s))",
        localization_catalog.pack_version,
        game_id,
        version,
        installed_languages.len()
    ));
    Ok(LocalizationPackInstallResult {
        game_id,
        version: localization_catalog.pack_version,
        installed_languages,
    })
}

#[tauri::command]
async fn install_offline_runtime_package(
    path: String,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<ComponentUpdateResult, String> {
    let package_path = PathBuf::from(path);
    let current_launcher = env!("CARGO_PKG_VERSION").to_string();
    let inspect_path = package_path.clone();
    let release = tauri::async_runtime::spawn_blocking(move || {
        updates::inspect_offline_package(&inspect_path, &current_launcher)
    })
    .await
    .map_err(|error| format!("Offline package validation worker failed: {error}"))??;

    if release.kind != updates::ComponentKind::Runtime {
        return Err("Offline installation accepts signed game runtime ZIPs only".into());
    }

    let installed = installed_components_for_compatibility(&state.manifests, &release)?;
    updates::validate_release_compatibility(&release, env!("CARGO_PKG_VERSION"), &installed)?;
    let store = game_component_store()?;
    {
        let _operation_gate = state
            .operation_gate
            .lock()
            .map_err(|_| "Operation gate lock failed")?;
        ensure_library_idle(&state)?;
        ensure_component_game_stopped(&release, &state)?;
        if state
            .setup_jobs
            .lock()
            .map_err(|_| "Game setup state lock failed")?
            .values()
            .any(|running| *running)
        {
            return Err(
                "Wait for game setup to finish before installing an offline runtime".into(),
            );
        }
        if !state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?
            .is_empty()
        {
            return Err(
                "Wait for localization work to finish before installing an offline runtime".into(),
            );
        }
        let mut update_running = state
            .component_update_job
            .lock()
            .map_err(|_| "Component update state lock failed")?;
        if *update_running {
            return Err("Another component update is already running".into());
        }
        *update_running = true;
    }

    let worker_store = store.clone();
    let worker_release = release.clone();
    let worker_path = package_path.clone();
    let worker_app = app.clone();
    let worker_result = tauri::async_runtime::spawn_blocking(move || {
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "validating".into(),
                progress: 20,
                detail: "Validating signed runtime ZIP...".into(),
                bytes_done: 0,
                bytes_total: worker_release.size,
            },
        );
        if let Err(error) = worker_store.stage_local_artifact(&worker_release, &worker_path) {
            let _ = worker_store.record_action(&worker_release.id, "failed", &error);
            return Err(error);
        }
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "installing".into(),
                progress: 70,
                detail: "Installing offline runtime package...".into(),
                bytes_done: worker_release.size,
                bytes_total: worker_release.size,
            },
        );
        if let Err(error) = worker_store.prepare_repair(&worker_release.id, &worker_release.version)
        {
            let _ = worker_store.record_action(&worker_release.id, "failed", &error);
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: worker_release.id.clone(),
                    stage: "failed".into(),
                    progress: 0,
                    detail: error.clone(),
                    bytes_done: worker_release.size,
                    bytes_total: worker_release.size,
                },
            );
            return Err(error);
        }
        if let Err(error) = worker_store.install_staged(&worker_release) {
            if matches!(
                worker_store.recover_component(&worker_release.id),
                Ok(Some(updates::RecoveryResult::Completed(_)))
            ) {
                let _ = worker_store.record_action(
                    &worker_release.id,
                    "installed",
                    "Offline runtime installed successfully after journal recovery",
                );
            } else {
                let _ = worker_store.record_action(&worker_release.id, "failed", &error);
                emit_component_update_progress(
                    &worker_app,
                    ComponentUpdateProgress {
                        component_id: worker_release.id.clone(),
                        stage: "failed".into(),
                        progress: 0,
                        detail: error.clone(),
                        bytes_done: worker_release.size,
                        bytes_total: worker_release.size,
                    },
                );
                return Err(error);
            }
        }
        emit_component_update_progress(
            &worker_app,
            ComponentUpdateProgress {
                component_id: worker_release.id.clone(),
                stage: "complete".into(),
                progress: 100,
                detail: "Offline runtime installed successfully.".into(),
                bytes_done: worker_release.size,
                bytes_total: worker_release.size,
            },
        );
        Ok(ComponentUpdateResult {
            component_id: worker_release.id,
            state: "installed".into(),
            restart_required: false,
        })
    })
    .await;

    if let Ok(mut running) = state.component_update_job.lock() {
        *running = false;
    }
    let result =
        worker_result.map_err(|error| format!("Offline runtime worker failed: {error}"))?;
    if release.id == "runtime.cot" && result.is_ok() {
        let _ = cleanup_legacy_runtime_cache_if_ready();
    }
    result
}

#[tauri::command]
async fn install_offline_localization_pack(
    path: String,
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<LocalizationPackInstallResult, String> {
    let package_path = PathBuf::from(path);
    let staging_root = std::env::temp_dir().join(format!(
        "mojorecomp-localization-pack-{}-{}",
        std::process::id(),
        timestamp_seconds()
    ));
    let inspect_path = package_path.clone();
    let inspect_staging = staging_root.clone();
    let pack = tauri::async_runtime::spawn_blocking(move || {
        updates::extract_offline_localization_pack(&inspect_path, &inspect_staging)
    })
    .await
    .map_err(|error| format!("Localization Pack validation worker failed: {error}"))??;

    let validation =
        (|| -> Result<(Vec<updates::ComponentRelease>, updates::ComponentStore), String> {
            manifest(&state, &pack.game_id)?;
            let store = game_component_store()?;
            let current_launcher = env!("CARGO_PKG_VERSION").to_string();
            let mut releases = Vec::with_capacity(pack.languages.len());
            for language in &pack.languages {
                let release =
                    updates::inspect_offline_package(&language.package_path, &current_launcher)?;
                if release.kind != updates::ComponentKind::Language
                    || release.game_id.as_deref() != Some(pack.game_id.as_str())
                    || release.id != language.id
                    || release.version != language.version
                    || release.locale.as_deref() != Some(language.locale.as_str())
                    || release.display_name.as_deref() != Some(language.display_name.as_str())
                    || release.xbox_language != Some(language.xbox_language)
                    || release.translation_version.as_deref() != language.translation_version.as_deref()
                {
                    return Err(
                        "Localization Pack language metadata does not match its signed component"
                            .into(),
                    );
                }
                let installed = installed_components_for_compatibility(&state.manifests, &release)?;
                updates::validate_release_compatibility(
                    &release,
                    env!("CARGO_PKG_VERSION"),
                    &installed,
                )?;
                releases.push(release);
            }
            Ok((releases, store))
        })();
    let (releases, store) = match validation {
        Ok(value) => value,
        Err(error) => {
            let _ = fs::remove_dir_all(&staging_root);
            return Err(error);
        }
    };

    let operation_ready = (|| -> Result<(), String> {
        let _operation_gate = state
            .operation_gate
            .lock()
            .map_err(|_| "Operation gate lock failed")?;
        ensure_library_idle(&state)?;
        ensure_game_stopped(&pack.game_id, &state)?;
        if state
            .setup_jobs
            .lock()
            .map_err(|_| "Game setup state lock failed")?
            .values()
            .any(|running| *running)
        {
            let _ = fs::remove_dir_all(&staging_root);
            return Err(
                "Wait for game setup to finish before installing a Localization Pack".into(),
            );
        }
        if !state
            .localization_jobs
            .lock()
            .map_err(|_| "Localization state lock failed")?
            .is_empty()
        {
            let _ = fs::remove_dir_all(&staging_root);
            return Err(
                "Wait for localization work to finish before installing a Localization Pack".into(),
            );
        }
        let mut update_running = state
            .component_update_job
            .lock()
            .map_err(|_| "Component update state lock failed")?;
        if *update_running {
            return Err("Another component update is already running".into());
        }
        *update_running = true;
        Ok(())
    })();
    if let Err(error) = operation_ready {
        let _ = fs::remove_dir_all(&staging_root);
        return Err(error);
    }

    let worker_store = store.clone();
    let worker_app = app.clone();
    let worker_packages = pack
        .languages
        .iter()
        .map(|language| language.package_path.clone())
        .collect::<Vec<_>>();
    let worker_releases = releases.clone();
    let worker_result = tauri::async_runtime::spawn_blocking(move || {
        let mut installed_languages = Vec::new();
        for (index, (release, package)) in worker_releases
            .iter()
            .zip(worker_packages.iter())
            .enumerate()
        {
            let ordinal = index + 1;
            let total = worker_releases.len();
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: release.id.clone(),
                    stage: "validating".into(),
                    progress: 20,
                    detail: format!(
                        "Validating Localization Pack language {ordinal} of {total}..."
                    ),
                    bytes_done: 0,
                    bytes_total: release.size,
                },
            );
            worker_store.stage_local_artifact(release, package)?;
            worker_store.prepare_repair(&release.id, &release.version)?;
            worker_store.install_staged(release)?;
            installed_languages.push(release.locale.clone().unwrap_or_else(|| release.id.clone()));
            emit_component_update_progress(
                &worker_app,
                ComponentUpdateProgress {
                    component_id: release.id.clone(),
                    stage: "complete".into(),
                    progress: 100,
                    detail: format!("Installed Localization Pack language {ordinal} of {total}."),
                    bytes_done: release.size,
                    bytes_total: release.size,
                },
            );
        }
        Ok::<_, String>(installed_languages)
    })
    .await;

    if let Ok(mut running) = state.component_update_job.lock() {
        *running = false;
    }
    let _ = fs::remove_dir_all(&staging_root);
    let installed_languages = worker_result
        .map_err(|error| format!("Localization Pack install worker failed: {error}"))??;
    append_launcher_log(&format!(
        "Installed Localization Pack {} for {} ({} language component(s))",
        pack.version,
        pack.game_id,
        installed_languages.len()
    ));
    Ok(LocalizationPackInstallResult {
        game_id: pack.game_id,
        version: pack.version,
        installed_languages,
    })
}

#[tauri::command]
fn activate_runtime_version(
    component_id: String,
    version: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let _operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    let game_id = ensure_runtime_version_operation_idle(&component_id, &state)?;
    let store = game_component_store()?;
    store.activate_version(&component_id, &version)?;
    append_launcher_log(&format!(
        "Activated {} runtime version {} for {}",
        component_id, version, game_id
    ));
    Ok(())
}

#[tauri::command]
fn remove_runtime_version(
    component_id: String,
    version: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let _operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    let game_id = ensure_runtime_version_operation_idle(&component_id, &state)?;
    let store = game_component_store()?;
    store.remove_version(&component_id, &version)?;
    append_launcher_log(&format!(
        "Removed {} runtime version {} for {}",
        component_id, version, game_id
    ));
    Ok(())
}

#[tauri::command]
fn apply_launcher_update(
    state: tauri::State<'_, AppState>,
    app: tauri::AppHandle,
) -> Result<(), String> {
    let _operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_component_update_idle(&state)?;
    ensure_library_idle(&state)?;
    for game in &state.manifests {
        ensure_game_stopped(&game.id, &state)?;
    }
    if state
        .setup_jobs
        .lock()
        .map_err(|_| "Game setup state lock failed")?
        .values()
        .any(|running| *running)
    {
        return Err("Wait for game setup to finish before restarting the launcher".into());
    }
    if !state
        .localization_jobs
        .lock()
        .map_err(|_| "Localization state lock failed")?
        .is_empty()
    {
        return Err("Wait for localization work to finish before restarting the launcher".into());
    }

    let prepared = launcher_component_store()?.prepare_launcher_replacement()?;
    game_component_store()?.ensure_launcher_version_compatible(&prepared.version)?;
    let current_exe = std::env::current_exe()
        .map_err(|error| format!("Could not resolve current launcher executable: {error}"))?;
    if !current_exe
        .file_name()
        .and_then(|value| value.to_str())
        .is_some_and(|name| name.eq_ignore_ascii_case("mojorecomp-launcher.exe"))
    {
        return Err("Launcher self-update is only available from a packaged portable build".into());
    }
    let helper = std::env::temp_dir().join(format!(
        "mojorecomp-launcher-update-{}-{}.exe",
        std::process::id(),
        timestamp_seconds()
    ));
    fs::copy(&current_exe, &helper)
        .map_err(|error| format!("Could not prepare launcher update helper: {error}"))?;
    Command::new(&helper)
        .arg("--apply-launcher-update")
        .arg(std::process::id().to_string())
        .arg(&prepared.source_root)
        .arg(&current_exe)
        .spawn()
        .map_err(|error| format!("Could not start launcher update helper: {error}"))?;
    append_launcher_log(&format!(
        "Applying verified launcher update {} on restart",
        prepared.version
    ));
    app.exit(0);
    Ok(())
}

#[tauri::command]
fn rollback_component_update(
    component_id: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let _operation_gate = state
        .operation_gate
        .lock()
        .map_err(|_| "Operation gate lock failed")?;
    ensure_component_update_idle(&state)?;
    let store = if component_id == "launcher" {
        launcher_component_store()?
    } else {
        game_component_store()?
    };
    let game_id = component_id.strip_prefix("runtime.").or_else(|| {
        component_id
            .strip_prefix("language.")
            .and_then(|rest| rest.split('.').next())
    });
    if let Some(game_id) = game_id {
        ensure_game_stopped(game_id, &state)?;
    }
    match store.rollback(&component_id) {
        Ok(()) => Ok(()),
        Err(error)
            if (component_id == "runtime.cot" || component_id.starts_with("language."))
                && error.contains("no previous version") =>
        {
            store.deactivate(&component_id)
        }
        Err(error) => Err(error),
    }
}

#[tauri::command]
fn open_update_url(url: String) -> Result<(), String> {
    let url = updates::validate_resolved_public_https_url(&url)?;
    Command::new("explorer.exe")
        .arg(url.as_str())
        .spawn()
        .map_err(|e| format!("Could not open the update page: {e}"))?;
    Ok(())
}

fn main() {
    if let Some(exit_code) = run_launcher_update_helper_from_args() {
        std::process::exit(exit_code);
    }
    cleanup_launcher_update_helpers();
    let manifests = GAME_MANIFESTS
        .iter()
        .map(|text| toml::from_str::<GameManifest>(text))
        .collect::<Result<Vec<_>, _>>()
        .expect("embedded game manifests must be valid TOML");
    let suite: SuiteManifest =
        toml::from_str(SUITE_MANIFEST).expect("embedded suite manifest must be valid TOML");
    let _ = &suite.release_channel;
    let migration_recovery_notice = storage_layout()
        .and_then(|layout| layout.recover_library_migration())
        .unwrap_or_else(|error| Some(format!("Game library recovery failed: {error}")));
    let mut component_recovery = Vec::new();
    match launcher_component_store().and_then(|store| {
        store.cleanup_launcher_ready(env!("CARGO_PKG_VERSION"))?;
        store.recover_all()
    }) {
        Ok(recovered) => component_recovery.extend(recovered),
        Err(error) => append_launcher_log(&format!("Launcher update recovery failed: {error}")),
    }
    match game_component_store().and_then(|store| store.recover_all()) {
        Ok(recovered) => component_recovery.extend(recovered),
        Err(error) => append_launcher_log(&format!("Game component recovery failed: {error}")),
    }
    match migrate_legacy_game_components() {
        Ok(report) => {
            for id in report.migrated {
                append_launcher_log(&format!(
                    "Migrated legacy Local AppData component into the game library: {id}"
                ));
            }
            for (id, error) in report.deferred {
                append_launcher_log(&format!(
                    "Legacy component migration was deferred for {id}: {error}"
                ));
            }
        }
        Err(error) => append_launcher_log(&format!(
            "Legacy game component migration was deferred: {error}"
        )),
    }
    if let Some(cot) = manifests.iter().find(|game| game.id == "cot") {
        match migrate_legacy_embedded_runtime_cache(&cot.runtime_version) {
            Ok(true) => append_launcher_log(
                "Migrated validated embedded runtime cache into the game library",
            ),
            Ok(false) => {}
            Err(error) => append_launcher_log(&format!(
                "Legacy embedded runtime migration was deferred: {error}"
            )),
        }
    }
    match cleanup_legacy_runtime_cache_if_ready() {
        Ok(true) => append_launcher_log(
            "Removed legacy Local AppData runtime cache after validating runtime.cot",
        ),
        Ok(false) => {}
        Err(error) => append_launcher_log(&format!(
            "Legacy runtime cache cleanup was deferred: {error}"
        )),
    }
    match cleanup_legacy_component_storage_if_ready() {
        Ok(ids) => {
            for id in ids {
                append_launcher_log(&format!(
                    "Removed replaced legacy Local AppData component after validation: {id}"
                ));
            }
        }
        Err(error) => {
            append_launcher_log(&format!("Legacy component cleanup was deferred: {error}"))
        }
    }
    migrate_legacy_diagnostics();
    if let Some(notice) = migration_recovery_notice {
        append_launcher_log(&notice);
    }
    for recovery in component_recovery {
        match recovery {
            updates::RecoveryResult::Completed(id) => {
                append_launcher_log(&format!("Recovered completed component update: {id}"));
            }
            updates::RecoveryResult::RolledBack(id) => {
                append_launcher_log(&format!("Rolled back interrupted component update: {id}"));
            }
        }
    }
    append_launcher_log(&format!(
        "Launcher {} started on {} channel",
        env!("CARGO_PKG_VERSION"),
        suite.release_channel
    ));

    let discord_activity_enabled = storage_layout()
        .and_then(|layout| layout.load_launcher_settings())
        .ok()
        .flatten()
        .map(|settings| settings.discord_activity_enabled)
        .unwrap_or(true);
    let presence = presence::PresenceController::new(
        discord_activity_enabled,
        configured_discord_application_id(&suite),
    );
    let runtime_job = RuntimeJob::new().expect("runtime Job Object must be available");
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(AppState {
            manifests,
            processes: Mutex::new(HashMap::new()),
            setup_jobs: Mutex::new(HashMap::new()),
            localization_jobs: Mutex::new(HashMap::new()),
            library_job: Mutex::new(false),
            component_update_job: Mutex::new(false),
            operation_gate: Mutex::new(()),
            runtime_job,
            presence,
        })
        .on_window_event(|window, event| {
            if matches!(event, tauri::WindowEvent::CloseRequested { .. }) {
                let state = window.app_handle().state::<AppState>();
                state.presence.shutdown();
                terminate_all_processes(&state.processes);
            }
        })
        .invoke_handler(tauri::generate_handler![
            get_launcher_storage,
            complete_game_language_setup,
            set_discord_activity_enabled,
            inspect_game_library,
            set_game_library,
            open_game_library,
            list_games,
            import_game_iso,
            uninstall_game,
            load_settings,
            save_settings,
            localization_status,
            prepare_localization,
            cancel_localization,
            probe_hardware,
            launch_game,
            process_status,
            open_title_folder,
            open_license_notices,
            create_support_package,
            check_component_updates,
            runtime_additional_content,
            check_launcher_update,
            install_component_update,
            install_runtime_additional_content,
            install_offline_runtime_package,
            install_offline_localization_pack,
            activate_runtime_version,
            remove_runtime_version,
            apply_launcher_update,
            rollback_component_update,
            open_update_url
        ])
        .run(tauri::generate_context!())
        .expect("error while running MojoRecomp Launcher");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cot_runtime_history_keeps_legacy_release_as_archived_only() {
        let history = archived_runtime_releases("runtime.cot").expect("COT runtime history");
        assert_eq!(history.len(), 1);
        assert_eq!(history[0].version, "0.1.0-alpha");
        assert_eq!(history[0].published, "2026-09-29");
        assert!(!history[0].downloadable);
        assert_eq!(history[0].size, 0);
        assert!(history[0].notes_url.ends_with("/releases/tag/v1.0.0"));
        assert!(archived_runtime_releases("runtime.mom")
            .expect("MOM runtime history")
            .is_empty());
    }

    #[test]
    fn cot_local_runtime_history_keeps_current_manifest_release_visible() {
        let manifest: GameManifest =
            toml::from_str(GAME_MANIFESTS[0]).expect("embedded COT manifest");
        let history = local_runtime_releases(&manifest, "runtime.cot")
            .expect("COT local runtime release history");

        assert_eq!(history[0].version, manifest.runtime_version);
        assert!(!history[0].downloadable);
        assert!(history.iter().any(|release| release.version == "0.1.0-alpha"));
    }

    #[test]
    fn runtime_processes_disable_the_obs_vulkan_capture_layer() {
        let mut command = Command::new("runtime-placeholder");
        apply_runtime_compatibility_environment(&mut command);

        let configured = command
            .get_envs()
            .find(|(name, _)| *name == std::ffi::OsStr::new(OBS_VULKAN_CAPTURE_DISABLE_ENV))
            .and_then(|(_, value)| value);

        assert_eq!(configured, Some(std::ffi::OsStr::new("1")));
    }

    fn isolated_test_root() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "mojorecomp-launcher-test-{}-{}",
            std::process::id(),
            nonce
        ))
    }

    #[test]
    fn lgpl_library_override_replaces_the_embedded_payload() {
        let root = isolated_test_root();
        let payload = root.join("runtime");
        let overrides = root.join("lgpl-overrides");
        fs::create_dir_all(&overrides).expect("override directory");
        fs::write(overrides.join("library.dll"), b"modified-library").expect("write override");

        let path = materialize_replaceable_library_at(
            &payload,
            &overrides,
            "library.dll",
            b"embedded-library",
        )
        .expect("materialize override");

        assert_eq!(
            fs::read(path).expect("read materialized library"),
            b"modified-library"
        );
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn support_text_sanitization_hides_sensitive_paths() {
        let game_root = PathBuf::from(r"D:\Games\Crash of the Titans");
        let mut sample = format!(
            "game={} app={} user={}",
            game_root.display(),
            app_root().expect("app root").display(),
            std::env::var("USERPROFILE").unwrap_or_else(|_| r"C:\Users\Unknown".into())
        );
        sample = sanitize_text(sample, Some(&game_root));
        assert!(sample.contains("<GAME_ROOT>"));
        assert!(sample.contains("<APP_DATA>"));
        assert!(sample.contains("<USER_PROFILE>"));
        assert!(!sample.contains(&game_root.to_string_lossy().to_string()));
    }

    #[test]
    fn support_crash_incident_pairs_only_matching_artifacts() {
        let root = isolated_test_root();
        fs::create_dir_all(&root).expect("crash directory");
        let older = root.join("crash-2026-10-05-120000-pid10.log");
        let newer = root.join("crash-2026-10-05-120001-pid11.log");
        fs::write(&older, b"older").expect("older log");
        std::thread::sleep(Duration::from_millis(20));
        fs::write(&newer, b"newer").expect("newer log");
        fs::write(newer.with_extension("json"), b"{}\n").expect("matching json");
        fs::write(older.with_extension("dmp"), b"MDMPold").expect("different dump");

        let incident = newest_crash_incident(&root).expect("newest crash incident");
        assert_eq!(incident.log, newer);
        assert!(incident.json.is_some());
        assert!(incident.dump.is_none());

        fs::write(incident.log.with_extension("dmp"), b"MDMPnew").expect("matching dump");
        let paired = newest_crash_incident(&root).expect("paired crash incident");
        assert_eq!(paired.dump, Some(paired.log.with_extension("dmp")));
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn support_binary_zip_entry_preserves_bytes_without_text_sanitization() {
        let root = isolated_test_root();
        fs::create_dir_all(&root).expect("support directory");
        let source = root.join("crash.dmp");
        let payload = [0x4d, 0x44, 0x4d, 0x50, 0x00, 0xff, 0x10, 0x7f];
        fs::write(&source, payload).expect("binary support artifact");
        let archive_path = root.join("support.zip");
        let file = File::create(&archive_path).expect("support archive");
        let mut writer = ZipWriter::new(file);
        add_binary_file_to_zip(
            &mut writer,
            "crash/test.dmp",
            &source,
            SUPPORT_BINARY_FILE_LIMIT,
        )
        .expect("stream binary artifact");
        writer.finish().expect("finish support archive");

        let file = File::open(&archive_path).expect("open support archive");
        let mut archive = zip::ZipArchive::new(file).expect("parse support archive");
        let mut entry = archive.by_name("crash/test.dmp").expect("binary entry");
        let mut restored = Vec::new();
        entry.read_to_end(&mut restored).expect("read binary entry");
        assert_eq!(restored, payload);
        drop(entry);
        drop(archive);
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn support_timestamp_is_utc_iso8601() {
        let timestamp = utc_timestamp_iso8601();
        assert_eq!(timestamp.len(), 20);
        assert_eq!(&timestamp[4..5], "-");
        assert_eq!(&timestamp[7..8], "-");
        assert_eq!(&timestamp[10..11], "T");
        assert_eq!(&timestamp[19..20], "Z");
    }

    #[test]
    fn cot_manifest_validation_requires_expected_game_files() {
        let root = isolated_test_root();
        fs::create_dir_all(&root).expect("game root");
        let manifest: GameManifest =
            toml::from_str(GAME_MANIFESTS[0]).expect("embedded COT manifest");

        let missing = validate_root(&manifest, &root);
        assert!(!missing.valid);
        assert!(missing.missing.contains(&"default.xex".to_string()));
        assert!(missing.missing.contains(&"default.rcf".to_string()));

        fs::write(root.join("default.xex"), b"test-xex").expect("test xex");
        fs::write(root.join("default.rcf"), b"test-rcf").expect("test rcf");
        let valid = validate_root(&manifest, &root);
        assert!(valid.valid);
        assert!(valid.missing.is_empty());

        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn library_reserve_includes_known_game_size_and_safety_margin() {
        let manifests = GAME_MANIFESTS
            .iter()
            .map(|text| toml::from_str::<GameManifest>(text).expect("embedded game manifest"))
            .collect::<Vec<_>>();
        let cot = &manifests[0];
        let known_cot_bytes = cot
            .files
            .integrity
            .iter()
            .fold(0u64, |total, entry| total.saturating_add(entry.size));

        assert_eq!(
            minimum_library_free_bytes(&manifests),
            known_cot_bytes.saturating_add(DISK_MINIMUM_MARGIN_BYTES)
        );
        assert!(minimum_library_free_bytes(&manifests) > 2 * 1024 * 1024 * 1024);
    }

    #[test]
    fn uninstall_transient_cleanup_is_scoped_to_the_selected_game() {
        let root = isolated_test_root();
        let staging = root.join(".mojorecomp").join("staging").join("games");
        fs::create_dir_all(staging.join("cot")).expect("cot staging");
        fs::write(staging.join("cot").join("partial.bin"), b"partial").expect("cot staging file");
        fs::write(staging.join("cot-extract-xiso.log"), b"log").expect("cot extraction log");
        fs::create_dir_all(root.join(".backup").join("cot-100")).expect("cot backup");
        fs::write(root.join(".backup").join("cot-100").join("old.bin"), b"old")
            .expect("cot backup file");
        fs::create_dir_all(root.join(".backup").join("mom-100")).expect("mom backup");
        fs::write(
            root.join(".backup").join("mom-100").join("keep.bin"),
            b"keep",
        )
        .expect("mom backup file");

        cleanup_game_transients(&root, "cot").expect("cleanup cot transients");

        assert!(!staging.join("cot").exists());
        assert!(!staging.join("cot-extract-xiso.log").exists());
        assert!(!root.join(".backup").join("cot-100").exists());
        assert!(
            root.join(".backup")
                .join("mom-100")
                .join("keep.bin")
                .is_file()
        );

        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn library_transient_cleanup_does_not_turn_an_empty_library_into_a_migration() {
        let root = isolated_test_root();
        initialize_library(&root).expect("initialize library");
        fs::create_dir_all(root.join(".staging").join("cot")).expect("legacy staging");
        fs::write(root.join(".staging").join("cot").join("partial.bin"), b"partial")
            .expect("legacy staging file");
        fs::create_dir_all(root.join(".backup").join("cot-old")).expect("legacy backup");
        fs::write(root.join(".backup").join("cot-old").join("old.bin"), b"old")
            .expect("legacy backup file");
        fs::create_dir_all(root.join(".mojorecomp").join("downloads").join("runtime.cot"))
            .expect("component download staging");
        fs::write(
            root.join(".mojorecomp")
                .join("downloads")
                .join("runtime.cot")
                .join("partial.zip"),
            b"partial",
        )
        .expect("partial component download");
        fs::create_dir_all(
            root.join(".mojorecomp")
                .join("staging")
                .join("components")
                .join("runtime.cot"),
        )
        .expect("component staging");
        fs::create_dir_all(root.join(".mojorecomp").join("components"))
            .expect("empty component store");

        cleanup_library_transients(&root).expect("cleanup library transients");

        assert!(!library_has_persistent_data(&root).expect("inspect cleaned library"));
        assert!(!root.join(".staging").exists());
        assert!(!root.join(".backup").exists());
        assert!(!root.join(".mojorecomp").exists());
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn library_transient_cleanup_preserves_installed_components_as_persistent_data() {
        let root = isolated_test_root();
        initialize_library(&root).expect("initialize library");
        let component = root
            .join(".mojorecomp")
            .join("components")
            .join("runtime.cot")
            .join("versions")
            .join("0.2.0")
            .join("payload");
        fs::create_dir_all(&component).expect("runtime component payload");
        fs::write(component.join("cot-runtime.exe"), b"runtime").expect("runtime payload");
        fs::create_dir_all(root.join(".mojorecomp").join("downloads").join("runtime.cot"))
            .expect("download staging");
        fs::write(
            root.join(".mojorecomp")
                .join("downloads")
                .join("runtime.cot")
                .join("partial.zip"),
            b"partial",
        )
        .expect("partial download");

        cleanup_library_transients(&root).expect("cleanup library transients");

        assert!(component.join("cot-runtime.exe").is_file());
        assert!(library_has_persistent_data(&root).expect("inspect runtime library"));
        assert!(!root.join(".mojorecomp").join("downloads").exists());
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn integrity_validation_rejects_modified_game_files() {
        let root = isolated_test_root();
        fs::create_dir_all(&root).expect("game root");
        let expected = FileIntegrity {
            path: "default.xex".into(),
            size: 19,
            sha256: "0d4efa5af028288140b379b93fdd09967aeca77cbe6fdbd060561d1adf68019d".into(),
            verify_on_launch: true,
        };

        fs::write(root.join("default.xex"), b"supported-game-data").expect("test game file");
        validate_integrity_entry(&root, &expected).expect("matching game file");

        fs::write(root.join("default.xex"), b"modified-game-data!").expect("modified game file");
        assert!(validate_integrity_entry(&root, &expected).is_err());

        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn runtime_settings_round_trip_preserves_public_options() {
        let mut settings = RuntimeSettings::default();
        settings.display.mode = "fullscreen".into();
        settings.display.resolution_scale = 3;
        settings.display.aspect_ratio = "21:9".into();
        settings.display.vsync = true;
        settings.graphics.anti_aliasing = "fxaa_extreme".into();
        settings.graphics.texture_filtering = "16x".into();
        settings.graphics.frame_rate = "60".into();
        settings.advanced.logging_enabled = false;
        settings.localization.profile = "pt-BR".into();
        settings.localization.xbox_language = 1;

        let serialized = toml::to_string_pretty(&settings).expect("serialize settings");
        assert!(!serialized.contains("debug_mode_on_start"));
        let restored: RuntimeSettings = toml::from_str(&serialized).expect("parse settings");
        assert_eq!(restored.schema_version, 1);
        assert_eq!(restored.localization.profile, "pt-BR");
        assert_eq!(restored.localization.xbox_language, 1);
        assert_eq!(restored.display.mode, "fullscreen");
        assert_eq!(restored.display.resolution_scale, 3);
        assert_eq!(restored.display.aspect_ratio, "21:9");
        assert!(restored.display.vsync);
        assert_eq!(restored.graphics.anti_aliasing, "fxaa_extreme");
        assert_eq!(restored.graphics.texture_filtering, "16x");
        assert_eq!(restored.graphics.frame_rate, "60");
        assert!(!restored.advanced.logging_enabled);
    }

    #[test]
    fn runtime_settings_defaults_are_balanced_and_native() {
        let settings = RuntimeSettings::default();
        assert_eq!(settings.display.resolution_scale, 1);
        assert_eq!(settings.display.aspect_ratio, "16:9");
        assert!(!settings.display.vsync);
        assert_eq!(settings.graphics.anti_aliasing, "fxaa_extreme");
        assert_eq!(settings.graphics.texture_filtering, "8x");
        assert_eq!(settings.graphics.frame_rate, "30");
    }

    #[test]
    fn legacy_settings_enable_logging_by_default() {
        let legacy = r#"
schema_version = 1

[display]
mode = "windowed"
monitor = "primary"
output_resolution = "desktop"
resolution_scale = 1
aspect_ratio = "16:9"
vsync = false

[graphics]
anti_aliasing = "off"
texture_filtering = "default"

[advanced]
"#;
        let restored: RuntimeSettings = toml::from_str(legacy).expect("parse legacy settings");
        assert!(restored.advanced.logging_enabled);
        assert_eq!(restored.graphics.frame_rate, "30");
        assert_eq!(restored.localization.profile, "en");
        assert_eq!(restored.localization.xbox_language, 1);
    }

    #[test]
    fn partial_localization_settings_keep_english_xbox_language_default() {
        let partial = r#"
schema_version = 1

[localization]
profile = "pt-BR"

[display]
mode = "windowed"
monitor = "primary"
output_resolution = "desktop"
resolution_scale = 1
aspect_ratio = "16:9"
vsync = false

[graphics]
anti_aliasing = "off"
texture_filtering = "default"

[advanced]
"#;
        let restored: RuntimeSettings =
            toml::from_str(partial).expect("parse partial localization settings");
        assert_eq!(restored.localization.profile, "pt-BR");
        assert_eq!(restored.localization.xbox_language, 1);
        assert_eq!(restored.graphics.frame_rate, "30");
    }

    #[test]
    fn localization_profiles_are_title_scoped_and_keep_english_voice_language() {
        assert_eq!(localization_xbox_language("cot", "en"), Some(1));
        assert_eq!(localization_xbox_language("cot", "de"), Some(3));
        assert_eq!(localization_xbox_language("cot", "fr"), Some(4));
        assert_eq!(localization_xbox_language("cot", "es"), Some(5));
        assert_eq!(localization_xbox_language("cot", "it"), Some(6));
        assert_eq!(localization_xbox_language("cot", "nl"), Some(16));
        assert_eq!(localization_xbox_language("cot", "pt-BR"), Some(1));
        assert_eq!(localization_xbox_language("mom", "pt-BR"), None);
        assert_eq!(localization_xbox_language("cot", "ja"), None);
    }

    #[test]
    fn launcher_update_catalog_is_empty_or_uses_public_https() {
        let suite = parsed_suite_manifest().expect("suite manifest");
        assert_eq!(suite.version, "1.1.0");
        assert_eq!(suite.release_channel, "development");
        if !suite.update_catalog.is_empty() {
            updates::validate_public_https_url(&suite.update_catalog)
                .expect("configured update catalog must use public HTTPS");
        }
        if !suite.localization_catalog.is_empty() {
            updates::validate_public_https_url(&suite.localization_catalog)
                .expect("configured localization catalog must use public HTTPS");
        }
    }

    #[test]
    fn missing_runtime_component_is_reported_as_not_installed() {
        assert_eq!(
            local_runtime_install_state(None, false),
            (None, "not_installed")
        );
        let active = updates::ActiveComponentStatus {
            id: "runtime.cot".into(),
            version: "0.1.0-alpha".into(),
            healthy: true,
            repair_required: false,
            can_rollback: false,
            last_action: None,
        };
        assert_eq!(
            local_runtime_install_state(Some(&active), true),
            (Some("0.1.0-alpha".into()), "up_to_date")
        );
        assert_eq!(
            local_runtime_install_state(Some(&active), false),
            (Some("0.1.0-alpha".into()), "corrupted")
        );
    }

    #[test]
    fn licenses_and_third_party_notices_are_embedded_and_materialize() {
        assert!(PROJECT_LICENSE.contains("ISC License"));
        assert!(PROJECT_LICENSE.contains("Alex \"OAleex\" Félix"));
        assert!(THIRD_PARTY_NOTICE_SUMMARY.contains("Third-Party Notice Inventory"));
        assert!(
            THIRD_PARTY_LICENSES
                .iter()
                .any(|(name, _)| *name == "Cargo-ThirdPartyNotices.txt")
        );
        assert!(
            THIRD_PARTY_LICENSES
                .iter()
                .any(|(name, _)| *name == "Npm-ThirdPartyNotices.txt")
        );
        assert!(
            THIRD_PARTY_LICENSES
                .iter()
                .any(|(name, _)| *name == "DXC-ThirdPartyNotices.txt")
        );

        let root = isolated_test_root().join("licenses");
        let summary = materialize_license_notices(&root).expect("materialize notices");
        assert_eq!(summary, root.join("THIRD_PARTY_NOTICES.md"));
        assert!(summary.is_file());
        assert_eq!(
            fs::read_to_string(root.join("LICENSE")).expect("read project license"),
            PROJECT_LICENSE
        );
        assert!(root.join("Cargo-ThirdPartyNotices.txt").is_file());
        assert!(root.join("Npm-ThirdPartyNotices.txt").is_file());
        assert!(root.join("FFmpeg-LGPL-2.1.txt").is_file());
        fs::remove_dir_all(root.parent().unwrap()).expect("notice test cleanup");
    }

    #[test]
    fn per_game_paths_are_isolated() {
        let layout = storage_layout().expect("storage layout");
        let title_data = layout.local_root().join("titles");
        let cot = title_root("cot").expect("cot root");
        let mom = title_root("mom").expect("mom root");
        assert_ne!(cot, mom);
        assert_eq!(cot.parent(), Some(title_data.as_path()));
        assert_eq!(mom.parent(), Some(title_data.as_path()));
        assert_eq!(cot.file_name().and_then(|name| name.to_str()), Some("cot"));
        assert_eq!(mom.file_name().and_then(|name| name.to_str()), Some("mom"));
        assert_ne!(save_root("cot").unwrap(), cot.join("save"));
        assert_ne!(cache_root("cot").unwrap(), cot.join("cache"));
        assert_ne!(settings_path("cot").unwrap(), settings_path("mom").unwrap());
    }

    #[test]
    fn legacy_migration_keeps_every_conflicting_file() {
        let root = isolated_test_root();
        let destination = root.join("settings.toml");
        let first = root.join("first.toml");
        let second = root.join("second.toml");
        fs::create_dir_all(&root).expect("test root");
        fs::write(&destination, b"current").expect("current file");
        fs::write(&first, b"legacy-one").expect("first legacy file");
        fs::write(&second, b"legacy-two").expect("second legacy file");

        move_file_preserving_existing(&first, &destination);
        move_file_preserving_existing(&second, &destination);

        let mut contents = fs::read_dir(&root)
            .expect("migrated files")
            .map(|entry| fs::read(entry.expect("directory entry").path()).expect("file contents"))
            .collect::<Vec<_>>();
        contents.sort();
        assert_eq!(
            contents,
            vec![
                b"current".to_vec(),
                b"legacy-one".to_vec(),
                b"legacy-two".to_vec()
            ]
        );
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[cfg(windows)]
    #[test]
    fn process_exit_detection_reports_clean_exit_and_removes_child() {
        let processes = Mutex::new(HashMap::<String, Child>::new());
        let child = Command::new("cmd.exe")
            .args(["/C", "exit /B 0"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn clean-exit child");
        processes.lock().unwrap().insert("cot".into(), child);

        let mut observed = None;
        for _ in 0..100 {
            let status = inspect_process_status("cot", &processes).expect("inspect process status");
            if !status.running {
                observed = Some(status);
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }

        let status = observed.expect("child exit was detected");
        assert_eq!(status.exit_code, Some(0));
        assert!(status.pid.is_none());
        assert!(processes.lock().unwrap().is_empty());
    }

    #[cfg(windows)]
    #[test]
    fn runtime_job_kills_assigned_process_when_job_closes() {
        let job = RuntimeJob::new().expect("create runtime job");
        let mut command = Command::new("cmd.exe");
        command
            .args(["/C", "ping -n 30 127.0.0.1 >nul"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        hide_child_console(&mut command);
        let mut child = command.spawn().expect("spawn long-running child");
        job.assign(&child).expect("assign child to job");
        assert!(child.try_wait().expect("initial child status").is_none());

        drop(job);
        let mut exited = false;
        for _ in 0..200 {
            if child
                .try_wait()
                .expect("child status after job close")
                .is_some()
            {
                exited = true;
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        if !exited {
            let _ = child.kill();
        }
        assert!(
            exited,
            "closing the launcher Job Object must terminate the runtime tree"
        );
    }

    #[test]
    fn extract_xiso_listing_parser_counts_only_files_and_bytes() {
        let listing = r#"
extract-xiso v2.7.1 (01.11.14) for win32 - written by in <in@fishtank.com>

listing test.iso:

\folder\ (0 bytes)
\folder\a.bin (1024 bytes)
\default.xex (5382144 bytes)
\empty.bin (0 bytes)
"#;
        let (files, bytes) = parse_iso_listing(listing);
        assert_eq!(files, 3);
        assert_eq!(bytes, 5_383_168);
    }

    #[test]
    fn managed_directory_cleanup_rejects_outside_paths() {
        let root = isolated_test_root();
        let managed = root.join("games");
        let outside = root.join("do-not-touch");
        fs::create_dir_all(&managed).expect("managed root");
        fs::create_dir_all(&outside).expect("outside root");
        fs::write(outside.join("marker.txt"), b"keep").expect("outside marker");

        assert!(remove_managed_directory(&outside, &managed).is_err());
        assert!(outside.join("marker.txt").is_file());

        fs::remove_dir_all(root).expect("test cleanup");
    }
}
