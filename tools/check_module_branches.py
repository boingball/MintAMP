#!/usr/bin/env python3
"""Check that every long PC-relative branch (bra.l/bsr.l) in a linked decoder
module lands either inside its own function section or exactly on the start
of another one, using the module's link map (<module>.map).

Guards against the hunk linker resolving cross-section PC-relative
relocations short of their target (see decoders/Makefile,
-fno-optimize-sibling-calls).  Exit status 1 if any branch is bad.

Usage: tools/check_module_branches.py decoders/*.decoder
"""
import re,subprocess,sys
def check(binary, mapfile):
    secs=[]
    lines=open(mapfile).read().splitlines()
    k=[i for i,l in enumerate(lines) if l.startswith('Linker script and memory map')]
    lines=lines[k[0]:] if k else lines
    for i,l in enumerate(lines):
        m=re.match(r'^ (\.text\S*)(?:\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+))?',l)
        if not m: continue
        if m.group(2): a,sz=int(m.group(2),16),int(m.group(3),16)
        else:
            m2=re.match(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s',lines[i+1] if i+1<len(lines) else '')
            if not m2: continue
            a,sz=int(m2.group(1),16),int(m2.group(2),16)
        if sz: secs.append((a,a+sz,m.group(1)))
    secs.sort()
    def sec_of(x):
        for a,b,n in secs:
            if a<=x<b: return (a,b,n)
    dis=subprocess.run(['m68k-amigaos-objdump','-d',binary],capture_output=True,text=True).stdout
    bad=0; total=0
    for l in dis.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\s+(60ff|61ff)\s+([0-9a-f]{4})\s+([0-9a-f]{4})',l)
        if not m: continue
        pc=int(m.group(1),16); disp=int(m.group(3)+m.group(4),16)
        if disp>=0x80000000: disp-=0x100000000
        tgt=pc+2+disp; total+=1
        s_from=sec_of(pc); s_to=sec_of(tgt)
        if not s_from: continue  # not code: data decoded as an instruction
        if s_from and s_to and s_from==s_to: continue
        if s_to and s_to[0]==tgt: continue
        bad+=1
        print('  BAD %s @%x -> %x (in %s, from %s)'%(m.group(2),pc,tgt,s_to and s_to[2],s_from and s_from[2]))
    print('%s: %d long branches, %d bad'%(binary,total,bad))
    return bad

if __name__ == '__main__':
    failed=0
    for b in sys.argv[1:]:
        failed+=check(b,b+'.map')
    sys.exit(1 if failed else 0)
