import { createHash } from "node:crypto";
import { readdir, readFile, stat } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { assertCanonicalProjectLicense, PROJECT_LICENSE_SPDX } from "./project-license.mjs";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");

function fail(message) {
  throw new Error(message);
}

function quotedValue(text, key) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"));
  if (!match) fail(`Missing ${key}`);
  return match[1];
}

async function sha256(path) {
  const bytes = await readFile(path);
  return createHash("sha256").update(bytes).digest("hex");
}

async function sha256NormalizedText(path) {
  const text = await readFile(path, "utf8");
  const normalized = text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").trimEnd() + "\n";
  return createHash("sha256").update(normalized, "utf8").digest("hex");
}

async function requireFile(path, label) {
  const info = await stat(path).catch(() => null);
  if (!info?.isFile()) fail(`${label} is missing: ${path}`);
}

async function requireDirectory(path, label) {
  const info = await stat(path).catch(() => null);
  if (!info?.isDirectory()) fail(`${label} is missing: ${path}`);
}

async function rejectLocalBuildPaths(path, label) {
  const bytes = await readFile(path);
  const forbidden = [projectRoot, homedir()].filter(Boolean);
  for (const localPath of forbidden) {
    for (const spelling of [localPath, localPath.replaceAll("\\", "/")]) {
      if (
        bytes.includes(Buffer.from(spelling, "utf8")) ||
        bytes.includes(Buffer.from(spelling, "utf16le"))
      ) {
        fail(`${label} contains a local build path: ${localPath}`);
      }
    }
  }
}

async function collectFiles(root, output = []) {
  const info = await stat(root).catch(() => null);
  if (!info) return output;
  if (info.isFile()) {
    output.push(root);
    return output;
  }
  for (const entry of await readdir(root, { withFileTypes: true })) {
    if (entry.isSymbolicLink()) fail(`Release staging contains a symbolic link: ${resolve(root, entry.name)}`);
    await collectFiles(resolve(root, entry.name), output);
  }
  return output;
}

const versionToml = await readFile(resolve(projectRoot, "version.toml"), "utf8");
const suiteVersion = quotedValue(versionToml, "suite");
const launcherVersion = quotedValue(versionToml, "launcher");
const cotVersion = quotedValue(versionToml, "cot_runtime");
const momVersion = quotedValue(versionToml, "mom_runtime");

const packageJson = JSON.parse(await readFile(resolve(launcherRoot, "package.json"), "utf8"));
const tauriConfig = JSON.parse(await readFile(resolve(launcherRoot, "src-tauri/tauri.conf.json"), "utf8"));
const cargoToml = await readFile(resolve(launcherRoot, "src-tauri/Cargo.toml"), "utf8");
const suiteToml = await readFile(resolve(launcherRoot, "resources/suite.toml"), "utf8");
const cotToml = await readFile(resolve(launcherRoot, "resources/games/cot.toml"), "utf8");
const momToml = await readFile(resolve(launcherRoot, "resources/games/mom.toml"), "utf8");
const noticeSummary = resolve(launcherRoot, "resources/THIRD_PARTY_NOTICES.md");
const projectLicense = resolve(projectRoot, "LICENSE");
const artworkAudit = resolve(launcherRoot, "public/art/SOURCES.md");
const overrideSources = resolve(launcherRoot, "resources/license-overrides/SOURCES.md");
const noticeFiles = [
  noticeSummary,
  resolve(launcherRoot, "resources/licenses/Cargo-ThirdPartyNotices.txt"),
  resolve(launcherRoot, "resources/licenses/Npm-ThirdPartyNotices.txt"),
  resolve(launcherRoot, "resources/licenses/DXC-LICENSE.txt"),
  resolve(launcherRoot, "resources/licenses/DXC-ThirdPartyNotices.txt"),
  resolve(launcherRoot, "resources/licenses/Vulkan-Headers-LICENSE.md"),
  resolve(launcherRoot, "resources/licenses/FFmpeg-LGPL-2.1.txt"),
  resolve(launcherRoot, "resources/licenses/extract-xiso-LICENSE.TXT"),
  resolve(launcherRoot, "resources/licenses/SDL3-LICENSE.txt"),
  resolve(launcherRoot, "resources/licenses/NVIDIA-FXAA-LICENSE.txt"),
];
for (const path of noticeFiles) await requireFile(path, "Third-party notice material");
await requireFile(artworkAudit, "Artwork provenance audit");
await requireFile(overrideSources, "License override provenance");
await requireFile(projectLicense, "MojoRecomp ISC license");
assertCanonicalProjectLicense(await readFile(projectLicense, "utf8"));
if (packageJson.private !== true) fail("launcher/package.json must remain private");
if (packageJson.license !== PROJECT_LICENSE_SPDX) {
  fail(`launcher/package.json license must be ${PROJECT_LICENSE_SPDX}`);
}
if (quotedValue(cargoToml, "license") !== PROJECT_LICENSE_SPDX) {
  fail(`launcher/src-tauri/Cargo.toml license must be ${PROJECT_LICENSE_SPDX}`);
}
console.log("OK: canonical MojoRecomp ISC license and launcher package metadata validated.");

const licenseOverrideHashes = new Map([
  ["alloc-stdlib-BSD-3-Clause.txt", "c0c56f26d9c051cac4d200c34c84e7ae9aaa853e01a982a1df08b09931e518ae"],
  ["defmt-Apache-2.0.txt", "8173d5c29b4f956d532781d2b86e4e30f83e6b7878dce18c919451d6ba707c90"],
  ["defmt-MIT.txt", "0d17b75c1867fd568bcbb735f329d0d4253846c4b756a65e4d440c1e4bd59187"],
  ["MPL-2.0.txt", "66a3107d5ad6a058aab753eaac2047ccb2ed0e39465dd0fe5844da3e300d5172"],
  ["rolldown-MIT.txt", "23ecfff35a5a2e80d92142f75228912c3b1abc4b5a8337a821ff4397e2f9f734"],
  ["rust-unic-Apache-2.0.txt", "a60eea817514531668d7e00765731449fe14d059d3249e0bc93b36de45f759f2"],
  ["rust-unic-MIT.txt", "23f18e03dc49df91622fe2a76176497404e46ced8a715d9d2b67a7446571cca3"],
  ["webview2-rs-MIT.txt", "0dcf41516e608bbcb6cdc5229feb7b86fe4a643b85e7df251133c93408fdac73"],
]);
for (const [name, expectedHash] of licenseOverrideHashes) {
  const path = resolve(launcherRoot, "resources/license-overrides", name);
  await requireFile(path, "License override");
  const actualHash = await sha256NormalizedText(path);
  if (actualHash !== expectedHash) fail(`License override hash mismatch for ${name}: ${actualHash}`);
}
console.log(`OK: ${licenseOverrideHashes.size} license override source texts match audited normalized hashes.`);

const auditedArtworkHashes = new Map([
  ["public/art/cot-icon.png", "595efb3ccfc6d66fefbcefd0153f4e26733c8e87ac22cd9b4b12527c79f78bd8"],
  ["public/art/cot-logo.png", "a8d41fa8318aa507b24ac59480b5e56bb8c53cdcd05900d093eaf1446d229b34"],
  ["public/art/mojorecomp.png", "6fc57b409278fde645b82374d8f5732690b130548ff82fde3b9fd1611acec1d5"],
  ["public/art/mom-icon.png", "8e5486df7b69104b540453619ed478528ea7f1d352db9534ee0901112d277760"],
  ["public/art/mom-logo.png", "efd259f842f9d977cf082466bee7586b71d443f404b09b35b497c912ec0e526b"],
  ["src-tauri/icons/icon.ico", "517e97168a856f7a44ab93cae6717b1406db15481e8aec2e24c28fb168a9b8b7"],
]);
for (const [relativePath, expectedHash] of auditedArtworkHashes) {
  const path = resolve(launcherRoot, relativePath);
  await requireFile(path, "Audited launcher artwork");
  const actualHash = await sha256(path);
  if (actualHash !== expectedHash) {
    fail(`Audited launcher artwork hash mismatch for ${relativePath}: ${actualHash}`);
  }
}

const configuredIcons = tauriConfig.bundle?.icon ?? [];
if (configuredIcons.length !== 1 || configuredIcons[0] !== "icons/icon.ico") {
  fail(`Expected audited Tauri icon configuration ["icons/icon.ico"], got: ${JSON.stringify(configuredIcons)}`);
}

const appSource = await readFile(resolve(launcherRoot, "src/App.svelte"), "utf8");
for (const expectedReference of [
  "/art/cot-logo.png",
  "/art/mom-logo.png",
  "/art/cot-icon.png",
  "/art/mom-icon.png",
  "../src-tauri/icons/icon.ico?url",
]) {
  if (!appSource.includes(expectedReference)) {
    fail(`Expected audited artwork reference is missing from App.svelte: ${expectedReference}`);
  }
}
console.log(`OK: ${auditedArtworkHashes.size} project-approved launcher artwork files match audited hashes.`);

const versionChecks = [
  ["package.json", packageJson.version, launcherVersion],
  ["tauri.conf.json", tauriConfig.version, launcherVersion],
  ["Cargo.toml", quotedValue(cargoToml, "version"), launcherVersion],
  ["suite.toml", quotedValue(suiteToml, "version"), suiteVersion],
  ["cot.toml", quotedValue(cotToml, "runtime_version"), cotVersion],
  ["mom.toml", quotedValue(momToml, "runtime_version"), momVersion],
];
for (const [name, actual, expected] of versionChecks) {
  if (actual !== expected) fail(`${name} version mismatch: ${actual} != ${expected}`);
}

if (launcherVersion !== suiteVersion) {
  fail(`Launcher ${launcherVersion} must match suite ${suiteVersion} for this release`);
}

const externalBin = tauriConfig.bundle?.externalBin ?? [];
if (externalBin.length !== 0) {
  fail(`Unexpected externalBin set: ${JSON.stringify(externalBin)}`);
}

const resources = tauriConfig.bundle?.resources ?? {};
if (Object.keys(resources).length !== 0) {
  fail(`Unexpected external resources: ${JSON.stringify(resources)}`);
}

const sourceRuntime = resolve(projectRoot, "runtime/build-smoke/cot-runtime.exe");
const stagedRuntime = resolve(launcherRoot, "src-tauri/binaries/cot-runtime-x86_64-pc-windows-msvc.exe");
const ffmpegSource = resolve(projectRoot, "runtime/build-smoke/mojorecomp-ffmpeg.dll");
const lzxSource = resolve(projectRoot, "runtime/build-smoke/mojorecomp-lzx.dll");
const dxcRoot = resolve(projectRoot, "thirdparty/XenosRecomp-src/thirdparty/dxc-bin/bin/x64");
const stagedLib = resolve(launcherRoot, "bundle/lib");
const extractXisoSource = resolve(projectRoot, "thirdparty/extract-xiso/extract-xiso.exe");
const stagedExtractXiso = resolve(launcherRoot, "bundle/tools/extract-xiso.exe");
for (const [source, staged, label] of [
  [sourceRuntime, stagedRuntime, "COT runtime"],
  [ffmpegSource, resolve(stagedLib, "mojorecomp-ffmpeg.dll"), "FFmpeg shared library"],
  [lzxSource, resolve(stagedLib, "mojorecomp-lzx.dll"), "libmspack LZX shared library"],
  [resolve(dxcRoot, "dxcompiler.dll"), resolve(stagedLib, "dxcompiler.dll"), "DXC compiler"],
  [resolve(dxcRoot, "dxil.dll"), resolve(stagedLib, "dxil.dll"), "DXIL library"],
  [extractXisoSource, stagedExtractXiso, "extract-xiso utility"],
]) {
  await requireFile(source, `${label} source`);
  await requireFile(staged, `${label} staged file`);
  const sourceHash = await sha256(source);
  const stagedHash = await sha256(staged);
  if (sourceHash !== stagedHash) fail(`${label} staging hash mismatch`);
  console.log(`OK: ${label} SHA-256 ${sourceHash}`);
  await rejectLocalBuildPaths(source, label);
  await rejectLocalBuildPaths(staged, `${label} staged file`);
}

const runtimeImage = await readFile(sourceRuntime);
for (const dependency of ["mojorecomp-ffmpeg.dll", "mojorecomp-lzx.dll"]) {
  if (!runtimeImage.includes(Buffer.from(dependency, "ascii"))) {
    fail(`COT runtime is not dynamically linked to ${dependency}`);
  }
}
const runtimeMap = await readFile(
  resolve(projectRoot, "runtime/build-smoke/MojoRecompBootProbe.map"),
  "utf8",
);
if (!runtimeMap.includes("libavcodec:mojorecomp-ffmpeg.dll")) {
  fail("Runtime map does not resolve FFmpeg through the replaceable DLL");
}
if (!runtimeMap.includes("mojorecomp-lzx:mojorecomp-lzx.dll")) {
  fail("Runtime map does not resolve libmspack LZX through the replaceable DLL");
}
if (runtimeMap.includes("XenonUtils:lzxd.c.obj")) {
  fail("Runtime still contains statically linked libmspack LZX object code");
}
console.log("OK: LGPL libraries are dynamically linked and replaceable");

const allowedRoots = [
  resolve(launcherRoot, "resources"),
  resolve(launcherRoot, "bundle"),
  resolve(launcherRoot, "src-tauri/binaries"),
];
const forbiddenNames = /(^|[\\/])(default\.xex|default\.rcf|userdata|save)([\\/]|$)/i;
const forbiddenExtensions = /\.(xex|rcf|iso)$/i;
for (const root of allowedRoots) {
  for (const file of await collectFiles(root)) {
    const rel = relative(launcherRoot, file);
    if (forbiddenNames.test(rel) || forbiddenExtensions.test(rel)) {
      fail(`Game/userdata content found in release staging: ${rel}`);
    }
  }
}

const portableRoot = process.env.MOJORECOMP_PRODUCTION_ROOT
  ? resolve(process.env.MOJORECOMP_PRODUCTION_ROOT)
  : resolve(projectRoot, "../game/production");
if (portableRoot === projectRoot || portableRoot === launcherRoot) {
  fail(`Refusing unsafe production root: ${portableRoot}`);
}
const portableFiles = [
  [resolve(portableRoot, "mojorecomp-launcher.exe"), "Portable launcher"],
];
for (const [path, label] of portableFiles) await requireFile(path, label);
const portableHashChecks = [
  [resolve(launcherRoot, "src-tauri/target/release/mojorecomp-launcher.exe"), resolve(portableRoot, "mojorecomp-launcher.exe"), "Launcher"],
];
for (const [source, portable, label] of portableHashChecks) {
  const [sourceHash, portableHash] = await Promise.all([sha256(source), sha256(portable)]);
  if (sourceHash !== portableHash) fail(`${label} portable hash mismatch`);
  console.log(`OK: portable ${label} SHA-256 ${portableHash}`);
  await rejectLocalBuildPaths(source, `${label} release build`);
  await rejectLocalBuildPaths(portable, `Portable ${label}`);
}
console.log("OK: release binaries do not contain local project or user-profile paths.");

const allowedPortableRootEntries = new Set(["mojorecomp-launcher.exe", "games", "userdata"]);
for (const entry of await readdir(portableRoot, { withFileTypes: true })) {
  if (!allowedPortableRootEntries.has(entry.name)) {
    fail(`Unexpected production root entry: ${entry.name}`);
  }
}

console.log(`OK: production launcher validated at ${portableRoot}`);
console.log("OK: single-executable application payload; legacy games/userdata directories are accepted only for in-app migration.");
