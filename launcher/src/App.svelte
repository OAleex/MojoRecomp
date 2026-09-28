<script lang="ts">
  import { onMount } from "svelte";
  import { invoke } from "@tauri-apps/api/core";
  import { listen } from "@tauri-apps/api/event";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import { open } from "@tauri-apps/plugin-dialog";
  import mojoRecompIcon from "../src-tauri/icons/icon.ico?url";
  import type {
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
    LocalizationProfile,
    LocalizationStatus,
    ProcessStatus,
    Settings,
    UpdateOverview
  } from "./lib/types";

  type View = "game" | "versions" | "settings" | "launcher-settings" | "support";
  type NoticeKind = "success" | "warning" | "error";
  const NOTICE_DURATION_MS: Record<NoticeKind, number> = {
    success: 4200,
    warning: 8000,
    error: 6500
  };
  const LANGUAGE_OPTIONS: Array<{
    profile: LocalizationProfile;
    xboxLanguage: number;
    label: string;
    flag: string;
  }> = [
    { profile: "en", xboxLanguage: 1, label: "English", flag: "flag-en" },
    { profile: "de", xboxLanguage: 3, label: "Deutsch", flag: "flag-de" },
    { profile: "fr", xboxLanguage: 4, label: "Français", flag: "flag-fr" },
    { profile: "es", xboxLanguage: 5, label: "Español", flag: "flag-es" },
    { profile: "it", xboxLanguage: 6, label: "Italiano", flag: "flag-it" },
    { profile: "nl", xboxLanguage: 16, label: "Nederlands", flag: "flag-nl" },
    { profile: "pt-BR", xboxLanguage: 1, label: "Português Brasileiro", flag: "flag-br" }
  ];

  let games: GameInfo[] = [];
  let selectedId = "cot";
  let activeView: View = "game";
  let settings: Settings | null = null;
  let savedSettingsFingerprint = "";
  let launcherStorage: LauncherStorageStatus | null = null;
  let libraryPath = "";
  let libraryProgress: LibraryMigrationProgress | null = null;
  let libraryPathStatus: LibraryPathStatus | null = null;
  let libraryBusy = false;
  let libraryProbeTimer: ReturnType<typeof setTimeout> | null = null;
  let libraryProbeSerial = 0;
  let hardware: HardwareInfo | null = null;
  let process: ProcessStatus = { running: false, pid: null, exit_code: null };
  let updates: UpdateOverview | null = null;
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
  let pollTimer: ReturnType<typeof setInterval> | null = null;
  let toastTimer: ReturnType<typeof setTimeout> | null = null;

  $: selected = games.find((game) => game.id === selectedId) ?? null;
  $: cotGame = games.find((game) => game.id === "cot") ?? null;
  $: momGame = games.find((game) => game.id === "mom") ?? null;
  $: selectedLogo = selectedId === "mom" ? "/art/mom-logo.png" : "/art/cot-logo.png";
  $: selectedIcon = selectedId === "mom" ? "/art/mom-icon.png" : "/art/cot-icon.png";
  $: selectedLanguage = LANGUAGE_OPTIONS.find((entry) => entry.profile === settings?.localization.profile) ?? LANGUAGE_OPTIONS[0];
  $: navigationLocked = setupBusy || localizationBusy || libraryBusy || updateBusy;
  $: firstLaunchPending = launcherStorage !== null && !launcherStorage.configured;
  $: gameSettingsDirty = !!settings && settingsFingerprint(settings) !== savedSettingsFingerprint;
  $: libraryDirty = firstLaunchPending || normalizedPath(libraryPath) !== normalizedPath(launcherStorage?.library_path ?? "");
  $: libraryProbeMatchesPath = !!libraryPathStatus && normalizedPath(libraryPathStatus.path) === normalizedPath(libraryPath);
  $: libraryCanApply = !!libraryPath.trim()
    && libraryDirty
    && libraryProbeMatchesPath
    && !!libraryPathStatus?.valid
    && !!libraryPathStatus?.enough_space;
  $: launcherComponent = updates?.components.find((component) => component.id === "launcher") ?? null;
  $: selectedRuntimeComponent = updates?.components.find(
    (component) => component.id === "runtime." + selectedId
  ) ?? null;
  $: selectedLanguageComponents = updates?.components.filter(
    (component) => component.kind === "language" && component.game_id === selectedId
  ) ?? [];
  $: actionableUpdates = updates?.components.filter(
    (component) => component.state === "update_available" || component.state === "corrupted"
  ) ?? [];
  $: updateCount = actionableUpdates.length;

  async function openGame(id: string) {
    if (navigationLocked || firstLaunchPending) return;
    activeView = "game";
    await selectGame(id);
  }

  function openSettings() {
    if (navigationLocked || firstLaunchPending) return;
    activeView = "settings";
  }

  async function openVersions() {
    if (navigationLocked || firstLaunchPending) return;
    versionFocus = null;
    activeView = "versions";
    if (!updates) await checkUpdates();
  }

  async function openAvailableUpdates() {
    if (navigationLocked || firstLaunchPending || updateCount === 0) return;
    const target = actionableUpdates[0] ?? null;
    if (!target) return;
    activeView = "versions";
    if (target.game_id && target.game_id !== selectedId) {
      await selectGame(target.game_id);
    }
    versionFocus = target.id;
  }

  function openLauncherSettings() {
    if (navigationLocked) return;
    if (launcherStorage) libraryPath = launcherStorage.library_path;
    activeView = "launcher-settings";
  }

  function showView(view: View) {
    if (firstLaunchPending && view !== "launcher-settings") return;
    if (navigationLocked && view !== activeView) return;
    activeView = view;
  }

  function toggleHelp() {
    if (navigationLocked || firstLaunchPending) return;
    activeView = activeView === "support" ? "game" : "support";
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
        ? run(() => invoke<LocalizationStatus>("localization_status", { gameId: id }))
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

  async function importIso() {
    if (!selected || selected.id !== "cot") return;
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
    if (!selected || !settings || process.running) return;
    if (settings.localization.profile === "pt-BR" && !localizationStatus?.overlay_ready) {
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
      gameId: selected.id
    }));
    if (value) localizationStatus = value;
  }

  function selectLocalizationProfile(profile: LocalizationProfile) {
    if (!settings) return;
    const option = LANGUAGE_OPTIONS.find((entry) => entry.profile === profile);
    if (!option) return;
    settings.localization.profile = profile;
    settings.localization.xbox_language = option.xboxLanguage;
    if (profile === "pt-BR") void refreshLocalizationStatus();
  }

  function changeLocalizationProfile(event: Event) {
    selectLocalizationProfile((event.currentTarget as HTMLSelectElement).value as LocalizationProfile);
  }

  async function installLanguagePack() {
    if (!selected || !settings || localizationBusy) return;
    localizationBusy = true;
    localizationProgress = {
      game_id: selected.id,
      profile: "pt-BR",
      stage: "preparing",
      progress: 0,
      detail: "Preparing Localization Pack...",
      bytes_done: 0,
      bytes_total: 0
    };
    const value = await run(() => invoke<LocalizationStatus>("prepare_localization", {
      gameId: selected.id,
      profile: "pt-BR"
    }));
    localizationBusy = false;
    if (value) {
      localizationStatus = value;
      localizationProgress = null;
      showNotice("Localization Pack installed.");
    }
  }

  async function cancelPtBr() {
    if (!selected || !localizationBusy) return;
    await runAction(() => invoke<void>("cancel_localization", { gameId: selected.id }));
  }

  async function uninstallGame() {
    if (!selected || !selected.installed || process.running || uninstalling) return;
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
    if (!requestedPath || libraryBusy || !libraryDirty) return;
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
    libraryBusy = true;
    libraryProgress = {
      stage: "planning",
      progress: 0,
      detail: "Checking the game library and available disk space...",
      bytes_done: 0,
      bytes_total: 0
    };
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
    await runAction(() => invoke<void>("open_game_library"));
  }

  async function createSupportArchive() {
    if (!selected) return;
    busy = true;
    const path = await run(() => invoke<string>("create_support_package", {
      gameId: selected.id
    }));
    busy = false;
    if (path) showNotice("Support package created.");
  }

  async function openLicenseNotices() {
    await runAction(() => invoke<void>("open_license_notices"));
  }

  async function checkUpdates() {
    checkingUpdates = true;
    const value = await run(() => invoke<UpdateOverview>("check_component_updates"));
    checkingUpdates = false;
    if (value) updates = value;
  }

  async function installComponentUpdate(component: ComponentUpdateStatus) {
    if (updateBusy) return;
    updateBusy = true;
    versionFocus = component.id;
    const result = await run(() => invoke<ComponentUpdateResult>("install_component_update", {
      componentId: component.id
    }));
    updateBusy = false;
    if (!result) return;
    await refreshGames();
    await checkUpdates();
    if (component.kind === "language" && component.game_id === selectedId) {
      await refreshLocalizationStatus();
    }
    if (result.restart_required) {
      showNotice("Verified launcher package is ready. Close the launcher before replacing it.", "warning");
    } else if (component.state === "corrupted") {
      showNotice("Component repaired successfully.");
    } else {
      showNotice("Component updated successfully.");
    }
  }

  async function rollbackComponentUpdate(component: ComponentUpdateStatus) {
    if (updateBusy) return;
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

  async function openLauncherUpdateFolder() {
    await runAction(() => invoke<void>("open_launcher_update_folder"));
  }

  function componentStatusLabel(component: ComponentUpdateStatus | null) {
    if (!component) return "Unavailable";
    if (component.last_action === "ready_manual") return "Ready to replace";
    if (component.state === "repair_unavailable") return "Repair unavailable";
    if (component.state === "corrupted") return "Repair required";
    if (component.last_action === "failed") return "Update failed";
    if (component.last_action === "rolled_back") return "Rolled back";
    if (component.state === "update_available") return "Update available";
    if (component.state === "available") return "Available";
    if (component.state === "incompatible") return "Incompatible";
    if (component.state === "not_installed") return "Not installed";
    if (component.last_action === "recovered") return "Recovered";
    if (component.last_action === "installed") return "Installed";
    return updates?.configured ? "Up to date" : "Installed";
  }

  function componentDescription(component: ComponentUpdateStatus | null) {
    if (!component) return "Component status is unavailable.";
    if (updates?.error) return "Couldn't check for updates.";
    if (!updates?.configured) return "Installed locally.";
    if (component.last_action === "ready_manual") {
      return "The verified portable launcher package is ready. Close this launcher before replacing the executable.";
    }
    if (component.state === "repair_unavailable") {
      return "This component is damaged, and no compatible repair is available.";
    }
    if (component.state === "corrupted") {
      return "This component failed its integrity check. A verified repair is available.";
    }
    if (component.last_action === "failed") {
      return "The previous update attempt failed. You can safely try again.";
    }
    if (component.last_action === "rolled_back") return "The previous component version is active again.";
    if (component.state === "update_available") return "A newer compatible component release is available.";
    if (component.state === "available") return "This optional component can be installed.";
    if (component.state === "incompatible") return "No compatible update is available.";
    if (component.state === "not_installed") return "This component is not installed.";
    if (component.last_action === "recovered") return "An interrupted update was recovered and verified.";
    if (component.last_action === "installed") return "The verified component update is installed.";
    return "This component is up to date.";
  }

  function componentCanInstall(component: ComponentUpdateStatus | null) {
    return !!component
      && !!updates?.configured
      && !updates.error
      && (component.state === "update_available"
        || component.state === "available"
        || component.state === "corrupted");
  }

  function componentActionLabel(component: ComponentUpdateStatus) {
    if (component.state === "corrupted") return "Repair";
    if (component.kind === "language" && component.state === "available") return "Install";
    if (component.kind === "launcher") return "Download verified update";
    return "Update";
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
        || event.payload.stage === "ready_manual"
      ) {
        updateBusy = false;
      }
    });
    void (async () => {
      const loadedStorage = await run(() => invoke<LauncherStorageStatus>("get_launcher_storage"));
      if (loadedStorage) {
        launcherStorage = loadedStorage;
        libraryPath = loadedStorage.library_path || loadedStorage.default_library_path;
        await inspectLibraryPath(libraryPath);
        if (!loadedStorage.configured) activeView = "launcher-settings";
      }
      await refreshGames();
      await selectGame(selectedId);
      await checkUpdates();
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
  class:utility-view={activeView === "launcher-settings" || activeView === "support"}
  class:game-uninstalled={(activeView === "game" || activeView === "versions") && !!selected && !selected.installed}
  class="app-shell"
>
  <header class="titlebar" data-tauri-drag-region>
    <div class="suite-brand" data-tauri-drag-region>
      <div class="suite-mark" aria-hidden="true"><img src={mojoRecompIcon} alt="" /></div>
      <div class="suite-name" data-tauri-drag-region><strong>MojoRecomp</strong><span>Launcher</span></div>
      <span class="launcher-version" data-tauri-drag-region>v{launcherComponent?.installed_version ?? "1.0.0"}</span>
    </div>
    {#if updateCount > 0}
      <button type="button" class="title-update" onclick={openAvailableUpdates} disabled={navigationLocked || firstLaunchPending}>
        <span></span>{updateCount === 1 ? "Update available" : `${updateCount} updates`}
      </button>
    {/if}
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
            aria-label={game.name}
            title={firstLaunchPending ? "Choose a game library before continuing" : navigationLocked ? "Finish the current operation before switching games" : game.name}
            onclick={() => openGame(game.id)}
            disabled={navigationLocked || firstLaunchPending}
          >
            <img src={game.id === "mom" ? "/art/mom-icon.png" : "/art/cot-icon.png"} alt="" />
            <span class:ready={game.installed && game.playable} class="rail-status"></span>
          </button>
        {/each}
      </div>
      <div class="rail-spacer"></div>
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
        class:active={activeView === "support"}
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
        {#key selectedId}<img class="character-ghost game-switch-ghost" src={selectedIcon} alt="" />{/key}<div class="grain"></div>
      </div>

      {#if selected && (activeView === "game" || activeView === "versions")}
        <nav class="game-section-tabs" aria-label={`${selected.name} pages`}>
          <button type="button" class:active={activeView === "game"} onclick={() => showView("game")}>Overview</button>
          <button type="button" class:active={activeView === "versions"} onclick={openVersions}>Versions</button>
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
              <button type="button" class="play-button" onclick={play} disabled={!settings || busy || setupBusy || localizationBusy || process.running || (settings?.localization.profile === "pt-BR" && !localizationStatus?.overlay_ready)}>
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 7 8 5-8 5Z" /></svg><span>{process.running ? "Running" : busy ? "Starting..." : "Play"}</span>
              </button>
              <div class="deck-actions installed-actions">
                <button type="button" onclick={openSettings} disabled={!settings || process.running || uninstalling}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 8.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Zm7.5 3.5-1.7-.65a6.1 6.1 0 0 0-.51-1.24l.73-1.67-2.46-2.46-1.67.73a6.1 6.1 0 0 0-1.24-.51L12 4.5H8.5l-.65 1.7c-.43.14-.85.31-1.24.51l-1.67-.73-2.46 2.46.73 1.67c-.2.39-.37.81-.51 1.24L1 12l1.7.65c.14.43.31.85.51 1.24l-.73 1.67 2.46 2.46 1.67-.73c.39.2.81.37 1.24.51l.65 1.7H12l.65-1.7c.43-.14.85-.31 1.24-.51l1.67.73 2.46-2.46-.73-1.67c.2-.39.37-.81.51-1.24Z" /></svg>Game Settings</button>
                <button type="button" onclick={() => openFolder("save")} disabled={uninstalling}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3.5 6.5h6l2 2h9v10h-17Z" /></svg>Save Folder</button>
                <button type="button" class="danger-action" onclick={uninstallGame} disabled={process.running || uninstalling}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 7h14M9 7V4h6v3m-8 0 1 13h8l1-13M10 10v7m4-7v7" /></svg>{uninstalling ? "Uninstalling..." : "Uninstall"}</button>
              </div>
            {:else if selected.playable}
              <button type="button" class="setup-button" onclick={importIso} disabled={busy || setupBusy || localizationBusy}>
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
                        >
                          {#each LANGUAGE_OPTIONS as language}
                            <option value={language.profile}>{language.label}{language.profile === "pt-BR" && !localizationStatus?.overlay_ready ? " - Not installed" : ""}</option>
                          {/each}
                        </select>
                      </div>
                    </div>
                    {#if settings.localization.profile === "pt-BR" && !localizationStatus?.overlay_ready}
                      <div class="localization-pack-actions">
                        <button
                          type="button"
                          class="secondary-button localization-pack-button"
                          onclick={installLanguagePack}
                          disabled={localizationBusy}
                        >Install Localization Pack</button>
                      </div>
                    {/if}
                  </section>
                {/if}
                <section class="panel-card display-card">
                  <div class="panel-heading compact"><div><h2>Display</h2></div></div>
                  <div class="field-grid">
                    <label><span>Display mode</span><select bind:value={settings.display.mode}><option value="windowed">Windowed</option><option value="fullscreen">Borderless fullscreen</option></select></label>
                    <label><span>Resolution scale</span><select bind:value={settings.display.resolution_scale}><option value={1}>1x - Native 720p</option><option value={2}>2x - 1440p</option><option value={3}>3x - 2160p / 4K</option></select></label>
                    <label><span>Aspect ratio</span><select bind:value={settings.display.aspect_ratio}><option value="16:9">16:9 Native</option><option value="21:9">21:9 Ultrawide (Experimental)</option><option value="32:9">32:9 Super Ultrawide (Experimental)</option><option value="16:10">16:10 (Experimental)</option><option value="4:3">4:3 (Experimental)</option></select></label>
                    <label class="toggle-row"><span><strong>VSync</strong></span><input type="checkbox" bind:checked={settings.display.vsync} /></label>
                  </div>
                </section>

                <section class="panel-card graphics-card">
                  <div class="panel-heading compact"><div><h2>Graphics</h2></div></div>
                  <div class="field-grid">
                    <label><span>Anti-aliasing</span><select bind:value={settings.graphics.anti_aliasing}><option value="off">Off</option><option value="fxaa">FXAA</option><option value="fxaa_extreme">FXAA Extreme</option></select></label>
                    <label><span>Texture filtering</span><select bind:value={settings.graphics.texture_filtering}><option value="default">Default</option><option value="1x">1x</option><option value="2x" disabled={filteringDisabled(2)}>2x</option><option value="4x" disabled={filteringDisabled(4)}>4x</option><option value="8x" disabled={filteringDisabled(8)}>8x</option><option value="16x" disabled={filteringDisabled(16)}>16x</option></select></label>
                  </div>
                </section>

                <section class="panel-card diagnostics-card full-width">
                  <div class="panel-heading compact"><div><h2>Diagnostics</h2></div></div>
                  <div class="diagnostic-options">
                    <label class="toggle-row"><span><strong>Enable logging</strong><small>Recommended for bug reports.</small></span><input type="checkbox" bind:checked={settings.advanced.logging_enabled} /></label>
                  </div>
                  <div class="button-row"><button type="button" class="secondary-button" onclick={() => openFolder("logs")}>Open logs</button></div>
                </section>
              </div>
              <footer class="settings-footer"><button type="button" class="secondary-button" onclick={() => showView("game")}>Back</button><button type="button" class="primary-button" onclick={() => saveSettings()} disabled={saving || !gameSettingsDirty}>{saving ? "Saving..." : "Save"}</button></footer>
            {:else}
              <div class="loading-state"><span></span><p>Loading game settings...</p></div>
            {/if}
        </section>
      {:else if activeView === "versions" && selected}
        <section class="page-view versions-view">
          <section class="versions-panel">
            <div class="versions-toolbar">
              <div><span class="overline">{selected.name}</span><h2>Versions</h2><p>Launcher, runtime, and optional language components.</p></div>
              <button type="button" class="secondary-button" onclick={checkUpdates} disabled={checkingUpdates || updateBusy}>{checkingUpdates ? "Checking..." : "Check updates"}</button>
            </div>

            <div class="version-summary-grid">
              <article class:update-available={launcherComponent?.state === "update_available" || launcherComponent?.state === "corrupted"} class:focused={versionFocus === "launcher"} class="version-card">
                <div class="version-card-heading"><span>Launcher</span><b>{updates?.error ? "Check failed" : componentStatusLabel(launcherComponent)}</b></div>
                <h3>MojoRecomp Launcher</h3>
                <div class="version-numbers"><div><span>Current</span><strong>{launcherComponent?.installed_version ?? "1.0.0"}</strong></div><div><span>Latest</span><strong>{launcherComponent?.latest_version ?? "-"}</strong></div></div>
                <p>{componentDescription(launcherComponent)}</p>
                {#if updateProgress?.component_id === "launcher"}
                  <div class="component-progress"><div><strong>{updateProgress.detail}</strong><span>{updateProgress.progress}%</span></div><div class="progress-track"><span style:width={updateProgress.progress + "%"}></span></div></div>
                {/if}
                <div class="button-row">
                  {#if launcherComponent?.last_action === "ready_manual"}
                    <button type="button" class="primary-button" onclick={openLauncherUpdateFolder}>Open verified package</button>
                  {:else if componentCanInstall(launcherComponent)}
                    <button type="button" class="primary-button" onclick={() => launcherComponent && installComponentUpdate(launcherComponent)} disabled={updateBusy}>{launcherComponent ? componentActionLabel(launcherComponent) : "Update"}</button>
                  {/if}
                  {#if launcherComponent?.notes_url}<button type="button" class="secondary-button" onclick={() => openUpdateUrl(launcherComponent?.notes_url ?? null)}>Release notes</button>{/if}
                </div>
              </article>

              <article class:update-available={selectedRuntimeComponent?.state === "update_available" || selectedRuntimeComponent?.state === "corrupted"} class:focused={versionFocus === selectedRuntimeComponent?.id} class:not-installed={!selectedRuntimeComponent?.installed_version} class="version-card">
                <div class="version-card-heading"><span>Game runtime</span><b>{updates?.error ? "Check failed" : componentStatusLabel(selectedRuntimeComponent)}</b></div>
                <h3>{selected.name}</h3>
                <div class="version-numbers"><div><span>Current</span><strong>{selectedRuntimeComponent?.installed_version ?? "-"}</strong></div><div><span>Latest</span><strong>{selectedRuntimeComponent?.latest_version ?? "-"}</strong></div></div>
                <p>{componentDescription(selectedRuntimeComponent)}</p>
                {#if updateProgress?.component_id === selectedRuntimeComponent?.id}
                  <div class="component-progress"><div><strong>{updateProgress.detail}</strong><span>{updateProgress.progress}%</span></div><div class="progress-track"><span style:width={updateProgress.progress + "%"}></span></div></div>
                {/if}
                <div class="button-row">
                  {#if componentCanInstall(selectedRuntimeComponent)}
                    <button type="button" class="primary-button" onclick={() => selectedRuntimeComponent && installComponentUpdate(selectedRuntimeComponent)} disabled={updateBusy}>{selectedRuntimeComponent ? componentActionLabel(selectedRuntimeComponent) : "Update"}</button>
                  {/if}
                  {#if selectedRuntimeComponent?.last_action === "installed"}<button type="button" class="secondary-button" onclick={() => selectedRuntimeComponent && rollbackComponentUpdate(selectedRuntimeComponent)} disabled={updateBusy}>Rollback</button>{/if}
                  {#if selectedRuntimeComponent?.notes_url}<button type="button" class="secondary-button" onclick={() => openUpdateUrl(selectedRuntimeComponent?.notes_url ?? null)}>Release notes</button>{/if}
                </div>
              </article>

              {#each selectedLanguageComponents as component}
                <article class:update-available={component.state === "update_available" || component.state === "corrupted"} class:focused={versionFocus === component.id} class:not-installed={!component.installed_version} class="version-card">
                  <div class="version-card-heading"><span>Localization Pack</span><b>{updates?.error ? "Check failed" : componentStatusLabel(component)}</b></div>
                  <h3>{component.locale ?? component.id}</h3>
                  <div class="version-numbers"><div><span>Current</span><strong>{component.installed_version ?? "-"}</strong></div><div><span>Latest</span><strong>{component.latest_version ?? "-"}</strong></div></div>
                  <p>{componentDescription(component)}</p>
                  {#if updateProgress?.component_id === component.id}
                    <div class="component-progress"><div><strong>{updateProgress.detail}</strong><span>{updateProgress.progress}%</span></div><div class="progress-track"><span style:width={updateProgress.progress + "%"}></span></div></div>
                  {/if}
                  <div class="button-row">
                    {#if componentCanInstall(component)}<button type="button" class="primary-button" onclick={() => installComponentUpdate(component)} disabled={updateBusy}>{componentActionLabel(component)}</button>{/if}
                    {#if component.last_action === "installed"}<button type="button" class="secondary-button" onclick={() => rollbackComponentUpdate(component)} disabled={updateBusy}>Rollback</button>{/if}
                    {#if component.notes_url}<button type="button" class="secondary-button" onclick={() => openUpdateUrl(component.notes_url)}>Release notes</button>{/if}
                  </div>
                </article>
              {/each}
            </div>

            <section class="release-history">
              <div class="release-history-heading"><div><span>Component status</span><strong>{selected.name}</strong></div></div>
              <div class="version-table" role="table" aria-label="Component versions">
                <div class="version-row version-head" role="row"><span>Status</span><span>Component</span><span>Version</span><span>Package</span></div>
                {#if selectedRuntimeComponent}
                  <div class="version-row" role="row">
                    <span data-label="Status"><i class:installed={!!selectedRuntimeComponent.installed_version}></i>{componentStatusLabel(selectedRuntimeComponent)}</span>
                    <strong data-label="Component">Runtime</strong>
                    <span data-label="Version">{selectedRuntimeComponent.installed_version ?? "-"}</span>
                    <span data-label="Package">{humanBytes(selectedRuntimeComponent.size)}</span>
                  </div>
                {/if}
                {#each selectedLanguageComponents as component}
                  <div class="version-row" role="row">
                    <span data-label="Status"><i class:installed={!!component.installed_version}></i>{componentStatusLabel(component)}</span>
                    <strong data-label="Component">{component.locale ?? "Language"}</strong>
                    <span data-label="Version">{component.installed_version ?? "-"}</span>
                    <span data-label="Package">{humanBytes(component.size)}</span>
                  </div>
                {/each}
              </div>
            </section>
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
                  <input type="text" bind:value={libraryPath} oninput={scheduleLibraryPathInspection} spellcheck="false" disabled={libraryBusy} />
                  <button type="button" class="secondary-button" onclick={chooseLibraryFolder} disabled={libraryBusy}>Browse</button>
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
                {#if !firstLaunchPending}<button type="button" class="secondary-button library-back" onclick={() => showView("game")}>Back</button>{/if}
                {#if !firstLaunchPending}<button type="button" class="secondary-button" onclick={openGameLibrary} disabled={libraryBusy}>Open Library</button>{/if}
                <button type="button" class="primary-button" onclick={applyLibraryLocation} disabled={libraryBusy || !libraryCanApply}>{libraryBusy ? "Moving..." : firstLaunchPending ? "Continue" : "Apply"}</button>
              </div>
            </section>
          </div>
        </section>
      {:else if activeView === "support"}
        <section class="page-view support-view">
          <header class="page-header compact-header"><div><h1>Support &amp; FAQ</h1><p>Create a support package before reporting a problem.</p></div></header>
          <div class="support-layout support-simple">
            <section class="panel-card full-width help-primary">
              <div class="panel-heading compact"><div><h2>Asking for help</h2></div></div>
              <p>The support package contains the logs and technical information needed to diagnose most problems.</p>
              <div class="button-row help-actions">
                <button type="button" class="primary-button" onclick={createSupportArchive} disabled={busy || !selected}>{busy ? "Creating..." : "Create Support Package"}</button>
                <button type="button" class="secondary-button" onclick={() => openFolder("all_logs")} disabled={!selected}>Open Log Folder</button>
                <button type="button" class="secondary-button" onclick={() => openFolder("save")} disabled={!selected}>Open Save Folder</button>
                <button type="button" class="secondary-button" onclick={openLicenseNotices}>Licenses &amp; Notices</button>
              </div>
            </section>

            <section class="panel-card">
              <div class="panel-heading compact"><div><h2>Report an issue</h2></div></div>
              <p class="support-copy">GitHub issue links will be enabled when the public repository is available.</p>
              <div class="button-row report-actions">
                <button type="button" class="secondary-button" disabled>Report Launcher Issue</button>
                <button type="button" class="secondary-button" disabled>Report Game Issue</button>
              </div>
            </section>

            <section class="panel-card">
              <div class="panel-heading compact"><div><h2>Information</h2></div></div>
              <div class="status-list">
                <div><span>Launcher</span><strong>v{launcherComponent?.installed_version ?? "1.0.0"}</strong></div>
                <div><span>Crash of the Titans</span><strong>{cotGame?.runtime_version ?? "Unavailable"}</strong></div>
                <div><span>Crash: Mind Over Mutant</span><strong>{momGame?.runtime_version ?? "0.0.0-dev"}</strong></div>
                <div><span>Creator &amp; Lead Developer</span><strong>Alex "OAleex" Félix</strong></div>
              </div>
              <div class="button-row"><button type="button" class="secondary-button" onclick={checkUpdates}>Check for Updates</button></div>
            </section>

            <p class="legal-note">
              MojoRecomp is an unofficial fan project. Official builds are provided free of charge. Crash Bandicoot, Crash of the Titans,
              Crash: Mind Over Mutant, their names, logos, characters, and related trademarks belong to their
              respective owners. MojoRecomp is not affiliated with or endorsed by those owners.
            </p>
          </div>
        </section>
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
          <button type="button" class="secondary-button localization-cancel" onclick={cancelPtBr}>Cancel</button>
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
