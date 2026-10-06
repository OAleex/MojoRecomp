import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { access, mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const licensesRoot = resolve(launcherRoot, "resources/licenses");
const overridesRoot = resolve(launcherRoot, "resources/license-overrides");
const cargoManifest = resolve(launcherRoot, "src-tauri/Cargo.toml");
const targetTriple = "x86_64-pc-windows-msvc";

const licenseNamePattern = /^(?:license|licence|copying|notice|unlicense)(?:[._-].*)?$/i;

const cargoOverrides = new Map([
  ["alloc-stdlib", ["alloc-stdlib-BSD-3-Clause.txt"]],
  ["defmt-parser", ["defmt-MIT.txt", "defmt-Apache-2.0.txt"]],
  ["selectors", ["MPL-2.0.txt"]],
  ["tauri-plugin", ["../licenses/Tauri-API-MIT.txt", "../licenses/Tauri-API-APACHE-2.0.txt"]],
  ["unic-char-property", ["rust-unic-MIT.txt", "rust-unic-Apache-2.0.txt"]],
  ["unic-char-range", ["rust-unic-MIT.txt", "rust-unic-Apache-2.0.txt"]],
  ["unic-common", ["rust-unic-MIT.txt", "rust-unic-Apache-2.0.txt"]],
  ["unic-ucd-ident", ["rust-unic-MIT.txt", "rust-unic-Apache-2.0.txt"]],
  ["unic-ucd-version", ["rust-unic-MIT.txt", "rust-unic-Apache-2.0.txt"]],
  ["webview2-com", ["webview2-rs-MIT.txt"]],
  ["webview2-com-macros", ["webview2-rs-MIT.txt"]],
  ["webview2-com-sys", ["webview2-rs-MIT.txt"]],
]);

const npmOverrides = new Map([
  ["@rolldown/binding-win32-x64-msvc", ["rolldown-MIT.txt"]],
  ["@tauri-apps/cli-win32-x64-msvc", ["../licenses/Tauri-API-MIT.txt", "../licenses/Tauri-API-APACHE-2.0.txt"]],
]);

function normalizeText(text) {
  return (
    text
      .replace(/\r\n/g, "\n")
      .replace(/\r/g, "\n")
      .replace(/[ \t]+$/gm, "")
      .trimEnd() + "\n"
  );
}

function sha256(text) {
  return createHash("sha256").update(text, "utf8").digest("hex");
}

async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}

async function writeTextIfChanged(path, text) {
  const current = await readFile(path, "utf8").catch(() => null);
  if (current === text) return false;
  await writeFile(path, text, "utf8");
  return true;
}

async function rootLicenseFiles(packageRoot) {
  const entries = await readdir(packageRoot, { withFileTypes: true });
  return entries
    .filter((entry) => entry.isFile() && licenseNamePattern.test(entry.name))
    .map((entry) => resolve(packageRoot, entry.name))
    .sort((a, b) => a.localeCompare(b));
}

async function overrideFiles(names) {
  const paths = [];
  for (const name of names ?? []) {
    const path = resolve(overridesRoot, name);
    if (!(await exists(path))) {
      throw new Error(`License override is missing: ${path}`);
    }
    paths.push(path);
  }
  return paths;
}

function packageSource(pkg) {
  return pkg.repository || pkg.homepage || pkg.source || "not supplied";
}

function genericMitNotice(name, version, authors) {
  const attribution = authors?.length ? authors.join("; ") : "not supplied by package metadata";
  return normalizeText(`Package: ${name} ${version}
License metadata: MIT
Author attribution from package metadata: ${attribution}
Upstream package/repository does not ship a separate LICENSE file in the audited tree.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
`);
}

async function collectTexts(paths) {
  const result = [];
  for (const path of paths) {
    result.push({
      source: path,
      text: normalizeText(await readFile(path, "utf8")),
    });
  }
  return result;
}

function edgeRole(parentRole, kinds) {
  const normalized = kinds?.length ? kinds : [{ kind: null }];
  let best = null;
  for (const { kind } of normalized) {
    if (kind === "dev") continue;
    const candidate = kind === "build" || parentRole === "build" ? "build" : "runtime";
    if (candidate === "runtime") return "runtime";
    best = best ?? candidate;
  }
  return best;
}

function cargoReachable(metadata) {
  const nodes = new Map(metadata.resolve.nodes.map((node) => [node.id, node]));
  const roles = new Map([[metadata.resolve.root, "runtime"]]);
  const queue = [metadata.resolve.root];
  while (queue.length) {
    const id = queue.shift();
    const parentRole = roles.get(id);
    const node = nodes.get(id);
    if (!node) continue;
    for (const dep of node.deps) {
      const role = edgeRole(parentRole, dep.dep_kinds);
      if (!role) continue;
      const previous = roles.get(dep.pkg);
      const next = previous === "runtime" || role === "runtime" ? "runtime" : "build";
      if (previous !== next) {
        roles.set(dep.pkg, next);
        queue.push(dep.pkg);
      }
    }
  }
  roles.delete(metadata.resolve.root);
  return roles;
}

async function cargoPackages() {
  const raw = execFileSync(
    "cargo",
    [
      "metadata",
      "--manifest-path", cargoManifest,
      "--format-version", "1",
      "--locked",
      "--filter-platform", targetTriple,
    ],
    {
      cwd: projectRoot,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "inherit"],
      maxBuffer: 32 * 1024 * 1024,
    },
  );
  const metadata = JSON.parse(raw);
  const roles = cargoReachable(metadata);
  const byId = new Map(metadata.packages.map((pkg) => [pkg.id, pkg]));
  const output = [];

  for (const [id, role] of [...roles.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
    const pkg = byId.get(id);
    if (!pkg) throw new Error(`Cargo metadata is missing package ${id}`);
    const packageRoot = dirname(pkg.manifest_path);
    let files = await rootLicenseFiles(packageRoot);
    if (pkg.license_file) {
      const declared = resolve(packageRoot, pkg.license_file);
      if (await exists(declared)) files.push(declared);
    }
    files = [...new Set(files)];
    if (!files.length) files = await overrideFiles(cargoOverrides.get(pkg.name));
    if (!files.length) {
      throw new Error(`No Cargo license text resolved for ${pkg.name} ${pkg.version} (${pkg.license ?? "no expression"})`);
    }
    output.push({
      ecosystem: "Cargo",
      role,
      name: pkg.name,
      version: pkg.version,
      license: pkg.license ?? "license-file only",
      source: packageSource(pkg),
      authors: pkg.authors ?? [],
      texts: await collectTexts(files),
    });
  }
  return output;
}

async function npmPackages() {
  const lock = JSON.parse(await readFile(resolve(launcherRoot, "package-lock.json"), "utf8"));
  const output = [];
  for (const [lockPath, entry] of Object.entries(lock.packages ?? {}).sort(([a], [b]) => a.localeCompare(b))) {
    if (!lockPath || !lockPath.includes("node_modules/")) continue;
    const packageRoot = resolve(launcherRoot, lockPath);
    if (!(await exists(packageRoot))) continue; // platform-specific optional package not installed here
    const packageJsonPath = resolve(packageRoot, "package.json");
    if (!(await exists(packageJsonPath))) continue;
    const pkg = JSON.parse(await readFile(packageJsonPath, "utf8"));
    const name = pkg.name ?? lockPath.split("node_modules/").at(-1);
    const version = pkg.version ?? entry.version ?? "unknown";
    const license = typeof pkg.license === "string" ? pkg.license : entry.license ?? "not supplied";
    const author = typeof pkg.author === "string" ? pkg.author : pkg.author?.name;
    const authors = author ? [author] : [];
    let files = await rootLicenseFiles(packageRoot);
    if (!files.length) files = await overrideFiles(npmOverrides.get(name));
    let texts;
    if (files.length) {
      texts = await collectTexts(files);
    } else if (license === "MIT" && authors.length) {
      texts = [{ source: "package metadata + MIT terms", text: genericMitNotice(name, version, authors) }];
    } else {
      throw new Error(`No npm license text resolved for ${name} ${version} (${license})`);
    }
    output.push({
      ecosystem: "npm",
      role: entry.dev ? "build" : "runtime",
      name,
      version,
      license,
      source: typeof pkg.repository === "string" ? pkg.repository : pkg.repository?.url || pkg.homepage || "not supplied",
      authors,
      texts,
    });
  }
  return output;
}

function renderNotice(title, packages) {
  const textByHash = new Map();
  for (const pkg of packages) {
    pkg.textIds = pkg.texts.map(({ text }) => {
      const hash = sha256(text);
      if (!textByHash.has(hash)) {
        textByHash.set(hash, { id: null, text });
      }
      return hash;
    });
  }

  let serial = 1;
  for (const item of [...textByHash.values()]) {
    item.id = `L${String(serial++).padStart(4, "0")}`;
  }

  const lines = [
    title,
    "=".repeat(title.length),
    "",
    `Target: ${targetTriple}`,
    "This file is generated from the locked local dependency graph. Runtime entries are",
    "reachable through normal dependencies; build entries are retained separately for",
    "source/build transparency. Duplicate license bodies are stored once and referenced",
    "by ID. Do not edit this file manually.",
    "",
    `Packages: ${packages.length}`,
    `Runtime dependency packages: ${packages.filter((pkg) => pkg.role === "runtime").length}`,
    `Build-time dependency packages: ${packages.filter((pkg) => pkg.role === "build").length}`,
    `Unique license/notice texts: ${textByHash.size}`,
    "",
    "PACKAGE INVENTORY",
    "-----------------",
  ];

  for (const pkg of [...packages].sort((a, b) =>
    a.role.localeCompare(b.role) || a.name.localeCompare(b.name) || a.version.localeCompare(b.version))) {
    const ids = pkg.textIds.map((hash) => textByHash.get(hash).id).join(", ");
    lines.push(
      "",
      `${pkg.name} ${pkg.version}`,
      `  Role: ${pkg.role}`,
      `  License: ${pkg.license}`,
      `  Source: ${pkg.source}`,
      `  License text(s): ${ids}`,
    );
  }

  lines.push("", "LICENSE AND NOTICE TEXTS", "------------------------");
  for (const [hash, item] of [...textByHash.entries()].sort((a, b) => a[1].id.localeCompare(b[1].id))) {
    lines.push(
      "",
      `${item.id}  SHA-256 ${hash}`,
      "-".repeat(80),
      item.text.trimEnd(),
    );
  }
  return lines.join("\n") + "\n";
}

await mkdir(licensesRoot, { recursive: true });
const [cargo, npm] = await Promise.all([cargoPackages(), npmPackages()]);
const cargoNoticeChanged = await writeTextIfChanged(
  resolve(licensesRoot, "Cargo-ThirdPartyNotices.txt"),
  renderNotice("MojoRecomp Cargo Third-Party Notices", cargo),
);
const npmNoticeChanged = await writeTextIfChanged(
  resolve(licensesRoot, "Npm-ThirdPartyNotices.txt"),
  renderNotice("MojoRecomp npm Third-Party Notices", npm),
);

console.log(`${cargoNoticeChanged ? "Generated" : "Unchanged"} Cargo notices: ${cargo.length} packages`);
console.log(`${npmNoticeChanged ? "Generated" : "Unchanged"} npm notices: ${npm.length} installed packages`);
