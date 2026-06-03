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

Default output is raw signed 16-bit big-endian PCM. If `outfile` names an
Amiga volume or directory and ends in `:`, `/`, or `\`, the decoder creates
an output file there from the input basename, using `.pcm`, `.s8`, or `.8svx`
for the selected output format.  For example, `RAM:` with `song.mp3` writes
`RAM:song.pcm`. Options:

- `--mono` mixes stereo input down to mono before writing.
- `--s8` writes raw signed 8-bit PCM.
- `--8svx` writes mono Amiga IFF-8SVX signed 8-bit output.
- `--fibdelta` writes 8SVX with Fibonacci Delta compression. The compressed
  `BODY` contains the two D1 predictor bytes plus one packed delta nibble per
  `oneShotHiSamples` output sample, so odd sample counts end with a padded low
  nibble.
- `--bench` prints elapsed time, realtime decode speed, and timing buckets for
  frame decode, PCM conversion/downsampling, 8SVX writing, Fibonacci
  compression, and low-level file writes.
- `--decode-only` decodes MP3 frames and skips PCM conversion plus all output.
  The output path argument is optional in this mode.
- `--no-output` runs PCM conversion/downsampling and 8SVX/Fibonacci compression
  paths but discards bytes instead of touching an output file.  The output path
  argument is optional in this mode.
- `--rate 22050`, `--rate 11025`, or `--rate 8287` post-decode downsamples the
  output with a lightweight nearest-sample decimator when the MP3 sample rate is
  higher than the requested output rate.
- `--fast-lowrate` is an experimental, lower-quality Amiga conversion mode for
  speed experiments, not hi-fi playback. It requires one of the `--rate` values
  above. In `AMIGA_M68K` + `AMIGA_FAST_POLYPHASE` builds it writes only every
  second polyphase output sample for 22050 Hz, every fourth for 11025 Hz, and
  roughly every fifth for 8287 Hz. This skips discarded polyphase sample work,
  but Huffman/dequant, IMDCT, and FDCT32 still run at full MP3 rate; `--bench`
  reports those unchanged buckets.
- `--selftest-mulshift` compares the portable C `MULSHIFT32` reference with the
  optional 68020+ assembly helper over edge cases and 100,000 pseudo-random
  input pairs.
- `--checksum` prints a 32-bit checksum of the decoded 16-bit PCM stream before
  optional mixing, downsampling, or output-format conversion. With
  `--fast-lowrate`, it instead covers the low-rate output samples so experiments
  can verify deterministic fast-lowrate output. Use it to compare the default
  polyphase path with optional optimized builds.
- `--debug-argv` or `--show-argv` prints `argc` and `argv` after Amiga
  command-tail normalization.  The normalizer also handles Amiga C runtimes
  that pass the whole command tail as one argument, including CR/LF
  whitespace and quoted paths.

The program prints the first decoded frame's sample rate, channel count, and
bitrate when available, followed by decoded frame count and output sample count.

## Optimization roadmap

1. Keep the C helper block as the reference backend for 68000 and non-GCC builds.
2. Benchmark with `--bench --decode-only`, `--bench --no-output`, and normal
   output on target hardware to separate frame decode, conversion/compression,
   and filesystem cost.
3. `AMIGA_M68K_ASM` currently optimizes only `MULSHIFT32` with optional 68020+
   `muls.l` inline assembly.  Build and prove it before touching additional
   helpers:

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_M68K_ASM -Ipub -Ireal \
     -o amiga_mp3dec.asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.asm --selftest-mulshift
   ```

4. `AMIGA_FAST_POLYPHASE` is an opt-in Amiga/m68k polyphase synthesis path
   for 68020+ builds.  It keeps the original implementation available when the
   flag is omitted, but replaces the 64-bit polyphase accumulator with 32-bit
   fixed-point high-multiply terms to reduce 68030 inner-loop overhead:

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   ```

5. Compare default and `AMIGA_FAST_POLYPHASE` builds with identical inputs,
   output modes, and checksum reporting:

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -Ipub -Ireal \
     -o amiga_mp3dec.c-ref amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.c-ref --bench --decode-only --checksum song.mp3
   amiga_mp3dec.fastpoly --bench --decode-only --checksum song.mp3
   amiga_mp3dec.c-ref --bench --no-output --fibdelta --rate 22050 --checksum song.mp3
   amiga_mp3dec.fastpoly --bench --no-output --fibdelta --rate 22050 --checksum song.mp3
   ```

6. Check final Amiga binaries for libgcc 64-bit helper calls before measuring:

   ```sh
   m68k-amigaos-nm -u amiga_mp3dec.asm | egrep '__muldi3|__ashrdi3|__lshrdi3'
   ```

   `MULSHIFT32` should not add those calls when `AMIGA_M68K_ASM` is enabled.
   `AMIGA_FAST_POLYPHASE` should remove the polyphase dependency on `MADD64` and
   `SAR64`; any remaining helper symbols should be attributed to non-polyphase
   paths before investigating them one at a time.
