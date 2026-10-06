import { generateKeyPairSync } from "node:crypto";
import { stat, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const privateKeyPath = resolve(projectRoot, "release-signing-private.pem");
const publicKeyPath = resolve(projectRoot, "release-signing-public.pem");

if ((await stat(privateKeyPath).catch(() => null)) || (await stat(publicKeyPath).catch(() => null))) {
  throw new Error(
    `Refusing to overwrite release signing material in ${projectRoot}. ` +
      "Back up and move both existing key files before intentionally rotating the key.",
  );
}

const { privateKey, publicKey } = generateKeyPairSync("ed25519");
const privatePem = privateKey.export({ format: "pem", type: "pkcs8" });
const publicPem = publicKey.export({ format: "pem", type: "spki" });
const publicDer = publicKey.export({ format: "der", type: "spki" });
const publicKeyHex = publicDer.subarray(publicDer.length - 32).toString("hex");

await writeFile(privateKeyPath, privatePem, { encoding: "utf8", flag: "wx", mode: 0o600 });
await writeFile(publicKeyPath, publicPem, { encoding: "utf8", flag: "wx", mode: 0o644 });

console.log(`Private release key created at ${privateKeyPath}`);
console.log(`Public release key created at ${publicKeyPath}`);
console.log(`Public Ed25519 key: ${publicKeyHex}`);
console.log("Back up the private key securely. Never commit or distribute it.");
