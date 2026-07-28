#!/usr/bin/env python3
"""Reject long register-pair multiply/divide instructions in a 68060 image."""

import argparse
import re
import subprocess
import sys


# GNU m68k objdump has several spellings for these instructions:
#
#   muls.l   d1,d2:d0
#   muls.l   d1,d2,d0
#   mulsl    d1,d2:d0
#   mulsll   d1,d2:d0
#   divsl.l  d1,d2,d0
#   divul.l  d1,d2,d0
#
# Match the long instruction mnemonic first, then separately inspect its
# operands for either the colon register-pair form or GNU objdump's
# three-operand form.
LONG_PAIR_MNEMONIC = re.compile(
    r"\b(?:muls|mulu|divs|divu)(?:\.l|l(?:\.l|l)?)\b",
    re.IGNORECASE,
)

# Traditional rendering:
#
#   muls.l d1,d2:d0
COLON_REGISTER_PAIR = re.compile(
    r"%?d[0-7]\s*:\s*%?d[0-7]",
    re.IGNORECASE,
)

# Rendering produced by the current m68k-amigaos-objdump:
#
#   muls.l d1,d2,d0
#
# Check specifically for two trailing data-register operands. This avoids
# treating a comma inside an indexed effective address as a third operand.
TRAILING_REGISTER_PAIR = re.compile(
    r",\s*%?d[0-7]\s*,\s*%?d[0-7]\s*$",
    re.IGNORECASE,
)


def is_emulated_pair_instruction(line: str) -> bool:
    """Return True when a disassembly line contains a register-pair operation."""

    mnemonic = LONG_PAIR_MNEMONIC.search(line)
    if mnemonic is None:
        return False

    operands = line[mnemonic.end():].strip()

    return bool(
        COLON_REGISTER_PAIR.search(operands)
        or TRAILING_REGISTER_PAIR.search(operands)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Audit an m68k executable for long register-pair multiply/divide "
            "instructions that require software emulation on a 68060."
        )
    )
    parser.add_argument("binary")
    parser.add_argument(
        "--objdump",
        default="m68k-amigaos-objdump",
        help="m68k objdump executable",
    )
    args = parser.parse_args()

    try:
        result = subprocess.run(
            [args.objdump, "-d", args.binary],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        parser.error(f"cannot run {args.objdump}: {exc}")

    if result.returncode:
        sys.stderr.write(result.stderr)
        return result.returncode

    offenders = [
        line
        for line in result.stdout.splitlines()
        if is_emulated_pair_instruction(line)
    ]

    if offenders:
        print(
            f"68060 audit failed: {len(offenders)} emulated register-pair "
            "multiply/divide instruction(s):",
            file=sys.stderr,
        )
        for line in offenders:
            print(line, file=sys.stderr)
        return 1

    print(f"68060 instruction audit passed: {args.binary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
