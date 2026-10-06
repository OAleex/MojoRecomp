from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
from pathlib import Path


TARGETS = (
    r"levels\L3_E3\statics.lua",
    r"package\5000af12.p3d",
    r"mdl\c80d681a.p3d",
    r"levels\L4_E4\props_normal.lua",
    r"package\7a8185b0.p3d",
    r"package\c1e387c7.p3d",
    r"package\7efdcd91.p3d",
    r"package\7a88cba0.p3d",
    r"package\cdd70a8c.p3d",
    r"package\97597d1b.p3d",
    r"package\b4c85fe7.p3d",
    r"package\bea9f18c.p3d",
    r"package\79aea37a.p3d",
    r"package\a2c6e833.p3d",
)

MAGIC = b"MJRDIF01"
BLOCK = 32
SOURCE_STEP = 16
MIN_COPY = 64
MAX_CANDIDATES = 8


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def read_rcf_entries(path: Path) -> dict[str, tuple[int, int]]:
    with path.open("rb") as stream:
        header = stream.read(60)
        if len(header) != 60 or not header.startswith(b"ATG CORE CEMENT LIBRARY"):
            fail(f"Unsupported RCF: {path}")
        if header[32:36] != bytes((2, 1, 1, 1)):
            fail("Unsupported RCF flags")
        table1_offset, table1_size, table2_offset, table2_size, reserved, count = struct.unpack(
            ">6I", header[36:60]
        )
        if reserved != 0 or count == 0 or table1_size != count * 12:
            fail("Invalid RCF index")
        stream.seek(table1_offset)
        table1 = stream.read(table1_size)
        entries = []
        for index in range(count):
            _, offset, size = struct.unpack_from(">III", table1, index * 12)
            entries.append([offset, size, ""])
        stream.seek(table2_offset)
        table2 = stream.read(table2_size)
        if len(table2) != table2_size or struct.unpack_from("<I", table2, 0)[0] != 2048:
            fail("Unsupported RCF name table")
        order = sorted(range(len(entries)), key=lambda index: entries[index][0])
        cursor = 8
        for index in order:
            if cursor + 16 > len(table2):
                fail("Truncated RCF name table")
            name_len = struct.unpack_from("<I", table2, cursor + 12)[0]
            start = cursor + 16
            end = start + name_len
            if name_len < 2 or end + 3 > len(table2):
                fail("Invalid RCF entry name")
            raw = table2[start:end]
            if raw[-1] != 0:
                fail("RCF entry name is not terminated")
            entries[index][2] = raw[:-1].decode("utf-8")
            cursor = end + 3
        return {
            name.replace("/", "\\").lower(): (offset, size)
            for offset, size, name in entries
        }


def read_rcf_entry(stream, entries: dict[str, tuple[int, int]], name: str) -> bytes:
    key = name.replace("/", "\\").lower()
    if key not in entries:
        fail(f"RCF entry is missing: {name}")
    offset, size = entries[key]
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        fail(f"RCF entry is truncated: {name}")
    return data


def translated_files(root: Path) -> dict[str, Path]:
    expected = {
        Path(target.replace("\\", "/")).name.lower()
        for target in TARGETS
    }
    found: dict[str, list[Path]] = {}
    for path in root.rglob("*"):
        if path.is_file() and path.name.lower() in expected:
            found.setdefault(path.name.lower(), []).append(path)
    result = {}
    for target in TARGETS:
        name = Path(target.replace("\\", "/")).name.lower()
        matches = found.get(name, [])
        if len(matches) != 1:
            fail(f"Expected exactly one translated {name}, found {len(matches)}")
        result[name] = matches[0]
    return result


def validate_translated_resource(target: str, data: bytes) -> None:
    if not data:
        fail(f"Translated resource is empty: {target}")
    if target.lower().endswith(".p3d") and not data.startswith(b"P3D\xff"):
        fail(f"Translated resource is not Pure3D: {target}")


def source_index(source: bytes) -> dict[bytes, list[int]]:
    index: dict[bytes, list[int]] = {}
    if len(source) < BLOCK:
        return index
    for offset in range(0, len(source) - BLOCK + 1, SOURCE_STEP):
        key = source[offset : offset + BLOCK]
        bucket = index.setdefault(key, [])
        if len(bucket) < MAX_CANDIDATES:
            bucket.append(offset)
    return index


def delta_operations(source: bytes, target: bytes):
    index = source_index(source)
    operations = []
    literal = bytearray()
    position = 0

    def flush_literal():
        if literal:
            operations.append((2, bytes(literal)))
            literal.clear()

    while position < len(target):
        best_offset = -1
        best_length = 0
        if position + BLOCK <= len(target):
            candidates = index.get(target[position : position + BLOCK], ())
            for offset in candidates:
                length = BLOCK
                limit = min(len(source) - offset, len(target) - position)
                while length < limit and source[offset + length] == target[position + length]:
                    length += 1
                if length > best_length:
                    best_offset = offset
                    best_length = length
        if best_length >= MIN_COPY:
            flush_literal()
            operations.append((1, best_offset, best_length))
            position += best_length
        else:
            literal.append(target[position])
            position += 1
    flush_literal()
    return operations


def write_delta(source: bytes, target: bytes, destination: Path) -> None:
    operations = delta_operations(source, target)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        output.write(MAGIC)
        output.write(hashlib.sha256(source).digest())
        output.write(hashlib.sha256(target).digest())
        output.write(struct.pack("<QI", len(target), len(operations)))
        for operation in operations:
            if operation[0] == 1:
                _, offset, length = operation
                output.write(b"\x01")
                output.write(struct.pack("<QQ", offset, length))
            else:
                _, data = operation
                output.write(b"\x02")
                output.write(struct.pack("<Q", len(data)))
                output.write(data)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build MojoRecomp COT localization deltas")
    parser.add_argument("--original-rcf", required=True, type=Path)
    parser.add_argument("--translated-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--locale", required=True)
    args = parser.parse_args()

    if not args.original_rcf.is_file():
        fail(f"Original RCF not found: {args.original_rcf}")
    if not args.translated_root.is_dir():
        fail(f"Translated source folder not found: {args.translated_root}")

    entries = read_rcf_entries(args.original_rcf)
    translated = translated_files(args.translated_root)
    staging = args.output.with_name(args.output.name + ".building")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    patch_rows = []
    with args.original_rcf.open("rb") as original:
        for target in TARGETS:
            source = read_rcf_entry(original, entries, target)
            name = Path(target.replace("\\", "/")).name
            result = translated[name.lower()].read_bytes()
            validate_translated_resource(target, result)
            relative_patch = f"patches/{Path(name).stem}.mjdelta"
            destination = staging / relative_patch
            write_delta(source, result, destination)
            patch_rows.append((target, relative_patch))
            print(f"{name}: {len(source)} -> {len(result)} bytes, delta {destination.stat().st_size} bytes")

    manifest = [
        "schema_version = 1",
        'game_id = "cot"',
        f'locale = "{args.locale}"',
        "",
    ]
    for target, patch in patch_rows:
        manifest.extend(
            [
                "[[patch]]",
                f'archive_path = {target!r}'.replace("'", '"'),
                f'file = "{patch}"',
                "",
            ]
        )
    (staging / "language-patches.toml").write_text("\n".join(manifest), encoding="utf-8")
    if args.output.exists():
        shutil.rmtree(args.output)
    staging.rename(args.output)
    print(f"Localization delta payload created: {args.output}")


if __name__ == "__main__":
    main()
