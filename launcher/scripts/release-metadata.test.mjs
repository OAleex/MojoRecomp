import assert from "node:assert/strict";
import test from "node:test";
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

test("GitHub release feed accepts both pre-releases and normal releases", () => {
  assert.equal(
    isGitHubReleaseFeedUrl("https://api.github.com/repos/OAleex/MojoRecomp/releases?per_page=30"),
    true,
  );
  assert.equal(
    newestGitHubReleaseAssetUrl([
      {
        draft: false,
        prerelease: false,
        published_at: "2026-09-20T12:00:00Z",
        assets: [{
          name: "update-catalog.toml",
          browser_download_url: "https://github.com/OAleex/MojoRecomp/releases/download/v0.9.0/update-catalog.toml",
        }],
      },
      {
        draft: false,
        prerelease: true,
        published_at: "2026-09-29T12:00:00Z",
        assets: [{
          name: "update-catalog.toml",
          browser_download_url: "https://github.com/OAleex/MojoRecomp/releases/download/v1.0.0/update-catalog.toml",
        }],
      },
      {
        draft: true,
        prerelease: false,
        published_at: "2026-09-30T12:00:00Z",
        assets: [{
          name: "update-catalog.toml",
          browser_download_url: "https://github.com/OAleex/MojoRecomp/releases/download/v1.1.0/update-catalog.toml",
        }],
      },
    ]),
    "https://github.com/OAleex/MojoRecomp/releases/download/v1.0.0/update-catalog.toml",
  );
});

test("release metadata accepts public GitHub HTTPS locations", () => {
  assert.equal(
    normalizedHttpsBase(
      "https://github.com/example/MojoRecomp/releases/download/v1/",
      "base",
    ),
    "https://github.com/example/MojoRecomp/releases/download/v1",
  );
  assert.equal(
    validatePublicHttpsUrl(
      "https://github.com/example/MojoRecomp/releases/latest/download/update-catalog.toml",
      "catalog",
    ),
    "https://github.com/example/MojoRecomp/releases/latest/download/update-catalog.toml",
  );
});

test("release metadata rejects local, credentialed, and ambiguous bases", () => {
  for (const value of [
    "http://github.com/example/releases",
    "https://user:secret@github.com/example/releases",
    "https://localhost/releases",
    "https://127.0.0.1/releases",
    "https://10.0.0.1/releases",
    "https://[::1]/releases",
    "https://github.com/example/releases?token=value",
    "https://github.com/example/releases#fragment",
  ]) {
    assert.throws(() => normalizedHttpsBase(value, "base"));
  }
});

test("release channel and date use the catalog contract", () => {
  assert.equal(validateReleaseChannel("development"), "development");
  assert.equal(validateReleaseDate("2028-02-29"), "2028-02-29");
  assert.throws(() => validateReleaseChannel("nightly"));
  assert.throws(() => validateReleaseDate("2027-02-29"));
  assert.throws(() => validateReleaseDate("2026-13-01"));
  assert.throws(() => validateReleaseDate("not-a-date"));
});

test("runtime history keeps older verified runtime releases", () => {
  const catalog = `schema_version = 1
channel = "development"

[[release]]
id = "runtime.cot"
kind = "runtime"
version = "0.2.0"
platform = "windows"
arch = "x86_64"
url = "https://github.com/example/MojoRecomp/releases/download/v1.1.0/runtime-0.2.0.zip"
size = 100
sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
published = "2026-09-28"
notes_url = "https://github.com/example/MojoRecomp/releases/tag/v1.1.0"
package = "zip"
unpacked_size = 200
entrypoint = "cot-runtime.exe"
required_files = ["cot-runtime.exe", "dxcompiler.dll"]
game_id = "cot"

[release.compatibility]
min_launcher = "1.1.0"

[[release]]
id = "runtime.cot"
kind = "runtime"
version = "0.1.0-alpha"
platform = "windows"
arch = "x86_64"
url = "https://github.com/example/MojoRecomp/releases/download/v1.0.0/runtime-0.1.0-alpha.zip"
size = 90
sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
published = "2026-09-20"
notes_url = "https://github.com/example/MojoRecomp/releases/tag/v1.0.0"
package = "zip"
unpacked_size = 180
entrypoint = "cot-runtime.exe"
required_files = ["cot-runtime.exe", "dxcompiler.dll"]
game_id = "cot"

[release.compatibility]
min_launcher = "1.0.0"
`;
  const history = runtimeHistoryFromCatalog(catalog, "0.2.0");
  assert.equal(history.length, 1);
  assert.equal(history[0].version, "0.1.0-alpha");
  assert.equal(history[0].minLauncher, "1.0.0");
});

test("launcher history keeps older verified portable releases", () => {
  const catalog = `schema_version = 1
channel = "stable"

[[release]]
id = "launcher"
kind = "launcher"
version = "1.1.0"
platform = "windows"
arch = "x86_64"
url = "https://github.com/example/MojoRecomp/releases/download/v1.1.0/launcher-1.1.0.zip"
size = 200
sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
published = "2026-09-28"
notes_url = "https://github.com/example/MojoRecomp/releases/tag/v1.1.0"
package = "portable-zip"

[release.compatibility]

[[release]]
id = "launcher"
kind = "launcher"
version = "1.0.0"
platform = "windows"
arch = "x86_64"
url = "https://github.com/example/MojoRecomp/releases/download/v1.0.0/launcher-1.0.0.zip"
size = 180
sha256 = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
published = "2026-09-20"
notes_url = "https://github.com/example/MojoRecomp/releases/tag/v1.0.0"
package = "portable-zip"

[release.compatibility]
`;
  const history = launcherHistoryFromCatalog(catalog, "1.1.0");
  assert.equal(history.length, 1);
  assert.equal(history[0].version, "1.0.0");
});
