# m68k asm for Tremor (OGG/Vorbis decoder)

Tremor (the fixed-point Vorbis decoder vendored under `decoders/tremor`,
used by `decoders/ogg.decoder`) already ships a platform-hook architecture
for exactly this: `src/tremor/misc.h` includes `asm_arm.h` unconditionally,
and every hot fixed-point primitive (`MULT32`, `MULT31`, `MULT31_SHIFT15`,
`CLIP_TO_15`, ...) is wrapped in an `#ifndef _V_WIDE_MATH` / `#ifndef
_V_CLIP_MATH` guard that a platform header can pre-empt when its own
`_xxx_ASSEM_` macro is defined. It's the same shape as this project's
`real/assembly.h` hooks (`MULSHIFT32`, `CLZ`) for the MP3 decoder, and the
same shape already used for AAC (`AMIGA_M68K_ASM_AAC_*` in
`decoders/Makefile`).

## What changed

* `decoders/tremor/src/tremor/asm_m68k.h` (new file, parallel to
  `asm_arm.h`): implements `MULT32`/`MULT31`/`MULT31_SHIFT15` via `muls.l`
  - the same 32x32->64 signed multiply idiom as
  `MULSHIFT32_AMIGA_M68K_ASM` in `real/assembly.h` (`MULT32` computes the
  identical value: the high 32 bits of a signed 64-bit product), and
  `CLIP_TO_15` via a plain compare/branch clamp to `[-32768, 32767]`
  (matching `bra.s`/`beq.s`-style short branches and numeric local labels
  already used in `real/amiga_m68k_polyphase.S`).
* `misc.h` gained one line: `#include "asm_m68k.h"` next to the existing
  `#include "asm_arm.h"`. Everything in the new header is gated behind
  `#ifdef _M68K_ASSEM_` plus a `__mc68020__`/`__mc68030__`/... GCC target
  check, so it's inert everywhere except a 68020+ Amiga build that opts in.
* `decoders/Makefile` gained an `OGGASM` switch (default on, mirroring
  `AACASM`) that adds `-D_M68K_ASSEM_` to `OGG_CFLAGS`.

`MULT31` is not its own asm block - like the ARM version, it's just
`MULT32(x, y) << 1`, reusing the asm `MULT32`.

## Why this is safe

* `MULT32`'s math is identical to `MULSHIFT32`, already proven correct and
  in production use in the MP3 decoder core via the exact same `muls.l`
  instruction and operand constraints.
* `MULT31_SHIFT15` combines the same `muls.l` hi:lo pair with the *same*
  arithmetic the portable C reference in `misc.h` uses
  (`((uint)lo >> 15) | (hi << 17)`), so it's a direct arithmetic
  transcription, not a new formula.
* `CLIP_TO_15` is a straightforward two-branch clamp, easy to verify
  against the C reference by inspection - no bit tricks.
* Gated behind `OGGASM` the same way `AACASM` already gates the AAC m68k
  helpers, so `OGGASM=0` gives an immediate, isolated fallback to plain C
  if anything is ever suspect.
* Verified with a native (non-m68k) smoke compile of every file under
  `decoders/tremor/src/tremor/*.c` with `-D_M68K_ASSEM_` defined: all
  compile clean. The asm bodies themselves don't get exercised natively
  (guarded by `__mc68020__`, which a native x86 build never defines), so
  this only confirms the wiring and C-level code around the asm are
  correct - the asm instruction sequences themselves are unverified
  without a target/emulator, same caveat as the earlier Huffman `bfextu`
  and MP3-decoder `MULSHIFT32`/`CLZ` work.

## Validating on target

```sh
git submodule update --init decoders/tremor decoders/libogg
make -C decoders ogg              # OGGASM=1 by default
make -C decoders ogg OGGASM=0     # plain-C fallback for comparison
```

Decode the same OGG file with both builds and diff the PCM output (or
compare `--checksum`-style output if the OGG module exposes one) to
confirm bit-exactness, then A/B decode time to see whether `MULT32`'s
`muls.l` measurably beats the portable `(int64_t)x * y` fallback on
68020/68030 - the same way `--exp-huff`/`--selftest-huffman` validated the
MP3-decoder changes.

## Left for a follow-up

`XPROD32`/`XPROD31`/`XNPROD31` (used in `mdct.c` for the MDCT butterfly
cross products) and the LSP loop asm (`lsp_loop_asm`/`lsp_norm_asm`, used
in `floor0.c`) are bigger, hand-scheduled multi-instruction ARM sequences,
not simple one-instruction wins like `MULT32`. Porting those well would
need a genuinely m68k-native instruction sequence rather than a mechanical
transcription, so they're out of scope here.
