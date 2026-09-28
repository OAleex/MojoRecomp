import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const userProfile = process.env.USERPROFILE ? resolve(process.env.USERPROFILE) : null;
const unitSeparator = String.fromCharCode(0x1f);

const rustFlags = [];
for (const [source, replacement] of [
  [projectRoot, "C:\\src\\MojoRecomp"],
  [userProfile, "C:\\build-user"],
]) {
  if (!source) continue;
  rustFlags.push("--remap-path-prefix", `${source}=${replacement}`);
}

const pathMapFlags = userProfile ? ` /pathmap:${userProfile}=C:\\build-user` : "";
const env = {
  ...process.env,
  CARGO_ENCODED_RUSTFLAGS: rustFlags.join(unitSeparator),
  CFLAGS: `${process.env.CFLAGS ?? ""}${pathMapFlags}`.trim(),
  CXXFLAGS: `${process.env.CXXFLAGS ?? ""}${pathMapFlags}`.trim(),
};

const tauri = resolve(launcherRoot, "node_modules/@tauri-apps/cli/tauri.js");
const result = spawnSync(process.execPath, [tauri, "build", "--no-bundle"], {
  cwd: launcherRoot,
  env,
  stdio: "inherit",
  windowsHide: true,
});

if (result.error) {
  throw result.error;
}
process.exit(result.status ?? 1);
