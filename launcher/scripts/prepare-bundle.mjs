import { createHash } from "node:crypto";
import { copyFile, mkdir, readFile, stat } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const launcherRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = resolve(launcherRoot, "..");
const runtimeSource = resolve(projectRoot, "runtime/build-smoke/cot-runtime.exe");
const ffmpegSource = resolve(projectRoot, "runtime/build-smoke/mojorecomp-ffmpeg.dll");
const lzxSource = resolve(projectRoot, "runtime/build-smoke/mojorecomp-lzx.dll");
const dxcRoot = resolve(projectRoot, "thirdparty/XenosRecomp-src/thirdparty/dxc-bin/bin/x64");
const runtimeDestination = resolve(
  launcherRoot,
  "src-tauri/binaries/cot-runtime-x86_64-pc-windows-msvc.exe"
);
const libraryDestination = resolve(launcherRoot, "bundle/lib");
const toolDestination = resolve(launcherRoot, "bundle/tools");
const extractXisoSource = resolve(projectRoot, "thirdparty/extract-xiso/extract-xiso.exe");

async function requireFile(path, label) {
  const info = await stat(path).catch(() => null);
  if (!info?.isFile()) {
    throw new Error(`${label} is missing: ${path}`);
  }
}

async function sha256(path) {
  const bytes = await readFile(path);
  return createHash("sha256").update(bytes).digest("hex");
}

async function copyVerified(source, destination, label) {
  const sourceHash = await sha256(source);
  const destinationInfo = await stat(destination).catch(() => null);
  if (destinationInfo?.isFile()) {
    const destinationHash = await sha256(destination);
    if (sourceHash === destinationHash) {
      console.log(`Prepared ${label}: SHA-256 ${destinationHash} (already current)`);
      return;
    }
  }

  await copyFile(source, destination);
  const destinationHash = await sha256(destination);
  if (sourceHash !== destinationHash) {
    throw new Error(`${label} copy verification failed`);
  }
  console.log(`Prepared ${label}: SHA-256 ${destinationHash}`);
}

await requireFile(runtimeSource, "COT runtime");
await requireFile(ffmpegSource, "FFmpeg shared library");
await requireFile(lzxSource, "libmspack LZX shared library");
await requireFile(resolve(dxcRoot, "dxcompiler.dll"), "DXC compiler library");
await requireFile(resolve(dxcRoot, "dxil.dll"), "DXIL support library");
await requireFile(extractXisoSource, "extract-xiso utility");

await mkdir(dirname(runtimeDestination), { recursive: true });
await mkdir(libraryDestination, { recursive: true });
await mkdir(toolDestination, { recursive: true });
await copyVerified(runtimeSource, runtimeDestination, "cot-runtime.exe");
await copyVerified(
  ffmpegSource,
  resolve(libraryDestination, "mojorecomp-ffmpeg.dll"),
  "mojorecomp-ffmpeg.dll",
);
await copyVerified(
  lzxSource,
  resolve(libraryDestination, "mojorecomp-lzx.dll"),
  "mojorecomp-lzx.dll",
);
await copyVerified(
  resolve(dxcRoot, "dxcompiler.dll"),
  resolve(libraryDestination, "dxcompiler.dll"),
  "dxcompiler.dll",
);
await copyVerified(
  resolve(dxcRoot, "dxil.dll"),
  resolve(libraryDestination, "dxil.dll"),
  "dxil.dll",
);
await copyVerified(
  extractXisoSource,
  resolve(toolDestination, "extract-xiso.exe"),
  "extract-xiso.exe",
);
