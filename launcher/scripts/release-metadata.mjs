import { isIP } from "node:net";

function invalid(label, detail) {
  throw new Error(`${label} ${detail}`);
}

function publicHttpsUrl(value, label) {
  const trimmed = value.trim();
  if (!trimmed) return null;
  let parsed;
  try {
    parsed = new URL(trimmed);
  } catch {
    invalid(label, "is not a valid URL");
  }
  const hostname = parsed.hostname.toLowerCase();
  const ipCandidate = hostname.startsWith("[") && hostname.endsWith("]")
    ? hostname.slice(1, -1)
    : hostname;
  if (
    parsed.protocol !== "https:"
    || !hostname
    || parsed.username
    || parsed.password
    || hostname === "localhost"
    || hostname.endsWith(".localhost")
    || isIP(ipCandidate) !== 0
  ) {
    invalid(label, "must use a public HTTPS hostname without embedded credentials");
  }
  return parsed;
}

export function normalizedHttpsBase(value, label) {
  const parsed = publicHttpsUrl(value, label);
  if (!parsed) return "";
  if (parsed.search || parsed.hash) {
    invalid(label, "must not contain a query string or fragment");
  }
  return parsed.toString().replace(/\/+$/, "");
}

export function validatePublicHttpsUrl(value, label) {
  const parsed = publicHttpsUrl(value, label);
  return parsed ? parsed.toString() : "";
}

export function isGitHubReleaseFeedUrl(value) {
  const parsed = publicHttpsUrl(value, "GitHub release feed");
  if (!parsed) return false;
  return parsed.hostname.toLowerCase() === "api.github.com"
    && /^\/repos\/[^/]+\/[^/]+\/releases\/?$/.test(parsed.pathname);
}

export function newestGitHubReleaseAssetUrl(releases, assetName = "update-catalog.toml") {
  if (!Array.isArray(releases)) throw new Error("GitHub release feed must be an array");
  const candidates = [];
  for (const release of releases) {
    if (!release || release.draft === true || !Array.isArray(release.assets)) continue;
    const asset = release.assets.find((entry) => entry?.name === assetName);
    if (!asset?.browser_download_url) continue;
    candidates.push({
      published: release.published_at || release.created_at || "",
      url: validatePublicHttpsUrl(asset.browser_download_url, `GitHub ${assetName} asset`),
    });
  }
  candidates.sort((left, right) => right.published.localeCompare(left.published));
  return candidates[0]?.url ?? "";
}

export function validateReleaseChannel(value) {
  if (!["stable", "beta", "development"].includes(value)) {
    throw new Error("release_channel must be stable, beta, or development");
  }
  return value;
}

export function validateReleaseDate(value) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value)) {
    throw new Error(`Invalid release date: ${value}`);
  }
  const [year, month, day] = value.split("-").map(Number);
  const date = new Date(Date.UTC(year, month - 1, day));
  if (
    year < 2020
    || date.getUTCFullYear() !== year
    || date.getUTCMonth() !== month - 1
    || date.getUTCDate() !== day
  ) {
    throw new Error(`Invalid release date: ${value}`);
  }
  return value;
}

function blockString(block, key) {
  return block.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"))?.[1] ?? null;
}

function blockInteger(block, key) {
  const value = block.match(new RegExp(`^${key}\\s*=\\s*([1-9]\\d*)\\s*$`, "m"))?.[1];
  if (!value) return null;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) ? parsed : null;
}

function blockStringArray(block, key) {
  const raw = block.match(new RegExp(`^${key}\\s*=\\s*\\[([^\\]]*)\\]\\s*$`, "m"))?.[1];
  if (raw === undefined) return null;
  const values = [];
  const pattern = /"([^"]+)"/g;
  let match;
  while ((match = pattern.exec(raw)) !== null) values.push(match[1]);
  const residue = raw.replace(pattern, "").replace(/[\s,]/g, "");
  return residue ? null : values;
}

function compatibilityRequirements(block) {
  const sections = block
    .split(/(?=^\[\[release\.compatibility\.require\]\]\s*$)/m)
    .slice(1);
  const requirements = [];
  for (const section of sections) {
    const id = blockString(section, "id");
    const minVersion = blockString(section, "min_version");
    const maxVersion = blockString(section, "max_version");
    if (!id || (!minVersion && !maxVersion)) return null;
    requirements.push({ id, minVersion, maxVersion });
  }
  return requirements;
}

export function runtimeHistoryFromCatalog(catalogText, currentVersion) {
  const normalized = catalogText.replace(/\r\n/g, "\n");
  if (!/^schema_version\s*=\s*1\s*$/m.test(normalized)) return [];
  const blocks = normalized
    .split(/(?=^\[\[release\]\]\s*$)/m)
    .map((block) => block.trim())
    .filter((block) => block.startsWith("[[release]]"));
  const seen = new Set();
  const releases = [];

  for (const block of blocks) {
    const id = blockString(block, "id");
    const kind = blockString(block, "kind");
    const version = blockString(block, "version");
    const platform = blockString(block, "platform");
    const arch = blockString(block, "arch");
    const url = blockString(block, "url");
    const sha256 = blockString(block, "sha256");
    const published = blockString(block, "published");
    const notesUrl = blockString(block, "notes_url");
    const packageFormat = blockString(block, "package");
    const entrypoint = blockString(block, "entrypoint");
    const gameId = blockString(block, "game_id");
    const size = blockInteger(block, "size");
    const unpackedSize = blockInteger(block, "unpacked_size");
    const requiredFiles = blockStringArray(block, "required_files");
    const localizationCatalogUrl = blockString(block, "localization_catalog_url");
    const minLauncher = blockString(block, "min_launcher");
    const maxLauncher = blockString(block, "max_launcher");
    const requirements = compatibilityRequirements(block);

    if (
      id !== "runtime.cot"
      || kind !== "runtime"
      || !version
      || version === currentVersion
      || seen.has(version)
      || !/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/.test(version)
      || platform !== "windows"
      || arch !== "x86_64"
      || packageFormat !== "zip"
      || entrypoint !== "cot-runtime.exe"
      || gameId !== "cot"
      || !size
      || !unpackedSize
      || !requiredFiles?.includes("cot-runtime.exe")
      || !requiredFiles.includes("mojorecomp-package.toml")
      || !requiredFiles.includes("mojorecomp-package.sig")
      || requirements === null
      || !/^[0-9a-f]{64}$/i.test(sha256 ?? "")
    ) {
      continue;
    }

    try {
      validatePublicHttpsUrl(url ?? "", "historical runtime URL");
      validatePublicHttpsUrl(notesUrl ?? "", "historical runtime notes URL");
      if (localizationCatalogUrl) {
        validatePublicHttpsUrl(localizationCatalogUrl, "historical runtime localization catalog URL");
      }
      validateReleaseDate(published ?? "");
    } catch {
      continue;
    }

    seen.add(version);
    releases.push({
      version,
      url,
      size,
      sha256: sha256.toLowerCase(),
      published,
      notesUrl,
      unpackedSize,
      requiredFiles,
      minLauncher,
      maxLauncher,
      requirements,
      localizationCatalogUrl,
    });
  }
  return releases;
}
