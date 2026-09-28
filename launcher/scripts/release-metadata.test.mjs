import assert from "node:assert/strict";
import test from "node:test";
import {
  normalizedHttpsBase,
  validatePublicHttpsUrl,
  validateReleaseChannel,
  validateReleaseDate,
} from "./release-metadata.mjs";

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
