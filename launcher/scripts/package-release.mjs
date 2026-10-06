import { createHash, createPrivateKey, createPublicKey, sign as signBytes } from "node:crypto";
import { copyFile, cp, mkdir, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import { dirname, relative, resolve, sep } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertCanonicalProjectLicense } from "./project-license.mjs";
import {
  isGitHubReleaseFeedUrl,
  newestGitHubReleaseAssetUrl,
  normalizedHttpsBase,
  runtimeHistoryFromCatalog,
  validatePublicHttpsUrl,
  validateReleaseChannel,
  validateReleaseDate,
} from "./release-metadata.mjs";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const releaseRoot = resolve(projectRoot, ".release");
const releaseStateRoot = resolve(projectRoot, ".release-state");
const privateReleaseSymbolsRoot = resolve(releaseStateRoot, "symbols");
const releaseHistoryCachePath = resolve(releaseStateRoot, "history/update-catalog.toml");
const releasePublicKeyPath = resolve(launcherRoot, "release-signing-public-key.hex");
const defaultReleasePrivateKeyPath = resolve(
  projectRoot,
  "release-signing-private.pem",
);
const offlinePackageManifest = "mojorecomp-package.toml";
const offlinePackageSignature = "mojorecomp-package.sig";
const launcherSignatureMagic = Buffer.from("MOJORECOMP-LAUNCHER-SIG-V1", "ascii");
const launcherSignatureDomain = Buffer.from("MojoRecomp launcher executable signature v1\0", "utf8");
const productionRoot = process.env.MOJORECOMP_PRODUCTION_ROOT
  ? resolve(process.env.MOJORECOMP_PRODUCTION_ROOT)
  : resolve(projectRoot, "../game/production");

function fail(message) {
  throw new Error(message);
}

function quotedValue(text, key) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"));
  if (!match) fail(`Missing ${key} in version.toml`);
  return match[1];
}

function quotedValueAllowEmpty(text, key, label) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*"([^"]*)"`, "m"));
  if (!match) fail(`Missing ${key} in ${label}`);
  return match[1];
}

function integerValue(text, key, label) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*([0-9]+)\\s*$`, "m"));
  if (!match) fail(`Missing ${key} in ${label}`);
  const value = Number(match[1]);
  if (!Number.isSafeInteger(value)) fail(`Invalid ${key} in ${label}`);
  return value;
}

function booleanValue(text, key, label, defaultValue = false) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*(true|false)\\s*$`, "m"));
  if (!match) return defaultValue;
  return match[1] === "true";
}

async function requireFile(path, label) {
  const info = await stat(path).catch(() => null);
  if (!info?.isFile()) fail(`${label} is missing: ${path}`);
}

async function sha256(path) {
  const bytes = await readFile(path);
  return createHash("sha256").update(bytes).digest("hex");
}

async function releaseSigningKey() {
  const privateKeyPath = process.env.MOJORECOMP_RELEASE_SIGNING_KEY
    ? resolve(process.env.MOJORECOMP_RELEASE_SIGNING_KEY)
    : defaultReleasePrivateKeyPath;
  const privatePem = await readFile(privateKeyPath, "utf8").catch(() => null);
  if (!privatePem) {
    fail(
      `Release signing key is missing: ${privateKeyPath}. ` +
        "Set MOJORECOMP_RELEASE_SIGNING_KEY or run npm run generate:release-signing-key once.",
    );
  }
  const privateKey = createPrivateKey(privatePem);
  if (privateKey.asymmetricKeyType !== "ed25519") {
    fail("The release signing key must be an Ed25519 private key");
  }
  const expectedPublicKey = (await readFile(releasePublicKeyPath, "utf8")).trim().toLowerCase();
  if (!/^[0-9a-f]{64}$/.test(expectedPublicKey)) {
    fail("release-signing-public-key.hex must contain one 32-byte Ed25519 public key");
  }
  const publicDer = createPublicKey(privateKey).export({ format: "der", type: "spki" });
  const actualPublicKey = publicDer.subarray(publicDer.length - 32).toString("hex");
  if (actualPublicKey !== expectedPublicKey) {
    fail("The release signing key does not match the public key embedded in the launcher");
  }
  return privateKey;
}

async function writeSignedLauncherExecutable(source, destination, version) {
  const payload = await readFile(source);
  if (payload.length < 2 || payload[0] !== 0x4d || payload[1] !== 0x5a) {
    fail("Validated launcher is not a Windows executable");
  }
  const versionBytes = Buffer.from(version, "utf8");
  if (versionBytes.length === 0 || versionBytes.length > 0xffff) {
    fail("Launcher version is too long for the signed executable trailer");
  }
  const payloadHash = createHash("sha256").update(payload).digest();
  const message = Buffer.concat([
    launcherSignatureDomain,
    versionBytes,
    Buffer.from([0]),
    payloadHash,
  ]);
  const signature = signBytes(null, message, await releaseSigningKey());
  if (signature.length !== 64) fail("Ed25519 produced an invalid launcher signature length");
  const versionLength = Buffer.alloc(2);
  versionLength.writeUInt16LE(versionBytes.length);
  await writeFile(
    destination,
    Buffer.concat([payload, versionBytes, signature, versionLength, launcherSignatureMagic]),
  );
}

function compatibilityRequirementsToml(requirements, tableName) {
  const semanticVersion = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/;
  return (requirements ?? [])
    .map((requirement) => {
      if (!/^[a-z0-9][a-z0-9._-]*$/.test(requirement.id ?? "")) {
        fail(`Invalid compatibility component id: ${requirement.id ?? ""}`);
      }
      if (requirement.minVersion && !semanticVersion.test(requirement.minVersion)) {
        fail(`Invalid minimum compatibility version for ${requirement.id}: ${requirement.minVersion}`);
      }
      if (requirement.maxVersion && !semanticVersion.test(requirement.maxVersion)) {
        fail(`Invalid maximum compatibility version for ${requirement.id}: ${requirement.maxVersion}`);
      }
      const fields = [`id = ${JSON.stringify(requirement.id)}`];
      if (requirement.minVersion) fields.push(`min_version = ${JSON.stringify(requirement.minVersion)}`);
      if (requirement.maxVersion) fields.push(`max_version = ${JSON.stringify(requirement.maxVersion)}`);
      if (fields.length === 1) fail(`Compatibility requirement ${requirement.id} has no version bounds`);
      return `[[${tableName}]]\n${fields.join("\n")}`;
    })
    .join("\n\n");
}

async function writeSignedPackageManifest(root, metadata, requirements = []) {
  const files = [];
  for (const path of await collectFiles(root)) {
    const relativePath = relative(root, path).split(sep).join("/");
    if (
      relativePath.toLowerCase() === offlinePackageManifest ||
      relativePath.toLowerCase() === offlinePackageSignature
    ) {
      fail(`Package metadata already exists before signing: ${relativePath}`);
    }
    const info = await stat(path);
    files.push({ path: relativePath, size: info.size, sha256: await sha256(path) });
  }
  files.sort((left, right) => left.path.localeCompare(right.path, "en"));
  const fileBlocks = files
    .map(
      (file) =>
        `[[files]]\npath = ${JSON.stringify(file.path)}\nsize = ${file.size}\nsha256 = "${file.sha256}"`,
    )
    .join("\n\n");
  const requirementBlocks = compatibilityRequirementsToml(requirements, "require");
  const manifest = `schema_version = 2\n${metadata.trim()}${requirementBlocks ? `\n\n${requirementBlocks}` : ""}\n\n${fileBlocks}\n`;
  const signature = signBytes(null, Buffer.from(manifest, "utf8"), await releaseSigningKey());
  if (signature.length !== 64) fail("Ed25519 produced an invalid release signature length");
  await writeFile(resolve(root, offlinePackageManifest), manifest, "utf8");
  await writeFile(resolve(root, offlinePackageSignature), `${signature.toString("hex")}\n`, "utf8");
}

async function writeSignedLocalizationPackManifest(root, metadata) {
  const languageBlocks = metadata.languages.map((language) => `[[language]]
id = ${JSON.stringify(language.id)}
locale = ${JSON.stringify(language.locale)}
display_name = ${JSON.stringify(language.displayName)}
xbox_language = ${language.xboxLanguage}
version = ${JSON.stringify(language.version)}
file = ${JSON.stringify(language.file)}
size = ${language.size}
sha256 = ${JSON.stringify(language.sha256)}`).join("\n\n");
  const manifest = `schema_version = 1
game_id = ${JSON.stringify(metadata.gameId)}
version = ${JSON.stringify(metadata.version)}

${languageBlocks}
`;
  const signature = signBytes(null, Buffer.from(manifest, "utf8"), await releaseSigningKey());
  if (signature.length !== 64) fail("Ed25519 produced an invalid Localization Pack signature length");
  await writeFile(resolve(root, "localization-pack.toml"), manifest, "utf8");
  await writeFile(resolve(root, "localization-pack.sig"), `${signature.toString("hex")}\n`, "utf8");
}

async function collectFiles(root, output = []) {
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = resolve(root, entry.name);
    if (entry.isSymbolicLink()) fail(`Release staging contains a symbolic link: ${path}`);
    if (entry.isDirectory()) await collectFiles(path, output);
    else if (entry.isFile()) output.push(path);
  }
  return output;
}

async function directorySize(root) {
  let total = 0;
  for (const file of await collectFiles(root)) {
    total += (await stat(file)).size;
  }
  return total;
}

async function discoverLocalizationLanguages(gameId) {
  const languagesRoot = resolve(projectRoot, "localization", gameId, "languages");
  const rootInfo = await stat(languagesRoot).catch(() => null);
  if (!rootInfo?.isDirectory()) return [];
  const entries = (await readdir(languagesRoot, { withFileTypes: true }))
    .filter((entry) => entry.isDirectory())
    .sort((left, right) => left.name.localeCompare(right.name, "en"));
  const languages = [];
  for (const entry of entries) {
    const languageRoot = resolve(languagesRoot, entry.name);
    const manifestPath = resolve(languageRoot, "language.toml");
    const manifest = await readFile(manifestPath, "utf8").catch(() => null);
    if (!manifest) continue;
    if (!booleanValue(manifest, "publish", manifestPath, false)) {
      console.warn(`Localization language ${entry.name} is configured but not publishable yet; skipping.`);
      continue;
    }
    const id = quotedValue(manifest, "id");
    const locale = quotedValue(manifest, "locale");
    const displayName = quotedValue(manifest, "display_name");
    const version = quotedValue(manifest, "version");
    const translationVersion = quotedValue(manifest, "translation_version");
    const creditsUrl = quotedValue(manifest, "credits_url");
    const xboxLanguage = integerValue(manifest, "xbox_language", manifestPath);
    const expectedId = `language.${gameId}.${locale.toLowerCase()}`;
    if (id !== expectedId) fail(`${manifestPath} id must be ${expectedId}`);
    if (!/^[A-Za-z0-9](?:[A-Za-z0-9-]{0,33}[A-Za-z0-9])?$/.test(locale)) {
      fail(`${manifestPath} has an invalid locale`);
    }
    if (!displayName.trim() || displayName.length > 80) fail(`${manifestPath} has an invalid display_name`);
    if (!translationVersion.trim() || translationVersion.length > 40) fail(`${manifestPath} has an invalid translation_version`);
    validatePublicHttpsUrl(creditsUrl, `${manifestPath} credits_url`);
    if (xboxLanguage < 1 || xboxLanguage > 255) fail(`${manifestPath} has an invalid xbox_language`);
    if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/.test(version)) {
      fail(`${manifestPath} has an invalid language version`);
    }
    const payloadRoot = resolve(languageRoot, "payload");
    const payloadInfo = await stat(payloadRoot).catch(() => null);
    if (!payloadInfo?.isDirectory()) fail(`${manifestPath} is publishable but its payload directory is missing`);
    const creditsPath = resolve(languageRoot, "CREDITS.txt");
    await requireFile(creditsPath, `${manifestPath} credits file`);
    const creditsText = await readFile(creditsPath, "utf8");
    if (!creditsText.trim()) fail(`${creditsPath} is empty`);
    const payloadFiles = await collectFiles(payloadRoot);
    if (payloadFiles.length === 0) fail(`${manifestPath} is publishable but its payload is empty`);
    const requiredFiles = payloadFiles
      .map((path) => relative(payloadRoot, path).split(sep).join("/"))
      .sort((left, right) => left.localeCompare(right, "en"));
    if (requiredFiles.some((path) => path.startsWith("../") || path === ".." || path.includes("\\"))) {
      fail(`${manifestPath} contains an unsafe payload path`);
    }
    if (!requiredFiles.includes("language-patches.toml")) {
      fail(`${manifestPath} payload is missing language-patches.toml`);
    }
    for (const path of requiredFiles) {
      const lower = path.toLowerCase();
      if (lower === "language-patches.toml") continue;
      if (!lower.startsWith("patches/") || !lower.endsWith(".mjdelta")) {
        fail(`${manifestPath} payload must contain only MojoRecomp delta patches: ${path}`);
      }
    }
    languages.push({
      id,
      locale,
      displayName,
      xboxLanguage,
      version,
      translationVersion,
      creditsUrl,
      creditsPath,
      payloadRoot,
      requiredFiles,
    });
  }
  return languages;
}

async function writeConsolidatedNotices(destination, heading, introduction, licenseFiles) {
  const sections = [
    `${heading}\n${"=".repeat(heading.length)}\n\n${introduction.trim()}\n`,
  ];
  for (const name of licenseFiles) {
    const source = resolve(licenseRoot, name);
    await requireFile(source, `Third-party notice ${name}`);
    const body = (await readFile(source, "utf8")).replace(/\r\n/g, "\n").trimEnd();
    sections.push(`\n\n----- ${name} -----\n\n${body}\n`);
  }
  await writeFile(destination, sections.join(""), "utf8");
}

function assertSafeReleaseRoot() {
  const rel = relative(projectRoot, releaseRoot);
  if (rel !== ".release" || rel.startsWith(`..${sep}`) || releaseRoot === projectRoot) {
    fail(`Refusing unsafe release directory: ${releaseRoot}`);
  }
}

function runTar(args, cwd) {
  const result = spawnSync("tar.exe", args, {
    cwd,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error) fail(`Could not start tar.exe: ${result.error.message}`);
  if (result.status !== 0) {
    fail(`tar.exe failed (${result.status}): ${(result.stderr || result.stdout).trim()}`);
  }
  return result.stdout;
}

function runProgram(program, args, cwd) {
  const result = spawnSync(program, args, {
    cwd,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error) fail(`Could not start ${program}: ${result.error.message}`);
  if (result.status !== 0) {
    fail(`${program} failed (${result.status}): ${(result.stderr || result.stdout).trim()}`);
  }
  return result.stdout.trim();
}

async function previousReleaseHistory(catalogUrl, runtimeVersion) {
  const catalogs = [];
  try {
    if (catalogUrl) {
      const requestOptions = () => ({
        redirect: "follow",
        cache: "no-store",
        signal: AbortSignal.timeout(5000),
        headers: {
          Accept: "application/vnd.github+json",
          "User-Agent": "MojoRecomp-release-builder",
        },
      });
      const response = await fetch(catalogUrl, requestOptions());
      if (response.ok) {
        validatePublicHttpsUrl(response.url, "resolved previous update catalog URL");
        if (isGitHubReleaseFeedUrl(catalogUrl)) {
          const assetUrl = newestGitHubReleaseAssetUrl(
            await response.json(),
            "update-catalog.toml",
          );
          if (assetUrl) {
            const assetResponse = await fetch(assetUrl, requestOptions());
            if (assetResponse.ok) {
              validatePublicHttpsUrl(assetResponse.url, "resolved previous update catalog asset URL");
              catalogs.push(await assetResponse.text());
            } else {
              console.warn(`Previous public update catalog asset is unavailable (${assetResponse.status}).`);
            }
          } else {
            console.warn("No published GitHub release or pre-release contains update-catalog.toml yet.");
          }
        } else {
          catalogs.push(await response.text());
        }
      } else {
        console.warn(`Previous public update catalog is unavailable (${response.status}).`);
      }
    }
  } catch (error) {
    console.warn(`Previous public update catalog could not be read: ${error.message}`);
  }
  const cached = await readFile(releaseHistoryCachePath, "utf8").catch(() => null);
  if (cached) catalogs.push(cached);

  const runtimeSeen = new Set();
  const runtimeHistory = [];
  for (const catalog of catalogs) {
    for (const release of runtimeHistoryFromCatalog(catalog, runtimeVersion)) {
      if (runtimeSeen.has(release.version)) continue;
      runtimeSeen.add(release.version);
      runtimeHistory.push(release);
    }
  }
  console.log(`Preserving ${runtimeHistory.length} previous signed COT runtime release(s) in the update catalog.`);
  return { runtimeHistory };
}

function launcherCatalogBlock(release) {
  return `[[release]]
id = "launcher"
kind = "launcher"
version = ${JSON.stringify(release.version)}
platform = "windows"
arch = "x86_64"
url = ${JSON.stringify(release.url)}
size = ${release.size}
sha256 = ${JSON.stringify(release.sha256)}
published = ${JSON.stringify(release.published)}
notes_url = ${JSON.stringify(release.notesUrl)}
package = "portable-exe"

[release.compatibility]`;
}

function runtimeCatalogBlock(release) {
  const compatibility = [
    release.minLauncher ? `min_launcher = ${JSON.stringify(release.minLauncher)}` : "",
    release.maxLauncher ? `max_launcher = ${JSON.stringify(release.maxLauncher)}` : "",
  ].filter(Boolean).join("\n");
  const requirements = compatibilityRequirementsToml(
    release.requirements,
    "release.compatibility.require",
  );
  return `[[release]]
id = "runtime.cot"
kind = "runtime"
version = ${JSON.stringify(release.version)}
platform = "windows"
arch = "x86_64"
url = ${JSON.stringify(release.url)}
size = ${release.size}
sha256 = ${JSON.stringify(release.sha256)}
published = ${JSON.stringify(release.published)}
notes_url = ${JSON.stringify(release.notesUrl)}
package = "zip"
unpacked_size = ${release.unpackedSize}
entrypoint = "cot-runtime.exe"
required_files = [${release.requiredFiles.map((name) => JSON.stringify(name)).join(", ")}]
game_id = "cot"
${release.localizationCatalogUrl ? `localization_catalog_url = ${JSON.stringify(release.localizationCatalogUrl)}\n` : ""}

[release.compatibility]${compatibility ? `\n${compatibility}` : ""}${requirements ? `\n\n${requirements}` : ""}`;
}

function localizationCatalogText({
  gameId,
  runtimeVersion,
  packVersion,
  url,
  size,
  sha256,
  published,
  notesUrl,
  minLauncher,
  releases,
}) {
  const languages = releases.map((release) => `[[language]]
id = ${JSON.stringify(release.id)}
game_id = ${JSON.stringify(release.gameId)}
locale = ${JSON.stringify(release.locale)}
display_name = ${JSON.stringify(release.displayName)}
xbox_language = ${release.xboxLanguage}
version = ${JSON.stringify(release.version)}
component_size = ${release.localizationPack.componentSize}
component_sha256 = ${JSON.stringify(release.localizationPack.componentSha256)}
unpacked_size = ${release.unpackedSize}
required_files = [${release.requiredFiles.map((name) => JSON.stringify(name)).join(", ")}]`).join("\n\n");
  return `schema_version = 2
game_id = ${JSON.stringify(gameId)}
runtime_version = ${JSON.stringify(runtimeVersion)}
pack_version = ${JSON.stringify(packVersion)}
url = ${JSON.stringify(url)}
size = ${size}
sha256 = ${JSON.stringify(sha256)}
published = ${JSON.stringify(published)}
notes_url = ${JSON.stringify(notesUrl)}
min_launcher = ${JSON.stringify(minLauncher)}
${languages ? `\n${languages}\n` : ""}`;
}

async function stageCorrespondingSource({
  id,
  directoryName,
  label,
  repository,
  expectedCommit,
  buildFiles,
  licenseFile,
  bundleRoot,
  additionalNotices = [],
}) {
  const actualCommit = runProgram("git.exe", ["rev-parse", "HEAD"], repository);
  if (actualCommit.toLowerCase() !== expectedCommit.toLowerCase()) {
    fail(`${label} source is at ${actualCommit}, expected ${expectedCommit}`);
  }

  const stagingRoot = resolve(bundleRoot, directoryName);
  const sourceRoot = resolve(stagingRoot, "upstream-source");
  const recipeRoot = resolve(stagingRoot, "mojorecomp-build");
  const sourceTar = resolve(releaseRoot, `.${id}-source.tmp.tar`);
  await mkdir(sourceRoot, { recursive: true });
  runProgram(
    "git.exe",
    ["archive", "--format=tar", "-o", sourceTar, expectedCommit],
    repository,
  );
  runTar(["-x", "-f", sourceTar, "-C", sourceRoot], releaseRoot);
  await rm(sourceTar, { force: true });
  await cp(buildFiles, recipeRoot, { recursive: true });
  await copyFile(projectLicense, resolve(stagingRoot, "LICENSE"));
  await copyFile(licenseFile, resolve(stagingRoot, "COPYING.LGPL-2.1.txt"));
  for (const notice of additionalNotices) {
    await requireFile(notice.source, `${label} additional license notice`);
    await copyFile(notice.source, resolve(stagingRoot, notice.destination));
  }
  const additionalNoticeText = additionalNotices.length
    ? `Additional license notices: ${additionalNotices.map((notice) => notice.destination).join(", ")}\n`
    : "";
  await writeFile(
    resolve(stagingRoot, "MOJORECOMP-SOURCE-INFO.txt"),
    `${label} corresponding source for MojoRecomp ${suiteVersion}\n\n` +
      `Upstream commit: ${expectedCommit}\n` +
      "The exact upstream source is under upstream-source.\n" +
      "The maintained MojoRecomp build recipe is under mojorecomp-build.\n" +
      "The MojoRecomp recipe is covered by the ISC License in LICENSE.\n" +
      additionalNoticeText +
      "No upstream source files are patched in place; the recipe selects and builds the shipped library.\n",
    "utf8",
  );

  for (const required of [
    "LICENSE",
    "COPYING.LGPL-2.1.txt",
    "MOJORECOMP-SOURCE-INFO.txt",
    ...additionalNotices.map((notice) => notice.destination),
  ]) {
    await requireFile(resolve(stagingRoot, required), `${label} corresponding-source file ${required}`);
  }
  if ((await readdir(sourceRoot)).length === 0) {
    fail(`${label} corresponding source has no upstream source tree`);
  }
  if ((await readdir(recipeRoot)).length === 0) {
    fail(`${label} corresponding source has no MojoRecomp build recipe`);
  }
}

const versions = await readFile(resolve(projectRoot, "version.toml"), "utf8");
const suiteManifest = await readFile(resolve(launcherRoot, "resources/suite.toml"), "utf8");
const suiteVersion = quotedValue(versions, "suite");
const launcherVersion = quotedValue(versions, "launcher");
const cotRuntimeVersion = quotedValue(versions, "cot_runtime");
const cotLocalizationPackVersion = quotedValue(versions, "cot_localization_pack");
const releaseChannel = validateReleaseChannel(
  quotedValueAllowEmpty(suiteManifest, "release_channel", "suite.toml"),
);
const configuredCatalogUrl = validatePublicHttpsUrl(
  quotedValueAllowEmpty(suiteManifest, "update_catalog", "suite.toml"),
  "suite.toml update_catalog",
);
const configuredLocalizationCatalogUrl = validatePublicHttpsUrl(
  quotedValueAllowEmpty(suiteManifest, "localization_catalog", "suite.toml"),
  "suite.toml localization_catalog",
);
const updateBaseUrl = normalizedHttpsBase(
  process.env.MOJORECOMP_UPDATE_BASE_URL || "",
  "MOJORECOMP_UPDATE_BASE_URL",
);
const updateNotesUrl = validatePublicHttpsUrl(
  process.env.MOJORECOMP_UPDATE_NOTES_URL || "",
  "MOJORECOMP_UPDATE_NOTES_URL",
);
if (!configuredCatalogUrl) {
  fail("Configure the public HTTPS update catalog before creating a release package");
}
if (!configuredLocalizationCatalogUrl) {
  fail("Configure the public HTTPS localization catalog before creating a release package");
}
if (!updateBaseUrl) {
  fail("MOJORECOMP_UPDATE_BASE_URL is required to create a release package");
}
if (updateBaseUrl && !updateNotesUrl) {
  fail("MOJORECOMP_UPDATE_NOTES_URL is required when MOJORECOMP_UPDATE_BASE_URL is set");
}
const releaseDate = validateReleaseDate(
  process.env.MOJORECOMP_RELEASE_DATE || new Date().toISOString().slice(0, 10),
);
if (suiteVersion !== launcherVersion) {
  fail(`Suite ${suiteVersion} and launcher ${launcherVersion} must match`);
}

const launcherExe = resolve(productionRoot, "mojorecomp-launcher.exe");
const projectLicense = resolve(projectRoot, "LICENSE");
const licenseRoot = resolve(launcherRoot, "resources/licenses");
const runtimeNoticeFiles = [
  "DXC-LICENSE.txt",
  "DXC-ThirdPartyNotices.txt",
  "extract-xiso-LICENSE.TXT",
  "FFmpeg-LGPL-2.1.txt",
  "FFmpeg-LICENSE.md",
  "fmt-LICENSE.txt",
  "libmspack-LGPL-2.1.txt",
  "LLVM-compiler-rt-LICENSE.txt",
  "NVIDIA-FXAA-LICENSE.txt",
  "ReXGlue-LICENSE.txt",
  "SDL3-LICENSE.txt",
  "SIMDe-LICENSE.txt",
  "tiny-AES-c-UNLICENSE.txt",
  "TinySHA1-NOTICE.txt",
  "tomlplusplus-LICENSE.txt",
  "Vulkan-Headers-Apache-2.0.txt",
  "Vulkan-Headers-LICENSE.md",
  "Vulkan-Headers-MIT.txt",
  "XenonRecomp-LICENSE.md",
  "XenosRecomp-LICENSE.md",
  "xxHash-LICENSE.txt",
];
const cotRuntimeFiles = [
  [resolve(projectRoot, "runtime/build-smoke/cot-runtime.exe"), "cot-runtime.exe"],
  [resolve(launcherRoot, "bundle/lib/dxcompiler.dll"), "dxcompiler.dll"],
  [resolve(launcherRoot, "bundle/lib/dxil.dll"), "dxil.dll"],
  [resolve(launcherRoot, "bundle/lib/mojorecomp-ffmpeg.dll"), "mojorecomp-ffmpeg.dll"],
  [resolve(launcherRoot, "bundle/lib/mojorecomp-lzx.dll"), "mojorecomp-lzx.dll"],
  [resolve(launcherRoot, "bundle/tools/extract-xiso.exe"), "extract-xiso.exe"],
];
const cotRuntimePdb = resolve(projectRoot, "runtime/build-smoke/cot-runtime.pdb");
const cotRuntimeRequirements = [];
await requireFile(launcherExe, "Validated production launcher");
await requireFile(projectLicense, "MojoRecomp ISC license");
assertCanonicalProjectLicense(await readFile(projectLicense, "utf8"));
for (const [source, name] of cotRuntimeFiles) {
  await requireFile(source, "COT runtime component file " + name);
}
await requireFile(cotRuntimePdb, "COT runtime private symbols");
await releaseSigningKey();

assertSafeReleaseRoot();
await rm(releaseRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 250 });
await mkdir(releaseRoot, { recursive: true });

const lgplSourcesName = `MojoRecomp-LGPL-Sources-${suiteVersion}`;
const lgplSourcesRoot = resolve(releaseRoot, lgplSourcesName);
await mkdir(lgplSourcesRoot, { recursive: true });
await stageCorrespondingSource({
  id: "FFmpeg",
  directoryName: "FFmpeg",
  label: "FFmpeg",
  repository: resolve(projectRoot, "thirdparty/FFmpeg"),
  expectedCommit: "0604b464c7cb4ebc94940cf1f324a3b26b87717c",
  buildFiles: resolve(projectRoot, "tools/ffmpeg-rexglue"),
  licenseFile: resolve(licenseRoot, "FFmpeg-LGPL-2.1.txt"),
  bundleRoot: lgplSourcesRoot,
  additionalNotices: [
    {
      source: resolve(licenseRoot, "ReXGlue-LICENSE.txt"),
      destination: "ReXGlue-LICENSE.txt",
    },
  ],
});
await stageCorrespondingSource({
  id: "libmspack",
  directoryName: "libmspack",
  label: "libmspack LZX",
  repository: resolve(projectRoot, "thirdparty/XenonRecomp/thirdparty/libmspack"),
  expectedCommit: "305907723a4e7ab2018e58040059ffb5e77db837",
  buildFiles: resolve(projectRoot, "tools/libmspack-lzx"),
  licenseFile: resolve(licenseRoot, "libmspack-LGPL-2.1.txt"),
  bundleRoot: lgplSourcesRoot,
});
await writeFile(
  resolve(lgplSourcesRoot, "README.txt"),
  `MojoRecomp ${suiteVersion} - LGPL Corresponding Sources\n\n` +
    "This archive contains the exact corresponding source and MojoRecomp build recipes\n" +
    "for the replaceable FFmpeg and libmspack LZX libraries distributed with this release.\n\n" +
    "FFmpeg: 0604b464c7cb4ebc94940cf1f324a3b26b87717c\n" +
    "libmspack: 305907723a4e7ab2018e58040059ffb5e77db837\n",
  "utf8",
);
const lgplSourcesArchiveName = `${lgplSourcesName}.zip`;
const lgplSourcesArchivePath = resolve(releaseRoot, lgplSourcesArchiveName);
runTar(["-a", "-c", "-f", lgplSourcesArchivePath, lgplSourcesName], releaseRoot);
const lgplSourcesListing = runTar(["-t", "-f", lgplSourcesArchivePath], releaseRoot)
  .split(/\r?\n/)
  .filter(Boolean);
for (const directoryName of ["FFmpeg", "libmspack"]) {
  if (!lgplSourcesListing.some((entry) => entry.includes(`/${directoryName}/upstream-source/`))) {
    fail(`${directoryName} is missing its upstream source tree from the LGPL source archive`);
  }
  if (!lgplSourcesListing.some((entry) => entry.includes(`/${directoryName}/mojorecomp-build/`))) {
    fail(`${directoryName} is missing its MojoRecomp build recipe from the LGPL source archive`);
  }
  for (const required of ["LICENSE", "COPYING.LGPL-2.1.txt", "MOJORECOMP-SOURCE-INFO.txt"]) {
    if (!lgplSourcesListing.some((entry) => entry.endsWith(`/${directoryName}/${required}`))) {
      fail(`${directoryName} is missing ${required} from the LGPL source archive`);
    }
  }
}
if (!lgplSourcesListing.some((entry) => entry.endsWith("/FFmpeg/ReXGlue-LICENSE.txt"))) {
  fail("FFmpeg is missing ReXGlue-LICENSE.txt from the LGPL source archive");
}
if (!lgplSourcesListing.some((entry) => entry.endsWith("/README.txt"))) {
  fail("LGPL source archive is missing README.txt");
}
const lgplSourcesArchiveHash = await sha256(lgplSourcesArchivePath);
await rm(lgplSourcesRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 250 });

const runtimeComponentName =
  "MojoRecomp-COT-Runtime-" + cotRuntimeVersion + "-windows-x64";
const runtimeComponentRoot = resolve(releaseRoot, runtimeComponentName);
await mkdir(runtimeComponentRoot, { recursive: true });
for (const [source, name] of cotRuntimeFiles) {
  await copyFile(source, resolve(runtimeComponentRoot, name));
}
await copyFile(projectLicense, resolve(runtimeComponentRoot, "LICENSE"));
await writeConsolidatedNotices(
  resolve(runtimeComponentRoot, "THIRD_PARTY_NOTICES.txt"),
  "MojoRecomp COT Runtime - Third-Party Notices",
  `This file consolidates the license and notice texts applicable to the distributed
Crash of the Titans runtime component. Original MojoRecomp code is covered by the
ISC license in LICENSE. FFmpeg and libmspack LZX are shipped as replaceable LGPL
libraries; their exact corresponding source is published in the MojoRecomp LGPL Sources archive beside this release.`,
  runtimeNoticeFiles,
);
await writeSignedPackageManifest(
  runtimeComponentRoot,
  `id = "runtime.cot"
kind = "runtime"
version = ${JSON.stringify(cotRuntimeVersion)}
platform = "windows"
arch = "x86_64"
package = "zip"
entrypoint = "cot-runtime.exe"
required_files = [${cotRuntimeFiles.map(([, name]) => JSON.stringify(name)).join(", ")}]
min_launcher = ${JSON.stringify(launcherVersion)}`,
  cotRuntimeRequirements,
);
const runtimeUnpackedSize = await directorySize(runtimeComponentRoot);
const runtimeArchiveName = runtimeComponentName + ".zip";
const runtimeArchivePath = resolve(releaseRoot, runtimeArchiveName);
runTar(
  [
    "-a",
    "-c",
    "-f",
    runtimeArchivePath,
    runtimeComponentName,
  ],
  releaseRoot,
);
const runtimeArchiveListing = runTar(["-t", "-f", runtimeArchivePath], releaseRoot)
  .split(/\r?\n/)
  .filter(Boolean);
for (const [, name] of cotRuntimeFiles) {
  if (!runtimeArchiveListing.some((entry) => entry === name || entry.endsWith("/" + name))) {
    fail("COT runtime component archive is missing " + name);
  }
}
for (const name of [
  "LICENSE",
  "THIRD_PARTY_NOTICES.txt",
  offlinePackageManifest,
  offlinePackageSignature,
]) {
  if (!runtimeArchiveListing.some((entry) => entry === name || entry.endsWith("/" + name))) {
    fail("COT runtime component archive is missing " + name);
  }
}
if (runtimeArchiveListing.some((entry) => /(^|\/)licenses\//i.test(entry))) {
  fail("COT runtime component archive must not contain a loose licenses directory");
}
const runtimeArchiveHash = await sha256(runtimeArchivePath);
const runtimeArchiveSize = (await stat(runtimeArchivePath)).size;
const cotRuntimeExecutableHash = await sha256(cotRuntimeFiles[0][0]);
const cotRuntimeBuildId = `git-${runProgram("git.exe", ["rev-parse", "--short=12", "HEAD"], projectRoot)}`;
const runtimeSymbolsRoot = resolve(
  privateReleaseSymbolsRoot,
  "cot",
  cotRuntimeVersion,
  cotRuntimeExecutableHash,
);
await mkdir(runtimeSymbolsRoot, { recursive: true });
await copyFile(cotRuntimePdb, resolve(runtimeSymbolsRoot, "cot-runtime.pdb"));
await writeFile(
  resolve(runtimeSymbolsRoot, "build-info.json"),
  `${JSON.stringify({
    schema_version: 1,
    game_id: "cot",
    runtime_version: cotRuntimeVersion,
    build_id: cotRuntimeBuildId,
    runtime_sha256: cotRuntimeExecutableHash,
    executable: "cot-runtime.exe",
    pdb: "cot-runtime.pdb",
  }, null, 2)}\n`,
  "utf8",
);
await rm(runtimeComponentRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 250 });

const forbiddenEntry = /(^|[\\/])(games?|userdata|saves?|logs?|cache|support|\.private|default\.xex|default\.rcf)([\\/]|$)/i;
const forbiddenExtension = /\.(iso|xex|xexp|rcf|pdb|dmp|log)$/i;
for (const entry of runtimeArchiveListing) {
  if (forbiddenEntry.test(entry) || forbiddenExtension.test(entry)) {
    fail("Forbidden game, user, or diagnostic content in runtime component: " + entry);
  }
}

const localizationSources = await discoverLocalizationLanguages("cot");
const languageReleases = [];
let localizationPackArchiveName = null;
let localizationPackArchiveHash = null;
let localizationPackArchiveSize = null;
if (localizationSources.length > 0) {
  const packName = `MojoRecomp-COT-Localization-Pack-${cotLocalizationPackVersion}`;
  const packRoot = resolve(releaseRoot, packName);
  const packLanguagesRoot = resolve(packRoot, "languages");
  await mkdir(packLanguagesRoot, { recursive: true });
  const packLanguages = [];

  for (const language of localizationSources) {
    const componentName = `MojoRecomp-COT-Language-${language.locale}-${language.version}`;
    const componentRoot = resolve(releaseRoot, componentName);
    await cp(language.payloadRoot, componentRoot, { recursive: true });
    await copyFile(projectLicense, resolve(componentRoot, "LICENSE"));
    await copyFile(language.creditsPath, resolve(componentRoot, "CREDITS.txt"));
    await writeSignedPackageManifest(
      componentRoot,
      `id = ${JSON.stringify(language.id)}
kind = "language"
version = ${JSON.stringify(language.version)}
platform = "windows"
arch = "x86_64"
package = "zip"
required_files = [${[...language.requiredFiles, "CREDITS.txt"].map((name) => JSON.stringify(name)).join(", ")}]
game_id = "cot"
locale = ${JSON.stringify(language.locale)}
display_name = ${JSON.stringify(language.displayName)}
xbox_language = ${language.xboxLanguage}
min_launcher = ${JSON.stringify(launcherVersion)}`,
      [{ id: "runtime.cot", minVersion: cotRuntimeVersion, maxVersion: null }],
    );
    const unpackedSize = await directorySize(componentRoot);
    const nestedName = `${language.locale}.zip`;
    const archivePath = resolve(packLanguagesRoot, nestedName);
    runTar(["-a", "-c", "-f", archivePath, componentName], releaseRoot);
    const archiveHash = await sha256(archivePath);
    const archiveSize = (await stat(archivePath)).size;
    packLanguages.push({
      id: language.id,
      locale: language.locale,
      displayName: language.displayName,
      xboxLanguage: language.xboxLanguage,
      version: language.version,
      file: `languages/${nestedName}`,
      size: archiveSize,
      sha256: archiveHash,
    });
    languageReleases.push({
      id: language.id,
      version: language.version,
      url: null,
      size: null,
      sha256: null,
      published: releaseDate,
      notesUrl: updateNotesUrl,
      unpackedSize,
      requiredFiles: [
        ...language.requiredFiles,
        "CREDITS.txt",
        offlinePackageManifest,
        offlinePackageSignature,
      ],
      gameId: "cot",
      locale: language.locale,
      displayName: language.displayName,
      xboxLanguage: language.xboxLanguage,
      minLauncher: launcherVersion,
      maxLauncher: null,
      requirements: [{ id: "runtime.cot", minVersion: cotRuntimeVersion, maxVersion: null }],
      localizationPack: {
        version: cotLocalizationPackVersion,
        componentSize: archiveSize,
        componentSha256: archiveHash,
      },
    });
    await rm(componentRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 250 });
  }

  await writeSignedLocalizationPackManifest(packRoot, {
    gameId: "cot",
    version: cotLocalizationPackVersion,
    languages: packLanguages,
  });
  localizationPackArchiveName = `${packName}.zip`;
  const localizationPackArchivePath = resolve(releaseRoot, localizationPackArchiveName);
  runTar(["-a", "-c", "-f", localizationPackArchivePath, packName], releaseRoot);
  localizationPackArchiveHash = await sha256(localizationPackArchivePath);
  localizationPackArchiveSize = (await stat(localizationPackArchivePath)).size;
  for (const release of languageReleases) {
    release.url = `${updateBaseUrl}/${localizationPackArchiveName}`;
    release.size = localizationPackArchiveSize;
    release.sha256 = localizationPackArchiveHash;
  }
  await rm(packRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 250 });
  console.log(
    `Localization Pack ${cotLocalizationPackVersion}: ${languageReleases.length} language component(s), ${localizationPackArchiveSize} bytes`,
  );
} else {
  console.warn(
    "No publishable COT localization language payloads were found; Localization Pack release asset was skipped.",
  );
}

const launcherReleaseName = `MojoRecomp-Launcher-${launcherVersion}-windows-x64.exe`;
const launcherReleasePath = resolve(releaseRoot, launcherReleaseName);
await writeSignedLauncherExecutable(launcherExe, launcherReleasePath, launcherVersion);
const launcherReleaseHash = await sha256(launcherReleasePath);
const launcherReleaseSize = (await stat(launcherReleasePath)).size;
const checksumEntries = [
  { file: launcherReleaseName, sha256: launcherReleaseHash },
  { file: runtimeArchiveName, sha256: runtimeArchiveHash },
  { file: lgplSourcesArchiveName, sha256: lgplSourcesArchiveHash },
];
if (localizationPackArchiveName && localizationPackArchiveHash) {
  checksumEntries.push({ file: localizationPackArchiveName, sha256: localizationPackArchiveHash });
}
if (updateBaseUrl) {
  const catalogName = "update-catalog.toml";
  const catalogPath = resolve(releaseRoot, catalogName);
  const { runtimeHistory } = await previousReleaseHistory(
    configuredCatalogUrl,
    cotRuntimeVersion,
  );
  const launcherReleases = [{
    version: launcherVersion,
    url: `${updateBaseUrl}/${launcherReleaseName}`,
    size: launcherReleaseSize,
    sha256: launcherReleaseHash,
    published: releaseDate,
    notesUrl: updateNotesUrl,
  }];
  const runtimeReleases = [
    {
      version: cotRuntimeVersion,
      url: `${updateBaseUrl}/${runtimeArchiveName}`,
      size: runtimeArchiveSize,
      sha256: runtimeArchiveHash,
      published: releaseDate,
      notesUrl: updateNotesUrl,
      unpackedSize: runtimeUnpackedSize,
      requiredFiles: [
        ...cotRuntimeFiles.map(([, name]) => name),
        offlinePackageManifest,
        offlinePackageSignature,
      ],
      minLauncher: launcherVersion,
      maxLauncher: null,
      requirements: cotRuntimeRequirements,
      localizationCatalogUrl: languageReleases.length > 0
        ? `${updateBaseUrl}/localization-catalog.toml`
        : null,
    },
    ...runtimeHistory,
  ];
  const catalog = `schema_version = 1
channel = "${releaseChannel}"

${launcherReleases.map(launcherCatalogBlock).join("\n\n")}

${runtimeReleases.map(runtimeCatalogBlock).join("\n\n")}
`;
  await writeFile(catalogPath, catalog, "utf8");
  await mkdir(dirname(releaseHistoryCachePath), { recursive: true });
  await writeFile(releaseHistoryCachePath, catalog, "utf8");
  checksumEntries.push({ file: catalogName, sha256: await sha256(catalogPath) });
  if (languageReleases.length > 0) {
    const localizationCatalogName = "localization-catalog.toml";
    const localizationCatalogPath = resolve(releaseRoot, localizationCatalogName);
    await writeFile(
      localizationCatalogPath,
      localizationCatalogText({
        gameId: "cot",
        runtimeVersion: cotRuntimeVersion,
        packVersion: cotLocalizationPackVersion,
        url: `${updateBaseUrl}/${localizationPackArchiveName}`,
        size: localizationPackArchiveSize,
        sha256: localizationPackArchiveHash,
        published: releaseDate,
        notesUrl: updateNotesUrl,
        minLauncher: launcherVersion,
        releases: languageReleases,
      }),
      "utf8",
    );
    checksumEntries.push({
      file: localizationCatalogName,
      sha256: await sha256(localizationCatalogPath),
    });
  }
}
await writeFile(
  resolve(releaseRoot, "SHA256SUMS.txt"),
  `${checksumEntries.map((entry) => `${entry.sha256}  ${entry.file}`).join("\n")}\n`,
  "utf8",
);

console.log(`Launcher executable: ${launcherReleasePath}`);
console.log(`SHA-256: ${launcherReleaseHash}`);
console.log("Platform: Windows x64 (x86 and ARM64 are not supported)");
