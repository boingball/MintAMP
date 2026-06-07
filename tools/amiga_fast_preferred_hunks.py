#!/usr/bin/env python3
"""Make Amiga load-file hunks prefer Fast RAM without requiring it.

Classic Amiga load files carry allocation preference bits in the hunk-size table.
A table entry with both high bits clear may be loaded into any memory and the
AmigaDOS loader prefers Fast RAM when it is available.  This keeps the binary
usable on chip-only machines while avoiding accidental chip-only/fast-only hunk
flags from object or linker inputs.
"""

import argparse
import struct
import sys
from pathlib import Path

HUNK_HEADER = 0x000003F3
MEM_MASK = 0xC0000000
SIZE_MASK = 0x3FFFFFFF
EXTENDED_MEM_FLAGS = MEM_MASK


def read_u32(data, pos):
    if pos + 4 > len(data):
        raise ValueError("truncated Amiga hunk file")
    return struct.unpack_from(">I", data, pos)[0], pos + 4


def skip_hunk_strings(data, pos):
    while True:
        words, pos = read_u32(data, pos)
        if words == 0:
            return pos
        byte_count = words * 4
        if pos + byte_count > len(data):
            raise ValueError("truncated resident-library name table")
        pos += byte_count


def mark_fast_preferred(path):
    data = bytearray(path.read_bytes())
    value, pos = read_u32(data, 0)
    if value != HUNK_HEADER:
        raise ValueError(f"{path} is not an Amiga hunk load file")

    pos = skip_hunk_strings(data, pos)
    table_size, pos = read_u32(data, pos)
    first_hunk, pos = read_u32(data, pos)
    last_hunk, pos = read_u32(data, pos)
    if last_hunk < first_hunk:
        raise ValueError("invalid hunk range")
    hunk_count = last_hunk - first_hunk + 1
    if table_size < hunk_count:
        raise ValueError("hunk table is smaller than the declared hunk range")

    changed = 0
    for _ in range(hunk_count):
        entry_pos = pos
        entry, pos = read_u32(data, pos)
        mem_bits = entry & MEM_MASK
        if mem_bits == EXTENDED_MEM_FLAGS:
            raise ValueError("extended hunk memory flags are not supported")
        if mem_bits:
            struct.pack_into(">I", data, entry_pos, entry & SIZE_MASK)
            changed += 1

    if changed:
        path.write_bytes(data)
    return changed


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Clear Amiga hunk memory-type bits so hunks prefer Fast RAM when available."
    )
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args(argv)

    for path in args.files:
        try:
            changed = mark_fast_preferred(path)
        except (OSError, ValueError) as exc:
            print(f"amiga_fast_preferred_hunks.py: {exc}", file=sys.stderr)
            return 1
        print(f"{path}: Fast-preferred hunk table ({changed} entr{'y' if changed == 1 else 'ies'} updated)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
