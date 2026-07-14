#!/usr/bin/env python3
import argparse, re, subprocess, sys

MOVEA_RE = re.compile(r'\bmovea\.l\s+(?:0x)?([0-9a-fA-F]+)')

def disasm(binary):
    return subprocess.check_output([
        'm68k-amigaos-objdump', '-d', '--disassemble=___free_all', binary
    ], text=True, errors='replace')

def heads(binary):
    text = disasm(binary)
    vals = []
    for line in text.splitlines():
        m = MOVEA_RE.search(line)
        if not m:
            continue
        v = int(m.group(1), 16)
        if v not in vals:
            vals.append(v)
    if len(vals) != 3:
        sys.stderr.write('expected exactly three ___free_all movea.l list-head addresses, found %d\n' % len(vals))
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
