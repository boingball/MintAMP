# 68060 register-pair multiply/divide audit

The Motorola 68060 does not implement the 64-bit-result long multiply
(`muls.l Dn,Dl:Dh`) or the 64-bit-dividend long divide (`divs.l <ea>,Dr:Dq`)
in hardware. Each execution traps into OS software emulation. In MintAMP's hot
MP3 decode loops this is the root cause of the mouse lag, audio starvation and
dropouts observed on a real ~75 MHz 68060 with the assembly-heavy build.

`tools/audit_68060_instructions.py` finds these instructions in a compiled
binary. GNU m68k objdump prints them in a three-operand form
(`mulsl %d1,%d2,%d0`), which the script recognises in addition to the older
colon form (`muls.l %d1,%d2:%d0`). It counts multiplies and divides
separately, groups by symbol, and separates decoder hot-path functions from
unrelated application/library code. In diff mode it compares a clean baseline
binary against a candidate.

## How GCC helps, and where the offenders come from

At `-m68060`, GCC's own codegen avoids the emulated instruction: the C
`MULSHIFT32` reference (`(int)(((long long)x*y)>>32)`) compiles to a `__muldi3`
call rather than a register-pair `muls.l`. So a clean C `CPU=60` build has
essentially zero emulated multiplies in the decoder. The offenders are the
hand-written `.S` kernels and the inline asm that force `muls.l Dn,Dl:Dh`
regardless of `-mcpu`.

## Symbol-level comparison (decoder core)

Numbers below are for the portable Helix decoder core
(`mp3dec.c mp3tabs.c real/*.c` plus the relevant `.S`) compiled at `-m68060`.

> Toolchain note: these were produced with `m68k-linux-gnu-gcc 13` /
> `m68k-linux-gnu-objdump` as a stand-in for `m68k-amigaos-gcc`. GCC's decision
> to emit or avoid the register-pair `muls.l` is a shared m68k-backend property,
> so the C-path counts are representative, but the **absolute** numbers differ
> from a full `m68k-amigaos` application build (which also links GUI,
> networking, PNG and libnix/CRT code). Re-run the audit against the real Amiga
> binaries with `m68k-amigaos-objdump` before drawing release conclusions.

| Config (`ASM60_GROUPS`)        | reg-pair mul | of which hot-path | div |
|--------------------------------|-------------:|------------------:|----:|
| baseline (clean C)             |            0 |                 0 |   0 |
| `huffman`                      |            0 |                 0 |   0 |
| `huffman dequant imdct`        |           55 |                55 |   0 |
| `asm_polyphase`                |         8337 |              8281 |   0 |
| `full030` (whole 030 bundle)   |         8543 |              8487 |   0 |

The existing 68030 polyphase `.S` kernel accounts for ~97% of all emulated
multiplies. Everything else in the decoder is comparatively minor: the huffman
asm kernel adds none, and dequant+imdct together add ~55. This is why the
68060 work starts with a dedicated polyphase rewrite and why `huffman`,
`dequant` and `imdct` are the first groups worth hardware-testing.

The per-symbol breakdown shows the count spread across every stride/phase
variant of the polyphase kernel, e.g.:

```
504  AmigaM68KPolyphaseMonoFast
496  StereoFastPolyphaseStride2Phase0_Amiga_m68k
352  StereoFastPolyphaseStride3Phase0_Amiga_m68k
...
```

## Reproducing

Build the candidate binaries with the CPU=60 audit targets and diff them:

```sh
make -f Makefile.amiga cpu60-audit-stages OBJDUMP=m68k-amigaos-objdump
make -f Makefile.amiga cpu60-audit-asm_polyphase OBJDUMP=m68k-amigaos-objdump
```

Or point the script directly at any two binaries:

```sh
tools/audit_68060_instructions.py --objdump m68k-amigaos-objdump \
    --baseline amiga_mp3dec.cpu60-baseline \
    --candidate amiga_mp3dec.cpu60-group-asm_polyphase
```

`tools/audit_68060_instructions.py --self-test` validates the instruction
classifier (three-operand and colon forms, 32-bit vs 64-bit divide) offline.

## Status

No config here is release-safe on the strength of a static count alone. A low
count is necessary but not sufficient; every candidate must still be validated
on the physical 68060 for both correctness and real-time behaviour. No
dedicated 68060 binary ships until then.
