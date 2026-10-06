use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};

#[cfg(windows)]
use std::ffi::{OsString, c_void};
#[cfg(windows)]
use std::os::windows::ffi::OsStringExt;
#[cfg(windows)]
use windows_sys::Win32::{Globalization::lstrlenW, System::Com::CoTaskMemFree, UI::Shell};

const LIBRARY_SCHEMA_VERSION: u32 = 1;
const MIGRATION_MINIMUM_MARGIN: u64 = 1024 * 1024 * 1024;
pub const LIBRARY_MARKER: &str = ".mojorecomp-library.toml";

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct LauncherSettings {
    pub schema_version: u32,
    pub game_library: PathBuf,
    #[serde(default = "default_discord_activity_enabled")]
    pub discord_activity_enabled: bool,
    #[serde(default)]
    pub language_setup_completed_games: Vec<String>,
}

fn default_discord_activity_enabled() -> bool {
    true
}

impl LauncherSettings {
    pub fn new(game_library: PathBuf) -> Self {
        Self {
            schema_version: LIBRARY_SCHEMA_VERSION,
            game_library,
            discord_activity_enabled: true,
            language_setup_completed_games: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct LibraryMigrationPlan {
    pub source: PathBuf,
    pub destination: PathBuf,
    pub files: u64,
    pub bytes: u64,
    pub required_free_bytes: u64,
    pub same_volume: bool,
}

#[derive(Clone, Debug)]
pub struct LibraryMigrationOutcome {
    pub source_to_clean: Option<PathBuf>,
}

#[derive(Debug, Deserialize, Serialize)]
struct LibraryMigrationJournal {
    schema_version: u32,
    source: PathBuf,
    destination: PathBuf,
}

#[derive(Clone, Debug)]
pub struct StorageLayout {
    local_root: PathBuf,
    saved_games_root: PathBuf,
    executable_root: PathBuf,
    default_library_root: PathBuf,
}

impl StorageLayout {
    pub fn new(
        local_root: PathBuf,
        saved_games_root: PathBuf,
        executable_root: PathBuf,
        default_library_root: PathBuf,
    ) -> Self {
        Self {
            local_root,
            saved_games_root,
            executable_root,
            default_library_root,
        }
    }

    pub fn discover(executable_root: PathBuf) -> Result<Self, String> {
        let local_base = platform_local_app_data()
            .ok_or_else(|| "The Windows Local AppData folder is unavailable".to_string())?;
        let saved_games_base = platform_saved_games()
            .ok_or_else(|| "The Windows Saved Games folder is unavailable".to_string())?;
        let profile = platform_user_profile()
            .ok_or_else(|| "The Windows user profile folder is unavailable".to_string())?;
        Ok(Self::new(
            local_base.join("MojoRecomp"),
            saved_games_base.join("MojoRecomp"),
            executable_root,
            profile.join("Games").join("MojoRecomp-Games"),
        ))
    }

    pub fn local_root(&self) -> &Path {
        &self.local_root
    }

    pub fn saved_games_root(&self) -> &Path {
        &self.saved_games_root
    }

    pub fn settings_path(&self, game_id: &str) -> PathBuf {
        self.local_root
            .join("titles")
            .join(game_id)
            .join("settings.toml")
    }

    pub fn title_data_root(&self, game_id: &str) -> PathBuf {
        self.local_root.join("titles").join(game_id)
    }

    pub fn save_root(&self, game_id: &str) -> PathBuf {
        self.saved_games_root.join(game_id)
    }

    pub fn content_root(&self, game_id: &str) -> PathBuf {
        self.title_data_root(game_id).join("content")
    }

    pub fn utility_root(&self, game_id: &str) -> PathBuf {
        self.title_data_root(game_id).join("utility")
    }

    pub fn cache_root(&self, game_id: &str) -> PathBuf {
        self.local_root.join("cache").join(game_id)
    }

    pub fn logs_root(&self) -> PathBuf {
        self.local_root.join("logs")
    }

    pub fn game_logs_root(&self, game_id: &str) -> PathBuf {
        self.logs_root().join(game_id)
    }

    pub fn runtime_root(&self, version: &str) -> PathBuf {
        self.local_root.join("runtime").join(version)
    }

    pub fn components_root(&self) -> PathBuf {
        self.local_root.join("components")
    }

    pub fn licenses_root(&self) -> PathBuf {
        self.local_root.join("licenses")
    }

    pub fn support_root(&self) -> PathBuf {
        self.local_root.join("support")
    }

    pub fn launcher_settings_path(&self) -> PathBuf {
        self.local_root.join("launcher.toml")
    }

    fn migration_journal_path(&self) -> PathBuf {
        self.local_root.join("library-migration.toml")
    }

    pub fn begin_library_migration(&self, plan: &LibraryMigrationPlan) -> Result<(), String> {
        fs::create_dir_all(&self.local_root)
            .map_err(|error| format!("Could not create launcher data directory: {error}"))?;
        let journal = LibraryMigrationJournal {
            schema_version: LIBRARY_SCHEMA_VERSION,
            source: plan.source.clone(),
            destination: plan.destination.clone(),
        };
        let text = toml::to_string_pretty(&journal)
            .map_err(|error| format!("Could not serialize migration journal: {error}"))?;
        let path = self.migration_journal_path();
        let temporary = path.with_extension(format!("toml.{}.tmp", std::process::id()));
        let mut file = fs::File::create(&temporary)
            .map_err(|error| format!("Could not create migration journal: {error}"))?;
        file.write_all(text.as_bytes())
            .and_then(|_| file.sync_all())
            .map_err(|error| format!("Could not write migration journal: {error}"))?;
        fs::rename(&temporary, &path)
            .map_err(|error| format!("Could not activate migration journal: {error}"))
    }

    pub fn clear_library_migration_journal(&self) {
        let _ = fs::remove_file(self.migration_journal_path());
    }

    pub fn recover_library_migration(&self) -> Result<Option<String>, String> {
        let path = self.migration_journal_path();
        if !path.is_file() {
            return Ok(None);
        }
        let text = fs::read_to_string(&path)
            .map_err(|error| format!("Could not read migration journal: {error}"))?;
        let journal: LibraryMigrationJournal = toml::from_str(&text)
            .map_err(|error| format!("Could not parse migration journal: {error}"))?;
        if journal.schema_version != LIBRARY_SCHEMA_VERSION {
            return Err("Unsupported migration journal schema".into());
        }
        let source = normalize_library_path(&journal.source)?;
        let destination = normalize_library_path(&journal.destination)?;
        let staging = migration_staging_path(&destination)?;
        if staging.exists()
            && let Err(error) = fs::remove_dir_all(&staging)
        {
            return Ok(Some(format!(
                "A previous game library migration needs cleanup at {}: {error}",
                staging.display()
            )));
        }
        let configured_destination = self
            .load_launcher_settings()?
            .map(|settings| {
                comparable_path(&settings.game_library) == comparable_path(&destination)
            })
            .unwrap_or(false);
        let cleanup_target = if configured_destination {
            &source
        } else {
            &destination
        };
        if cleanup_target.exists()
            && let Err(error) = fs::remove_dir_all(cleanup_target)
        {
            return Ok(Some(format!(
                "A previous game library migration needs cleanup at {}: {error}",
                cleanup_target.display()
            )));
        }
        self.clear_library_migration_journal();
        Ok(None)
    }

    fn launcher_settings_backup_path(&self) -> PathBuf {
        self.local_root.join("launcher.toml.bak")
    }

    fn recover_launcher_settings(&self) -> Result<(), String> {
        let path = self.launcher_settings_path();
        let backup = self.launcher_settings_backup_path();
        if !path.exists() && backup.is_file() {
            fs::rename(&backup, &path)
                .map_err(|error| format!("Could not recover launcher settings: {error}"))?;
        }
        Ok(())
    }

    pub fn load_launcher_settings(&self) -> Result<Option<LauncherSettings>, String> {
        self.recover_launcher_settings()?;
        let path = self.launcher_settings_path();
        if !path.is_file() {
            return Ok(None);
        }
        let text = fs::read_to_string(&path)
            .map_err(|error| format!("Could not read launcher settings: {error}"))?;
        let mut settings: LauncherSettings = toml::from_str(&text)
            .map_err(|error| format!("Could not parse launcher settings: {error}"))?;
        if settings.schema_version != LIBRARY_SCHEMA_VERSION {
            return Err(format!(
                "Unsupported launcher settings schema: {}",
                settings.schema_version
            ));
        }
        settings.game_library = self.normalize_game_library(&settings.game_library)?;
        let backup = self.launcher_settings_backup_path();
        if backup.exists() {
            let _ = fs::remove_file(backup);
        }
        Ok(Some(settings))
    }

    pub fn save_launcher_settings(&self, settings: &LauncherSettings) -> Result<(), String> {
        if settings.schema_version != LIBRARY_SCHEMA_VERSION {
            return Err("Unsupported launcher settings schema".into());
        }
        let mut normalized_settings = settings.clone();
        normalized_settings.game_library = self.normalize_game_library(&settings.game_library)?;
        fs::create_dir_all(&self.local_root)
            .map_err(|error| format!("Could not create launcher data directory: {error}"))?;
        self.recover_launcher_settings()?;
        let text = toml::to_string_pretty(&normalized_settings)
            .map_err(|error| format!("Could not serialize launcher settings: {error}"))?;
        let path = self.launcher_settings_path();
        let temporary = path.with_extension(format!("toml.{}.tmp", std::process::id()));
        let mut file = fs::File::create(&temporary)
            .map_err(|error| format!("Could not create launcher settings: {error}"))?;
        file.write_all(text.as_bytes())
            .and_then(|_| file.sync_all())
            .map_err(|error| format!("Could not write launcher settings: {error}"))?;
        let backup = self.launcher_settings_backup_path();
        if backup.exists() {
            fs::remove_file(&backup)
                .map_err(|error| format!("Could not clear stale launcher backup: {error}"))?;
        }
        if path.exists() {
            fs::rename(&path, &backup)
                .map_err(|error| format!("Could not stage previous launcher settings: {error}"))?;
        }
        if let Err(error) = fs::rename(&temporary, &path) {
            if backup.exists() {
                let _ = fs::rename(&backup, &path);
            }
            return Err(format!("Could not activate launcher settings: {error}"));
        }
        if backup.exists() {
            let _ = fs::remove_file(backup);
        }
        Ok(())
    }

    pub fn default_library_root(&self) -> PathBuf {
        self.default_library_root.clone()
    }

    pub fn normalize_game_library(&self, path: &Path) -> Result<PathBuf, String> {
        let normalized = normalize_library_path(path)?;
        if paths_overlap(&normalized, &self.local_root)? {
            return Err("The game library must be separate from Local AppData".into());
        }
        if paths_overlap(&normalized, &self.saved_games_root)? {
            return Err("The game library must be separate from Saved Games".into());
        }
        Ok(normalized)
    }

    pub fn legacy_portable_root(&self) -> PathBuf {
        self.executable_root.join("userdata")
    }

    pub fn legacy_game_library(&self) -> PathBuf {
        self.executable_root.join("games")
    }
}

#[cfg(windows)]
fn known_folder(folder_id: windows_sys::core::GUID) -> Option<PathBuf> {
    unsafe {
        let mut path_ptr: windows_sys::core::PWSTR = std::ptr::null_mut();
        let result =
            Shell::SHGetKnownFolderPath(&folder_id, 0, std::ptr::null_mut(), &mut path_ptr);
        if result != 0 || path_ptr.is_null() {
            CoTaskMemFree(path_ptr as *const c_void);
            return None;
        }
        let length = lstrlenW(path_ptr) as usize;
        let path = std::slice::from_raw_parts(path_ptr, length);
        let value = PathBuf::from(OsString::from_wide(path));
        CoTaskMemFree(path_ptr as *const c_void);
        Some(value)
    }
}

#[cfg(windows)]
fn platform_local_app_data() -> Option<PathBuf> {
    known_folder(Shell::FOLDERID_LocalAppData)
}

#[cfg(windows)]
fn platform_saved_games() -> Option<PathBuf> {
    known_folder(Shell::FOLDERID_SavedGames)
}

#[cfg(windows)]
fn platform_user_profile() -> Option<PathBuf> {
    known_folder(Shell::FOLDERID_Profile)
}

#[cfg(not(windows))]
fn platform_local_app_data() -> Option<PathBuf> {
    std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| platform_user_profile().map(|path| path.join(".local").join("share")))
}

#[cfg(not(windows))]
fn platform_saved_games() -> Option<PathBuf> {
    platform_user_profile().map(|path| path.join(".local").join("share").join("games"))
}

#[cfg(not(windows))]
fn platform_user_profile() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from)
}

pub fn validate_library_path(path: &Path) -> Result<(), String> {
    normalize_library_path(path).map(|_| ())
}

pub fn normalize_library_path(path: &Path) -> Result<PathBuf, String> {
    if !path.is_absolute() {
        return Err("The game library path must be absolute".into());
    }
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Prefix(prefix) => {
                #[cfg(windows)]
                if !matches!(
                    prefix.kind(),
                    std::path::Prefix::Disk(_) | std::path::Prefix::VerbatimDisk(_)
                ) {
                    return Err("The game library must use a local Windows drive".into());
                }
                normalized.push(component.as_os_str());
            }
            Component::RootDir | Component::Normal(_) => {
                normalized.push(component.as_os_str());
            }
            Component::CurDir => {}
            Component::ParentDir => {
                if !normalized.pop() || normalized.parent().is_none() {
                    return Err("The game library path cannot escape its drive".into());
                }
            }
        }
    }
    if normalized.parent().is_none() {
        return Err("A drive root cannot be used as the game library".into());
    }
    Ok(normalized)
}

#[cfg(windows)]
fn comparable_path(path: &Path) -> String {
    let mut value = path.to_string_lossy().replace('/', "\\");
    if let Some(stripped) = value.strip_prefix(r"\\?\UNC\") {
        value = format!(r"\\{stripped}");
    } else if let Some(stripped) = value.strip_prefix(r"\\?\") {
        value = stripped.to_string();
    }
    value.trim_end_matches('\\').to_lowercase()
}

#[cfg(not(windows))]
fn comparable_path(path: &Path) -> String {
    path.to_string_lossy().to_string()
}

fn resolved_path_identity(path: &Path) -> Result<PathBuf, String> {
    let normalized = normalize_library_path(path)?;
    let mut existing = normalized.clone();
    let mut tail = Vec::new();
    while !existing.exists() {
        let name = existing
            .file_name()
            .ok_or_else(|| "The game library has no existing drive ancestor".to_string())?
            .to_os_string();
        tail.push(name);
        if !existing.pop() {
            return Err("The game library has no existing drive ancestor".into());
        }
    }
    let mut resolved = fs::canonicalize(&existing).map_err(|error| {
        format!(
            "Could not resolve the game library path {}: {error}",
            existing.display()
        )
    })?;
    for component in tail.into_iter().rev() {
        resolved.push(component);
    }
    Ok(resolved)
}

pub fn paths_overlap(left: &Path, right: &Path) -> Result<bool, String> {
    let left = resolved_path_identity(left)?;
    let right = resolved_path_identity(right)?;
    let left_key = comparable_path(&left);
    let right_key = comparable_path(&right);
    let separator = std::path::MAIN_SEPARATOR;
    Ok(left_key == right_key
        || left_key.starts_with(&format!("{right_key}{separator}"))
        || right_key.starts_with(&format!("{left_key}{separator}")))
}

pub fn library_has_persistent_data(path: &Path) -> Result<bool, String> {
    if !path.exists() {
        return Ok(false);
    }
    for entry in
        fs::read_dir(path).map_err(|error| format!("Could not inspect game library: {error}"))?
    {
        let entry = entry.map_err(|error| format!("Could not inspect game library: {error}"))?;
        let name = entry.file_name();
        if name != LIBRARY_MARKER && name != ".staging" && name != ".backup" {
            return Ok(true);
        }
    }
    Ok(false)
}

fn destination_has_game_data(path: &Path) -> Result<bool, String> {
    library_has_persistent_data(path)
}

fn paths_share_volume(left: &Path, right: &Path) -> bool {
    let left_prefix = left.components().find_map(|component| match component {
        Component::Prefix(prefix) => Some(prefix.as_os_str().to_os_string()),
        Component::RootDir => Some(std::ffi::OsString::from("/")),
        _ => None,
    });
    let right_prefix = right.components().find_map(|component| match component {
        Component::Prefix(prefix) => Some(prefix.as_os_str().to_os_string()),
        Component::RootDir => Some(std::ffi::OsString::from("/")),
        _ => None,
    });
    left_prefix == right_prefix
}

fn directory_stats(root: &Path) -> Result<(u64, u64), String> {
    fn walk(path: &Path, files: &mut u64, bytes: &mut u64) -> Result<(), String> {
        for entry in fs::read_dir(path)
            .map_err(|error| format!("Could not inspect {}: {error}", path.display()))?
        {
            let entry =
                entry.map_err(|error| format!("Could not inspect library entry: {error}"))?;
            let file_type = entry
                .file_type()
                .map_err(|error| format!("Could not inspect library entry type: {error}"))?;
            if file_type.is_symlink() {
                return Err(format!(
                    "Game library contains an unsupported symbolic link: {}",
                    entry.path().display()
                ));
            }
            if file_type.is_dir() {
                walk(&entry.path(), files, bytes)?;
            } else if file_type.is_file() {
                *files = files.saturating_add(1);
                *bytes = bytes.saturating_add(
                    entry
                        .metadata()
                        .map_err(|error| format!("Could not inspect library file: {error}"))?
                        .len(),
                );
            }
        }
        Ok(())
    }

    if !root.exists() {
        return Ok((0, 0));
    }
    let mut files = 0;
    let mut bytes = 0;
    walk(root, &mut files, &mut bytes)?;
    Ok((files, bytes))
}

pub fn plan_library_migration(
    source: &Path,
    destination: &Path,
) -> Result<LibraryMigrationPlan, String> {
    let source = normalize_library_path(source)?;
    let destination = normalize_library_path(destination)?;
    if comparable_path(&source) == comparable_path(&destination) {
        return Err("The new game library is already selected".into());
    }
    if paths_overlap(&source, &destination)? {
        return Err("Game libraries cannot be nested inside each other".into());
    }
    if destination_has_game_data(&destination)? {
        return Err("The destination must be empty or an existing MojoRecomp library".into());
    }
    let (files, bytes) = directory_stats(&source)?;
    let same_volume = paths_share_volume(&source, &destination);
    let required_free_bytes = bytes.saturating_add((bytes / 10).max(MIGRATION_MINIMUM_MARGIN));
    Ok(LibraryMigrationPlan {
        source,
        destination,
        files,
        bytes,
        required_free_bytes,
        same_volume,
    })
}

pub fn initialize_library(path: &Path) -> Result<(), String> {
    validate_library_path(path)?;
    fs::create_dir_all(path).map_err(|error| format!("Could not create game library: {error}"))?;
    let marker = path.join(LIBRARY_MARKER);
    if !marker.is_file() {
        fs::write(&marker, "schema_version = 1\nproduct = \"MojoRecomp\"\n")
            .map_err(|error| format!("Could not initialize game library: {error}"))?;
    }
    Ok(())
}

fn sha256_file(path: &Path) -> Result<[u8; 32], String> {
    let mut file = fs::File::open(path)
        .map_err(|error| format!("Could not verify {}: {error}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 1024 * 1024];
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|error| format!("Could not verify {}: {error}", path.display()))?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(hasher.finalize().into())
}

fn copy_tree_verified<F>(
    source: &Path,
    destination: &Path,
    bytes_total: u64,
    bytes_done: &mut u64,
    progress: &mut F,
) -> Result<(), String>
where
    F: FnMut(u64, u64),
{
    fs::create_dir_all(destination)
        .map_err(|error| format!("Could not create migration directory: {error}"))?;
    for entry in fs::read_dir(source)
        .map_err(|error| format!("Could not read {}: {error}", source.display()))?
    {
        let entry = entry.map_err(|error| format!("Could not read library entry: {error}"))?;
        let source_path = entry.path();
        let destination_path = destination.join(entry.file_name());
        let file_type = entry
            .file_type()
            .map_err(|error| format!("Could not inspect library entry: {error}"))?;
        if file_type.is_symlink() {
            return Err(format!(
                "Game library contains an unsupported symbolic link: {}",
                source_path.display()
            ));
        }
        if file_type.is_dir() {
            copy_tree_verified(
                &source_path,
                &destination_path,
                bytes_total,
                bytes_done,
                progress,
            )?;
            continue;
        }
        if !file_type.is_file() {
            continue;
        }
        let length = entry
            .metadata()
            .map_err(|error| format!("Could not inspect library file: {error}"))?
            .len();
        fs::copy(&source_path, &destination_path).map_err(|error| {
            format!(
                "Could not copy {} to {}: {error}",
                source_path.display(),
                destination_path.display()
            )
        })?;
        if sha256_file(&source_path)? != sha256_file(&destination_path)? {
            return Err(format!(
                "Migration verification failed for {}",
                source_path.display()
            ));
        }
        *bytes_done = bytes_done.saturating_add(length);
        progress(*bytes_done, bytes_total);
    }
    Ok(())
}

fn migration_staging_path(destination: &Path) -> Result<PathBuf, String> {
    let parent = destination
        .parent()
        .ok_or_else(|| "The destination library has no parent directory".to_string())?;
    let name = destination
        .file_name()
        .ok_or_else(|| "The destination library has no directory name".to_string())?;
    let mut staging_name = std::ffi::OsString::from(".");
    staging_name.push(name);
    staging_name.push(".mojorecomp-migration");
    Ok(parent.join(staging_name))
}

pub fn execute_library_migration<F>(
    plan: &LibraryMigrationPlan,
    mut progress: F,
) -> Result<LibraryMigrationOutcome, String>
where
    F: FnMut(u64, u64),
{
    let current = plan_library_migration(&plan.source, &plan.destination)?;
    if current.files != plan.files || current.bytes != plan.bytes {
        return Err("The game library changed after migration was planned; try again".into());
    }
    let parent = plan
        .destination
        .parent()
        .ok_or_else(|| "The destination library has no parent directory".to_string())?;
    fs::create_dir_all(parent)
        .map_err(|error| format!("Could not create destination directory: {error}"))?;
    if fs2::available_space(parent)
        .map_err(|error| format!("Could not query destination free space: {error}"))?
        < plan.required_free_bytes
    {
        return Err(format!(
            "Not enough free space to move the game library. {} bytes are required",
            plan.required_free_bytes
        ));
    }

    if plan.destination.exists() {
        let _ = fs::remove_file(plan.destination.join(LIBRARY_MARKER));
        fs::remove_dir(&plan.destination)
            .map_err(|error| format!("Destination library is not empty: {error}"))?;
    }

    let staging = migration_staging_path(&plan.destination)?;
    if staging.exists() {
        return Err(format!(
            "Migration staging path already exists: {}",
            staging.display()
        ));
    }
    let mut bytes_done = 0;
    let copied = copy_tree_verified(
        &plan.source,
        &staging,
        plan.bytes,
        &mut bytes_done,
        &mut progress,
    );
    if let Err(error) = copied {
        let _ = fs::remove_dir_all(&staging);
        return Err(error);
    }
    let (files, bytes) = directory_stats(&staging)?;
    if files != plan.files || bytes != plan.bytes {
        let _ = fs::remove_dir_all(&staging);
        return Err("Copied game library did not pass verification".into());
    }
    initialize_library(&staging)?;
    fs::rename(&staging, &plan.destination)
        .map_err(|error| format!("Could not activate migrated game library: {error}"))?;
    Ok(LibraryMigrationOutcome {
        source_to_clean: Some(plan.source.clone()),
    })
}

pub fn rollback_library_migration(plan: &LibraryMigrationPlan) -> Result<(), String> {
    let staging = migration_staging_path(&plan.destination)?;
    for path in [&staging, &plan.destination] {
        if path.exists() {
            fs::remove_dir_all(path).map_err(|error| {
                format!(
                    "Could not remove uncommitted migration data at {}: {error}",
                    path.display()
                )
            })?;
        }
    }
    Ok(())
}

pub fn move_file_verified(source: &Path, destination: &Path) -> Result<(), String> {
    if !source.is_file() {
        return Ok(());
    }
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("Could not create migration directory: {error}"))?;
    }
    if fs::rename(source, destination).is_ok() {
        return Ok(());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| "Migration destination has no parent directory".to_string())?;
    let required = source
        .metadata()
        .map_err(|error| format!("Could not inspect legacy file: {error}"))?
        .len();
    if fs2::available_space(parent)
        .map_err(|error| format!("Could not query migration free space: {error}"))?
        < required
    {
        return Err(format!(
            "Not enough free space to migrate {}",
            source.display()
        ));
    }
    let temporary = destination.with_extension(format!("migration-{}.tmp", std::process::id()));
    let _ = fs::remove_file(&temporary);
    if let Err(error) = fs::copy(source, &temporary) {
        let _ = fs::remove_file(&temporary);
        return Err(format!("Could not copy legacy file: {error}"));
    }
    if sha256_file(source)? != sha256_file(&temporary)? {
        let _ = fs::remove_file(&temporary);
        return Err(format!(
            "Legacy file verification failed for {}",
            source.display()
        ));
    }
    fs::rename(&temporary, destination)
        .map_err(|error| format!("Could not activate migrated legacy file: {error}"))?;
    fs::remove_file(source)
        .map_err(|error| format!("Could not remove migrated legacy file: {error}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn layout_separates_library_machine_data_and_saved_games() {
        let layout = StorageLayout::new(
            PathBuf::from(r"C:\Users\Test\AppData\Local\MojoRecomp"),
            PathBuf::from(r"C:\Users\Test\Saved Games\MojoRecomp"),
            PathBuf::from(r"D:\Apps\MojoRecomp"),
            PathBuf::from(r"C:\Users\Test\Games\MojoRecomp-Games"),
        );

        assert_eq!(
            layout.settings_path("cot"),
            PathBuf::from(r"C:\Users\Test\AppData\Local\MojoRecomp\titles\cot\settings.toml")
        );
        assert_eq!(
            layout.save_root("cot"),
            PathBuf::from(r"C:\Users\Test\Saved Games\MojoRecomp\cot")
        );
        assert_eq!(
            layout.cache_root("cot"),
            PathBuf::from(r"C:\Users\Test\AppData\Local\MojoRecomp\cache\cot")
        );
        assert_eq!(
            layout.default_library_root(),
            PathBuf::from(r"C:\Users\Test\Games\MojoRecomp-Games")
        );
        assert_eq!(
            layout.legacy_portable_root(),
            PathBuf::from(r"D:\Apps\MojoRecomp\userdata")
        );
    }

    #[test]
    fn library_choice_round_trips_through_launcher_settings() {
        let root =
            std::env::temp_dir().join(format!("mojorecomp-storage-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let layout = StorageLayout::new(
            root.join("local"),
            root.join("saves"),
            root.join("app"),
            root.join("default-library"),
        );
        let chosen = root.join("custom").join("MojoRecomp-Games");

        layout
            .save_launcher_settings(&LauncherSettings {
                schema_version: 1,
                game_library: chosen.clone(),
                discord_activity_enabled: true,
                language_setup_completed_games: vec!["cot".into()],
            })
            .expect("save launcher settings");
        let restored = layout
            .load_launcher_settings()
            .expect("load launcher settings")
            .expect("settings exist");

        assert_eq!(restored.game_library, chosen);
        assert!(restored.discord_activity_enabled);
        assert_eq!(restored.language_setup_completed_games, ["cot"]);
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn legacy_launcher_settings_default_discord_activity_on() {
        let parsed: LauncherSettings = toml::from_str(
            r#"
schema_version = 1
game_library = "C:\\Games\\MojoRecomp-Games"
"#,
        )
        .expect("legacy launcher settings parse");
        assert!(parsed.discord_activity_enabled);
        assert!(parsed.language_setup_completed_games.is_empty());
    }

    #[test]
    fn clean_install_paths_support_spaces_and_unicode() {
        let root = std::env::temp_dir().join(format!(
            "MojoRecomp Unicode Path \u{00E9} {}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let layout = StorageLayout::new(
            root.join("Local Data"),
            root.join("Saved Games"),
            root.join("Portable Application"),
            root.join("Default Library"),
        );
        let chosen = root.join("User Library").join("MojoRecomp-Games");

        initialize_library(&chosen).expect("initialize unicode library");
        layout
            .save_launcher_settings(&LauncherSettings::new(chosen.clone()))
            .expect("save unicode launcher settings");
        let restored = layout
            .load_launcher_settings()
            .expect("load unicode launcher settings")
            .expect("unicode launcher settings exist");

        assert_eq!(restored.game_library, chosen);
        assert!(restored.game_library.join(LIBRARY_MARKER).is_file());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn launcher_settings_recover_after_interrupted_replacement() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-settings-recovery-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let layout = StorageLayout::new(
            root.join("local"),
            root.join("saves"),
            root.join("app"),
            root.join("default-library"),
        );
        let chosen = root.join("games");
        layout
            .save_launcher_settings(&LauncherSettings::new(chosen.clone()))
            .expect("save launcher settings");
        std::fs::rename(
            layout.launcher_settings_path(),
            layout.launcher_settings_backup_path(),
        )
        .expect("simulate interrupted replacement");

        let recovered = layout
            .load_launcher_settings()
            .expect("recover launcher settings")
            .expect("settings exist");
        assert_eq!(recovered.game_library, chosen);
        assert!(layout.launcher_settings_path().is_file());
        assert!(!layout.launcher_settings_backup_path().exists());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn migration_plan_counts_source_without_changing_it() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-migration-plan-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        std::fs::create_dir_all(source.join("cot")).expect("source tree");
        std::fs::write(source.join("cot").join("default.xex"), [1u8; 7]).expect("source file");
        std::fs::write(source.join("cot").join("default.rcf"), [2u8; 11]).expect("source file");

        let plan = plan_library_migration(&source, &destination).expect("migration plan");

        assert_eq!(plan.files, 2);
        assert_eq!(plan.bytes, 18);
        assert!(plan.required_free_bytes >= 18);
        assert!(source.join("cot").join("default.xex").is_file());
        assert!(!destination.exists());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn library_transient_entries_do_not_count_as_persistent_game_data() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-transient-library-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        let library = root.join("library");
        initialize_library(&library).expect("initialize library");
        fs::create_dir_all(library.join(".staging").join("cot")).expect("staging directory");
        fs::write(
            library.join(".staging").join("cot").join("partial.bin"),
            b"partial",
        )
        .expect("staging file");
        fs::create_dir_all(library.join(".backup").join("cot-old")).expect("backup directory");
        fs::write(
            library.join(".backup").join("cot-old").join("backup.bin"),
            b"backup",
        )
        .expect("backup file");

        assert!(!library_has_persistent_data(&library).expect("inspect transient-only library"));

        fs::create_dir_all(library.join("cot")).expect("game directory");
        fs::write(library.join("cot").join("default.xex"), b"game").expect("game file");
        assert!(library_has_persistent_data(&library).expect("inspect game library"));

        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn migration_executes_only_after_preflight_and_preserves_file_contents() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-migration-execute-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        std::fs::create_dir_all(source.join("cot")).expect("source tree");
        std::fs::write(source.join("cot").join("default.xex"), b"known game data")
            .expect("source file");
        let plan = plan_library_migration(&source, &destination).expect("migration plan");

        let outcome = execute_library_migration(&plan, |_, _| {}).expect("execute migration");

        assert!(source.exists());
        assert_eq!(outcome.source_to_clean.as_deref(), Some(source.as_path()));
        assert_eq!(
            std::fs::read(destination.join("cot").join("default.xex")).expect("migrated file"),
            b"known game data"
        );
        assert!(destination.join(LIBRARY_MARKER).is_file());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn same_volume_migration_can_be_rolled_back_before_settings_commit() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-storage-rollback-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        std::fs::create_dir_all(source.join("cot")).expect("source tree");
        std::fs::write(source.join("cot").join("default.xex"), b"guest-data").expect("source file");

        let plan = plan_library_migration(&source, &destination).expect("migration plan");
        assert!(plan.same_volume);
        execute_library_migration(&plan, |_, _| {}).expect("execute migration");
        rollback_library_migration(&plan).expect("rollback migration");

        assert!(source.join("cot").join("default.xex").is_file());
        assert!(!destination.exists());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn verified_copy_keeps_source_until_settings_commit() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-storage-copy-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        std::fs::create_dir_all(source.join("cot")).expect("source tree");
        std::fs::write(source.join("cot").join("default.xex"), b"guest-data").expect("source file");

        let plan = plan_library_migration(&source, &destination).expect("migration plan");
        let outcome = execute_library_migration(&plan, |_, _| {}).expect("execute verified copy");

        assert_eq!(outcome.source_to_clean.as_deref(), Some(source.as_path()));
        assert!(source.join("cot").join("default.xex").is_file());
        assert!(destination.join("cot").join("default.xex").is_file());
        let staging = migration_staging_path(&destination).expect("staging path");
        std::fs::create_dir_all(&staging).expect("simulated leftover staging");
        std::fs::write(staging.join("partial.bin"), b"partial").expect("partial staging file");
        rollback_library_migration(&plan).expect("remove uncommitted copy");
        assert!(source.join("cot").join("default.xex").is_file());
        assert!(!destination.exists());
        assert!(!staging.exists());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }

    #[cfg(windows)]
    #[test]
    fn locked_destination_preserves_source_and_journal_until_recovery_can_finish() {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_READ, FILE_SHARE_WRITE};

        let root = std::env::temp_dir().join(format!(
            "mojorecomp-storage-locked-destination-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        let layout = StorageLayout::new(
            root.join("local"),
            root.join("saves"),
            root.join("app"),
            root.join("default-library"),
        );
        fs::create_dir_all(source.join("cot")).expect("source tree");
        fs::write(source.join("cot").join("default.xex"), b"guest-data")
            .expect("source file");
        initialize_library(&destination).expect("destination marker");
        layout
            .save_launcher_settings(&LauncherSettings::new(source.clone()))
            .expect("save current library");

        let plan = plan_library_migration(&source, &destination).expect("migration plan");
        layout
            .begin_library_migration(&plan)
            .expect("migration journal");
        let marker = fs::OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
            .open(destination.join(LIBRARY_MARKER))
            .expect("lock destination marker against deletion");

        let error = execute_library_migration(&plan, |_, _| {})
            .expect_err("locked destination must stop activation");
        assert!(error.contains("Destination library is not empty"));
        assert!(source.join("cot").join("default.xex").is_file());
        assert!(destination.join(LIBRARY_MARKER).is_file());
        assert!(rollback_library_migration(&plan).is_err());
        assert!(layout.migration_journal_path().is_file());

        drop(marker);
        assert_eq!(
            layout
                .recover_library_migration()
                .expect("recover after releasing destination"),
            None
        );
        assert!(source.join("cot").join("default.xex").is_file());
        assert!(!destination.exists());
        assert!(!layout.migration_journal_path().exists());
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[cfg(windows)]
    #[test]
    fn locked_source_before_migration_fails_closed_without_creating_partial_destination() {
        use std::os::windows::fs::OpenOptionsExt;

        let root = std::env::temp_dir().join(format!(
            "mojorecomp-storage-locked-source-preflight-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        fs::create_dir_all(source.join("cot")).expect("source tree");
        let source_file = source.join("cot").join("default.xex");
        fs::write(&source_file, b"guest-data").expect("source file");
        let plan = plan_library_migration(&source, &destination).expect("migration plan");

        let locked_source = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .share_mode(0)
            .open(&source_file)
            .expect("lock source exclusively");
        let error = execute_library_migration(&plan, |_, _| {})
            .expect_err("exclusive source lock must stop migration");

        assert!(!error.is_empty());
        assert!(source_file.is_file());
        assert!(!destination.exists());
        assert!(!migration_staging_path(&destination)
            .expect("migration staging path")
            .exists());

        drop(locked_source);
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[cfg(windows)]
    #[test]
    fn locked_old_source_after_settings_commit_keeps_new_library_active_until_cleanup_recovers() {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_READ, FILE_SHARE_WRITE};

        let root = std::env::temp_dir().join(format!(
            "mojorecomp-storage-locked-source-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        let layout = StorageLayout::new(
            root.join("local"),
            root.join("saves"),
            root.join("app"),
            root.join("default-library"),
        );
        fs::create_dir_all(source.join("cot")).expect("source tree");
        let source_file = source.join("cot").join("default.xex");
        fs::write(&source_file, b"guest-data").expect("source file");
        let plan = plan_library_migration(&source, &destination).expect("migration plan");
        layout
            .begin_library_migration(&plan)
            .expect("migration journal");
        execute_library_migration(&plan, |_, _| {}).expect("verified copy");
        layout
            .save_launcher_settings(&LauncherSettings::new(destination.clone()))
            .expect("commit destination setting");

        let locked_source = fs::OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
            .open(&source_file)
            .expect("lock old source against deletion");
        let notice = layout
            .recover_library_migration()
            .expect("deferred cleanup result")
            .expect("locked source should defer cleanup");
        assert!(notice.contains("needs cleanup"));
        assert!(source_file.is_file());
        assert!(destination.join("cot").join("default.xex").is_file());
        assert!(layout.migration_journal_path().is_file());

        drop(locked_source);
        assert_eq!(
            layout
                .recover_library_migration()
                .expect("recover after releasing source"),
            None
        );
        assert!(!source.exists());
        assert!(destination.join("cot").join("default.xex").is_file());
        assert!(!layout.migration_journal_path().exists());
        fs::remove_dir_all(root).expect("test cleanup");
    }

    #[test]
    fn normalized_paths_reject_roots_aliases_and_storage_overlap() {
        assert!(normalize_library_path(Path::new(r"C:\.")).is_err());
        assert!(normalize_library_path(Path::new(r"C:\Games\..\..")).is_err());
        assert_eq!(
            normalize_library_path(Path::new(r"C:\Games\.\Mojo\..\MojoRecomp-Games"))
                .expect("normalized path"),
            PathBuf::from(r"C:\Games\MojoRecomp-Games")
        );
        assert!(
            paths_overlap(
                Path::new(r"C:\Users\Test\Saved Games\MojoRecomp\library"),
                Path::new(r"c:\users\test\saved games\mojorecomp")
            )
            .expect("overlap check")
        );

        let layout = StorageLayout::new(
            PathBuf::from(r"C:\Users\Test\AppData\Local\MojoRecomp"),
            PathBuf::from(r"C:\Users\Test\Saved Games\MojoRecomp"),
            PathBuf::from(r"D:\Apps\MojoRecomp"),
            PathBuf::from(r"C:\Users\Test\Games\MojoRecomp-Games"),
        );
        assert!(
            layout
                .normalize_game_library(Path::new(r"C:\Users\Test\AppData\Local\MojoRecomp\games"))
                .is_err()
        );
        assert!(
            layout
                .normalize_game_library(Path::new(r"C:\Users\Test\Saved Games"))
                .is_err()
        );
    }

    #[test]
    fn migration_journal_recovers_before_and_after_settings_commit() {
        let root = std::env::temp_dir().join(format!(
            "mojorecomp-migration-recovery-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let source = root.join("source");
        let destination = root.join("destination");
        let layout = StorageLayout::new(
            root.join("local"),
            root.join("saves"),
            root.join("app"),
            root.join("default-library"),
        );
        std::fs::create_dir_all(source.join("cot")).expect("source tree");
        std::fs::write(source.join("cot").join("default.xex"), b"guest-data").expect("source file");

        let plan = plan_library_migration(&source, &destination).expect("migration plan");
        layout
            .begin_library_migration(&plan)
            .expect("begin uncommitted migration");
        execute_library_migration(&plan, |_, _| {}).expect("copy uncommitted migration");
        assert!(
            layout
                .recover_library_migration()
                .expect("recover uncommitted migration")
                .is_none()
        );
        assert!(source.exists());
        assert!(!destination.exists());

        let plan = plan_library_migration(&source, &destination).expect("second migration plan");
        layout
            .begin_library_migration(&plan)
            .expect("begin committed migration");
        execute_library_migration(&plan, |_, _| {}).expect("copy committed migration");
        layout
            .save_launcher_settings(&LauncherSettings::new(destination.clone()))
            .expect("commit destination settings");
        assert!(
            layout
                .recover_library_migration()
                .expect("recover committed migration")
                .is_none()
        );
        assert!(!source.exists());
        assert!(destination.exists());
        std::fs::remove_dir_all(root).expect("test cleanup");
    }
}
