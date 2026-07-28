#!/usr/bin/env python3
"""Reject long register-pair multiply/divide instructions in a 68060 image."""

import argparse
import re
import subprocess
import sys


PAIR_INSN = re.compile(
    # GNU m68k objdump versions spell the size as either ``muls.l``,
    # ``mulsl`` or ``mulsll``.  Match all spellings but require the colon
    # register pair, avoiding ordinary 16x16 multiply instructions.
    r"\b(?:muls|mulu|divs|divu)(?:\.l|ll?|\.ll)\b[^\n]*"
    r"(?:%?d[0-7])\s*:\s*(?:%?d[0-7])",
    re.IGNORECASE,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--objdump", default="m68k-amigaos-objdump")
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

    offenders = [line for line in result.stdout.splitlines() if PAIR_INSN.search(line)]
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
