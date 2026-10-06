import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const distRoot = resolve(launcherRoot, "dist");
const cachePath = resolve(launcherRoot, ".vite/release-build-cache.json");

async function collectFiles(path, output = []) {
  const info = await stat(path).catch(() => null);
  if (!info) return output;
  if (info.isFile()) {
    output.push(path);
    return output;
  }
  const entries = await readdir(path, { withFileTypes: true });
  entries.sort((left, right) => left.name.localeCompare(right.name, "en"));
  for (const entry of entries) {
    if (entry.isSymbolicLink()) continue;
    await collectFiles(resolve(path, entry.name), output);
  }
  return output;
}

async function contentHash(files, root) {
  const digest = createHash("sha256");
  for (const path of files.sort((left, right) => left.localeCompare(right, "en"))) {
    digest.update(relative(root, path).replaceAll("\\", "/"));
    digest.update("\0");
    digest.update(await readFile(path));
    digest.update("\0");
  }
  return digest.digest("hex");
}

async function frontendInputHash() {
  const files = [];
  for (const path of [
    resolve(launcherRoot, "src"),
    resolve(launcherRoot, "public"),
    resolve(launcherRoot, "index.html"),
    resolve(launcherRoot, "package.json"),
    resolve(launcherRoot, "package-lock.json"),
    resolve(launcherRoot, "tsconfig.json"),
    resolve(launcherRoot, "vite.config.js"),
    resolve(launcherRoot, "vite.config.mjs"),
    resolve(launcherRoot, "vite.config.ts"),
    resolve(launcherRoot, "src-tauri/icons/icon.ico"),
  ]) {
    await collectFiles(path, files);
  }
  return contentHash(files, launcherRoot);
}

async function distHash() {
  const files = await collectFiles(distRoot);
  if (files.length === 0) return null;
  return contentHash(files, distRoot);
}

const inputHash = await frontendInputHash();
const cache = JSON.parse(await readFile(cachePath, "utf8").catch(() => "null"));
const currentDistHash = await distHash();
if (
  cache?.schema_version === 1 &&
  cache.input_sha256 === inputHash &&
  cache.output_sha256 === currentDistHash &&
  currentDistHash
) {
  console.log(`Frontend unchanged: reusing dist (SHA-256 ${currentDistHash})`);
  process.exit(0);
}

const vite = resolve(launcherRoot, "node_modules/vite/bin/vite.js");
const result = spawnSync(process.execPath, [vite, "build"], {
  cwd: launcherRoot,
  stdio: "inherit",
  windowsHide: true,
});
if (result.error) throw result.error;
if (result.status !== 0) process.exit(result.status ?? 1);

const outputHash = await distHash();
if (!outputHash) throw new Error("Vite completed without producing a frontend dist");
await mkdir(dirname(cachePath), { recursive: true });
await writeFile(
  cachePath,
  `${JSON.stringify({ schema_version: 1, input_sha256: inputHash, output_sha256: outputHash }, null, 2)}\n`,
  "utf8",
);
console.log(`Frontend release cache updated: SHA-256 ${outputHash}`);
