export type GameInfo = {
  id: string;
  name: string;
  status: string;
  runtime_version: string;
  installed: boolean;
  game_root: string | null;
  managed: boolean;
  playable: boolean;
  capabilities: {
    resolution_scale: boolean;
    aspect_ratio: boolean;
    fxaa: boolean;
    anisotropic_filtering: boolean;
    debug_mode: boolean;
    localization: boolean;
  };
};

export type LocalizationProfile = string;

export type Settings = {
  schema_version: number;
  localization: {
    profile: LocalizationProfile;
    xbox_language: number;
  };
  display: {
    mode: "windowed" | "fullscreen";
    monitor: string;
    output_resolution: string;
    resolution_scale: number;
    aspect_ratio: string;
    vsync: boolean;
  };
  graphics: {
    anti_aliasing: "off" | "fxaa" | "fxaa_extreme";
    texture_filtering: "default" | "1x" | "2x" | "4x" | "8x" | "16x";
    frame_rate: "30" | "60";
  };
  advanced: {
    logging_enabled: boolean;
  };
};

export type HardwareInfo = {
  adapter: string;
  vendor_id: number;
  device_id: number;
  vulkan_api: string;
  sampler_anisotropy: boolean;
  max_anisotropy: number;
  bc_texture_compression: boolean;
  supported_present_modes: string[];
  max_internal_extent: [number, number];
};

export type ProcessStatus = {
  running: boolean;
  pid: number | null;
  exit_code: number | null;
};

export type ComponentUpdateStatus = {
  id: string;
  kind: "launcher" | "runtime" | "language";
  game_id: string | null;
  locale: string | null;
  display_name: string | null;
  xbox_language: number | null;
  translation_version: string | null;
  installed_version: string | null;
  latest_version: string | null;
  state:
    | "up_to_date"
    | "update_available"
    | "available"
    | "corrupted"
    | "repair_unavailable"
    | "incompatible"
    | "not_installed";
  download_url: string | null;
  size: number | null;
  published: string | null;
  notes_url: string | null;
  last_action: string | null;
  can_rollback: boolean;
  releases: ComponentReleaseStatus[];
  installed_versions: InstalledComponentVersionStatus[];
};

export type InstalledComponentVersionStatus = {
  version: string;
  healthy: boolean;
};

export type ComponentReleaseStatus = {
  version: string;
  published: string;
  notes_url: string;
  size: number;
};

export type UpdateOverview = {
  configured: boolean;
  components: ComponentUpdateStatus[];
  error: string | null;
};

export type ComponentUpdateProgress = {
  component_id: string;
  stage:
    | "downloading"
    | "validating"
    | "installing"
    | "ready_restart"
    | "complete"
    | "failed";
  progress: number;
  detail: string;
  bytes_done: number;
  bytes_total: number;
};

export type ComponentUpdateResult = {
  component_id: string;
  state: "installed" | "downloaded" | "ready_restart";
  restart_required: boolean;
};

export type RuntimeAdditionalLanguageStatus = {
  id: string;
  locale: string;
  display_name: string;
  version: string;
  translation_version: string | null;
  installed_version: string | null;
};

export type RuntimeAdditionalContentStatus = {
  game_id: string;
  runtime_version: string;
  pack_version: string;
  pack_size: number;
  languages: RuntimeAdditionalLanguageStatus[];
};

export type LocalizationPackInstallResult = {
  game_id: string;
  version: string;
  installed_languages: string[];
};

export type GameSetupProgress = {
  game_id: string;
  stage: "analyzing" | "extracting" | "validating" | "installing" | "complete" | "failed";
  progress: number;
  detail: string;
  files_done: number;
  files_total: number;
  bytes_done: number;
  bytes_total: number;
};

export type GameSetupResult = {
  game_root: string;
  files: number;
  bytes: number;
};

export type LauncherStorageStatus = {
  configured: boolean;
  library_path: string;
  default_library_path: string;
  existing_library_detected: boolean;
  available_bytes: number;
  discord_activity_enabled: boolean;
  language_setup_completed_games: string[];
  notice?: string | null;
};

export type LibraryPathStatus = {
  path: string;
  available_bytes: number;
  required_free_bytes: number;
  valid: boolean;
  enough_space: boolean;
  error: string | null;
};

export type LibraryMigrationProgress = {
  stage: "planning" | "moving" | "complete" | "failed";
  progress: number;
  detail: string;
  bytes_done: number;
  bytes_total: number;
};

export type LocalizationStatus = {
  profile: string;
  source_installed: boolean;
  overlay_ready: boolean;
  detail: string;
};

export type LocalizationProgress = {
  game_id: string;
  profile: string;
  stage: "importing" | "imported" | "preparing" | "building" | "complete" | "failed" | "cancelled";
  progress: number;
  detail: string;
  bytes_done: number;
  bytes_total: number;
};
