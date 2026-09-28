import { createHash } from "node:crypto";

export const PROJECT_LICENSE_SPDX = "ISC";
export const PROJECT_LICENSE_SHA256 =
  "3465c50abd0f434abaf3d9d20668c743dda4b20ac4181b53de6546b225060ad1";

export function normalizeLicenseText(text) {
  return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").trimEnd() + "\n";
}

export function assertCanonicalProjectLicense(text, label = "MojoRecomp LICENSE") {
  const actual = createHash("sha256").update(normalizeLicenseText(text)).digest("hex");
  if (actual !== PROJECT_LICENSE_SHA256) {
    throw new Error(
      label + " does not match the canonical ISC license approved for original MojoRecomp code",
    );
  }
}
