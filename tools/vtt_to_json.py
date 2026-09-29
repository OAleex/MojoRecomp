#!/usr/bin/env python3
"""Compile author-authored WebVTT dialogue files into MojoRecomp JSON."""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path
import re
import sys


TIMING_RE = re.compile(
    r"^(?P<start>(?:\d{2}:)?\d{2}:\d{2}[\.,]\d{3})\s+-->\s+"
    r"(?P<end>(?:\d{2}:)?\d{2}:\d{2}[\.,]\d{3})(?:\s+.*)?$"
)
VOICE_RE = re.compile(r"^<v(?:\.[^ >]+)?\s+([^>]+)>(.*)$", re.IGNORECASE | re.DOTALL)
TAG_RE = re.compile(r"<[^>]+>")


def timestamp_to_ms(value: str) -> int:
    value = value.replace(",", ".")
    parts = value.split(":")
    if len(parts) == 2:
        hours = 0
        minutes = int(parts[0])
        seconds_text = parts[1]
    elif len(parts) == 3:
        hours = int(parts[0])
        minutes = int(parts[1])
        seconds_text = parts[2]
    else:
        raise ValueError(f"Invalid WebVTT timestamp: {value}")
    seconds, milliseconds = seconds_text.split(".", 1)
    if minutes >= 60 or int(seconds) >= 60 or len(milliseconds) != 3:
        raise ValueError(f"Invalid WebVTT timestamp: {value}")
    return ((hours * 60 + minutes) * 60 + int(seconds)) * 1000 + int(milliseconds)


def plain_text(value: str) -> str:
    value = TAG_RE.sub("", value)
    value = html.unescape(value)
    return " ".join(value.split())


def parse_payload(lines: list[str], source: Path, cue_index: int) -> tuple[str, str]:
    payload = "\n".join(lines).strip()
    match = VOICE_RE.match(payload)
    if not match:
        raise ValueError(
            f"{source}: cue {cue_index} is missing a WebVTT voice tag such as <v Coco>Text"
        )
    speaker = html.unescape(match.group(1)).strip()
    text = plain_text(match.group(2).replace("</v>", ""))
    if not speaker or not text:
        raise ValueError(f"{source}: cue {cue_index} has an empty speaker or text")
    return speaker, text


def parse_vtt(path: Path) -> list[dict[str, object]]:
    text = path.read_text(encoding="utf-8-sig")
    lines = text.splitlines()
    if not lines or lines[0].strip() != "WEBVTT":
        raise ValueError(f"{path}: missing WEBVTT header")

    cues: list[dict[str, object]] = []
    index = 1
    while index < len(lines):
        while index < len(lines) and not lines[index].strip():
            index += 1
        if index >= len(lines):
            break

        if lines[index].lstrip().startswith("NOTE"):
            index += 1
            while index < len(lines) and lines[index].strip():
                index += 1
            continue
        if lines[index].strip() in {"STYLE", "REGION"}:
            index += 1
            while index < len(lines) and lines[index].strip():
                index += 1
            continue

        timing_line = lines[index].strip()
        match = TIMING_RE.match(timing_line)
        if not match:
            # WebVTT allows a cue identifier on the line before the timing.
            index += 1
            if index >= len(lines):
                raise ValueError(f"{path}: cue identifier is missing a timing line")
            timing_line = lines[index].strip()
            match = TIMING_RE.match(timing_line)
        if not match:
            raise ValueError(f"{path}: invalid cue timing: {timing_line}")

        start_ms = timestamp_to_ms(match.group("start"))
        end_ms = timestamp_to_ms(match.group("end"))
        if end_ms <= start_ms:
            raise ValueError(f"{path}: cue end must be after cue start")

        index += 1
        payload: list[str] = []
        while index < len(lines) and lines[index].strip():
            payload.append(lines[index])
            index += 1
        speaker, cue_text = parse_payload(payload, path, len(cues) + 1)
        cues.append(
            {
                "start_ms": start_ms,
                "end_ms": end_ms,
                "speaker": speaker,
                "text": cue_text,
            }
        )

    previous_start = -1
    for cue in cues:
        start_ms = int(cue["start_ms"])
        if start_ms < previous_start:
            raise ValueError(f"{path}: cues must be ordered by start time")
        previous_start = start_ms
    return cues


def discover_vtts(root: Path) -> list[Path]:
    return sorted(root.rglob("*.vtt"), key=lambda path: str(path).casefold())


def compile_dialogues(root: Path, locale: str) -> dict[str, object]:
    dialogues: dict[str, list[dict[str, object]]] = {}
    for vtt_path in discover_vtts(root):
        asset_name = f"{vtt_path.stem}.rsd"
        expected_rsd = root / asset_name
        expected_folder = root / vtt_path.stem
        if vtt_path.parent.resolve() != expected_folder.resolve():
            raise ValueError(
                f"{vtt_path}: expected VTT inside sibling folder {expected_folder}"
            )
        if not expected_rsd.is_file():
            raise ValueError(f"{vtt_path}: matching RSD does not exist: {expected_rsd}")
        cues = parse_vtt(vtt_path)
        if asset_name in dialogues:
            raise ValueError(f"Duplicate VTT asset: {asset_name}")
        dialogues[asset_name] = cues

    return {
        "schema_version": 1,
        "locale": locale,
        "dialogues": dialogues,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile WebVTT dialogue authoring files into MojoRecomp JSON."
    )
    parser.add_argument("root", type=Path, help="Audio authoring root containing RSDs and VTT folders")
    parser.add_argument("output", type=Path, help="Output Dialogues_<locale>.json path")
    parser.add_argument("--locale", required=True, help="BCP-47 locale, for example en-US or pt-BR")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    output = args.output.resolve()
    if not root.is_dir():
        print(f"Authoring root does not exist: {root}", file=sys.stderr)
        return 2

    try:
        document = compile_dialogues(root, args.locale)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    cue_count = sum(len(cues) for cues in document["dialogues"].values())
    print(
        f"Compiled {len(document['dialogues'])} VTT file(s), {cue_count} cue(s) -> {output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
