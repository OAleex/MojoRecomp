<script lang="ts">
  import { onMount, tick } from "svelte";
  import { invoke } from "@tauri-apps/api/core";
  import { listen } from "@tauri-apps/api/event";
  import { getVersion } from "@tauri-apps/api/app";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import { open } from "@tauri-apps/plugin-dialog";
  import mojoRecompIcon from "../src-tauri/icons/icon.ico?url";
  import type {
    ComponentReleaseStatus,
    ComponentUpdateProgress,
    ComponentUpdateResult,
    ComponentUpdateStatus,
    GameInfo,
    GameSetupProgress,
    GameSetupResult,
    HardwareInfo,
    LauncherStorageStatus,
    LibraryPathStatus,
    LibraryMigrationProgress,
    LocalizationProgress,
    LocalizationPackInstallResult,
    LocalizationProfile,
    LocalizationStatus,
    ProcessStatus,
    RuntimeAdditionalContentStatus,
    Settings,
    UpdateOverview
  } from "./lib/types";

  type View = "game" | "versions" | "settings" | "launcher" | "launcher-settings" | "help";
  type NoticeKind = "success" | "warning" | "error";
  type RuntimeVersionRow = ComponentReleaseStatus & { local_only: boolean };
  type LanguageOption = {
    profile: LocalizationProfile;
    xboxLanguage: number;
    label: string;
    flag: string;
    componentId: string | null;
  };
  type PendingRuntimeAction =
    | {
        kind: "install";
        component: ComponentUpdateStatus;
        version: string;
        reinstall: boolean;
        activate: boolean;
      }
    | {
        kind: "activate";
        component: ComponentUpdateStatus;
        version: string;
      };
  const NOTICE_DURATION_MS: Record<NoticeKind, number> = {
    success: 4200,
    warning: 8000,
    error: 6500
  };
  const ORIGINAL_LANGUAGE_OPTIONS: LanguageOption[] = [
    { profile: "en", xboxLanguage: 1, label: "English", flag: "flag-en", componentId: null },
    { profile: "de", xboxLanguage: 3, label: "Deutsch", flag: "flag-de", componentId: null },
    { profile: "fr", xboxLanguage: 4, label: "Français", flag: "flag-fr", componentId: null },
    { profile: "es", xboxLanguage: 5, label: "Español", flag: "flag-es", componentId: null },
    { profile: "it", xboxLanguage: 6, label: "Italiano", flag: "flag-it", componentId: null },
    { profile: "nl", xboxLanguage: 16, label: "Nederlands", flag: "flag-nl", componentId: null }
  ];
  let games: GameInfo[] = [];
  let launcherVersion = "";
  let selectedId = "cot";
  let activeView: View = "game";
  let settings: Settings | null = null;
  let savedSettingsFingerprint = "";
  let launcherStorage: LauncherStorageStatus | null = null;
  let discordActivityEnabled = true;
  let includeSupportMinidump = false;
  let libraryPath = "";
  let libraryProgress: LibraryMigrationProgress | null = null;
  let libraryPathStatus: LibraryPathStatus | null = null;
  let libraryBusy = false;
  let libraryProbeTimer: ReturnType<typeof setTimeout> | null = null;
  let libraryProbeSerial = 0;
  let hardware: HardwareInfo | null = null;
  let process: ProcessStatus = { running: false, pid: null, exit_code: null };
  let updates: UpdateOverview | null = null;
  let launcherUpdates: UpdateOverview | null = null;
  let updateProgress: ComponentUpdateProgress | null = null;
  let checkingUpdates = false;
  let updateBusy = false;
  let versionFocus: string | null = null;
  let setupProgress: GameSetupProgress | null = null;
  let localizationStatus: LocalizationStatus | null = null;
  let localizationProgress: LocalizationProgress | null = null;
  let localizationBusy = false;
  let setupBusy = false;
  let busy = false;
  let saving = false;
  let uninstalling = false;
  let message = "";
  let warning = "";
  let error = "";
  let toastSerial = 0;
  let toastDuration = NOTICE_DURATION_MS.success;
  let startupReady = false;
  let languageSetupSelection: LocalizationProfile = "en";
  let languageSetupSaving = false;
  let languageSetupOpen = false;
  let continuePlayAfterLanguageSetup = false;
  let runtimeAdditionalContent: RuntimeAdditionalContentStatus | null = null;
  let runtimeAdditionalSelection: string[] = [];
  let pendingRuntimeAction: PendingRuntimeAction | null = null;
  let runtimeAdditionalBusy = false;
  let pollTimer: ReturnType<typeof setInterval> | null = null;
  let toastTimer: ReturnType<typeof setTimeout> | null = null;

  $: selected = games.find((game) => game.id === selectedId) ?? null;
  $: cotGame = games.find((game) => game.id === "cot") ?? null;
  $: momGame = games.find((game) => game.id === "mom") ?? null;
  $: selectedLogo = selectedId === "mom" ? "/art/mom-logo.png" : "/art/cot-logo.png";
  $: selectedIcon = selectedId === "mom" ? "/art/mom-icon.png" : "/art/cot-icon.png";
  $: languageOptions = buildLanguageOptions(selectedId, selectedLanguageComponents);
  $: cotLanguageOptions = buildLanguageOptions(
    "cot",
    updates?.components.filter((component) => component.kind === "language" && component.game_id === "cot") ?? []
  );
  $: selectedLanguage = languageOptions.find((entry) => entry.profile === settings?.localization.profile) ?? languageOptions[0] ?? ORIGINAL_LANGUAGE_OPTIONS[0];
  $: currentLauncherVersion = launcherComponent?.installed_version || launcherVersion || "...";
  $: selectedRuntimeVersions = buildRuntimeVersionRows(selectedRuntimeComponent);
  $: navigationLocked = setupBusy || localizationBusy || libraryBusy || updateBusy || busy || saving || uninstalling || languageSetupSaving || languageSetupOpen || runtimeAdditionalBusy || !!pendingRuntimeAction;
  $: updateInteractionBusy = checkingUpdates || updateBusy;
  $: firstLaunchPending = launcherStorage !== null && !launcherStorage.configured;
  $: gameSettingsDirty = !!settings && settingsFingerprint(settings) !== savedSettingsFingerprint;
  $: libraryDirty = firstLaunchPending || normalizedPath(libraryPath) !== normalizedPath(launcherStorage?.library_path ?? "");
  $: libraryProbeMatchesPath = !!libraryPathStatus && normalizedPath(libraryPathStatus.path) === normalizedPath(libraryPath);
  $: libraryCanApply = !!libraryPath.trim()
    && libraryDirty
    && libraryProbeMatchesPath
    && !!libraryPathStatus?.valid
    && !!libraryPathStatus?.enough_space;
  $: launcherComponent = launcherUpdates?.components.find((component) => component.id === "launcher") ?? null;
  $: launcherUpdateError = launcherUpdates?.error ?? null;
  $: selectedRuntimeComponent = updates?.components.find(
    (component) => component.id === "runtime." + selectedId
  ) ?? null;
  $: selectedLanguageComponents = updates?.components.filter(
    (component) => component.kind === "language" && component.game_id === selectedId
  ) ?? [];
  $: initialLanguageSetupRequired = startupReady
    && !!launcherStorage?.configured
    && games.some((game) => game.id === "cot" && game.playable)
    && !launcherStorage.language_setup_completed_games.includes("cot");

  function languageFlag(profile: string) {
    if (profile.toLowerCase() === "pt-br") return "flag-br";
    const base = profile.split("-")[0]?.toLowerCase() ?? "";
    return ["en", "de", "fr", "es", "it", "nl"].includes(base) ? `flag-${base}` : "flag-generic";
  }

  function buildLanguageOptions(gameId: string, components: ComponentUpdateStatus[]): LanguageOption[] {
    const options = gameId === "cot" ? ORIGINAL_LANGUAGE_OPTIONS.map((entry) => ({ ...entry })) : [ORIGINAL_LANGUAGE_OPTIONS[0]];
    const dynamic = new Map<string, LanguageOption>();
    for (const component of components) {
      if (!component.installed_version) continue;
      if (!component.locale || component.xbox_language === null) continue;
      dynamic.set(component.locale.toLowerCase(), {
        profile: component.locale,
        xboxLanguage: component.xbox_language,
        label: component.display_name || component.locale,
        flag: languageFlag(component.locale),
        componentId: component.id
      });
    }
    return [...options, ...dynamic.values()];
  }

  function languageComponent(profile: string, gameId = selectedId) {
    return updates?.components.find(
      (component) => component.kind === "language"
        && component.game_id === gameId
        && component.locale?.toLowerCase() === profile.toLowerCase()
    ) ?? null;
  }

  function languageRequiresPack(profile: string, gameId = selectedId) {
    if (gameId !== "cot") return false;
    return !["en", "de", "fr", "es", "it", "nl"].includes(profile);
  }
  function componentHasDownload(component: ComponentUpdateStatus) {
    return !!component.latest_version && !!component.download_url;
  }

  function buildRuntimeVersionRows(component: ComponentUpdateStatus | null): RuntimeVersionRow[] {
    if (!component) return [];
    const rows: RuntimeVersionRow[] = component.releases.map((release) => ({
      ...release,
      local_only: false
    }));
    const known = new Set(rows.map((release) => release.version));
    for (const installed of component.installed_versions) {
      if (known.has(installed.version)) continue;
      rows.push({
        version: installed.version,
        published: "",
        notes_url: "",
        size: 0,
        downloadable: false,
        local_only: true
      });
    }
    return rows;
  }

  function installedRuntimeVersion(version: string) {
    return selectedRuntimeComponent?.installed_versions.find((entry) => entry.version === version) ?? null;
  }

  function runtimeVersionStatusLabel(release: RuntimeVersionRow, index: number) {
    if (release.version === selectedRuntimeComponent?.installed_version) return "Active";
    const installed = installedRuntimeVersion(release.version);
    if (installed && !installed.healthy) return "Damaged";
    if (installed) return "Downloaded";
    if (release.version === selectedRuntimeComponent?.latest_version && !release.downloadable) return "Known";
    if (!release.downloadable) return "Archived";
    return index === 0 ? "Latest" : "Available";
  }

  function componentNeedsAttention(component: ComponentUpdateStatus | null) {
    if (!component) return false;
    if (component.state === "update_available" || component.state === "corrupted" || component.state === "repair_unavailable") {
      return true;
    }
    if (component.kind === "runtime" && (component.state === "available" || component.state === "not_installed")) {
      return componentHasDownload(component);
    }
    return component.kind === "launcher"
      && (component.state === "available" || component.state === "not_installed")
      && componentHasDownload(component);
  }

  function gameNeedsAttention(gameId: string) {
    if (!games.some((game) => game.id === gameId)) return false;
    return updates?.components.some(
      (component) => component.game_id === gameId && componentNeedsAttention(component)
    ) ?? false;
  }

  $: versionsNeedsAttention = gameNeedsAttention(selectedId);
  $: launcherUpdateProgress = updateProgress?.component_id === "launcher" ? updateProgress : null;

  async function openGame(id: string) {
    if (navigationLocked || firstLaunchPending) return;
    activeView = "game";
    await selectGame(id);
  }

  function openSettings() {
    if (navigationLocked || firstLaunchPending || process.running) return;
    activeView = "settings";
  }

  async function openVersions() {
    if (navigationLocked || firstLaunchPending) return;
    versionFocus = null;
    activeView = "versions";
    if (!updates) await checkUpdates();
  }

  function openLauncher() {
    if (navigationLocked || firstLaunchPending) return;
    activeView = "launcher";
  }

  function openLauncherSettings() {
    if (navigationLocked) return;
    if (launcherStorage) libraryPath = launcherStorage.library_path;
    activeView = "launcher-settings";
  }

  async function updateDiscordActivity() {
    const requested = discordActivityEnabled;
    const value = await run(() => invoke<boolean>("set_discord_activity_enabled", {
      enabled: requested
    }));
    if (value === null) {
      discordActivityEnabled = !requested;
      return;
    }
    discordActivityEnabled = value;
    if (launcherStorage) {
      launcherStorage = { ...launcherStorage, discord_activity_enabled: value };
    }
  }

  function showView(view: View) {
    if (firstLaunchPending && view !== "launcher-settings") return;
    if (navigationLocked && view !== activeView) return;
    activeView = view;
  }

  function toggleHelp() {
    if (navigationLocked || firstLaunchPending) return;
    activeView = activeView === "help" ? "game" : "help";
  }

  function showNotice(text: string, kind: NoticeKind = "success") {
    if (toastTimer) clearTimeout(toastTimer);
    toastSerial += 1;
    toastDuration = NOTICE_DURATION_MS[kind];
    message = kind === "success" ? text : "";
    warning = kind === "warning" ? text : "";
    error = kind === "error" ? text : "";
    toastTimer = setTimeout(() => {
      message = "";
      warning = "";
      error = "";
      toastTimer = null;
    }, NOTICE_DURATION_MS[kind]);
  }

  async function run<T>(operation: () => Promise<T>): Promise<T | null> {
    try {
      return await operation();
    } catch (value) {
      showNotice(String(value), "error");
      return null;
    }
  }

  async function runAction(operation: () => Promise<unknown>): Promise<boolean> {
    try {
      await operation();
      return true;
    } catch (value) {
      showNotice(String(value), "error");
      return false;
    }
  }

  async function openUpdateUrl(url: string | null) {
    if (!url) {
      showNotice("No public link is available for this release yet.", "warning");
      return;
    }
    await runAction(() => invoke("open_update_url", { url }));
  }

  async function refreshGames() {
    const value = await run(() => invoke<GameInfo[]>("list_games"));
    if (value) games = value;
  }

  async function selectGame(id: string) {
    selectedId = id;
    settings = null;
    savedSettingsFingerprint = "";
    hardware = null;
    localizationStatus = null;
    localizationProgress = null;
    process = { running: false, pid: null, exit_code: null };
    const game = games.find((entry) => entry.id === id);
    if (!game) return;
    if (!game.playable) {
      return;
    }
    const [loadedSettings, loadedHardware, loadedProcess, loadedLocalization] = await Promise.all([
      run(() => invoke<Settings>("load_settings", { gameId: id })),
      run(() => invoke<HardwareInfo>("probe_hardware", { gameId: id })),
      run(() => invoke<ProcessStatus>("process_status", { gameId: id })),
      game.capabilities.localization && game.installed
        ? run(() => invoke<LocalizationStatus>("localization_status", { gameId: id, profile: null }))
        : Promise.resolve(null)
    ]);
    if (loadedSettings) {
      settings = loadedSettings;
      savedSettingsFingerprint = settingsFingerprint(loadedSettings);
    }
    if (loadedHardware) hardware = loadedHardware;
    if (loadedProcess) process = loadedProcess;
    if (loadedLocalization) localizationStatus = loadedLocalization;
  }

  async function ensureSelectedRuntimeReady(action: string): Promise<boolean> {
    if (!selected?.playable) return true;
    const overview = await checkUpdates();
    const runtime = overview?.components.find((component) => component.id === `runtime.${selected.id}`)
      ?? selectedRuntimeComponent;
    const runtimeReady = !!runtime?.installed_version
      && runtime.state !== "corrupted"
      && runtime.state !== "repair_unavailable"
      && runtime.state !== "not_installed"
      && runtime.state !== "available";
    if (!runtimeReady) {
      activeView = "versions";
      versionFocus = `runtime.${selected.id}`;
      showNotice(`Install a game runtime from Versions before ${action}.`, "warning");
      return false;
    }
    return true;
  }

  async function importIso() {
    if (!selected || selected.id !== "cot" || navigationLocked) return;
    if (!await ensureSelectedRuntimeReady("setting up the game")) return;
    setupBusy = true;
    setupProgress = {
      game_id: selected.id,
      stage: "analyzing",
      progress: 0,
      detail: "Select ISO",
      files_done: 0,
      files_total: 0,
      bytes_done: 0,
      bytes_total: 0
    };
    const picked = await open({
      directory: false,
      multiple: false,
      title: `Import ${selected.name} ISO`,
      filters: [{ name: "Xbox ISO", extensions: ["iso"] }]
    });
    if (!picked || Array.isArray(picked)) {
      setupBusy = false;
      setupProgress = null;
      return;
    }

    setupProgress = {
      game_id: selected.id,
      stage: "analyzing",
      progress: 1,
      detail: "Preparing game setup...",
      files_done: 0,
      files_total: 0,
      bytes_done: 0,
      bytes_total: 0
    };
    const result = await run(() => invoke<GameSetupResult>("import_game_iso", {
      gameId: selected.id,
      isoPath: picked
    }));
    setupBusy = false;
    if (!result) return;
    showNotice("Setup complete.");
    await refreshGames();
    await selectGame(selected.id);
    if (
      settings
      && languageRequiresPack(settings.localization.profile, selected.id)
      && !localizationStatus?.overlay_ready
      && languageComponent(settings.localization.profile, selected.id)?.installed_version
    ) {
      await installLanguagePack();
    }
  }

  async function saveSettings(notify = true): Promise<boolean> {
    if (!selected || !settings) return false;
    if (!gameSettingsDirty) return true;
    const submittedSettings = structuredClone(settings);
    const submittedFingerprint = settingsFingerprint(submittedSettings);
    saving = true;
    const ok = await runAction(() => invoke<void>("save_settings", {
      gameId: selected.id,
      settings: submittedSettings
    }));
    saving = false;
    if (ok) {
      savedSettingsFingerprint = submittedFingerprint;
      if (notify) showNotice("Saved.");
    }
    return ok;
  }

  async function play() {
    if (!selected || !settings || process.running || navigationLocked) return;
    if (selected.id === "cot" && initialLanguageSetupRequired) {
      languageSetupSelection = settings.localization.profile ?? "en";
      continuePlayAfterLanguageSetup = true;
      languageSetupOpen = true;
      return;
    }
    if (!await ensureSelectedRuntimeReady("starting the game")) return;
    if (languageRequiresPack(settings.localization.profile) && !localizationStatus?.overlay_ready) {
      showNotice("Install the selected Localization Pack in Settings before starting the game.", "error");
      return;
    }
    if (!await saveSettings(false)) return;
    busy = true;
    const value = await run(() => invoke<ProcessStatus>("launch_game", { gameId: selected.id }));
    busy = false;
    if (value) {
      process = value;
      showNotice("Game started.");
    }
  }

  async function refreshLocalizationStatus() {
    if (!selected?.installed || !selected.capabilities.localization) {
      localizationStatus = null;
      return;
    }
    const value = await run(() => invoke<LocalizationStatus>("localization_status", {
      gameId: selected.id,
      profile: settings?.localization.profile ?? null
    }));
    if (value) localizationStatus = value;
  }

  function selectLocalizationProfile(profile: LocalizationProfile) {
    if (!settings) return;
    const option = languageOptions.find((entry) => entry.profile === profile);
    if (!option) return;
    settings.localization.profile = profile;
    settings.localization.xbox_language = option.xboxLanguage;
    void refreshLocalizationStatus();
  }

  function changeLocalizationProfile(event: Event) {
    selectLocalizationProfile((event.currentTarget as HTMLSelectElement).value as LocalizationProfile);
  }

  async function completeInitialLanguageSetup() {
    if (!launcherStorage?.configured || languageSetupSaving) return;
    const option = cotLanguageOptions.find((entry) => entry.profile === languageSetupSelection);
    if (!option) return;
    languageSetupSaving = true;
    const value = await run(() => invoke<LauncherStorageStatus>("complete_game_language_setup", {
      gameId: "cot",
      profile: option.profile,
      xboxLanguage: option.xboxLanguage
    }));
    languageSetupSaving = false;
    if (!value) return;
    launcherStorage = value;
    const shouldContinuePlay = continuePlayAfterLanguageSetup;
    continuePlayAfterLanguageSetup = false;
    languageSetupOpen = false;
    if (selectedId === "cot" && settings) {
      settings.localization.profile = option.profile;
      settings.localization.xbox_language = option.xboxLanguage;
      savedSettingsFingerprint = settingsFingerprint(settings);
      await refreshLocalizationStatus();
    }
    await tick();
    const component = languageComponent(option.profile, "cot");
    if (component && selectedRuntimeComponent?.installed_version && componentCanInstall(component)) {
      await installComponentUpdate(component, false, component.latest_version, false, true, true);
    }
    if (selectedId === "cot" && selected?.installed && languageRequiresPack(option.profile, "cot")) {
      await installLanguagePack();
    }
    showNotice(`Game language set to ${option.label}.`);
    if (shouldContinuePlay) await play();
  }

  async function installLanguagePack() {
    if (!selected || !settings || navigationLocked) return;
    const profile = settings.localization.profile;
    if (!languageRequiresPack(profile, selected.id)) return;
    localizationBusy = true;
    localizationProgress = {
      game_id: selected.id,
      profile,
      stage: "preparing",
      progress: 0,
      detail: "Preparing Localization Pack...",
      bytes_done: 0,
      bytes_total: 0
    };
    const value = await run(() => invoke<LocalizationStatus>("prepare_localization", {
      gameId: selected.id,
      profile
    }));
    localizationBusy = false;
    if (value) {
      localizationStatus = value;
      localizationProgress = null;
      showNotice("Localization Pack installed.");
    }
  }

  async function cancelLocalization() {
    if (!selected || !localizationBusy) return;
    await runAction(() => invoke<void>("cancel_localization", { gameId: selected.id }));
  }

  async function uninstallGame() {
    if (!selected || !selected.installed || process.running || navigationLocked) return;
    const approved = window.confirm(
      `Uninstall ${selected.name}?\n\nThis removes the managed game files only. Save data and settings will be preserved.`
    );
    if (!approved) return;

    uninstalling = true;
    const gameId = selected.id;
    const gameName = selected.name;
    const ok = await runAction(() => invoke<void>("uninstall_game", { gameId }));
    uninstalling = false;
    if (!ok) return;

    await refreshGames();
    await selectGame(gameId);
    activeView = "game";
    showNotice(`${gameName} uninstalled.`);
  }

  async function pollProcess() {
    if (!selected?.playable) return;
    const value = await run(() => invoke<ProcessStatus>("process_status", { gameId: selected.id }));
    if (!value) return;
    const wasRunning = process.running;
    process = value;
    if (wasRunning && !value.running) {
      showNotice(processExitMessage(value.exit_code), value.exit_code === 0 ? "success" : "error");
    }
  }

  function processExitMessage(code: number | null) {
    if (code === 0) return "Game closed.";
    if (code === null) return "Game exited unexpectedly.";
    const status = code >>> 0;
    const hex = `0x${status.toString(16).padStart(8, "0").toUpperCase()}`;
    if (status === 0xC0000374) {
      return `Game crashed (${hex}: heap corruption). Create a Support Package for diagnostics.`;
    }
    return `Game exited unexpectedly (${hex}, ${code}).`;
  }

  async function openFolder(kind: "save" | "logs" | "all_logs") {
    if (!selected) return;
    await runAction(() => invoke<void>("open_title_folder", { gameId: selected.id, kind }));
  }

  async function chooseLibraryFolder() {
    if (navigationLocked) return;
    const picked = await open({
      directory: true,
      multiple: false,
      title: "Choose the MojoRecomp game library",
      defaultPath: libraryPath || launcherStorage?.default_library_path
    });
    if (picked && !Array.isArray(picked)) {
      libraryPath = picked;
      await inspectLibraryPath(picked);
    }
  }

  async function inspectLibraryPath(path = libraryPath.trim()) {
    const serial = ++libraryProbeSerial;
    if (!path) {
      libraryPathStatus = null;
      return null;
    }
    try {
      const value = await invoke<LibraryPathStatus>("inspect_game_library", { path });
      if (serial === libraryProbeSerial) libraryPathStatus = value;
      return value;
    } catch {
      if (serial === libraryProbeSerial) libraryPathStatus = null;
      return null;
    }
  }

  function scheduleLibraryPathInspection() {
    libraryPathStatus = null;
    if (libraryProbeTimer) clearTimeout(libraryProbeTimer);
    libraryProbeTimer = setTimeout(() => {
      libraryProbeTimer = null;
      void inspectLibraryPath();
    }, 180);
  }

  async function applyLibraryLocation() {
    const requestedPath = libraryPath.trim();
    if (!requestedPath || navigationLocked || !libraryDirty) return;
    const pathStatus = libraryProbeMatchesPath && libraryPathStatus
      ? libraryPathStatus
      : await inspectLibraryPath(requestedPath);
    if (!pathStatus?.valid) {
      showNotice(pathStatus?.error ?? "Choose a valid game library location.", "warning");
      return;
    }
    if (!pathStatus.enough_space) {
      showNotice(
        `Not enough free space. ${formatBytes(pathStatus.required_free_bytes)} is required.`,
        "warning"
      );
      return;
    }
    const wasFirstLaunch = firstLaunchPending;
    libraryProgress = null;
    libraryBusy = true;
    const value = await run(() => invoke<LauncherStorageStatus>("set_game_library", {
      path: requestedPath
    }));
    libraryBusy = false;
    if (!value) return;
    launcherStorage = value;
    libraryPath = value.library_path;
    libraryProgress = null;
    await refreshGames();
    await selectGame(selectedId);
    activeView = wasFirstLaunch ? "game" : "launcher-settings";
    showNotice(value.notice ?? "Game library ready.", value.notice ? "warning" : "success");
  }

  async function openGameLibrary() {
    if (navigationLocked) return;
    await runAction(() => invoke<void>("open_game_library"));
  }

  async function createSupportArchive() {
    if (!selected) return;
    busy = true;
    const path = await run(() => invoke<string>("create_support_package", {
      gameId: selected.id,
      includeMinidump: includeSupportMinidump
    }));
    busy = false;
    if (path) showNotice("Support package created.");
  }

  async function openLicenseNotices() {
    await runAction(() => invoke<void>("open_license_notices"));
  }

  async function checkUpdates(offlineOnly = false): Promise<UpdateOverview | null> {
    if (checkingUpdates) return updates;
    checkingUpdates = true;
    const value = await run(() => invoke<UpdateOverview>("check_component_updates", { offlineOnly }));
    checkingUpdates = false;
    if (value) {
      updates = value;
      if (settings && languageRequiresPack(settings.localization.profile, selectedId)) {
        const installedLanguage = value.components.find(
          (component) => component.kind === "language"
            && component.game_id === selectedId
            && component.locale?.toLowerCase() === settings?.localization.profile.toLowerCase()
            && !!component.installed_version
        );
        if (!installedLanguage) {
          settings.localization.profile = "en";
          settings.localization.xbox_language = 1;
        }
      }
    }
    return value;
  }

  async function handleLauncherUpdate(overview: UpdateOverview | null, manual = false) {
    if (!overview) return;
    if (overview.error) {
      if (manual) showNotice("Couldn't check for launcher updates.", "warning");
      return;
    }
    const component = overview.components.find((entry) => entry.id === "launcher") ?? null;
    if (!component) return;
    if (component.last_action === "applying") return;
    if (component.last_action === "apply_failed") {
      if (manual) {
        showNotice("Retrying the verified launcher update...", "warning");
        await applyLauncherUpdate();
      }
      return;
    }
    if (component.state === "incompatible") {
      if (manual) showNotice("The available launcher update is not compatible with the active game components.", "warning");
      return;
    }
    if (component.last_action === "ready_restart") {
      showNotice("Verified launcher update is ready. Restarting to install it...", "warning");
      await applyLauncherUpdate();
      return;
    }
    if (component.state === "update_available" && componentHasDownload(component)) {
      showNotice(`Launcher ${component.latest_version ?? "update"} found. Downloading automatically...`, "warning");
      await installComponentUpdate(component, false, component.latest_version, true);
      return;
    }
    if (manual) showNotice(`MojoRecomp Launcher ${component.installed_version ?? ""} is up to date.`);
  }

  async function checkLauncherUpdates(manual = false) {
    if (checkingUpdates || navigationLocked) return;
    checkingUpdates = true;
    const overview = await run(() => invoke<UpdateOverview>("check_launcher_update"));
    checkingUpdates = false;
    if (overview) launcherUpdates = overview;
    await handleLauncherUpdate(overview, manual);
  }

  async function checkLauncherUpdateNow() {
    await checkLauncherUpdates(true);
  }

  async function installComponentUpdate(
    component: ComponentUpdateStatus,
    reinstall = false,
    version: string | null = null,
    autoApplyLauncher = false,
    activate = true,
    silent = false
  ): Promise<ComponentUpdateResult | null> {
    if (updateInteractionBusy) return null;
    updateBusy = true;
    versionFocus = component.id;
    const result = await run(() => invoke<ComponentUpdateResult>("install_component_update", {
      componentId: component.id,
      version,
      reinstall,
      activate
    }));
    updateBusy = false;
    if (!result) return null;
    await refreshGames();
    if (component.kind === "launcher") launcherUpdates = null;
    await checkUpdates();
    if (component.kind === "language" && component.game_id === selectedId) {
      await refreshLocalizationStatus();
      if (
        selected?.installed
        && settings
        && component.locale?.toLowerCase() === settings.localization.profile.toLowerCase()
        && languageRequiresPack(settings.localization.profile, selectedId)
        && !localizationStatus?.overlay_ready
      ) {
        await installLanguagePack();
      }
    }
    if (silent) return result;
    if (result.state === "downloaded") {
      showNotice(`Runtime ${version ?? "version"} downloaded. Activate it when you want to use it.`);
    } else if (result.restart_required) {
      if (autoApplyLauncher && component.kind === "launcher") {
        showNotice("Launcher update verified. Restarting to install it...", "warning");
        await applyLauncherUpdate();
        return result;
      }
      showNotice("Launcher update verified. Restart the launcher to install it.", "warning");
    } else if (component.state === "corrupted") {
      showNotice("Component repaired successfully.");
    } else if (!component.installed_version) {
      showNotice("Component installed successfully.");
    } else if (reinstall || version === component.installed_version) {
      showNotice("Component reinstalled successfully.");
    } else if (version && version !== component.latest_version) {
      showNotice(component.kind === "runtime"
        ? `Runtime ${version} is now active.`
        : `Component version ${version} is ready.`);
    } else {
      showNotice("Component updated successfully.");
    }
    return result;
  }

  function toggleRuntimeAdditionalLanguage(id: string, checked: boolean) {
    runtimeAdditionalSelection = checked
      ? [...new Set([...runtimeAdditionalSelection, id])]
      : runtimeAdditionalSelection.filter((value) => value !== id);
  }

  function dismissRuntimeAdditionalContent() {
    runtimeAdditionalContent = null;
    runtimeAdditionalSelection = [];
    pendingRuntimeAction = null;
  }

  function closeRuntimeAdditionalContent() {
    if (runtimeAdditionalBusy) return;
    dismissRuntimeAdditionalContent();
  }

  async function executeRuntimeAction(action: PendingRuntimeAction, languageIds: string[]) {
    let completed = false;
    if (action.kind === "install") {
      const result = await installComponentUpdate(
        action.component,
        action.reinstall,
        action.version,
        false,
        action.activate,
        languageIds.length > 0
      );
      completed = !!result;
    } else {
      updateBusy = true;
      completed = await runAction(() => invoke<void>("activate_runtime_version", {
        componentId: action.component.id,
        version: action.version
      }));
      updateBusy = false;
      if (completed) {
        await refreshGames();
        await checkUpdates();
      }
    }
    if (!completed) return false;

    if (languageIds.length > 0) {
      runtimeAdditionalBusy = true;
      const installed = await run(() => invoke<LocalizationPackInstallResult>("install_runtime_additional_content", {
        componentId: action.component.id,
        version: action.version,
        languageIds
      }));
      runtimeAdditionalBusy = false;
      if (!installed) return false;
      await refreshGames();
      await checkUpdates();
      if (selected?.installed) await refreshLocalizationStatus();
      showNotice(
        `Runtime ${action.version} is ready with ${installed.installed_languages.length} selected Localization Pack language${installed.installed_languages.length === 1 ? "" : "s"}.`
      );
    } else if (action.kind === "activate") {
      showNotice(`Runtime ${action.version} is now active.`);
    }
    return true;
  }

  async function requestRuntimeAction(action: PendingRuntimeAction) {
    if (updateInteractionBusy || pendingRuntimeAction) return;
    if (action.kind === "install" && !action.activate) {
      await executeRuntimeAction(action, []);
      return;
    }
    const content = await run(() => invoke<RuntimeAdditionalContentStatus | null>("runtime_additional_content", {
      componentId: action.component.id,
      version: action.version
    }));
    if (!content || content.languages.length === 0) {
      await executeRuntimeAction(action, []);
      return;
    }
    if (content.languages.every((language) => language.installed_version === language.version)) {
      await executeRuntimeAction(action, []);
      return;
    }
    pendingRuntimeAction = action;
    runtimeAdditionalContent = content;
    runtimeAdditionalSelection = content.languages
      .filter((language) => language.installed_version !== language.version)
      .map((language) => language.id);
  }

  async function confirmRuntimeAdditionalContent() {
    if (!pendingRuntimeAction || runtimeAdditionalBusy) return;
    const action = pendingRuntimeAction;
    const selectedLanguages = [...runtimeAdditionalSelection];
    dismissRuntimeAdditionalContent();
    await executeRuntimeAction(action, selectedLanguages);
  }

  async function skipRuntimeAdditionalContent() {
    if (!pendingRuntimeAction || runtimeAdditionalBusy) return;
    const action = pendingRuntimeAction;
    dismissRuntimeAdditionalContent();
    await executeRuntimeAction(action, []);
  }

  async function installRuntimeRelease(release: ComponentReleaseStatus) {
    if (!selectedRuntimeComponent) return;
    if (process.running) {
      showNotice(`Close ${selected?.name ?? "the game"} before changing runtime versions.`, "warning");
      return;
    }
    const isActive = release.version === selectedRuntimeComponent.installed_version;
    const activate = !selectedRuntimeComponent.installed_version || isActive;
    await requestRuntimeAction({
      kind: "install",
      component: selectedRuntimeComponent,
      version: release.version,
      reinstall: isActive,
      activate
    });
  }

  async function downloadLatestRuntime(component: ComponentUpdateStatus) {
    if (!component.latest_version) return;
    const isActive = component.latest_version === component.installed_version;
    const activate = !component.installed_version || isActive;
    await requestRuntimeAction({
      kind: "install",
      component,
      version: component.latest_version,
      reinstall: isActive,
      activate
    });
  }

  async function activateRuntimeVersion(version: string) {
    if (!selectedRuntimeComponent || updateInteractionBusy) return;
    if (process.running) {
      showNotice(`Close ${selected?.name ?? "the game"} before changing runtime versions.`, "warning");
      return;
    }
    await requestRuntimeAction({
      kind: "activate",
      component: selectedRuntimeComponent,
      version
    });
  }

  async function removeRuntimeVersion(version: string) {
    if (!selectedRuntimeComponent || updateInteractionBusy) return;
    if (process.running) {
      showNotice(`Close ${selected?.name ?? "the game"} before removing a runtime version.`, "warning");
      return;
    }
    const isActive = version === selectedRuntimeComponent.installed_version;
    const approved = window.confirm(
      isActive
        ? `Remove the active ${selected?.name ?? "game"} Runtime ${version}?\n\nYou will need to activate another downloaded runtime before playing.`
        : `Remove the downloaded ${selected?.name ?? "game"} Runtime ${version}?`
    );
    if (!approved) return;
    updateBusy = true;
    const ok = await runAction(() => invoke<void>("remove_runtime_version", {
      componentId: selectedRuntimeComponent.id,
      version
    }));
    updateBusy = false;
    if (!ok) return;
    await refreshGames();
    await checkUpdates();
    showNotice(isActive
      ? `Runtime ${version} removed. Activate another downloaded version before playing.`
      : `Runtime ${version} removed.`,
      isActive ? "warning" : "success");
  }

  async function installOfflineRuntime() {
    if (updateInteractionBusy) return;
    const picked = await open({
      directory: false,
      multiple: false,
      title: "Install Game Runtime from ZIP",
      filters: [{ name: "MojoRecomp Runtime", extensions: ["zip"] }]
    });
    if (!picked || Array.isArray(picked)) return;
    updateBusy = true;
    const result = await run(() => invoke<ComponentUpdateResult>("install_offline_runtime_package", {
      path: picked
    }));
    updateBusy = false;
    if (!result) return;
    versionFocus = result.component_id;
    await refreshGames();
    await checkUpdates();
    showNotice("Offline runtime installed successfully.");
  }

  async function installOfflineLocalizationPack() {
    if (updateInteractionBusy || navigationLocked) return;
    const picked = await open({
      directory: false,
      multiple: false,
      title: "Install Localization Pack from ZIP",
      filters: [{ name: "MojoRecomp Localization Pack", extensions: ["zip"] }]
    });
    if (!picked || Array.isArray(picked)) return;
    updateBusy = true;
    const result = await run(() => invoke<LocalizationPackInstallResult>("install_offline_localization_pack", {
      path: picked
    }));
    updateBusy = false;
    if (!result) return;
    await checkUpdates(true);
    if (result.game_id === selectedId) {
      await refreshLocalizationStatus();
      if (selected?.installed && settings && languageRequiresPack(settings.localization.profile, selectedId)) {
        const installedSelected = result.installed_languages.some(
          (locale) => locale.toLowerCase() === settings?.localization.profile.toLowerCase()
        );
        if (installedSelected && !localizationStatus?.overlay_ready) await installLanguagePack();
      }
    }
    showNotice(`Localization Pack ${result.version} installed (${result.installed_languages.length} language${result.installed_languages.length === 1 ? "" : "s"}).`);
  }

  async function applyLauncherUpdate() {
    if (updateBusy || saving) return;
    if (gameSettingsDirty && !await saveSettings(false)) {
      showNotice("Save the current game settings before installing the launcher update.", "warning");
      return;
    }
    updateBusy = true;
    const ok = await runAction(() => invoke<void>("apply_launcher_update"));
    if (!ok) updateBusy = false;
  }

  async function rollbackComponentUpdate(component: ComponentUpdateStatus) {
    if (updateInteractionBusy) return;
    updateBusy = true;
    const ok = await runAction(() => invoke<void>("rollback_component_update", {
      componentId: component.id
    }));
    updateBusy = false;
    if (!ok) return;
    await refreshGames();
    await checkUpdates();
    if (component.kind === "language" && component.game_id === selectedId) {
      await refreshLocalizationStatus();
    }
    showNotice("Previous component version restored.");
  }

  function componentStatusLabel(component: ComponentUpdateStatus | null) {
    if (!component) return "Unavailable";
    const lastAction = component.installed_version ? component.last_action : null;
    if (lastAction === "ready_restart") return "Ready to install";
    if (component.state === "repair_unavailable") return "Repair unavailable";
    if (component.state === "corrupted") return "Repair required";
    if (lastAction === "failed") return "Update failed";
    if (lastAction === "rolled_back") return "Rolled back";
    if (component.state === "update_available") return "Update available";
    if (component.state === "available") return component.kind === "language" ? "Available" : "Download available";
    if (component.state === "incompatible") return "Incompatible";
    if (component.state === "not_installed") return "Not installed";
    if (lastAction === "recovered") return "Recovered";
    if (lastAction === "installed") return "Installed";
    return updates?.configured ? "Up to date" : "Installed";
  }

  function componentDescription(component: ComponentUpdateStatus | null) {
    if (!component) return "Component status is unavailable.";
    const lastAction = component.installed_version ? component.last_action : null;
    if (updates?.error) return "Couldn't check for updates.";
    if (!updates?.configured) return "Installed locally.";
    if (lastAction === "ready_restart") {
      return "The verified launcher executable is ready to install on restart.";
    }
    if (component.state === "repair_unavailable") {
      return "This component is damaged, and no compatible repair is available.";
    }
    if (component.state === "corrupted") {
      return "This component failed its integrity check. A verified repair is available.";
    }
    if (lastAction === "failed") {
      return "The previous update attempt failed. You can safely try again.";
    }
    if (lastAction === "rolled_back") return "The previous component version is active again.";
    if (component.state === "update_available") return "A newer compatible component release is available.";
    if (component.state === "available") {
      if (component.kind === "runtime") return "This required game runtime is not installed. Install it to set up and play the game.";
      if (component.kind === "language") return "This optional Localization Pack can be installed.";
      return "This component can be downloaded.";
    }
    if (component.state === "incompatible") return "No compatible update is available.";
    if (component.state === "not_installed") {
      if (component.kind === "runtime") return "This required game runtime is not installed.";
      return "This component is not installed.";
    }
    if (lastAction === "recovered") return "An interrupted update was recovered and verified.";
    if (lastAction === "installed") return "The verified component update is installed.";
    return "This component is up to date.";
  }

  function launcherDescription(component: ComponentUpdateStatus | null) {
    if (launcherUpdateError) return "Couldn't check for launcher updates.";
    if (!component) return "Launcher update status is unavailable.";
    if (component.state === "incompatible") return "The available launcher update is not compatible with the active game components.";
    if (component.last_action === "apply_failed") return "The previous launcher update could not be applied. You can try again.";
    if (component.last_action === "applying") return "The launcher update is being verified after restart.";
    if (component.last_action === "ready_restart") return "The verified launcher update is ready to install.";
    if (component.state === "update_available") return "A newer MojoRecomp Launcher release is available.";
    if (launcherUpdates !== null && !launcherUpdates.configured) return "No launcher update catalog is configured for this build.";
    return "MojoRecomp Launcher is up to date.";
  }

  function componentCanInstall(component: ComponentUpdateStatus | null) {
    return !!component
      && !!updates?.configured
      && !updates.error
      && componentHasDownload(component)
      && (component.state === "update_available"
        || component.state === "available"
        || component.state === "not_installed"
        || component.state === "corrupted");
  }

  function componentCanReinstall(component: ComponentUpdateStatus | null) {
    return !!component
      && component.kind === "runtime"
      && component.state === "up_to_date"
      && !!component.installed_version
      && !!updates?.configured
      && !updates.error
      && componentHasDownload(component);
  }

  function latestRuntimeCanActivate(component: ComponentUpdateStatus | null) {
    if (!component?.latest_version || component.latest_version === component.installed_version) return false;
    return component.installed_versions.some(
      (entry) => entry.version === component.latest_version && entry.healthy
    );
  }

  function componentActionLabel(component: ComponentUpdateStatus) {
    if (component.state === "corrupted") return "Repair";
    if (!component.installed_version) return component.kind === "language" ? "Install" : "Download";
    if (component.kind === "launcher") return "Download verified update";
    return "Update";
  }

  function latestRuntimeActionLabel(component: ComponentUpdateStatus) {
    if (component.state === "corrupted" && component.latest_version === component.installed_version) {
      return `Repair ${component.latest_version ?? "runtime"}`;
    }
    if (!component.installed_version) return `Install ${component.latest_version ?? "latest"}`;
    if (component.latest_version !== component.installed_version) return `Download ${component.latest_version ?? "latest"}`;
    return `Redownload ${component.latest_version ?? component.installed_version}`;
  }

  function latestReleaseBadgeLabel() {
    if (checkingUpdates && !updates) return "Checking releases";
    if (updates?.error) return "Release status";
    if (updates && !updates.configured) return "Local status";
    return "Latest release";
  }

  function latestReleaseHeadline(component: ComponentUpdateStatus | null) {
    if (checkingUpdates && !updates) return "Checking releases...";
    if (updates?.error) return "Update information unavailable";
    if (updates && !updates.configured) return "No release catalog configured";
    return component?.latest_version ?? "No compatible release";
  }

  function releaseHistoryEmptyMessage() {
    if (checkingUpdates && !updates) return "Checking release history...";
    if (updates?.error) return "Release history is unavailable while the update check is failing.";
    if (updates && !updates.configured) return "No update catalog is configured for this launcher build.";
    return "No compatible runtime releases are available from the update catalog.";
  }

  function formatReleaseDate(value: string | null) {
    if (!value) return "Unknown date";
    const parsed = new Date(`${value}T00:00:00`);
    return Number.isNaN(parsed.getTime())
      ? value
      : parsed.toLocaleDateString("en-US", { year: "numeric", month: "short", day: "numeric" });
  }

  function humanBytes(value: number | null) {
    if (!value) return "-";
    const mib = value / (1024 * 1024);
    return mib >= 1024
      ? (mib / 1024).toFixed(2) + " GiB"
      : mib.toFixed(1) + " MiB";
  }

  function settingsFingerprint(value: Settings) {
    return JSON.stringify(value);
  }

  function normalizedPath(value: string) {
    const path = value.trim().replaceAll("/", "\\");
    if (!path) return "";

    const uncRoot = path.match(/^\\\\([^\\]+)\\([^\\]+)(?:\\|$)/);
    const driveRoot = path.match(/^([a-zA-Z]:)(?:\\|$)/);
    const root = uncRoot
      ? `\\\\${uncRoot[1]}\\${uncRoot[2]}`
      : driveRoot?.[1] ?? "";
    const remainder = path.slice(uncRoot?.[0].length ?? driveRoot?.[0].length ?? 0);
    const parts: string[] = [];

    for (const part of remainder.split("\\")) {
      if (!part || part === ".") continue;
      if (part === "..") {
        if (parts.length > 0 && parts.at(-1) !== "..") parts.pop();
        else if (!root) parts.push(part);
        continue;
      }
      parts.push(part);
    }

    return [root, ...parts].filter(Boolean).join("\\").toLocaleLowerCase();
  }

  function filteringDisabled(level: number) {
    return !hardware?.sampler_anisotropy || (hardware?.max_anisotropy ?? 1) < level;
  }

  function formatBytes(bytes: number) {
    if (!bytes) return "0 B";
    const units = ["B", "KiB", "MiB", "GiB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
  }

  function setupStageTitle(stage: GameSetupProgress["stage"]) {
    switch (stage) {
      case "analyzing": return "Reading ISO";
      case "extracting": return "Extracting files";
      case "validating": return "Validating files";
      case "installing": return "Installing game";
      case "complete": return "Setup complete";
      case "failed": return "Setup failed";
    }
  }

  function minimizeWindow() {
    void getCurrentWindow().minimize();
  }

  function toggleMaximizeWindow() {
    void getCurrentWindow().toggleMaximize();
  }

  function closeWindow() {
    void getCurrentWindow().close();
  }

  function preventContextMenu(event: MouseEvent) {
    if (event.target instanceof HTMLInputElement || event.target instanceof HTMLTextAreaElement) return;
    event.preventDefault();
  }

  function preventSelectAll(event: KeyboardEvent) {
    if (
      event.target instanceof HTMLInputElement
      || event.target instanceof HTMLTextAreaElement
      || (event.target instanceof HTMLElement && event.target.isContentEditable)
    ) return;
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
      event.preventDefault();
    }
  }

  onMount(() => {
    const setupListener = listen<GameSetupProgress>("game-setup-progress", (event) => {
      if (event.payload.game_id === selectedId) setupProgress = event.payload;
    });
    const localizationListener = listen<LocalizationProgress>("localization-progress", (event) => {
      if (event.payload.game_id !== selectedId) return;
      localizationProgress = event.payload;
      if (
        event.payload.stage === "complete"
        || event.payload.stage === "failed"
        || event.payload.stage === "cancelled"
      ) {
        localizationBusy = false;
        void refreshLocalizationStatus();
      }
    });
    const libraryListener = listen<LibraryMigrationProgress>("library-migration-progress", (event) => {
      libraryProgress = event.payload;
      if (event.payload.stage === "failed" || event.payload.stage === "complete") {
        libraryBusy = false;
      }
    });
    const updateListener = listen<ComponentUpdateProgress>("component-update-progress", (event) => {
      updateProgress = event.payload;
      if (
        event.payload.stage === "failed"
        || event.payload.stage === "complete"
        || event.payload.stage === "ready_restart"
      ) {
        updateBusy = false;
      }
    });
    void (async () => {
      launcherVersion = await getVersion().catch(() => "");
      const loadedStorage = await run(() => invoke<LauncherStorageStatus>("get_launcher_storage"));
      if (loadedStorage) {
        launcherStorage = loadedStorage;
        discordActivityEnabled = loadedStorage.discord_activity_enabled;
        libraryPath = loadedStorage.library_path || loadedStorage.default_library_path;
        await inspectLibraryPath(libraryPath);
        if (!loadedStorage.configured) activeView = "launcher-settings";
      }
      await refreshGames();
      await selectGame(selectedId);
      await checkUpdates();
      await checkLauncherUpdates(false);
      languageSetupSelection = settings?.localization.profile ?? "en";
      startupReady = true;
    })();
    pollTimer = setInterval(pollProcess, 1200);
    window.addEventListener("contextmenu", preventContextMenu, true);
    window.addEventListener("keydown", preventSelectAll, true);
    return () => {
      if (pollTimer) clearInterval(pollTimer);
      if (toastTimer) clearTimeout(toastTimer);
      if (libraryProbeTimer) clearTimeout(libraryProbeTimer);
      window.removeEventListener("contextmenu", preventContextMenu, true);
      window.removeEventListener("keydown", preventSelectAll, true);
      void setupListener.then((unlisten) => unlisten());
      void localizationListener.then((unlisten) => unlisten());
      void libraryListener.then((unlisten) => unlisten());
      void updateListener.then((unlisten) => unlisten());
    };
  });
</script>

{#snippet setupMojo()}
  <div class="setup-mojo" aria-hidden="true"><span></span><span></span><span></span></div>
{/snippet}

<main
  class:theme-cot={selectedId === "cot"}
  class:theme-mom={selectedId === "mom"}
  class:utility-view={activeView === "launcher" || activeView === "launcher-settings" || activeView === "help"}
  class:game-uninstalled={(activeView === "game" || activeView === "versions") && !!selected && !selected.installed}
  class="app-shell"
>
  <header class="titlebar" data-tauri-drag-region>
    <div class="suite-brand" data-tauri-drag-region>
      <div class="suite-mark" aria-hidden="true"><img src={mojoRecompIcon} alt="" /></div>
      <div class="suite-name" data-tauri-drag-region><strong>MojoRecomp</strong><span>Launcher</span></div>
    </div>
    <div class="window-controls">
      <button type="button" aria-label="Minimize" title="Minimize" onclick={minimizeWindow}><svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 8.5h10" /></svg></button>
      <button type="button" aria-label="Maximize or restore" title="Maximize or restore" onclick={toggleMaximizeWindow}><svg viewBox="0 0 16 16" aria-hidden="true"><rect x="3.5" y="3.5" width="9" height="9" rx=".5" /></svg></button>
      <button class="close-window" type="button" aria-label="Close" title="Close" onclick={closeWindow}><svg viewBox="0 0 16 16" aria-hidden="true"><path d="m4 4 8 8m0-8-8 8" /></svg></button>
    </div>
  </header>

  <div class="workspace">
    <aside class="game-rail" aria-label="Launcher navigation">
      <div class="game-switcher">
        {#each games as game}
          <button
            type="button"
            class:active={selectedId === game.id && (activeView === "game" || activeView === "versions" || activeView === "settings")}
            class:installed={game.installed}
            class="rail-game"
            aria-label={gameNeedsAttention(game.id) ? `${game.name}, update available` : game.name}
            title={firstLaunchPending ? "Choose a game library before continuing" : navigationLocked ? "Finish the current operation before switching games" : game.name}
            onclick={() => openGame(game.id)}
            disabled={navigationLocked || firstLaunchPending}
          >
            <img src={game.id === "mom" ? "/art/mom-icon.png" : "/art/cot-icon.png"} alt="" />
            <span class:ready={game.installed && game.playable} class="rail-status"></span>
            {#if gameNeedsAttention(game.id)}<span class="rail-update-badge" aria-hidden="true">!</span>{/if}
          </button>
        {/each}
      </div>
      <div class="rail-spacer"></div>
      <button
        type="button"
        class:updating={!!launcherUpdateProgress && updateBusy}
        class:update-available={componentNeedsAttention(launcherComponent)}
        class:active={activeView === "launcher"}
        class="rail-launcher"
        aria-label={`MojoRecomp Launcher ${currentLauncherVersion}`}
        title={checkingUpdates
          ? "Checking for launcher updates..."
          : updateBusy && launcherUpdateProgress
            ? launcherUpdateProgress.detail
            : `MojoRecomp Launcher v${currentLauncherVersion}`}
        onclick={openLauncher}
        disabled={checkingUpdates || navigationLocked}
      >
        <img src={mojoRecompIcon} alt="" />
        <span class="rail-launcher-version">v{currentLauncherVersion}</span>
        {#if launcherUpdateProgress && updateBusy}
          <span class="rail-launcher-progress" aria-hidden="true">{launcherUpdateProgress.progress}%</span>
        {:else if componentNeedsAttention(launcherComponent)}
          <span class="rail-update-badge" aria-hidden="true">!</span>
        {/if}
      </button>
      <button
        type="button"
        class:active={activeView === "launcher-settings"}
        class="rail-help rail-settings"
        aria-label="Launcher Settings"
        title={navigationLocked ? "Finish the current operation before opening Launcher Settings" : "Launcher Settings"}
        onclick={openLauncherSettings}
        disabled={navigationLocked}
      ><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 8.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Zm7.5 3.5-1.7-.65a6.1 6.1 0 0 0-.51-1.24l.73-1.67-2.46-2.46-1.67.73a6.1 6.1 0 0 0-1.24-.51L12 4.5H8.5l-.65 1.7c-.43.14-.85.31-1.24.51l-1.67-.73-2.46 2.46.73 1.67c-.2.39-.37.81-.51 1.24L1 12l1.7.65c.14.43.31.85.51 1.24l-.73 1.67 2.46 2.46 1.67-.73c.39.2.81.37 1.24.51l.65 1.7H12l.65-1.7c.43-.14.85-.31 1.24-.51l1.67.73 2.46-2.46-.73-1.67c.2-.39.37-.81.51-1.24Z" /></svg></button>
      <button
        type="button"
        class:active={activeView === "help"}
        class="rail-help"
        aria-label="Help"
        title={firstLaunchPending ? "Choose a game library before opening Help" : navigationLocked ? "Finish the current operation before opening Help" : "Help"}
        onclick={toggleHelp}
        disabled={navigationLocked || firstLaunchPending}
      >?</button>
    </aside>

    <section class="main-stage">
      <div class="ambient-backdrop" aria-hidden="true">
        <div class="sun-glow"></div><div class="totem-ring ring-one"></div><div class="totem-ring ring-two"></div>
        <div class="leaf leaf-one"></div><div class="leaf leaf-two"></div>
        <div class="mojo-stream mojo-blue"></div><div class="mojo-stream mojo-core"></div>
        {#if activeView === "game" || activeView === "versions" || activeView === "settings"}
          {#key selectedId}<img class="character-ghost game-switch-ghost" src={selectedIcon} alt="" />{/key}
        {/if}
        <div class="grain"></div>
      </div>

      {#if selected && (activeView === "game" || activeView === "versions")}
        <nav class="game-section-tabs" aria-label={`${selected.name} pages`}>
          <button type="button" class:active={activeView === "game"} onclick={() => showView("game")} disabled={navigationLocked}>Overview</button>
          <button type="button" class:active={activeView === "versions"} class="versions-tab" aria-label={versionsNeedsAttention ? "Versions, update available" : "Versions"} onclick={openVersions} disabled={navigationLocked}>
            Versions
            {#if versionsNeedsAttention}<span class="versions-tab-badge" aria-hidden="true">!</span>{/if}
          </button>
        </nav>
      {/if}

      {#if activeView === "game" && selected}
        {#key selectedId}<section class="game-home game-switch-view" aria-labelledby="game-title">
          <div class="game-identity">
            <img class="game-logo" class:mom-logo={selected.id === "mom"} src={selectedLogo} alt={selected.name} />
            <h1 id="game-title" class="visually-hidden">{selected.name}</h1>
          </div>

          <div class="launch-deck">
            <div class="launch-status">
              <div class:live={process.running} class:ready={selected.installed && selected.playable} class="status-orb"></div>
              <div>
                <span>{process.running ? "Running" : selected.playable ? (selected.installed ? "Ready to play" : "Setup required") : "In development"}</span>
              </div>
            </div>

            {#if selected.playable && selected.installed}
              <button type="button" class="play-button" onclick={play} disabled={!settings || navigationLocked || process.running || (!!settings && languageRequiresPack(settings.localization.profile) && !localizationStatus?.overlay_ready)}>
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 7 8 5-8 5Z" /></svg><span>{process.running ? "Running" : busy ? "Starting..." : "Play"}</span>
              </button>
              <div class="deck-actions installed-actions">
                <button type="button" onclick={openSettings} disabled={!settings || navigationLocked || process.running}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 8.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Zm7.5 3.5-1.7-.65a6.1 6.1 0 0 0-.51-1.24l.73-1.67-2.46-2.46-1.67.73a6.1 6.1 0 0 0-1.24-.51L12 4.5H8.5l-.65 1.7c-.43.14-.85.31-1.24.51l-1.67-.73-2.46 2.46.73 1.67c-.2.39-.37.81-.51 1.24L1 12l1.7.65c.14.43.31.85.51 1.24l-.73 1.67 2.46 2.46 1.67-.73c.39.2.81.37 1.24.51l.65 1.7H12l.65-1.7c.43-.14.85-.31 1.24-.51l1.67.73 2.46-2.46-.73-1.67c.2-.39.37-.81.51-1.24Z" /></svg>Game Settings</button>
                <button type="button" onclick={() => openFolder("save")} disabled={uninstalling}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3.5 6.5h6l2 2h9v10h-17Z" /></svg>Save Folder</button>
                <button type="button" class="danger-action" onclick={uninstallGame} disabled={process.running || navigationLocked}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 7h14M9 7V4h6v3m-8 0 1 13h8l1-13M10 10v7m4-7v7" /></svg>{uninstalling ? "Uninstalling..." : "Uninstall"}</button>
              </div>
            {:else if selected.playable}
              <button type="button" class="setup-button" onclick={importIso} disabled={navigationLocked}>
                <span class="setup-glyph"><img src={mojoRecompIcon} alt="" /></span>
                <span><strong>SET UP</strong></span>
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 5 7 7-7 7" /></svg>
              </button>
            {:else}
              <div class="coming-message"><strong>Work In Progress...</strong></div>
            {/if}
          </div>

          {#if setupBusy && setupProgress}
            <section class="setup-experience setup-surface" aria-live="polite">
              {@render setupMojo()}
              <img class="setup-emblem" src={mojoRecompIcon} alt="" />
              <img class="setup-game-logo" src={selectedLogo} alt="" />
              <span class="overline">MojoRecomp game setup</span>
              <h2>{setupStageTitle(setupProgress.stage)}</h2>
              <p>{setupProgress.detail}</p>
              <div class="setup-progress-large">
                <div class="progress-track"><span style={`width: ${setupProgress.progress}%`}></span></div>
                <b>{setupProgress.progress}%</b>
              </div>
              <div class="setup-metrics">
                {#if setupProgress.files_total > 0}<span>{setupProgress.files_done} / {setupProgress.files_total} files</span>{/if}
                {#if setupProgress.bytes_total > 0}<span>{formatBytes(setupProgress.bytes_done)} / {formatBytes(setupProgress.bytes_total)}</span>{/if}
              </div>
            </section>
          {/if}
        </section>{/key}
      {:else if activeView === "settings" && selected}
        <section class="page-view settings-view">
          <header class="page-header compact-header"><div><span class="overline">{selected.name}</span><h1>Game Settings</h1></div></header>
          {#if settings}
              <div class="settings-layout minimal-settings">
                {#if selected.capabilities.localization}
                  <section class="panel-card language-card full-width">
                    <div class="panel-heading compact"><div><h2>Language</h2></div></div>
                    <div class="language-combo">
                      <span class={"language-flag " + selectedLanguage.flag} aria-hidden="true"></span>
                      <div class="language-combo-copy">
                        <select
                          aria-label="Game language"
                          value={settings.localization.profile}
                          onchange={changeLocalizationProfile}
                          disabled={navigationLocked}
                        >
                          {#each languageOptions as language}
                            <option value={language.profile}>{language.label}{languageRequiresPack(language.profile) && settings.localization.profile === language.profile && !localizationStatus?.overlay_ready ? " - Not installed" : ""}</option>
                          {/each}
                        </select>
                      </div>
                    </div>
                    {#if languageRequiresPack(settings.localization.profile) && !localizationStatus?.overlay_ready}
                      <div class="localization-pack-actions">
                        <button
                          type="button"
                          class="secondary-button localization-pack-button"
                          onclick={installLanguagePack}
                          disabled={navigationLocked}
                        >Install Localization Pack</button>
                        <button
                          type="button"
                          class="secondary-button localization-pack-button"
                          onclick={installOfflineLocalizationPack}
                          disabled={navigationLocked}
                        >Install Pack from ZIP</button>
                      </div>
                    {:else}
                      <div class="localization-pack-actions">
                        <button
                          type="button"
                          class="secondary-button localization-pack-button"
                          onclick={installOfflineLocalizationPack}
                          disabled={navigationLocked}
                        >Install Pack from ZIP</button>
                      </div>
                    {/if}
                  </section>
                {/if}
                <section class="panel-card display-card">
                  <div class="panel-heading compact"><div><h2>Display</h2></div></div>
                  <div class="field-grid">
                    <label><span>Display mode</span><select bind:value={settings.display.mode} disabled={navigationLocked}><option value="windowed">Windowed</option><option value="fullscreen">Borderless fullscreen</option></select></label>
                    <label><span>Resolution scale</span><select bind:value={settings.display.resolution_scale} disabled={navigationLocked}><option value={1}>1x - Native 720p</option><option value={2}>2x - 1440p</option><option value={3}>3x - 2160p / 4K</option></select></label>
                    <label><span>Aspect ratio</span><select bind:value={settings.display.aspect_ratio} disabled={navigationLocked}><option value="16:9">16:9 Native</option><option value="21:9">21:9 Ultrawide (Experimental)</option><option value="32:9">32:9 Super Ultrawide (Experimental)</option><option value="16:10">16:10 (Experimental)</option><option value="4:3">4:3 (Experimental)</option></select></label>
                    <label class="toggle-row"><span><strong>VSync</strong></span><input type="checkbox" bind:checked={settings.display.vsync} disabled={navigationLocked} /></label>
                  </div>
                </section>

                <section class="panel-card graphics-card">
                  <div class="panel-heading compact"><div><h2>Graphics</h2></div></div>
                  <div class="field-grid">
                    <label><span>Anti-aliasing</span><select bind:value={settings.graphics.anti_aliasing} disabled={navigationLocked}><option value="off">Off</option><option value="fxaa">FXAA</option><option value="fxaa_extreme">FXAA Extreme</option></select></label>
                    <label><span>Texture filtering</span><select bind:value={settings.graphics.texture_filtering} disabled={navigationLocked}><option value="default">Default</option><option value="1x">1x</option><option value="2x" disabled={filteringDisabled(2)}>2x</option><option value="4x" disabled={filteringDisabled(4)}>4x</option><option value="8x" disabled={filteringDisabled(8)}>8x</option><option value="16x" disabled={filteringDisabled(16)}>16x</option></select></label>
                    <label><span>Frame rate</span><select bind:value={settings.graphics.frame_rate} disabled={navigationLocked}><option value="30">30 FPS</option><option value="60">30+ FPS (Experimental)</option></select></label>
                  </div>
                </section>

                <section class="panel-card diagnostics-card full-width">
                  <div class="panel-heading compact"><div><h2>Diagnostics</h2></div></div>
                  <div class="diagnostic-options">
                    <label class="toggle-row"><span><strong>Enable logging</strong><small>Recommended for bug reports.</small></span><input type="checkbox" bind:checked={settings.advanced.logging_enabled} disabled={navigationLocked} /></label>
                  </div>
                  <div class="button-row"><button type="button" class="secondary-button" onclick={() => openFolder("logs")}>Open logs</button></div>
                </section>
              </div>
              <footer class="settings-footer"><button type="button" class="secondary-button" onclick={() => showView("game")} disabled={navigationLocked}>Back</button><button type="button" class="primary-button" onclick={() => saveSettings()} disabled={navigationLocked || !gameSettingsDirty}>{saving ? "Saving..." : "Save"}</button></footer>
            {:else}
              <div class="loading-state"><span></span><p>Loading game settings...</p></div>
            {/if}
        </section>
      {:else if activeView === "versions" && selected}
        <section class="page-view versions-view">
          <section class="versions-panel">
            <div class="versions-toolbar">
              <div><span class="overline">{selected.name}</span><h2>Versions</h2><p>Runtime and optional language components.</p></div>
              <div class="button-row">
                <button type="button" class="secondary-button" onclick={installOfflineRuntime} disabled={updateInteractionBusy}>Install Runtime from ZIP</button>
                <button type="button" class="secondary-button" onclick={() => checkUpdates()} disabled={updateInteractionBusy}>{checkingUpdates ? "Checking..." : "Check updates"}</button>
              </div>
            </div>

            <div class="version-component-stack">
              <section class="version-component-section runtime-version-section">
                <article class:update-available={componentNeedsAttention(selectedRuntimeComponent)} class:focused={versionFocus === selectedRuntimeComponent?.id} class:not-installed={!selectedRuntimeComponent?.installed_version} class="version-card latest-version-card">
                  <div class="version-card-heading"><span>{selected.name} Runtime</span><b>{updates?.error ? "Check failed" : componentStatusLabel(selectedRuntimeComponent)}</b></div>
                  <div class="latest-release-badge">{latestReleaseBadgeLabel()}</div>
                  <div class="component-release-overview">
                    <div class="component-release-copy">
                      <h3>{latestReleaseHeadline(selectedRuntimeComponent)}</h3>
                      {#if selectedRuntimeComponent?.latest_version && !updates?.error}
                        <div class="release-meta">
                          <span>{formatReleaseDate(selectedRuntimeComponent.published)}</span>
                          <span>{humanBytes(selectedRuntimeComponent.size)}</span>
                        </div>
                      {/if}
                      <p>{componentDescription(selectedRuntimeComponent)}</p>
                      <small>Active runtime: <strong>{selectedRuntimeComponent?.installed_version ?? "None"}</strong></small>
                    </div>
                    <div class="component-release-actions">
                      {#if latestRuntimeCanActivate(selectedRuntimeComponent)}
                        <button type="button" class="primary-button" onclick={() => selectedRuntimeComponent?.latest_version && activateRuntimeVersion(selectedRuntimeComponent.latest_version)} disabled={updateInteractionBusy}>Activate {selectedRuntimeComponent?.latest_version}</button>
                      {:else if componentCanInstall(selectedRuntimeComponent)}
                        <button type="button" class="primary-button" onclick={() => selectedRuntimeComponent && downloadLatestRuntime(selectedRuntimeComponent)} disabled={updateInteractionBusy}>{selectedRuntimeComponent ? latestRuntimeActionLabel(selectedRuntimeComponent) : "Download"}</button>
                      {:else if componentCanReinstall(selectedRuntimeComponent)}
                        <button type="button" class="secondary-button" onclick={() => selectedRuntimeComponent && downloadLatestRuntime(selectedRuntimeComponent)} disabled={updateInteractionBusy}>{selectedRuntimeComponent ? latestRuntimeActionLabel(selectedRuntimeComponent) : "Redownload"}</button>
                      {/if}
                      {#if selectedRuntimeComponent?.notes_url}<button type="button" class="secondary-button" onclick={() => openUpdateUrl(selectedRuntimeComponent?.notes_url ?? null)}>Release notes</button>{/if}
                      {#if selectedRuntimeComponent?.can_rollback}<button type="button" class="secondary-button" onclick={() => selectedRuntimeComponent && rollbackComponentUpdate(selectedRuntimeComponent)} disabled={updateInteractionBusy}>Rollback</button>{/if}
                      {#if selectedRuntimeComponent?.installed_version}<button type="button" class="danger-button" onclick={() => selectedRuntimeComponent?.installed_version && removeRuntimeVersion(selectedRuntimeComponent.installed_version)} disabled={updateInteractionBusy}>Remove Version</button>{/if}
                    </div>
                  </div>
                  {#if updateProgress?.component_id === selectedRuntimeComponent?.id}
                    <div class="component-progress"><div><strong>{updateProgress.detail}</strong><span>{updateProgress.progress}%</span></div><div class="progress-track"><span style:width={updateProgress.progress + "%"}></span></div></div>
                  {/if}
                </article>

                <div class="release-history-heading"><div><span>Runtime versions</span><strong>{selected.name}</strong></div></div>
                <div class="version-table" role="table" aria-label="Runtime versions">
                  <div class="version-row version-head" role="row"><span>Status</span><span>Version</span><span>Released</span><span>Package</span><span>Action</span></div>
                  {#each selectedRuntimeVersions as release, index}
                    <div class:active-version={release.version === selectedRuntimeComponent?.installed_version} class="version-row" role="row">
                      <span data-label="Status"><i class:installed={!!installedRuntimeVersion(release.version)}></i>{runtimeVersionStatusLabel(release, index)}</span>
                      <strong data-label="Version">{release.version}</strong>
                      <span data-label="Released">{release.local_only ? "Local" : formatReleaseDate(release.published)}</span>
                      <span data-label="Package">{release.local_only ? "Installed" : release.downloadable ? humanBytes(release.size) : release.version === selectedRuntimeComponent?.latest_version ? "Unavailable" : "Archived"}</span>
                      <div class="version-row-actions" data-label="Action">
                        {#if release.version === selectedRuntimeComponent?.installed_version}
                          {#if !release.local_only && release.downloadable}<button type="button" class="secondary-button" onclick={() => installRuntimeRelease(release)} disabled={updateInteractionBusy}>Redownload</button>{/if}
                          <button type="button" class="danger-button compact" onclick={() => removeRuntimeVersion(release.version)} disabled={updateInteractionBusy}>Remove</button>
                        {:else if installedRuntimeVersion(release.version)?.healthy}
                          <button type="button" class="primary-button" onclick={() => activateRuntimeVersion(release.version)} disabled={updateInteractionBusy}>Activate</button>
                          <button type="button" class="secondary-button" onclick={() => removeRuntimeVersion(release.version)} disabled={updateInteractionBusy}>Remove</button>
                        {:else if installedRuntimeVersion(release.version)}
                          {#if !release.local_only && release.downloadable}<button type="button" class="primary-button" onclick={() => installRuntimeRelease(release)} disabled={updateInteractionBusy}>Repair</button>{/if}
                          <button type="button" class="secondary-button" onclick={() => removeRuntimeVersion(release.version)} disabled={updateInteractionBusy}>Remove</button>
                        {:else if release.downloadable}
                          <button type="button" class="primary-button" onclick={() => installRuntimeRelease(release)} disabled={updateInteractionBusy}>{selectedRuntimeComponent?.installed_version ? "Download" : "Install"}</button>
                        {/if}
                        {#if release.notes_url}<button type="button" class="version-notes-button" onclick={() => openUpdateUrl(release.notes_url)}>Notes</button>{/if}
                      </div>
                    </div>
                  {:else}
                    <div class="version-row version-empty" role="row"><span>{releaseHistoryEmptyMessage()}</span></div>
                  {/each}
                </div>
              </section>

              {#if selectedLanguageComponents.length > 0}
                <section class="version-component-section optional-version-section">
                  <div class="release-history-heading"><div><span>Optional components</span><strong>Localization Pack</strong></div></div>
                  <div class="optional-component-grid">
                    {#each selectedLanguageComponents as component}
                      <article class:update-available={componentNeedsAttention(component)} class:focused={versionFocus === component.id} class:not-installed={!component.installed_version} class="version-card optional-version-card">
                        <div class="version-card-heading"><span>Language</span><b>{updates?.error ? "Check failed" : componentStatusLabel(component)}</b></div>
                        <h3>{component.display_name ?? component.locale ?? component.id}</h3>
                        {#if component.translation_version}<p>Translation v{component.translation_version}</p>{/if}
                        <div class="version-numbers"><div><span>Current</span><strong>{component.installed_version ?? "-"}</strong></div><div><span>Latest</span><strong>{component.latest_version ?? "-"}</strong></div></div>
                        <p>{componentDescription(component)}</p>
                        {#if updateProgress?.component_id === component.id}
                          <div class="component-progress"><div><strong>{updateProgress.detail}</strong><span>{updateProgress.progress}%</span></div><div class="progress-track"><span style:width={updateProgress.progress + "%"}></span></div></div>
                        {/if}
                        <div class="button-row">
                          {#if componentCanInstall(component)}<button type="button" class="primary-button" onclick={() => installComponentUpdate(component)} disabled={updateInteractionBusy}>{componentActionLabel(component)}</button>{/if}
                          {#if component.can_rollback}<button type="button" class="secondary-button" onclick={() => rollbackComponentUpdate(component)} disabled={updateInteractionBusy}>Rollback</button>{/if}
                          {#if component.notes_url}<button type="button" class="secondary-button" onclick={() => openUpdateUrl(component.notes_url)}>Release notes</button>{/if}
                        </div>
                      </article>
                    {/each}
                  </div>
                </section>
              {/if}
            </div>
          </section>
        </section>
      {:else if activeView === "launcher"}
        <section class="page-view versions-view launcher-view">
          <section class="versions-panel launcher-panel">
            <div class="versions-toolbar">
              <div><span class="overline">MojoRecomp</span><h2>Launcher</h2><p>Launcher version, updates, and project information.</p></div>
              <div class="button-row">
                <button type="button" class="secondary-button" onclick={checkLauncherUpdateNow} disabled={checkingUpdates || navigationLocked}>{checkingUpdates ? "Checking..." : "Check for Updates"}</button>
              </div>
            </div>

            <div class="version-component-stack">
              <section class="version-component-section launcher-version-section">
                <article class:update-available={componentNeedsAttention(launcherComponent)} class="version-card latest-version-card">
                  <div class="version-card-heading"><span>MojoRecomp Launcher</span><b>{launcherUpdateError ? "Check failed" : componentStatusLabel(launcherComponent)}</b></div>
                  <div class="latest-release-badge">CURRENT LAUNCHER</div>
                  <div class="component-release-overview">
                    <div class="component-release-copy">
                      <h3>v{currentLauncherVersion}</h3>
                      {#if launcherComponent?.state === "update_available" && launcherComponent.latest_version && !launcherUpdateError}
                        <div class="release-meta"><span>Latest: {launcherComponent.latest_version}</span></div>
                      {/if}
                      <p>{launcherDescription(launcherComponent)}</p>
                    </div>
                    <div class="component-release-actions">
                      {#if launcherComponent?.notes_url}<button type="button" class="secondary-button" onclick={() => openUpdateUrl(launcherComponent?.notes_url ?? null)}>Release notes</button>{/if}
                    </div>
                  </div>
                  {#if launcherUpdateProgress}
                    <div class="component-progress"><div><strong>{launcherUpdateProgress.detail}</strong><span>{launcherUpdateProgress.progress}%</span></div><div class="progress-track"><span style:width={launcherUpdateProgress.progress + "%"}></span></div></div>
                  {/if}
                </article>
              </section>

              <section class="panel-card launcher-information-card">
                <div class="panel-heading compact"><div><h2>Information</h2></div></div>
                <div class="status-list">
                  <div><span>Launcher</span><strong>v{currentLauncherVersion}</strong></div>
                  <div><span>Crash of the Titans Runtime</span><strong>{cotGame?.runtime_version ?? "Unavailable"}</strong></div>
                  <div><span>Crash: Mind Over Mutant Runtime</span><strong>{momGame?.runtime_version ?? "0.0.0-dev"}</strong></div>
                  <div>
                    <span>Official Repository</span>
                    <button type="button" class="status-link" onclick={() => openUpdateUrl("https://github.com/OAleex/MojoRecomp")}>github.com/OAleex/MojoRecomp</button>
                  </div>
                </div>
              </section>
            </div>
          </section>
        </section>
      {:else if activeView === "launcher-settings"}
        <section class:first-launch={firstLaunchPending} class:first-launch-surface={firstLaunchPending} class="page-view launcher-settings-view">
          {#if firstLaunchPending}
            {@render setupMojo()}
          {/if}
          <header class="page-header compact-header">
            <div>
              <span class="overline">{firstLaunchPending ? "Welcome to MojoRecomp" : "Launcher"}</span>
              <h1>{firstLaunchPending ? "Choose your game library" : "Launcher Settings"}</h1>
              <p>{firstLaunchPending ? "Game files extracted from your own discs will be stored here." : "Choose where game files are stored."}</p>
            </div>
          </header>
          <div class="launcher-settings-layout">
            <section class="panel-card library-card">
              <div class="panel-heading compact"><div><h2>Game Library</h2></div></div>
              {#if launcherStorage?.existing_library_detected && firstLaunchPending}
                <div class="library-existing"><strong>Existing installation found</strong><span>You can keep this location or choose a new one. Choosing another drive will safely move and verify the installed games.</span></div>
              {/if}
              <label class="library-path-field">
                <span>Library location</span>
                <div class="path-picker">
                  <input type="text" bind:value={libraryPath} oninput={scheduleLibraryPathInspection} spellcheck="false" disabled={navigationLocked} />
                  <button type="button" class="secondary-button" onclick={chooseLibraryFolder} disabled={navigationLocked}>Browse</button>
                </div>
              </label>
              <div class:insufficient={libraryProbeMatchesPath && !!libraryPathStatus && !libraryPathStatus.enough_space} class="library-facts">
                <span><strong>{libraryProbeMatchesPath && libraryPathStatus ? formatBytes(libraryPathStatus.available_bytes) : "Checking..."}</strong> free at the selected location</span>
                <span>Minimum setup reserve <strong>{libraryPathStatus ? formatBytes(libraryPathStatus.required_free_bytes) : "-"}</strong></span>
              </div>
              {#if libraryProbeMatchesPath && libraryPathStatus?.error}
                <p class="library-space-warning">{libraryPathStatus.error}</p>
              {:else if libraryProbeMatchesPath && libraryPathStatus && !libraryPathStatus.enough_space}
                <p class="library-space-warning">Not enough free space. Choose a location with at least {formatBytes(libraryPathStatus.required_free_bytes)} available.</p>
              {:else}
                <p class="library-storage-note">Saves remain separate in the Windows Saved Games folder.</p>
              {/if}
              {#if libraryProgress}
                <div class:failed={libraryProgress.stage === "failed"} class="library-progress" aria-live="polite">
                  <div><strong>{libraryProgress.detail}</strong><span>{libraryProgress.progress}%</span></div>
                  <div class="progress-track"><span style={`width: ${libraryProgress.progress}%`}></span></div>
                  {#if libraryProgress.bytes_total > 0}<small>{formatBytes(libraryProgress.bytes_done)} / {formatBytes(libraryProgress.bytes_total)}</small>{/if}
                </div>
              {/if}
              <div class="button-row library-actions">
                {#if !firstLaunchPending}<button type="button" class="secondary-button library-back" onclick={() => showView("game")} disabled={navigationLocked}>Back</button>{/if}
                {#if !firstLaunchPending}<button type="button" class="secondary-button" onclick={openGameLibrary} disabled={navigationLocked}>Open Library</button>{/if}
                <button type="button" class="primary-button" onclick={applyLibraryLocation} disabled={navigationLocked || !libraryCanApply}>{libraryBusy ? (libraryProgress?.stage === "moving" ? "Moving..." : libraryProgress?.stage === "planning" ? "Checking..." : firstLaunchPending ? "Setting up..." : "Applying...") : firstLaunchPending ? "Continue" : "Apply"}</button>
              </div>
            </section>
            {#if !firstLaunchPending}
              <section class="panel-card discord-activity-card">
                <div class="panel-heading compact"><div><h2>Discord</h2></div></div>
                <label class="toggle-row">
                  <span>
                    <strong>Enable Discord activity</strong>
                    <small>Show MojoRecomp in Discord.</small>
                  </span>
                  <input type="checkbox" bind:checked={discordActivityEnabled} onchange={updateDiscordActivity} disabled={navigationLocked} />
                </label>
              </section>
            {/if}
          </div>
        </section>
      {:else if activeView === "help"}
        <section class="page-view support-view">
          <header class="page-header compact-header"><div><h1>Help</h1><p>Create a support package before reporting a problem.</p></div></header>
          <div class="support-layout support-simple">
            <section class="panel-card full-width help-primary">
              <div class="panel-heading compact"><div><h2>Asking for help</h2></div></div>
              <p>The support package contains the logs and technical information needed to diagnose most problems.</p>
              <label class="toggle-row support-minidump-toggle">
                <span>
                  <strong>Include memory diagnostic (.dmp)</strong>
                  <small>The minidump can identify the function or driver that crashed. It may contain small fragments of memory used by the runtime and is never uploaded automatically.</small>
                </span>
                <input type="checkbox" bind:checked={includeSupportMinidump} disabled={busy || !selected} />
              </label>
              <div class="button-row help-actions">
                <button type="button" class="primary-button" onclick={createSupportArchive} disabled={busy || !selected}>{busy ? "Creating..." : "Create Support Package"}</button>
                <button type="button" class="secondary-button" onclick={() => openFolder("all_logs")} disabled={!selected}>Open Log Folder</button>
                <button type="button" class="secondary-button" onclick={() => openFolder("save")} disabled={!selected}>Open Save Folder</button>
                <button type="button" class="secondary-button" onclick={openLicenseNotices}>Licenses &amp; Notices</button>
              </div>
            </section>

            <section class="panel-card">
              <div class="panel-heading compact"><div><h2>Report an issue</h2></div></div>
              <p class="support-copy">Report problems on the official GitHub repository. Attach a Support Package when it is relevant.</p>
              <div class="button-row report-actions">
                <button type="button" class="secondary-button" onclick={() => openUpdateUrl("https://github.com/OAleex/MojoRecomp/issues/new?title=%5BLauncher%5D%20")}>Report Launcher Issue</button>
                <button type="button" class="secondary-button" onclick={() => openUpdateUrl("https://github.com/OAleex/MojoRecomp/issues/new?title=%5BGame%5D%20")}>Report Game Issue</button>
              </div>
            </section>

            <p class="legal-note">
              MojoRecomp is an unofficial fan project. Official builds are provided free of charge. Crash Bandicoot, Crash of the Titans,
              Crash: Mind Over Mutant, their names, logos, characters, and related trademarks belong to their
              respective owners. MojoRecomp is not affiliated with or endorsed by those owners.
            </p>
          </div>
        </section>
      {/if}

      {#if pendingRuntimeAction && runtimeAdditionalContent}
        <div class="language-setup-backdrop" role="presentation">
          <div class="language-setup-modal additional-content-modal" role="dialog" aria-modal="true" aria-labelledby="additional-content-title">
            <img class="language-setup-mark" src={mojoRecompIcon} alt="" />
            <span class="overline">{selected?.name ?? "Game"} Runtime {runtimeAdditionalContent.runtime_version}</span>
            <h2 id="additional-content-title">Additional content</h2>
            <p>This runtime release also provides optional Localization Pack content. Choose what you want to install with this runtime.</p>
            <div class="additional-pack-summary">
              <div><span>Localization Pack</span><strong>{runtimeAdditionalContent.pack_version}</strong></div>
              <span>{humanBytes(runtimeAdditionalContent.pack_size)}</span>
            </div>
            <div class="additional-language-list">
              {#each runtimeAdditionalContent.languages as language}
                <label class:installed={language.installed_version === language.version} class="additional-language-option">
                  <input
                    type="checkbox"
                    checked={language.installed_version === language.version || runtimeAdditionalSelection.includes(language.id)}
                    disabled={runtimeAdditionalBusy || language.installed_version === language.version}
                    onchange={(event) => toggleRuntimeAdditionalLanguage(language.id, (event.currentTarget as HTMLInputElement).checked)}
                  />
                  <span class={`language-flag ${languageFlag(language.locale)}`} aria-hidden="true"></span>
                  <span class="additional-language-copy">
                    <strong>{language.display_name}</strong>
                    <small>{language.locale}{language.translation_version ? ` · Translation v${language.translation_version}` : ` · Component v${language.version}`}{language.installed_version ? ` · Component ${language.version} installed` : ""}</small>
                  </span>
                </label>
              {/each}
            </div>
            <div class="button-row additional-content-actions">
              <button type="button" class="secondary-button" onclick={closeRuntimeAdditionalContent} disabled={runtimeAdditionalBusy}>Cancel</button>
              <button type="button" class="secondary-button" onclick={skipRuntimeAdditionalContent} disabled={runtimeAdditionalBusy}>Runtime only</button>
              <button type="button" class="primary-button" onclick={confirmRuntimeAdditionalContent} disabled={runtimeAdditionalBusy}>
                {runtimeAdditionalBusy ? "Installing..." : "Continue"}
              </button>
            </div>
          </div>
        </div>
      {/if}

      {#if languageSetupOpen && initialLanguageSetupRequired}
        <div class="language-setup-backdrop" role="presentation">
          <div class="language-setup-modal" role="dialog" aria-modal="true" aria-labelledby="language-setup-title">
            <img class="language-setup-mark" src={mojoRecompIcon} alt="" />
            <span class="overline">Crash of the Titans</span>
            <h2 id="language-setup-title">Choose your game language</h2>
            <p>You can change this later in Game Settings.</p>
            <label class="language-setup-field">
              <span>Language</span>
              <select bind:value={languageSetupSelection} disabled={languageSetupSaving}>
                {#each cotLanguageOptions as language}
                  <option value={language.profile}>{language.label}</option>
                {/each}
              </select>
            </label>
            <button type="button" class="primary-button language-setup-confirm" onclick={completeInitialLanguageSetup} disabled={languageSetupSaving || !languageSetupSelection}>
              {languageSetupSaving ? "Saving..." : "Continue"}
            </button>
          </div>
        </div>
      {/if}

      {#if localizationBusy && localizationProgress}
        <section class="setup-experience setup-surface localization-experience" aria-live="polite">
          {@render setupMojo()}
          <img class="setup-emblem" src={mojoRecompIcon} alt="" />
          <img class="setup-game-logo" src={selectedLogo} alt="" />
          <span class="overline">MojoRecomp Localization Pack - {selectedLanguage.label}</span>
          <h2>Installing Localization Pack</h2>
          <p>{localizationProgress.detail}</p>
          <div class="setup-progress-large">
            <div class="progress-track"><span style:width={localizationProgress.progress + "%"}></span></div>
            <b>{localizationProgress.progress}%</b>
          </div>
          <div class="setup-metrics">
            <span>{selectedLanguage.label}</span>
            {#if localizationProgress.bytes_total > 0}<span>{formatBytes(localizationProgress.bytes_done)} / {formatBytes(localizationProgress.bytes_total)}</span>{/if}
          </div>
          <button type="button" class="secondary-button localization-cancel" onclick={cancelLocalization}>Cancel</button>
        </section>
      {/if}

      {#if message || warning || error}
        {#key toastSerial}
          <div class:error={!!error} class:warning={!!warning} class="app-toast" role="status" style={`--toast-duration: ${toastDuration}ms`}>
            <span>{error ? "!" : warning ? "i" : "OK"}</span><p>{error || warning || message}</p><i class="toast-progress" aria-hidden="true"></i>
          </div>
        {/key}
      {/if}
    </section>
  </div>
</main>
