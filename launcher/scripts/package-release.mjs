import { createHash } from "node:crypto";
import { copyFile, cp, mkdir, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import { dirname, relative, resolve, sep } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertCanonicalProjectLicense } from "./project-license.mjs";
import {
  isGitHubReleaseFeedUrl,
  launcherHistoryFromCatalog,
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
const releaseHistoryCachePath = resolve(projectRoot, ".private/release-history/update-catalog.toml");
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

async function requireFile(path, label) {
  const info = await stat(path).catch(() => null);
  if (!info?.isFile()) fail(`${label} is missing: ${path}`);
}

async function sha256(path) {
  const bytes = await readFile(path);
  return createHash("sha256").update(bytes).digest("hex");
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

async function previousReleaseHistory(catalogUrl, launcherVersion, runtimeVersion) {
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

  const launcherSeen = new Set();
  const runtimeSeen = new Set();
  const launcherHistory = [];
  const runtimeHistory = [];
  for (const catalog of catalogs) {
    for (const release of launcherHistoryFromCatalog(catalog, launcherVersion)) {
      if (launcherSeen.has(release.version)) continue;
      launcherSeen.add(release.version);
      launcherHistory.push(release);
    }
    for (const release of runtimeHistoryFromCatalog(catalog, runtimeVersion)) {
      if (runtimeSeen.has(release.version)) continue;
      runtimeSeen.add(release.version);
      runtimeHistory.push(release);
    }
  }
  console.log(`Preserving ${launcherHistory.length} previous launcher release(s) and ${runtimeHistory.length} previous COT runtime release(s) in the update catalog.`);
  return { launcherHistory, runtimeHistory };
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
package = "portable-zip"

[release.compatibility]`;
}

function runtimeCatalogBlock(release) {
  const compatibility = [
    release.minLauncher ? `min_launcher = ${JSON.stringify(release.minLauncher)}` : "",
    release.maxLauncher ? `max_launcher = ${JSON.stringify(release.maxLauncher)}` : "",
  ].filter(Boolean).join("\n");
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

[release.compatibility]${compatibility ? `\n${compatibility}` : ""}`;
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
const releaseChannel = validateReleaseChannel(
  quotedValueAllowEmpty(suiteManifest, "release_channel", "suite.toml"),
);
const configuredCatalogUrl = validatePublicHttpsUrl(
  quotedValueAllowEmpty(suiteManifest, "update_catalog", "suite.toml"),
  "suite.toml update_catalog",
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
const launcherNoticeFiles = (await readdir(licenseRoot, { withFileTypes: true }))
  .filter((entry) => entry.isFile())
  .map((entry) => entry.name)
  .sort((left, right) => left.localeCompare(right, "en"));
const cotRuntimeFiles = [
  [resolve(projectRoot, "runtime/build-smoke/cot-runtime.exe"), "cot-runtime.exe"],
  [resolve(launcherRoot, "bundle/lib/dxcompiler.dll"), "dxcompiler.dll"],
  [resolve(launcherRoot, "bundle/lib/dxil.dll"), "dxil.dll"],
  [resolve(launcherRoot, "bundle/lib/mojorecomp-ffmpeg.dll"), "mojorecomp-ffmpeg.dll"],
  [resolve(launcherRoot, "bundle/lib/mojorecomp-lzx.dll"), "mojorecomp-lzx.dll"],
  [resolve(launcherRoot, "bundle/tools/extract-xiso.exe"), "extract-xiso.exe"],
];
await requireFile(launcherExe, "Validated production launcher");
await requireFile(projectLicense, "MojoRecomp ISC license");
assertCanonicalProjectLicense(await readFile(projectLicense, "utf8"));
for (const [source, name] of cotRuntimeFiles) {
  await requireFile(source, "COT runtime component file " + name);
}

assertSafeReleaseRoot();
await rm(releaseRoot, { recursive: true, force: true });
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
await rm(lgplSourcesRoot, { recursive: true, force: true });

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
for (const name of ["LICENSE", "THIRD_PARTY_NOTICES.txt"]) {
  if (!runtimeArchiveListing.some((entry) => entry === name || entry.endsWith("/" + name))) {
    fail("COT runtime component archive is missing " + name);
  }
}
if (runtimeArchiveListing.some((entry) => /(^|\/)licenses\//i.test(entry))) {
  fail("COT runtime component archive must not contain a loose licenses directory");
}
const runtimeArchiveHash = await sha256(runtimeArchivePath);
const runtimeArchiveSize = (await stat(runtimeArchivePath)).size;
await rm(runtimeComponentRoot, { recursive: true, force: true });

const packageName = `MojoRecomp-Launcher-${launcherVersion}-windows-x64-portable`;
const packageRoot = resolve(releaseRoot, packageName);
const packagedExe = resolve(packageRoot, "mojorecomp-launcher.exe");
await mkdir(packageRoot, { recursive: true });
await copyFile(launcherExe, packagedExe);
await copyFile(projectLicense, resolve(packageRoot, "LICENSE"));
await writeConsolidatedNotices(
  resolve(packageRoot, "THIRD_PARTY_NOTICES.txt"),
  "MojoRecomp Launcher - Third-Party Notices",
  `This file consolidates the third-party license and notice texts distributed with
MojoRecomp Launcher. Original MojoRecomp code is covered by the ISC license in
LICENSE. The launcher also exposes Licenses & Notices under Support & FAQ. Exact
corresponding source for the replaceable FFmpeg and libmspack LZX libraries is
published in the MojoRecomp LGPL Sources archive beside this release.`,
  launcherNoticeFiles,
);

const portableReadme = `MojoRecomp Launcher ${launcherVersion} - Windows x64 Portable
================================================================

Requirements
------------
- 64-bit Windows 10 or Windows 11
- Microsoft Edge WebView2 Runtime
- A Vulkan 1.3-capable GPU with an up-to-date driver
- Your own legally obtained Xbox 360 Crash of the Titans ISO

Run mojorecomp-launcher.exe and follow the setup instructions. MojoRecomp does
not include or download the original game. The launcher extracts and validates
the required data from the ISO supplied by the user.

Updates
-------
This portable release updates manually. Download the newer MojoRecomp Launcher
package and replace the old executable. Installed game data, saves, settings,
and caches are stored separately and are not removed when the launcher is
replaced.

This build is not code-signed. Windows may display a SmartScreen warning for a
new or uncommon download. Verify the archive against the published SHA-256 hash
before running it.

WebView2
--------
If the launcher does not open, install or repair Microsoft Edge WebView2
Evergreen Runtime from:
https://developer.microsoft.com/microsoft-edge/webview2/

Legal
-----
MojoRecomp is an unofficial fan project and is not affiliated with or endorsed
by the game's rights holders. Original MojoRecomp code is licensed under ISC;
see LICENSE. Third-party license material is consolidated in THIRD_PARTY_NOTICES.txt.
The project publishes its official builds free of charge; the ISC license itself
permits distribution of covered code with or without fee. Game files, saves,
logs, caches, artwork rights, trademarks, and other excluded material are not
granted by the ISC license.

LGPL libraries
--------------
The runtime dynamically loads replaceable FFmpeg and libmspack LZX libraries.
Their exact corresponding source is published in the MojoRecomp LGPL Sources archive
beside this package. See
THIRD_PARTY_NOTICES.txt for the applicable license texts and the local
override directory for compatible modified library builds.
`;
await writeFile(resolve(packageRoot, "README.txt"), portableReadme, "utf8");

const forbiddenEntry = /(^|[\\/])(games?|userdata|saves?|logs?|cache|support|\.private|default\.xex|default\.rcf)([\\/]|$)/i;
const forbiddenExtension = /\.(iso|xex|xexp|rcf|dmp|log)$/i;
for (const entry of runtimeArchiveListing) {
  if (forbiddenEntry.test(entry) || forbiddenExtension.test(entry)) {
    fail("Forbidden game, user, or diagnostic content in runtime component: " + entry);
  }
}
for (const file of await collectFiles(packageRoot)) {
  const rel = relative(packageRoot, file);
  if (forbiddenEntry.test(rel) || forbiddenExtension.test(rel)) {
    fail(`Forbidden game, user, or diagnostic content in release package: ${rel}`);
  }
}

const archiveName = `${packageName}.zip`;
const archivePath = resolve(releaseRoot, archiveName);
runTar(["-a", "-c", "-f", archivePath, packageName], releaseRoot);

const archiveListing = runTar(["-t", "-f", archivePath], releaseRoot)
  .split(/\r?\n/)
  .filter(Boolean);
if (!archiveListing.some((entry) => entry.endsWith("/mojorecomp-launcher.exe"))) {
  fail("Portable archive does not contain mojorecomp-launcher.exe");
}
if (!archiveListing.some((entry) => entry.endsWith("/LICENSE"))) {
  fail("Portable archive does not contain the MojoRecomp ISC LICENSE");
}
if (!archiveListing.some((entry) => entry.endsWith("/THIRD_PARTY_NOTICES.txt"))) {
  fail("Portable archive does not contain consolidated third-party notices");
}
if (archiveListing.some((entry) => /(^|\/)licenses\//i.test(entry))) {
  fail("Portable archive must not contain a loose licenses directory");
}
if (archiveListing.some((entry) => entry.endsWith("/release-manifest.json"))) {
  fail("Portable archive contains obsolete duplicate release-manifest.json metadata");
}
for (const entry of archiveListing) {
  if (forbiddenEntry.test(entry) || forbiddenExtension.test(entry)) {
    fail(`Forbidden entry found in completed archive: ${entry}`);
  }
}

const archiveHash = await sha256(archivePath);
const checksumEntries = [
  { file: archiveName, sha256: archiveHash },
  { file: runtimeArchiveName, sha256: runtimeArchiveHash },
  { file: lgplSourcesArchiveName, sha256: lgplSourcesArchiveHash },
];
if (updateBaseUrl) {
  const catalogName = "update-catalog.toml";
  const catalogPath = resolve(releaseRoot, catalogName);
  const { launcherHistory, runtimeHistory } = await previousReleaseHistory(
    configuredCatalogUrl,
    launcherVersion,
    cotRuntimeVersion,
  );
  const launcherReleases = [
    {
      version: launcherVersion,
      url: `${updateBaseUrl}/${archiveName}`,
      size: (await stat(archivePath)).size,
      sha256: archiveHash,
      published: releaseDate,
      notesUrl: updateNotesUrl,
    },
    ...launcherHistory,
  ];
  const runtimeReleases = [
    {
      version: cotRuntimeVersion,
      url: `${updateBaseUrl}/${runtimeArchiveName}`,
      size: runtimeArchiveSize,
      sha256: runtimeArchiveHash,
      published: releaseDate,
      notesUrl: updateNotesUrl,
      unpackedSize: runtimeUnpackedSize,
      requiredFiles: cotRuntimeFiles.map(([, name]) => name),
      minLauncher: launcherVersion,
      maxLauncher: null,
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
}
await writeFile(
  resolve(releaseRoot, "SHA256SUMS.txt"),
  `${checksumEntries.map((entry) => `${entry.sha256}  ${entry.file}`).join("\n")}\n`,
  "utf8",
);

await rm(packageRoot, { recursive: true, force: true });

console.log(`Release archive: ${archivePath}`);
console.log(`SHA-256: ${archiveHash}`);
console.log("Platform: Windows x64 (x86 and ARM64 are not supported)");
