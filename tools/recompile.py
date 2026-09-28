from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
XENON_WORK = ROOT / "thirdparty" / "work" / "XenonRecomp"
RECOMP = ROOT / "thirdparty" / "build" / "XenonRecomp" / "XenonRecomp" / "XenonRecomp.exe"
CONFIG = ROOT / "config" / "CrashOfTheTitans.toml"
CONTEXT = XENON_WORK / "XenonUtils" / "ppc_context.h"
RECOMPILER_SOURCE = XENON_WORK / "XenonRecomp" / "recompiler.cpp"
GENERATED = ROOT / "ppc"


def require(path: Path, label: str) -> None:
    if not path.exists():
        raise RuntimeError(f"{label} is missing: {path}")


for path, label in (
    (RECOMP, "XenonRecomp"),
    (CONFIG, "config"),
    (CONTEXT, "PPC context header"),
    (RECOMPILER_SOURCE, "XenonRecomp source"),
):
    require(path, label)

recompiler_text = RECOMPILER_SOURCE.read_text(encoding="utf-8")
for marker in (
    "switch ({}.u32)",
    "PPC_INST_DCBST",
    "PPC_INST_FRSQRTE",
    "PPC_INST_LVXL128",
    "PPC_INST_STVLXL128",
    "PPC_INST_VANDC",
):
    if marker not in recompiler_text:
        raise RuntimeError(
            f"MojoRecomp Xenon runtime patch is incomplete ({marker} missing). "
            "Run tools/apply-xenon-patches.bat first."
        )

GENERATED.mkdir(parents=True, exist_ok=True)
subprocess.run([str(RECOMP), str(CONFIG), str(CONTEXT)], cwd=ROOT, check=True)

generated_context = GENERATED / "ppc_context.h"
require(generated_context, "generated PPC context")
context_text = generated_context.read_text(encoding="utf-8")

mmio_pattern = re.compile(
    r"#ifndef PPC_MM_STORE_U32\s*\r?\n"
    r"#define PPC_MM_STORE_U32\(x, y\)\s+PPC_STORE_U32\(x, y\)\r?\n"
    r"#endif",
    re.MULTILINE | re.DOTALL,
)
mmio_replacement = """extern "C" void MojoRecompMmioStoreU32(uint8_t* base, uint32_t address, uint32_t value);

#ifndef PPC_MM_STORE_U32
#define PPC_MM_STORE_U32(x, y)  MojoRecompMmioStoreU32(base, uint32_t(x), uint32_t(y))
#endif"""
context_text, count = mmio_pattern.subn(mmio_replacement, context_text, count=1)
if count != 1:
    raise RuntimeError("Could not install MojoRecomp MMIO U32 hook in generated ppc_context.h")

call_pattern = re.compile(
    r"#ifndef PPC_CALL_INDIRECT_FUNC\s*\r?\n"
    r"#define PPC_CALL_INDIRECT_FUNC\(x\) \(PPC_LOOKUP_FUNC\(base, x\)\)\(ctx, base\)\r?\n"
    r"#endif",
    re.MULTILINE | re.DOTALL,
)
call_replacement = """extern "C" void MojoRecompTraceIndirectCall(uint32_t target, uint32_t lr, uint32_t object);

#ifndef PPC_CALL_INDIRECT_FUNC
#define PPC_CALL_INDIRECT_FUNC(x) (MojoRecompTraceIndirectCall(uint32_t(x), uint32_t(ctx.lr), ctx.r3.u32), (PPC_LOOKUP_FUNC(base, x))(ctx, base))
#endif"""
context_text, count = call_pattern.subn(call_replacement, context_text, count=1)
if count != 1:
    raise RuntimeError("Could not install MojoRecomp indirect-call crash breadcrumb in generated ppc_context.h")

generated_context.write_text(context_text, encoding="utf-8", newline="")
files = [path for path in GENERATED.iterdir() if path.is_file()]
total_bytes = sum(path.stat().st_size for path in files)
print(f"Generated {len(files)} files ({total_bytes} bytes) in ppc\\")
