#!/usr/bin/env python3
import argparse, re, subprocess, sys

# libnix ___free_all has appeared in two equivalent code-generation shapes in
# the Amiga GCC toolchains used by this project:
#
#  1. three direct absolute movea.l loads, historically annotated by objdump as
#     _errno+offset; and
#  2. a libnix layout that does one absolute lea into an address register,
#     loads two heads as 8(reg)/12(reg), then loads the third head directly.
#
# Support both. The addresses are discovered from the exact binary being
# relinked, so this remains safer than hard-coding libnix internals.
MOVEA_RE = re.compile(r'\bmovea\.l\s+(?:0x)?([0-9a-fA-F]+)')
MOVEA_ABS_A0_RE = re.compile(
    r'\bmovea\.l\s+(?:0x)?([0-9a-fA-F]+)(?:\s+[^,]*)?,a0\b')
LEA_ABS_RE = re.compile(
    r'\blea\s+(?:0x)?([0-9a-fA-F]+)(?:\s+[^,]*)?,a([0-7])\b')
MOVEA_OFFSET_A0_RE = re.compile(
    r'\bmovea\.l\s+(-?(?:0x[0-9a-fA-F]+|[0-9]+))\(a([0-7])\),a0\b')
FUNC_LABEL_RE = re.compile(r'\b___free_all:\s*$')
RTS_RE = re.compile(r'\brts\b')


def disasm(binary):
    return subprocess.check_output([
        'm68k-amigaos-objdump', '-d', '--disassemble=___free_all', binary
    ], text=True, errors='replace')


def free_all_lines(text):
    """Return only the first ___free_all function body, through its first rts."""
    lines = []
    started = False
    for line in text.splitlines():
        if not started:
            if FUNC_LABEL_RE.search(line):
                started = True
            continue
        lines.append(line)
        if RTS_RE.search(line):
            break
    return lines


def append_unique(vals, value):
    if value not in vals:
        vals.append(value)


def parse_objdump_displacement(value):
    """objdump prints address-register displacements in decimal by default."""
    if value.lower().startswith(('0x', '-0x')):
        return int(value, 0)
    return int(value, 10)


def heads(binary):
    text = disasm(binary)
    lines = free_all_lines(text)

    # Historical layout: three direct movea.l operands annotated as
    # _errno+offset. Ignore other absolute loads such as _SysBase.
    vals = []
    for line in lines:
        if '_errno+' not in line:
            continue
        m = MOVEA_RE.search(line)
        if m:
            append_unique(vals, int(m.group(1), 16))
    if len(vals) == 3:
        return vals

    # Alternate libnix layout, seen with the CI GCC 6.5 toolchain:
    #
    #   lea      <base>,a2
    #   movea.l  8(a2),a0      -> first list head = base + 8
    #   ...
    #   movea.l  12(a2),a0     -> second list head = base + 12
    #   ...
    #   movea.l  <absolute>,a0  -> third list head
    #
    # Track absolute LEA bases generically so this is not tied to a2.
    vals = []
    bases = {}
    for line in lines:
        m = LEA_ABS_RE.search(line)
        if m:
            bases[int(m.group(2))] = int(m.group(1), 16)

        m = MOVEA_OFFSET_A0_RE.search(line)
        if m:
            reg = int(m.group(2))
            if reg in bases:
                append_unique(
                    vals,
                    bases[reg] + parse_objdump_displacement(m.group(1)))
            continue

        m = MOVEA_ABS_A0_RE.search(line)
        if m:
            append_unique(vals, int(m.group(1), 16))

    if len(vals) != 3:
        sys.stderr.write(
            'expected exactly three ___free_all list-head addresses, found %d\n' %
            len(vals))
        sys.stderr.write(text)
        sys.exit(1)
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('--verify', nargs=3, metavar=('H0','H1','H2'))
    ap.add_argument('--shell', action='store_true')
    ns = ap.parse_args()
    vals = heads(ns.binary)
    if ns.verify:
        want = [int(x, 0) for x in ns.verify]
        if vals != want:
            sys.stderr.write('___free_all head mismatch: found %s expected %s\n' %
                             ([hex(x) for x in vals], [hex(x) for x in want]))
            sys.exit(1)
    if ns.shell:
        print('FREEALL_HEAD0=0x%08x' % vals[0])
        print('FREEALL_HEAD1=0x%08x' % vals[1])
        print('FREEALL_HEAD2=0x%08x' % vals[2])
        print('FREEALL_LDFLAGS="-Wl,--defsym,___freeall_head0=0x%08x -Wl,--defsym,___freeall_head1=0x%08x -Wl,--defsym,___freeall_head2=0x%08x"' % tuple(vals))
    else:
        print(' '.join('0x%08x' % x for x in vals))


if __name__ == '__main__':
    main()
