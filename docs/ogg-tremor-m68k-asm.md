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

* `decoders/tremor/src/tremor/asm_m68k.h` (carried by the existing patch,
  parallel to `asm_arm.h`) implements the 68020/030/040
  `MULT32`/`MULT31`/`MULT31_SHIFT15` path via register-pair `muls.l` - the
  same 32x32->64 signed multiply idiom as
  `MULSHIFT32_AMIGA_M68K_ASM` in `real/assembly.h` (`MULT32` computes the
  identical value: the high 32 bits of a signed 64-bit product), and
  `CLIP_TO_15` via a plain compare/branch clamp to `[-32768, 32767]`
  (matching `bra.s`/`beq.s`-style short branches and numeric local labels
  already used in `real/amiga_m68k_polyphase.S`).
* `misc.h` gained one line: `#include "asm_m68k.h"` next to the existing
  `#include "asm_arm.h"`. Everything in the new header is gated behind
  `#ifdef _M68K_ASSEM_` plus a 68020/030/040 GCC target check.
* `decoders/ogg_m68k_68060.h` pre-empts Tremor's wide-math hook on CPU=60.
  It reconstructs the same high product using four hardware two-operand
  `MULS.L`/`MULU.L` partial products. It is force-included by the parent
  Makefile, so it also safely overrides an old patch already present in an
  existing Tremor submodule checkout.
* `decoders/Makefile` gained an `OGGASM` switch (default on, mirroring
  `AACASM`) that adds `-D_M68K_ASSEM_` to `OGG_CFLAGS`.

`MULT31` is not its own asm block - like the ARM version, it's just
`MULT32(x, y) << 1`, reusing the asm `MULT32`.

## Why this is safe

* On 68020/030/040, `MULT32`'s math is identical to `MULSHIFT32`, using the
  same fast full-result `muls.l` instruction and operand constraints.
* On 68060, Warren's signed high-multiply construction is bit-exact with the
  64-bit reference while using only multiply forms implemented in hardware.
* `MULT31_SHIFT15` combines the same `muls.l` hi:lo pair with the *same*
  arithmetic the portable C reference in `misc.h` uses
  (`((uint)lo >> 15) | (hi << 17)`), so it's a direct arithmetic
  transcription, not a new formula.
* `CLIP_TO_15` is a straightforward two-branch clamp, easy to verify
  against the C reference by inspection - no bit tricks.
* Gated behind `OGGASM` the same way `AACASM` already gates the AAC m68k
  helpers, so `OGGASM=0` gives an immediate, isolated fallback to plain C
  if anything is ever suspect.
* `make -f Makefile.amiga ogg-mulshift60-ref-test` verifies `MULT32`,
  `MULT31` and `MULT31_SHIFT15` against the 64-bit reference over edge cases
  and one million deterministic random pairs. CI then compiles the complete
  decoder with the Amiga cross-compiler for both CPU=30 and CPU=60.

## Validating on target

```sh
git submodule update --init decoders/tremor decoders/libogg
make -C decoders ogg              # OGGASM=1 by default
make -C decoders ogg OGGASM=0     # plain-C fallback for comparison
make -C decoders ogg CPU=60       # hardware-only 68060 multiply path
```

Decode the same OGG file with both builds and diff the PCM output (or
compare `--checksum`-style output if the OGG module exposes one) to
confirm bit-exactness, then A/B decode time to see whether `MULT32`'s
`muls.l` measurably beats the portable `(int64_t)x * y` fallback on
68020/68030/68040. Repeat the A/B on CPU=60 to measure the hardware-only
partial-product path against portable C.

## Left for a follow-up

The helper supplies the normal Tremor `XPROD32`/`XPROD31`/`XNPROD31`
compositions on top of the new multiply primitive. A genuinely hand-scheduled
m68k butterfly or the larger LSP loop asm (`lsp_loop_asm`/`lsp_norm_asm` in
`floor0.c`) remains follow-up work and should be driven by real profiling.
