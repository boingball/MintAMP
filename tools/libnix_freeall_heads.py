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
#
# 3. The same layout as 2 printed in MIT syntax by the GCC 16 toolchain's
#    objdump: "moveal" for movea.l, "a3@(8)" for 8(a3), the heads loaded
#    into a2 rather than a0.
#
# So both mnemonic spellings and both displacement syntaxes are accepted, and
# a head may be loaded into any of a0-a5. a6 is excluded because it only ever
# receives the library base (SysBase) in this function.
#
# 4. The historical layout as printed by the GCC 13 toolchain's objdump, which
#    repeats the address and leaves the symbol note unbracketed:
#    "movea.l 18d69c 18d69c _errno+0xc,a2".
MOVEA = r'\bmovea\.?l\s+'
NUM = r'(-?(?:0x[0-9a-fA-F]+|[0-9]+))'
HEAD_DEST = r'\s*,a([0-5])\b'
# Absolute operand: a hex address followed by whatever note this objdump
# prints for it ("<_errno+0x4>", "18d69c _errno+0xc", or nothing) up to the
# operand comma. A displacement operand ("8(a2)", "a3@(8)") cannot match: the
# note must start after whitespace.
ABS_OPERAND = r'(?:0x)?([0-9a-fA-F]+)(?:\s+[^,]*)?'
MOVEA_ABS_RE = re.compile(MOVEA + ABS_OPERAND + HEAD_DEST)
LEA_ABS_RE = re.compile(r'\blea\s+' + ABS_OPERAND + r'\s*,a([0-7])\b')
MOVEA_DISP_MOT_RE = re.compile(MOVEA + NUM + r'\(a([0-7])\)' + HEAD_DEST)
MOVEA_DISP_MIT_RE = re.compile(MOVEA + r'a([0-7])@\(' + NUM + r'\)' + HEAD_DEST)
REGISTER_RE = re.compile(r'^(?:[ad][0-7]|sp|pc|fp)$')
# "___free_all:" from the older objdump, "<___free_all>:" from GCC 16's.
FUNC_LABEL_RE = re.compile(r'(?:\b|<)___free_all>?:\s*$')
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


def absolute(m):
    """The absolute address of an abs-operand match, or None if the operand
    is actually a register name that happens to be valid hex (a2, d0...)."""
    if REGISTER_RE.match(m.group(1).lower()):
        return None
    return int(m.group(1), 16)


def heads_from_text(text):
    lines = free_all_lines(text)

    # Historical layout: three direct movea.l operands annotated as
    # _errno+offset. Ignore other absolute loads such as _SysBase.
    vals = []
    for line in lines:
        if '_errno+' not in line:
            continue
        m = MOVEA_ABS_RE.search(line)
        if m and absolute(m) is not None:
            append_unique(vals, absolute(m))
    if len(vals) == 3:
        return vals

    # lea layout (GCC 6.5 in Motorola syntax, GCC 16 in MIT syntax):
    #
    #   lea      <base>,aN
    #   movea.l  8(aN),aM      -> first list head = base + 8
    #   movea.l  12(aN),aM     -> second list head = base + 12
    #   movea.l  <absolute>,aM -> third list head
    #
    # Track absolute LEA bases per register so this is not tied to a2/a3.
    vals = []
    bases = {}
    for line in lines:
        m = LEA_ABS_RE.search(line)
        if m and absolute(m) is not None:
            bases[int(m.group(2))] = absolute(m)
            continue

        m = MOVEA_DISP_MOT_RE.search(line)
        if m:
            disp, reg = m.group(1), int(m.group(2))
        else:
            m = MOVEA_DISP_MIT_RE.search(line)
            if m:
                reg, disp = int(m.group(1)), m.group(2)
        if m:
            if reg in bases:
                append_unique(vals,
                              bases[reg] + parse_objdump_displacement(disp))
            continue

        m = MOVEA_ABS_RE.search(line)
        if m and absolute(m) is not None:
            append_unique(vals, absolute(m))
    return vals


def heads(binary):
    text = disasm(binary)
    vals = heads_from_text(text)
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
