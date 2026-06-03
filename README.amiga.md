# AmigaOS 3 / m68k-amigaos-gcc port notes

This tree now has a correctness-first Amiga m68k backend in `real/assembly.h`.
Define `AMIGA_M68K` when building for AmigaOS 3, or rely on the automatic
GNU m68k target detection used by m68k-amigaos-gcc.
The backend deliberately uses portable C `long long` fixed-point helpers first;
68020 inline assembly optimizations should be added only after comparing their
output against this fallback.

## Minimal decoder build

Build the command-line decoder with the public decoder files, the common table
file, every portable RealNetworks backend C file, and the `pub`/`real` include
paths:

```sh
m68k-amigaos-gcc -m68020 -O2 -Ipub -Ireal \
  -o amiga_mp3dec amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
```

For a native smoke build on a non-Amiga host, use a portable backend define such
as `-D__riscv` only if your compiler supports the helper assembly for that
architecture; otherwise build with `-DAMIGA_M68K` to exercise the plain-C
fallback.

## Usage

```sh
amiga_mp3dec [options] infile.mp3 outfile
```

Default output is raw signed 16-bit big-endian PCM. Options:

- `--mono` mixes stereo input down to mono before writing.
- `--s8` writes raw signed 8-bit PCM.
- `--8svx` writes mono Amiga IFF-8SVX signed 8-bit output.
- `--fibdelta` writes 8SVX with Fibonacci Delta compression. The compressed
  `BODY` contains the two D1 predictor bytes plus one packed delta nibble per
  `oneShotHiSamples` output sample, so odd sample counts end with a padded low
  nibble.
- `--bench` prints elapsed time and realtime decode speed.

The program prints the first decoded frame's sample rate, channel count, and
bitrate when available, followed by decoded frame count and output sample count.

## Optimization roadmap

1. Keep the C helper block as the reference backend for 68000 and non-GCC builds.
2. Benchmark with `--bench` on target hardware and profile helper use; the known
   hottest fixed-point paths are `MULSHIFT32`, `MADD64`, and `SAR64` in the DCT,
   IMDCT, stereo processing, dequantization, and polyphase synthesis code.
3. Add 68020-specific inline assembly behind a dedicated opt-in macro (for
   example `AMIGA_M68K_020_ASM`) and test every optimized helper against the C
   fallback over edge cases before enabling it in release builds.
