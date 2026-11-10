#!/usr/bin/env python3
"""
Converts Skylanders dumps in Flipper Zero format (.nfc) into .sky files (1024 bytes).
Not required for Portal Zero (it reads .nfc directly) - useful for other tools that need .sky.

Usage:
    python3 nfc2sky.py FOLDER_WITH_NFC_FILES [TARGET_FOLDER]

Without TARGET_FOLDER the .sky files end up in the subfolder "Skylanders" of the source folder.
"""
import re
import sys
from pathlib import Path

BLOCKS = 64          # MIFARE Classic 1K: 64 blocks of 16 bytes
BLOCK_RE = re.compile(r"^\s*Block\s+(\d+)\s*:\s*(.+?)\s*$", re.IGNORECASE)


def is_trailer(block):
    # every 4th block (3, 7, 11, ...) is a sector trailer with keys
    return block % 4 == 3


def convert(src: Path):
    text = src.read_text(encoding="utf-8", errors="replace")
    if "Mifare Classic" not in text and "MIFARE Classic" not in text:
        return None, "not a MIFARE Classic file (Skylanders are MIFARE Classic 1K)"

    data = bytearray(BLOCKS * 16)
    found = set()
    missing_data_blocks = []

    for line in text.splitlines():
        m = BLOCK_RE.match(line)
        if not m:
            continue
        block = int(m.group(1))
        if block >= BLOCKS:
            return None, "more than 64 blocks - not a 1K figure"
        parts = m.group(2).split()
        if len(parts) != 16:
            return None, f"block {block} has {len(parts)} instead of 16 bytes"
        unknown = False
        for i, p in enumerate(parts):
            if p == "??":
                unknown = True
                data[block * 16 + i] = 0
            else:
                data[block * 16 + i] = int(p, 16)
        if unknown and not is_trailer(block):
            missing_data_blocks.append(block)
        found.add(block)

    if len(found) != BLOCKS:
        missing = sorted(set(range(BLOCKS)) - found)
        return None, f"missing blocks: {missing[:8]}{' ...' if len(missing) > 8 else ''}"

    warning = None
    if missing_data_blocks:
        warning = (f"incompletely read (blocks {missing_data_blocks[:6]}"
                   f"{' ...' if len(missing_data_blocks) > 6 else ''}) - figure may not work")
    return bytes(data), warning


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src_dir = Path(sys.argv[1]).expanduser()
    dst_dir = Path(sys.argv[2]).expanduser() if len(sys.argv) > 2 else src_dir / "Skylanders"
    files = sorted(p for p in src_dir.iterdir() if p.suffix.lower() == ".nfc" and not p.name.startswith("."))
    if not files:
        print(f"No .nfc files found in {src_dir}.")
        sys.exit(1)

    dst_dir.mkdir(parents=True, exist_ok=True)
    ok = 0
    for f in files:
        data, msg = convert(f)
        if data is None:
            print(f"SKIPPED  {f.name}: {msg}")
            continue
        out = dst_dir / (f.stem + ".sky")
        out.write_bytes(data)
        ok += 1
        print(f"OK       {f.name} -> {out.name}" + (f"   WARNING: {msg}" if msg else ""))

    print(f"\n{ok} of {len(files)} files converted. Output in: {dst_dir}")


if __name__ == "__main__":
    main()
