import { createHash, createPublicKey, verify } from "node:crypto";
import { mkdtemp, readFile, readdir, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, relative, resolve, sep } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const releaseRoot = resolve(projectRoot, ".release");
const manifestName = "mojorecomp-package.toml";
const signatureName = "mojorecomp-package.sig";
const launcherSignatureMagic = Buffer.from("MOJORECOMP-LAUNCHER-SIG-V1", "ascii");
const launcherSignatureDomain = Buffer.from("MojoRecomp launcher executable signature v1\0", "utf8");

function fail(message) {
  throw new Error(message);
}

function quotedValue(text, key) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"));
  if (!match) fail(`Missing ${key} in version.toml`);
  return match[1];
}

async function sha256(path) {
  return createHash("sha256").update(await readFile(path)).digest("hex");
}

async function collectRegularFiles(root, current = root, output = []) {
  for (const entry of await readdir(current, { withFileTypes: true })) {
    const path = join(current, entry.name);
    if (entry.isSymbolicLink()) fail(`Signed release contains a symbolic link: ${path}`);
    if (entry.isDirectory()) await collectRegularFiles(root, path, output);
    else if (entry.isFile()) output.push(relative(root, path).split(sep).join("/"));
    else fail(`Signed release contains an unsupported filesystem entry: ${path}`);
  }
  return output;
}

function extractArchive(archive, destination) {
  const result = spawnSync("tar.exe", ["-xf", archive, "-C", destination], {
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error) fail(`Could not start tar.exe: ${result.error.message}`);
  if (result.status !== 0) {
    fail(`Could not extract ${archive}: ${(result.stderr || result.stdout).trim()}`);
  }
}

function parseSignedFiles(manifest) {
  const files = [];
  const pattern = /\[\[files\]\]\r?\npath = ("(?:\\.|[^"])*")\r?\nsize = (\d+)\r?\nsha256 = "([0-9a-f]{64})"/g;
  for (const match of manifest.matchAll(pattern)) {
    files.push({ path: JSON.parse(match[1]), size: Number(match[2]), sha256: match[3] });
  }
  if (files.length === 0) fail("Signed package manifest contains no payload files");
  return files;
}

async function verifyArchive(archive, expectedRoot, publicKey) {
  const temporary = await mkdtemp(join(tmpdir(), "mojorecomp-signed-release-"));
  try {
    extractArchive(archive, temporary);
    const roots = await readdir(temporary, { withFileTypes: true });
    if (roots.length !== 1 || !roots[0].isDirectory() || roots[0].name !== expectedRoot) {
      fail(`${archive} does not contain the exact expected package root ${expectedRoot}`);
    }
    const root = join(temporary, expectedRoot);
    const manifestBytes = await readFile(join(root, manifestName));
    const manifest = manifestBytes.toString("utf8");
    if (!/^schema_version\s*=\s*2\s*$/m.test(manifest)) {
      fail(`${archive} does not use signed offline package schema 2`);
    }
    const signatureText = (await readFile(join(root, signatureName), "utf8")).trim();
    if (!/^[0-9a-f]{128}$/.test(signatureText)) {
      fail(`${archive} contains a malformed Ed25519 signature`);
    }
    if (!verify(null, manifestBytes, publicKey, Buffer.from(signatureText, "hex"))) {
      fail(`${archive} has an invalid Ed25519 signature`);
    }

    const declared = parseSignedFiles(manifest);
    const actualPayload = (await collectRegularFiles(root))
      .filter((name) => name !== manifestName && name !== signatureName)
      .sort();
    const declaredPayload = declared.map((file) => file.path).sort();
    if (JSON.stringify(actualPayload) !== JSON.stringify(declaredPayload)) {
      fail(`${archive} payload does not exactly match its signed manifest`);
    }
    for (const file of declared) {
      if (
        file.path === "."
        || file.path.startsWith("/")
        || file.path.startsWith("../")
        || file.path.includes("\\")
        || file.path.split("/").some((part) => !part || part === "." || part === "..")
      ) {
        fail(`${archive} contains an unsafe signed payload path: ${file.path}`);
      }
      const path = join(root, file.path);
      const info = await stat(path);
      if (info.size !== file.size || (await sha256(path)) !== file.sha256) {
        fail(`${archive} failed signed payload verification: ${file.path}`);
      }
    }
    console.log(`OK: signed package ${expectedRoot} (${declared.length} payload files)`);
  } finally {
    await rm(temporary, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}

function parseLocalizationPackLanguages(manifest) {
  const sections = manifest.split(/(?=^\[\[language\]\]\s*$)/m).slice(1);
  if (sections.length === 0) fail("Localization Pack manifest contains no languages");
  return sections.map((section) => {
    const field = (key) => section.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"))?.[1] ?? null;
    const integer = (key) => Number(section.match(new RegExp(`^${key}\\s*=\\s*([0-9]+)\\s*$`, "m"))?.[1] ?? NaN);
    const language = {
      id: field("id"),
      locale: field("locale"),
      displayName: field("display_name"),
      xboxLanguage: integer("xbox_language"),
      version: field("version"),
      file: field("file"),
      size: integer("size"),
      sha256: field("sha256"),
    };
    if (
      !language.id
      || !language.locale
      || !language.displayName
      || !Number.isSafeInteger(language.xboxLanguage)
      || !language.version
      || !language.file
      || !Number.isSafeInteger(language.size)
      || !/^[0-9a-f]{64}$/.test(language.sha256 ?? "")
    ) {
      fail("Localization Pack manifest contains invalid language metadata");
    }
    return language;
  });
}

async function verifyLocalizationPackArchive(archive, expectedRoot, expectedVersion, publicKey) {
  const temporary = await mkdtemp(join(tmpdir(), "mojorecomp-localization-pack-"));
  try {
    extractArchive(archive, temporary);
    const roots = await readdir(temporary, { withFileTypes: true });
    if (roots.length !== 1 || !roots[0].isDirectory() || roots[0].name !== expectedRoot) {
      fail(`${archive} does not contain the exact expected Localization Pack root ${expectedRoot}`);
    }
    const root = join(temporary, expectedRoot);
    const manifestPath = join(root, "localization-pack.toml");
    const signaturePath = join(root, "localization-pack.sig");
    const manifestBytes = await readFile(manifestPath);
    const manifest = manifestBytes.toString("utf8");
    if (!/^schema_version\s*=\s*1\s*$/m.test(manifest)) {
      fail(`${archive} does not use Localization Pack schema 1`);
    }
    if (!new RegExp(`^version\\s*=\\s*"${expectedVersion.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}"\\s*$`, "m").test(manifest)) {
      fail(`${archive} Localization Pack version does not match ${expectedVersion}`);
    }
    const signatureText = (await readFile(signaturePath, "utf8")).trim();
    if (!/^[0-9a-f]{128}$/.test(signatureText)
        || !verify(null, manifestBytes, publicKey, Buffer.from(signatureText, "hex"))) {
      fail(`${archive} has an invalid Localization Pack signature`);
    }
    const languages = parseLocalizationPackLanguages(manifest);
    const actual = (await collectRegularFiles(root))
      .filter((name) => name !== "localization-pack.toml" && name !== "localization-pack.sig")
      .sort();
    const declared = languages.map((language) => language.file).sort();
    if (JSON.stringify(actual) !== JSON.stringify(declared)) {
      fail(`${archive} contains Localization Pack payloads not declared by its manifest`);
    }
    for (const language of languages) {
      const nested = join(root, ...language.file.split("/"));
      const info = await stat(nested);
      if (info.size !== language.size || (await sha256(nested)) !== language.sha256) {
        fail(`${archive} failed language payload verification: ${language.file}`);
      }
      await verifyArchive(
        nested,
        `MojoRecomp-COT-Language-${language.locale}-${language.version}`,
        publicKey,
      );
    }
    console.log(`OK: signed Localization Pack ${expectedVersion} (${languages.length} language(s))`);
    return languages;
  } finally {
    await rm(temporary, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}

function parseLocalizationCatalog(text) {
  if (!/^schema_version\s*=\s*2\s*$/m.test(text)) {
    fail("localization-catalog.toml does not use schema 2");
  }
  const header = text.split(/(?=^\[\[language\]\]\s*$)/m)[0];
  const field = (source, key) => source.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"))?.[1] ?? null;
  const integer = (source, key) => Number(source.match(new RegExp(`^${key}\\s*=\\s*([0-9]+)\\s*$`, "m"))?.[1] ?? NaN);
  const catalog = {
    gameId: field(header, "game_id"),
    packVersion: field(header, "pack_version"),
    url: field(header, "url"),
    size: integer(header, "size"),
    sha256: field(header, "sha256"),
    published: field(header, "published"),
    notesUrl: field(header, "notes_url"),
    minLauncher: field(header, "min_launcher"),
    languages: [],
  };
  if (!catalog.gameId || !catalog.packVersion || !catalog.url || !Number.isSafeInteger(catalog.size)
      || catalog.size <= 0 || !/^[0-9a-f]{64}$/.test(catalog.sha256 ?? "")
      || !catalog.published || !catalog.notesUrl || !catalog.minLauncher) {
    fail("localization-catalog.toml contains invalid Localization Pack metadata");
  }
  const sections = text.split(/(?=^\[\[language\]\]\s*$)/m).slice(1);
  catalog.languages = sections.map((section) => {
    const requiredFilesRaw = section.match(/^required_files\s*=\s*\[([^\]]*)\]\s*$/m)?.[1] ?? null;
    const requiredFiles = requiredFilesRaw === null
      ? null
      : [...requiredFilesRaw.matchAll(/"([^"]+)"/g)].map((match) => match[1]);
    const language = {
      id: field(section, "id"),
      gameId: field(section, "game_id"),
      locale: field(section, "locale"),
      displayName: field(section, "display_name"),
      xboxLanguage: integer(section, "xbox_language"),
      version: field(section, "version"),
      componentSize: integer(section, "component_size"),
      componentSha256: field(section, "component_sha256"),
      unpackedSize: integer(section, "unpacked_size"),
      requiredFiles,
    };
    if (
      !language.id
      || !language.gameId
      || !language.locale
      || !language.displayName
      || !Number.isSafeInteger(language.xboxLanguage)
      || !language.version
      || !Number.isSafeInteger(language.componentSize)
      || language.componentSize <= 0
      || !/^[0-9a-f]{64}$/.test(language.componentSha256 ?? "")
      || !Number.isSafeInteger(language.unpackedSize)
      || language.unpackedSize <= 0
      || !Array.isArray(language.requiredFiles)
      || language.requiredFiles.length === 0
    ) {
      fail("localization-catalog.toml contains invalid language metadata");
    }
    return language;
  });
  if (catalog.languages.length === 0) fail("localization-catalog.toml contains no languages");
  return catalog;
}

async function verifyLauncherExecutable(path, expectedVersion, publicKey) {
  const bytes = await readFile(path);
  if (bytes.length < launcherSignatureMagic.length + 2 + 64 + 1 || bytes[0] !== 0x4d || bytes[1] !== 0x5a) {
    fail(`${path} is not a signed Windows launcher executable`);
  }
  const magicStart = bytes.length - launcherSignatureMagic.length;
  if (!bytes.subarray(magicStart).equals(launcherSignatureMagic)) {
    fail(`${path} does not contain the MojoRecomp launcher signature trailer`);
  }
  const versionLengthPosition = magicStart - 2;
  const versionLength = bytes.readUInt16LE(versionLengthPosition);
  const signatureStart = versionLengthPosition - 64;
  const versionStart = signatureStart - versionLength;
  if (versionLength === 0 || versionStart < 2) {
    fail(`${path} contains a truncated launcher signature trailer`);
  }
  const versionBytes = bytes.subarray(versionStart, signatureStart);
  const version = versionBytes.toString("utf8");
  if (version !== expectedVersion || Buffer.byteLength(version, "utf8") !== versionLength) {
    fail(`${path} signed version ${version} does not match ${expectedVersion}`);
  }
  const payload = bytes.subarray(0, versionStart);
  if (payload[0] !== 0x4d || payload[1] !== 0x5a) {
    fail(`${path} payload is not a Windows executable`);
  }
  const payloadHash = createHash("sha256").update(payload).digest();
  const message = Buffer.concat([
    launcherSignatureDomain,
    versionBytes,
    Buffer.from([0]),
    payloadHash,
  ]);
  const signature = bytes.subarray(signatureStart, versionLengthPosition);
  if (!verify(null, message, publicKey, signature)) {
    fail(`${path} has an invalid Ed25519 launcher signature`);
  }
  console.log(`OK: signed launcher executable ${expectedVersion}`);
}

const versions = await readFile(resolve(projectRoot, "version.toml"), "utf8");
const launcherVersion = quotedValue(versions, "launcher");
const runtimeVersion = quotedValue(versions, "cot_runtime");
const localizationPackVersion = quotedValue(versions, "cot_localization_pack");
const publicKeyHex = (
  await readFile(resolve(launcherRoot, "release-signing-public-key.hex"), "utf8")
).trim();
if (!/^[0-9a-f]{64}$/.test(publicKeyHex)) fail("Release public key is malformed");
const publicKey = createPublicKey({
  key: Buffer.concat([
    Buffer.from("302a300506032b6570032100", "hex"),
    Buffer.from(publicKeyHex, "hex"),
  ]),
  format: "der",
  type: "spki",
});

await verifyLauncherExecutable(
  resolve(releaseRoot, `MojoRecomp-Launcher-${launcherVersion}-windows-x64.exe`),
  launcherVersion,
  publicKey,
);
await verifyArchive(
  resolve(releaseRoot, `MojoRecomp-COT-Runtime-${runtimeVersion}-windows-x64.zip`),
  `MojoRecomp-COT-Runtime-${runtimeVersion}-windows-x64`,
  publicKey,
);
const localizationPackName = `MojoRecomp-COT-Localization-Pack-${localizationPackVersion}`;
const localizationPackPath = resolve(releaseRoot, `${localizationPackName}.zip`);
let localizationPackLanguages = [];
if ((await stat(localizationPackPath).catch(() => null))?.isFile()) {
  localizationPackLanguages = await verifyLocalizationPackArchive(
    localizationPackPath,
    localizationPackName,
    localizationPackVersion,
    publicKey,
  );
  const localizationCatalog = parseLocalizationCatalog(
    await readFile(resolve(releaseRoot, "localization-catalog.toml"), "utf8"),
  );
  const localizationPackInfo = await stat(localizationPackPath);
  if (
    localizationCatalog.gameId !== "cot"
    || localizationCatalog.packVersion !== localizationPackVersion
    || localizationCatalog.size !== localizationPackInfo.size
    || localizationCatalog.sha256 !== await sha256(localizationPackPath)
    || !localizationCatalog.url.endsWith(`/${localizationPackName}.zip`)
  ) {
    fail("localization-catalog.toml does not match the published Localization Pack artifact");
  }
  if (localizationCatalog.languages.length !== localizationPackLanguages.length) {
    fail("localization-catalog.toml does not describe every Localization Pack language");
  }
  for (const language of localizationPackLanguages) {
    const metadata = localizationCatalog.languages.find(
      (entry) => entry.id === language.id && entry.version === language.version,
    );
    if (
      !metadata
      || metadata.gameId !== "cot"
      || metadata.locale !== language.locale
      || metadata.displayName !== language.displayName
      || metadata.xboxLanguage !== language.xboxLanguage
      || metadata.componentSize !== language.size
      || metadata.componentSha256 !== language.sha256
    ) {
      fail(`localization-catalog.toml does not match ${language.id} ${language.version}`);
    }
  }
}

const updateCatalogText = await readFile(resolve(releaseRoot, "update-catalog.toml"), "utf8");
if (/^\s*(display_name|xbox_language)\s*=/m.test(updateCatalogText)) {
  fail("update-catalog.toml contains localization metadata that belongs in localization-catalog.toml");
}
if (updateCatalogText.includes("[release.localization_pack]")) {
  fail("update-catalog.toml contains Localization Pack metadata that belongs in localization-catalog.toml");
}
if (/^\s*kind\s*=\s*"language"\s*$/m.test(updateCatalogText)) {
  fail("update-catalog.toml contains language releases; languages must be distributed through the Localization Pack catalog");
}

const recorded = new Map();
for (const line of (await readFile(resolve(releaseRoot, "SHA256SUMS.txt"), "utf8")).split(/\r?\n/)) {
  const match = line.match(/^([0-9a-f]{64})  (.+)$/);
  if (match) recorded.set(match[2], match[1]);
}
for (const [name, expected] of recorded) {
  const actual = await sha256(resolve(releaseRoot, name));
  if (actual !== expected) fail(`SHA256SUMS.txt mismatch for ${name}`);
}
console.log(`OK: ${recorded.size} release checksums match SHA256SUMS.txt`);
