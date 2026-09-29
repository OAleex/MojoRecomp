#!/usr/bin/env python3
"""Extract Radical audio RCF archives using human-readable RSD source names.

The archive-facing hash names are preserved inside the source RCF. This tool
uses the original WAV path embedded in each Radical RSD header to build a clean
authoring workspace:

    english/
      nis01.rsd
      nis01/
        nis01.wav

Game assets must be supplied by the user and output should stay in ignored
private development storage.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
from dataclasses import dataclass


MAGIC = b"ATG CORE CEMENT LIBRARY"
HEADER_SIZE = 60
TABLE1_RECORD_SIZE = 12
TABLE2_PREFIX_SIZE = 8
TABLE2_RECORD_HEADER_SIZE = 16
TABLE2_RECORD_PADDING_SIZE = 3
SUPPORTED_FLAGS = bytes((2, 1, 1, 1))
SOURCE_WAV_RE = re.compile(rb"[A-Za-z]:\\[^\x00-\x1f]*?\.wav", re.IGNORECASE)
INVALID_WINDOWS_CHARS_RE = re.compile(r'[<>:"/\\|?*]')
MULTICHANNEL_SUFFIX_RE = re.compile(r"_ch\*$", re.IGNORECASE)


@dataclass(frozen=True)
class RcfEntry:
    entry_id: int
    offset: int
    size: int
    archive_name: str


@dataclass(frozen=True)
class AudioEntry:
    rcf: RcfEntry
    source_wav: str
    base_name: str


def read_be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def read_le32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def align_up(value: int, alignment: int) -> int:
    if alignment <= 0 or alignment & (alignment - 1):
        raise ValueError(f"Invalid RCF alignment: {alignment}")
    return (value + alignment - 1) & ~(alignment - 1)


def parse_rcf(path: Path) -> list[RcfEntry]:
    source_size = path.stat().st_size
    with path.open("rb") as source:
        header = source.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE:
            raise ValueError("RCF is smaller than its fixed header")
        if not header.startswith(MAGIC) or any(header[len(MAGIC) : 32]):
            raise ValueError("Unsupported RCF signature")
        if header[32:36] != SUPPORTED_FLAGS:
            raise ValueError(f"Unsupported RCF flags: {header[32:36].hex()}")

        table1_offset = read_be32(header, 36)
        table1_size = read_be32(header, 40)
        table2_offset = read_be32(header, 44)
        table2_size = read_be32(header, 48)
        reserved = read_be32(header, 52)
        file_count = read_be32(header, 56)

        if reserved != 0 or file_count <= 0:
            raise ValueError("Invalid RCF header")
        if table1_size != file_count * TABLE1_RECORD_SIZE:
            raise ValueError("RCF table 1 size does not match file count")
        if table1_offset < HEADER_SIZE or table1_offset + table1_size > source_size:
            raise ValueError("RCF table 1 is outside the archive")
        if table2_offset < table1_offset + table1_size or table2_offset + table2_size > source_size:
            raise ValueError("RCF table 2 is outside the archive")

        source.seek(table1_offset)
        table1 = source.read(table1_size)
        raw_entries: list[list[int | str]] = []
        for index in range(file_count):
            at = index * TABLE1_RECORD_SIZE
            raw_entries.append(
                [
                    read_be32(table1, at),
                    read_be32(table1, at + 4),
                    read_be32(table1, at + 8),
                    "",
                ]
            )

        source.seek(table2_offset)
        table2 = source.read(table2_size)
        if len(table2) != table2_size or table2_size < TABLE2_PREFIX_SIZE:
            raise ValueError("Could not read RCF table 2")
        alignment = read_le32(table2, 0)
        if alignment != 2048 or read_le32(table2, 4) != 0:
            raise ValueError("Unsupported RCF table 2 header")

        data_start = align_up(table2_offset + table2_size, alignment)
        physical_order = sorted(range(file_count), key=lambda index: int(raw_entries[index][1]))
        if int(raw_entries[physical_order[0]][1]) != data_start:
            raise ValueError("RCF first data entry does not start at the aligned data boundary")

        cursor = TABLE2_PREFIX_SIZE
        previous_end = data_start
        seen_offsets: set[int] = set()
        seen_names: set[str] = set()
        for index in physical_order:
            entry_offset = int(raw_entries[index][1])
            entry_size = int(raw_entries[index][2])
            if (
                entry_offset < data_start
                or entry_offset % alignment != 0
                or entry_offset in seen_offsets
                or entry_offset < previous_end
                or entry_offset + entry_size > source_size
            ):
                raise ValueError("Invalid RCF data entry layout")
            seen_offsets.add(entry_offset)
            previous_end = entry_offset + entry_size

            if cursor + TABLE2_RECORD_HEADER_SIZE > len(table2):
                raise ValueError("RCF table 2 ended before all names")
            name_length = read_le32(table2, cursor + 12)
            name_start = cursor + TABLE2_RECORD_HEADER_SIZE
            name_end = name_start + name_length
            padding_end = name_end + TABLE2_RECORD_PADDING_SIZE
            if name_length < 2 or padding_end > len(table2):
                raise ValueError("Invalid RCF name record")
            if table2[name_end - 1] != 0 or any(table2[name_end:padding_end]):
                raise ValueError("Invalid RCF name terminator or padding")
            name_bytes = table2[name_start : name_end - 1]
            if b"\0" in name_bytes:
                raise ValueError("RCF name contains an embedded NUL")
            archive_name = name_bytes.decode("ascii")
            if archive_name in seen_names:
                raise ValueError(f"Duplicate RCF name: {archive_name}")
            seen_names.add(archive_name)
            raw_entries[index][3] = archive_name
            cursor = padding_end

        if cursor != len(table2):
            raise ValueError("RCF table 2 contains trailing data")
        if previous_end != source_size:
            raise ValueError("RCF data extent does not match archive size")

    return [
        RcfEntry(int(entry_id), int(offset), int(size), str(name))
        for entry_id, offset, size, name in raw_entries
    ]


def source_wav_from_header(header: bytes) -> str | None:
    matches = list(SOURCE_WAV_RE.finditer(header))
    if not matches:
        return None
    # Radical RSD headers used by COT contain one original source path. Prefer
    # the longest match if a future variant contains more than one candidate.
    raw = max((match.group(0) for match in matches), key=len)
    return raw.decode("ascii")


def clean_base_name(source_wav: str) -> str:
    file_name = source_wav.replace("/", "\\").rsplit("\\", 1)[-1]
    if file_name.lower().endswith(".wav"):
        file_name = file_name[:-4]
    file_name = MULTICHANNEL_SUFFIX_RE.sub("", file_name)
    file_name = INVALID_WINDOWS_CHARS_RE.sub("_", file_name).rstrip(" .")
    if not file_name:
        raise ValueError(f"Source WAV produced an empty file name: {source_wav}")
    return file_name


def discover_audio_entries(rcf_path: Path, entries: list[RcfEntry]) -> list[AudioEntry]:
    audio_entries: list[AudioEntry] = []
    with rcf_path.open("rb") as source:
        for entry in entries:
            if not entry.archive_name.lower().endswith(".rsd"):
                continue
            source.seek(entry.offset)
            header = source.read(min(entry.size, 4096))
            source_wav = source_wav_from_header(header)
            if not source_wav:
                raise ValueError(
                    f"RSD does not contain an original WAV path: {entry.archive_name}"
                )
            audio_entries.append(
                AudioEntry(entry, source_wav, clean_base_name(source_wav))
            )

    by_name: dict[str, AudioEntry] = {}
    for audio in audio_entries:
        key = audio.base_name.casefold()
        previous = by_name.get(key)
        if previous:
            raise ValueError(
                "Human-readable RSD name collision: "
                f"{previous.rcf.archive_name} and {audio.rcf.archive_name} -> "
                f"{audio.base_name}.rsd"
            )
        by_name[key] = audio
    return audio_entries


def copy_entry(source, entry: RcfEntry, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    source.seek(entry.offset)
    remaining = entry.size
    with destination.open("wb") as output:
        while remaining:
            block = source.read(min(1024 * 1024, remaining))
            if not block:
                raise OSError(f"Unexpected EOF extracting {entry.archive_name}")
            output.write(block)
            remaining -= len(block)


def extract_rsds(
    rcf_path: Path,
    output_root: Path,
    audio_entries: list[AudioEntry],
    overwrite: bool,
) -> tuple[int, int]:
    written = 0
    skipped = 0
    output_root.mkdir(parents=True, exist_ok=True)
    with rcf_path.open("rb") as source:
        for index, audio in enumerate(audio_entries, start=1):
            rsd_path = output_root / f"{audio.base_name}.rsd"
            wave_dir = output_root / audio.base_name
            wave_dir.mkdir(parents=True, exist_ok=True)
            if overwrite or not rsd_path.exists() or rsd_path.stat().st_size != audio.rcf.size:
                copy_entry(source, audio.rcf, rsd_path)
                written += 1
            else:
                skipped += 1
            if index % 500 == 0 or index == len(audio_entries):
                print(f"Extracted RSD metadata/files: {index}/{len(audio_entries)}")
    return written, skipped


def decode_one(vgmstream: Path, rsd_path: Path, wav_path: Path, overwrite: bool) -> tuple[str, str]:
    if not overwrite and wav_path.is_file() and wav_path.stat().st_size > 44:
        return "skipped", rsd_path.name
    wav_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = wav_path.with_name(wav_path.name + ".tmp.wav")
    try:
        temp_path.unlink(missing_ok=True)
        result = subprocess.run(
            [str(vgmstream), "-i", "-o", str(temp_path), str(rsd_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if result.returncode != 0 or not temp_path.is_file() or temp_path.stat().st_size <= 44:
            temp_path.unlink(missing_ok=True)
            detail = result.stdout.strip().splitlines()
            message = detail[-1] if detail else f"exit code {result.returncode}"
            return "failed", f"{rsd_path.name}: {message}"
        os.replace(temp_path, wav_path)
        return "decoded", rsd_path.name
    except Exception as error:  # Report individual conversion failures without losing the batch.
        temp_path.unlink(missing_ok=True)
        return "failed", f"{rsd_path.name}: {error}"


def decode_rsds(
    output_root: Path,
    audio_entries: list[AudioEntry],
    vgmstream: Path,
    jobs: int,
    overwrite: bool,
) -> tuple[int, int, list[str]]:
    decoded = 0
    skipped = 0
    failures: list[str] = []
    lock = threading.Lock()
    completed = 0

    def task(audio: AudioEntry) -> tuple[str, str]:
        rsd_path = output_root / f"{audio.base_name}.rsd"
        wav_path = output_root / audio.base_name / f"{audio.base_name}.wav"
        return decode_one(vgmstream, rsd_path, wav_path, overwrite)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = [executor.submit(task, audio) for audio in audio_entries]
        for future in concurrent.futures.as_completed(futures):
            status, detail = future.result()
            with lock:
                completed += 1
                if status == "decoded":
                    decoded += 1
                elif status == "skipped":
                    skipped += 1
                else:
                    failures.append(detail)
                if completed % 100 == 0 or completed == len(audio_entries):
                    print(
                        f"Decoded WAVs: {completed}/{len(audio_entries)} "
                        f"(new={decoded}, skipped={skipped}, failed={len(failures)})"
                    )
    return decoded, skipped, failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract Radical RSD audio from an RCF using original source names."
    )
    parser.add_argument("rcf", type=Path, help="Input RCF archive")
    parser.add_argument("output", type=Path, help="Output authoring directory")
    parser.add_argument(
        "--vgmstream",
        type=Path,
        help=(
            "Path to vgmstream-cli.exe. If omitted, the tool checks "
            ".private/tools/vgmstream/bin and PATH."
        ),
    )
    parser.add_argument(
        "--no-decode",
        action="store_true",
        help="Extract and rename RSD files without creating WAV files.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(4, os.cpu_count() or 1),
        help="Parallel vgmstream processes (default: up to 4).",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace existing RSD and WAV files.",
    )
    return parser.parse_args()


def resolve_vgmstream(explicit: Path | None) -> Path | None:
    if explicit:
        return explicit.resolve()

    repository_root = Path(__file__).resolve().parent.parent
    private_candidate = (
        repository_root
        / ".private"
        / "tools"
        / "vgmstream"
        / "bin"
        / "vgmstream-cli.exe"
    )
    if private_candidate.is_file():
        return private_candidate

    path_candidate = shutil.which("vgmstream-cli.exe") or shutil.which("vgmstream-cli")
    return Path(path_candidate).resolve() if path_candidate else None


def main() -> int:
    args = parse_args()
    rcf_path = args.rcf.resolve()
    output_root = args.output.resolve()
    if not rcf_path.is_file():
        print(f"Input RCF does not exist: {rcf_path}", file=sys.stderr)
        return 2
    if args.jobs < 1:
        print("--jobs must be at least 1", file=sys.stderr)
        return 2

    vgmstream = resolve_vgmstream(args.vgmstream)
    if not args.no_decode and (not vgmstream or not vgmstream.is_file()):
        print(
            "vgmstream-cli was not found. Pass --vgmstream, add it to PATH, or "
            "place it under .private/tools/vgmstream/bin.",
            file=sys.stderr,
        )
        return 2

    print(f"Reading RCF: {rcf_path}")
    entries = parse_rcf(rcf_path)
    audio_entries = discover_audio_entries(rcf_path, entries)
    print(f"RCF entries: {len(entries)}")
    print(f"RSD entries with original WAV names: {len(audio_entries)}")

    written, extract_skipped = extract_rsds(
        rcf_path, output_root, audio_entries, args.overwrite
    )
    print(f"RSD extraction complete: new={written}, skipped={extract_skipped}")

    if args.no_decode:
        return 0

    decoded, decode_skipped, failures = decode_rsds(
        output_root,
        audio_entries,
        vgmstream,
        args.jobs,
        args.overwrite,
    )
    print(
        f"WAV decoding complete: new={decoded}, skipped={decode_skipped}, "
        f"failed={len(failures)}"
    )
    if failures:
        print("Decode failures:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
