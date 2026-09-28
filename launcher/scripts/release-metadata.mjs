import { isIP } from "node:net";

function invalid(label, detail) {
  throw new Error(`${label} ${detail}`);
}

function publicHttpsUrl(value, label) {
  const trimmed = value.trim();
  if (!trimmed) return null;
  let parsed;
  try {
    parsed = new URL(trimmed);
  } catch {
    invalid(label, "is not a valid URL");
  }
  const hostname = parsed.hostname.toLowerCase();
  const ipCandidate = hostname.startsWith("[") && hostname.endsWith("]")
    ? hostname.slice(1, -1)
    : hostname;
  if (
    parsed.protocol !== "https:"
    || !hostname
    || parsed.username
    || parsed.password
    || hostname === "localhost"
    || hostname.endsWith(".localhost")
    || isIP(ipCandidate) !== 0
  ) {
    invalid(label, "must use a public HTTPS hostname without embedded credentials");
  }
  return parsed;
}

export function normalizedHttpsBase(value, label) {
  const parsed = publicHttpsUrl(value, label);
  if (!parsed) return "";
  if (parsed.search || parsed.hash) {
    invalid(label, "must not contain a query string or fragment");
  }
  return parsed.toString().replace(/\/+$/, "");
}

export function validatePublicHttpsUrl(value, label) {
  const parsed = publicHttpsUrl(value, label);
  return parsed ? parsed.toString() : "";
}

export function validateReleaseChannel(value) {
  if (!["stable", "beta", "development"].includes(value)) {
    throw new Error("release_channel must be stable, beta, or development");
  }
  return value;
}

export function validateReleaseDate(value) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value)) {
    throw new Error(`Invalid release date: ${value}`);
  }
  const [year, month, day] = value.split("-").map(Number);
  const date = new Date(Date.UTC(year, month - 1, day));
  if (
    year < 2020
    || date.getUTCFullYear() !== year
    || date.getUTCMonth() !== month - 1
    || date.getUTCDate() !== day
  ) {
    throw new Error(`Invalid release date: ${value}`);
  }
  return value;
}
