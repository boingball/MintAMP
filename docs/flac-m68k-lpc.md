# CPU-specific FLAC LPC restoration

libfoxenflac reconstructs every predicted sample by multiplying up to 32 LPC
coefficients by previous samples and accumulating the signed products in a
64-bit value. MintAMP previously passed `AMIGA_M68K_ASM`, but upstream
libfoxenflac did not inspect that define, so the flag changed no generated
code.

MintAMP now carries a small dispatch patch for the libfoxenflac submodule and
keeps the CPU implementation in `decoders/flac_m68k_lpc.h`.

## CPU paths

- 68020/68030/68040 performs each signed 32x32-to-64 product with hardware
  register-pair `MULS.L`, then accumulates it with `ADD.L`/`ADDX.L`.
- 68060 reconstructs both product halves from four hardware two-operand
  `MULS.L`/`MULU.L` partial products, then uses the same exact 64-bit add.

The final variable right shift is assembled from the two 32-bit accumulator
halves. FLAC validates the encoded LPC shift as non-negative and its positive
range is below 32, so no 64-bit runtime shift helper is needed here.

## Build and validation

The optimized helper is enabled by default. `FLACASM=0` selects upstream's
portable `int64_t` loop for A/B testing.

```sh
make -C decoders clean
make -C decoders flac CPU=30

make -C decoders clean
make -C decoders flac CPU=60

make -C decoders clean
make -C decoders flac CPU=60 FLACASM=0

make -f Makefile.amiga flac-lpc-ref-test
```

The host test checks one million multiply-accumulates, signed edge cases and
512 complete LPC restoration blocks for both CPU backends. CI then compiles
and links the full FLAC module in the 68030 and 68060 release jobs.

Measure the same FLAC file with identical output settings on real hardware.
Files using higher-order LPC predictors should show more benefit than fixed or
verbatim subframes. PiStorm and emulator timings are not representative of
real 68060 instruction-trap costs.
