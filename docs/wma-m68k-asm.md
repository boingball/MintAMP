# CPU-specific m68k math for Rockbox WMA

MintAMP's WMA module vendors Rockbox's fixed-point WMAv1/WMAv2 decoder. The
upstream code has ARM and ColdFire acceleration, but the normal Amiga build
previously fell back to signed `int64_t` C multiplication in the primitives
used throughout its FFT, IMDCT, complex rotations and reconstruction windows.

The ColdFire code is not directly reusable on classic 68k. Its multiply-heavy
paths require ColdFire EMAC accumulators (`mac.l`, `msac.l`, `movclr.l`), which
do not exist on either the 68030 or 68060.

## CPU paths

`decoders/wma/asm_m68k.h` provides two bit-exact implementations:

- 68020/68030/68040 uses the hardware register-pair form of `MULS.L` to obtain
  the complete signed 32x32-to-64 product in one instruction.
- 68060 uses Warren's signed high-multiply construction. Four hardware
  two-operand `MULS.L`/`MULU.L` partial products reconstruct both halves of
  the result without invoking the 68060 software package.

Keeping both product halves is important for WMA. `MULT31_SHIFT15`,
`MULT31_SHIFT16`, `fixmul32` and `fixmul32b` all select different overlapping
32-bit ranges from the same product. The 68060 construction derives the low
half from the existing partial products, avoiding a fifth multiply.

The central helpers cover:

- `MULT32`, `MULT31`, `MULT31_SHIFT15` and `MULT31_SHIFT16`;
- the Q16 `fixmul32` used by WMA coefficient and scale calculations;
- the Q31 `fixmul32b` and `CMUL` used by the IMDCT;
- forward/add and reverse reconstruction-window loops.

Window loops are unrolled by two. That reduces branch overhead on 68030 and
places independent multiply chains next to each other for the 68060 scheduler.

## Build and validation

The optimized path is the default. `WMAASM=0` selects the original portable C
path for troubleshooting and on-target A/B measurements.

```sh
make -C decoders clean
make -C decoders wma CPU=30

make -C decoders clean
make -C decoders wma CPU=60

make -C decoders clean
make -C decoders wma CPU=60 WMAASM=0

make -f Makefile.amiga wma-mulshift-ref-test
```

The host test builds the 030 and 060 arithmetic backends separately and checks
one million deterministic random inputs plus edge cases. It covers the full
product, every exposed shift, complex multiplication, and both window-vector
directions. CI then cross-compiles complete 68030 and 68060 release editions.

The final performance test must be done on real hardware with the same WMA
file and output settings. Compare the default CPU=60 decoder against
`WMAASM=0`; emulators and PiStorm CPU emulation may not reflect 68060 trap cost
or dual-pipeline scheduling accurately.
