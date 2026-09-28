import { cp, mkdir, readFile, readdir, rm, rmdir, stat } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { assertCanonicalProjectLicense } from "./project-license.mjs";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const projectLicense = resolve(projectRoot, "LICENSE");

function quotedValue(text, key) {
  const match = text.match(new RegExp(`^${key}\\s*=\\s*"([^"]+)"`, "m"));
  if (!match) throw new Error(`Missing ${key}`);
  return match[1];
}

async function requireFile(path, label) {
  const info = await stat(path).catch(() => null);
  if (!info?.isFile()) throw new Error(`${label} is missing: ${path}`);
}

async function removeEmptyDirectory(path) {
  try {
    const entries = await readdir(path);
    if (entries.length === 0) await rmdir(path);
  } catch (error) {
    if (error?.code !== "ENOENT") throw error;
  }
}

const versionToml = await readFile(resolve(projectRoot, "version.toml"), "utf8");
quotedValue(versionToml, "launcher");
// Developer builds are produced under MojoRecomp, but the persistent runnable copy
// lives in ../game/production. Updating production replaces only application-owned
// files and preserves games/ and userdata/ only when upgrading an older layout.
// CI/clean-source validation may redirect this to an isolated empty directory so
// it can exercise the exact packaging path without touching a real installation.
const portableRoot = process.env.MOJORECOMP_PRODUCTION_ROOT
  ? resolve(process.env.MOJORECOMP_PRODUCTION_ROOT)
  : resolve(projectRoot, "../game/production");
if (portableRoot === projectRoot || portableRoot === launcherRoot) {
  throw new Error(`Refusing unsafe production root: ${portableRoot}`);
}

const launcherExe = resolve(launcherRoot, "src-tauri/target/release/mojorecomp-launcher.exe");
await requireFile(projectLicense, "MojoRecomp ISC license");
assertCanonicalProjectLicense(await readFile(projectLicense, "utf8"));
await requireFile(launcherExe, "Launcher executable");

await mkdir(portableRoot, { recursive: true });
// Older builds kept game files and user data beside the executable. Preserve
// those directories during an upgrade so the launcher can migrate them safely;
// clean installations no longer create either directory.
for (const entry of await readdir(portableRoot, { withFileTypes: true })) {
  if (entry.name === "games" || entry.name === "userdata") continue;
  await rm(resolve(portableRoot, entry.name), { recursive: true, force: true });
}
await removeEmptyDirectory(resolve(portableRoot, "games/.staging"));
await removeEmptyDirectory(resolve(portableRoot, "games/.backup"));
await cp(launcherExe, resolve(portableRoot, "mojorecomp-launcher.exe"));

console.log(`Production launcher prepared: ${portableRoot}`);
