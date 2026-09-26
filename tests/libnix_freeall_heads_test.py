#!/usr/bin/env python3
"""Checks tools/libnix_freeall_heads.py against every ___free_all shape it
has had to read: no toolchain needed, just objdump text."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
from libnix_freeall_heads import heads_from_text  # noqa: E402

# The two older shapes are reconstructed from the script's own comments
# (bare "___free_all:" label, Motorola syntax); the GCC 16 one is verbatim.

# Historical: three direct movea.l loads annotated _errno+offset.
HISTORICAL = """
0020a100 ___free_all:
  20a100:\t48e7 0032      \tmovem.l a2-a3/a6,-(sp)
  20a104:\t2079 0020 c8b4 \tmovea.l 20c8b4 <_errno+0x4>,a0
  20a10a:\t2c79 0000 0004 \tmovea.l 4 <_SysBase>,a6
  20a110:\t2079 0020 c8b8 \tmovea.l 20c8b8 <_errno+0x8>,a0
  20a116:\t2079 0020 c8c8 \tmovea.l 20c8c8 <_errno+0x18>,a0
  20a11c:\t4e75           \trts
"""

# GCC 6.5 (CI image): lea base, two displaced loads, one absolute, Motorola.
GCC65 = """
000d4dc6 ___free_all:
   d4dc6:\t48e7 0032      \tmovem.l a2-a3/a6,-(sp)
   d4dca:\t45f9 0018 d684 \tlea 18d684 <_errno+0x4>,a2
   d4dd0:\t206a 0008      \tmovea.l 8(a2),a0
   d4ddc:\t2c79 0000 0014 \tmovea.l 14 <_____start+0x14>,a6
   d4df2:\t206a 000c      \tmovea.l 12(a2),a0
   d4e14:\t2079 0018 d6a0 \tmovea.l 18d6a0 <_errno+0x20>,a0
   d4e3c:\t4e75           \trts
"""

# GCC 16.2 (amiga16-rc11): the same layout in MIT syntax, heads into a2.
GCC16 = """
000d4dc6 <___free_all>:
   d4dc6:       48e7 0032       moveml a2-a3/a6,sp@-
   d4dca:       47f9 0018 d684  lea 18d684 <_errno+0x4>,a3
   d4dd0:       246b 0008       moveal a3@(8),a2
   d4dd4:       4a8a            .short 0x4a8a
   d4dd6:       671a            beqs d4df2 <___free_all+0x2c>
   d4dd8:       204a            moveal a2,a0
   d4dda:       2452            moveal a2@,a2
   d4ddc:       2c79 0000 0014  moveal 14 <_____start+0x14>,a6
   d4de2:       43e8 fffc       lea a0@(-4),a1
   d4de6:       2028 fffc       movel a0@(-4),d0
   d4dea:       4eae ff2e       jsr a6@(-210)
   d4dee:       4a8a            .short 0x4a8a
   d4df0:       66e6            bnes d4dd8 <___free_all+0x12>
   d4df2:       246b 000c       moveal a3@(12),a2
   d4df6:       4a8a            .short 0x4a8a
   d4df8:       671a            beqs d4e14 <___free_all+0x4e>
   d4dfa:       204a            moveal a2,a0
   d4dfc:       2452            moveal a2@,a2
   d4dfe:       2c79 0000 0014  moveal 14 <_____start+0x14>,a6
   d4e04:       43e8 fffc       lea a0@(-4),a1
   d4e08:       2028 fffc       movel a0@(-4),d0
   d4e0c:       4eae ff2e       jsr a6@(-210)
   d4e10:       4a8a            .short 0x4a8a
   d4e12:       66e6            bnes d4dfa <___free_all+0x34>
   d4e14:       2479 0018 d6a0  moveal 18d6a0 <_errno+0x20>,a2
   d4e1a:       4a8a            .short 0x4a8a
   d4e1c:       671a            beqs d4e38 <___free_all+0x72>
   d4e1e:       204a            moveal a2,a0
   d4e20:       2452            moveal a2@,a2
   d4e22:       2c79 0000 0014  moveal 14 <_____start+0x14>,a6
   d4e28:       43e8 fffc       lea a0@(-4),a1
   d4e2c:       2028 fffc       movel a0@(-4),d0
   d4e30:       4eae ff2e       jsr a6@(-210)
   d4e34:       4a8a            .short 0x4a8a
   d4e36:       66e6            bnes d4e1e <___free_all+0x58>
   d4e38:       4cdf 4c00       moveml sp@+,a2-a3/a6
   d4e3c:       4e75            rts
"""

# GCC 13: the historical layout, but objdump repeats each address and leaves
# the symbol note unbracketed. Verbatim from a CPU=30 MintAMP link.
GCC13 = """
000ce7e6 000ce7e6 ___free_all:
   ce7e6:       2f0e            move.l a6,-(sp)
   ce7e8:       2f0a            move.l a2,-(sp)
   ce7ea:       2479 0018 d69c  movea.l 18d69c 18d69c _errno+0xc,a2
   ce7f0:       4a8a            tst.l a2
   ce7f2:       671a            beq.s ce80e ce80e ___free_all+0x28
   ce7f4:       204a            movea.l a2,a0
   ce7f6:       2452            movea.l (a2),a2
   ce7f8:       2c79 0000 0018  movea.l 18 18 _SysBase,a6
   ce7fe:       43e8 fffc       lea -4(a0),a1
   ce802:       2028 fffc       move.l -4(a0),d0
   ce806:       4eae ff2e       jsr -210(a6)
   ce80a:       4a8a            tst.l a2
   ce80c:       66e6            bne.s ce7f4 ce7f4 ___free_all+0xe
   ce80e:       2479 0018 d6a0  movea.l 18d6a0 18d6a0 _errno+0x10,a2
   ce814:       4a8a            tst.l a2
   ce816:       671a            beq.s ce832 ce832 ___free_all+0x4c
   ce818:       204a            movea.l a2,a0
   ce81a:       2452            movea.l (a2),a2
   ce81c:       2c79 0000 0018  movea.l 18 18 _SysBase,a6
   ce822:       43e8 fffc       lea -4(a0),a1
   ce826:       2028 fffc       move.l -4(a0),d0
   ce82a:       4eae ff2e       jsr -210(a6)
   ce82e:       4a8a            tst.l a2
   ce830:       66e6            bne.s ce818 ce818 ___free_all+0x32
   ce832:       2479 0018 d6b0  movea.l 18d6b0 18d6b0 _errno+0x20,a2
   ce838:       4a8a            tst.l a2
   ce83a:       671a            beq.s ce856 ce856 ___free_all+0x70
   ce83c:       204a            movea.l a2,a0
   ce83e:       2452            movea.l (a2),a2
   ce840:       2c79 0000 0018  movea.l 18 18 _SysBase,a6
   ce846:       43e8 fffc       lea -4(a0),a1
   ce84a:       2028 fffc       move.l -4(a0),d0
   ce84e:       4eae ff2e       jsr -210(a6)
   ce852:       4a8a            tst.l a2
   ce854:       66e6            bne.s ce83c ce83c ___free_all+0x56
   ce856:       245f            movea.l (sp)+,a2
   ce858:       2c5f            movea.l (sp)+,a6
   ce85a:       4e75            rts
"""

# GCC 6.5's lea layout printed the GCC 13 way (repeated address, bare note).
GCC65_BARE = """
000d4dc6 000d4dc6 ___free_all:
   d4dc6:\t48e7 0032      \tmovem.l a2-a3/a6,-(sp)
   d4dca:\t45f9 0018 d684 \tlea 18d684 18d684 _errno+0x4,a2
   d4dd0:\t206a 0008      \tmovea.l 8(a2),a0
   d4ddc:\t2c79 0000 0014 \tmovea.l 14 14 _____start+0x14,a6
   d4df2:\t206a 000c      \tmovea.l 12(a2),a0
   d4e14:\t2079 0018 d6a0 \tmovea.l 18d6a0 18d6a0 _errno+0x20,a0
   d4e3c:\t4e75           \trts
"""

CASES = [
    ('historical', HISTORICAL, [0x20c8b4, 0x20c8b8, 0x20c8c8]),
    ('gcc 6.5 lea layout', GCC65, [0x18d68c, 0x18d690, 0x18d6a0]),
    ('gcc 16 MIT syntax', GCC16, [0x18d68c, 0x18d690, 0x18d6a0]),
    ('gcc 13 repeated address', GCC13, [0x18d69c, 0x18d6a0, 0x18d6b0]),
    ('lea layout, repeated address', GCC65_BARE, [0x18d68c, 0x18d690, 0x18d6a0]),
]

failed = 0
for name, text, want in CASES:
    got = heads_from_text(text)
    if got != want:
        print('FAIL %s: got %s want %s' % (
            name, [hex(x) for x in got], [hex(x) for x in want]))
        failed += 1
if failed:
    sys.exit(1)
print('libnix ___free_all head extraction: %d layouts OK' % len(CASES))
