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
amiga_mp3dec --play [--stereo] [--rate 8287|8820|11025|22050] [--buffer-seconds N] infile.mp3
amiga_mp3dec --play-lifecycle-test [--debug-play] [--buffer-seconds N]
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
  compression, and low-level file writes. In `--play` mode it also prints the
  audio-device underrun count. Playback mode always prints underruns at exit so
  target runs can show whether streaming kept up.
- `--play` is an experimental AmigaOS Paula streaming mode for CD32/TF330-style
  68030 testing. It opens `audio.device`, decodes to mono signed 8-bit PCM, and
  streams with two chip-memory buffers. The default playback rate is 8287 Hz for
  030 safety; `--rate 8820` and `--rate 11025` are accepted, and `--rate 22050`
  is also accepted as an experimental/high-CPU mono-first mode that may underrun
  on 030 systems. Playback rates imply `--fast-lowrate`; 22050 Hz fast-lowrate
  playback prints `22050 requires significantly more CPU and may underrun on
  030 systems.`
  Playback prints the requested and actual output rates when fixed stride output
  differs, and calculates the PAL audio period from the actual output rate using
  rounded `3546895 / actual_output_rate` ticks, so 22050 Hz uses period 161.
  Playback automatically uses the reduced-
  overhead fast path: checksums run only with `--checksum`, timing buckets and
  decode-core profiling run only with `--bench`, and export/8SVX/Fibonacci state
  is not touched while streaming to `audio.device`.
- `--stereo` is opt-in only and applies only with `--play`. It keeps the default
  mono path unchanged, writes signed 8-bit samples per channel, preserves decoded
  left/right channels for stereo MP3 input where possible, and duplicates mono
  MP3 input to both output channels. Stereo playback opens separate
  `audio.device` allocations for one left Paula channel and one right Paula
  channel, deinterleaving the playback buffers before submission. Stereo supports
  `--rate 8820` and `--rate 11025` first; `--rate 22050` is allowed only as an
  experimental/high-CPU stereo mode. `--rate 8287` is mono-only. Enabling stereo
  prints `Stereo playback needs significantly more CPU and may underrun on 030.`
- `--play-fast-path` is accepted as an explicit alias for `--play`; the normal
  `--play` mode already uses this reduced-overhead streaming path.
- `--buffer-seconds N` chooses the requested playback depth for each half of the
  `--play` double buffer; the default is 4 seconds for safer 030 playback. Values
  must be positive integers; values above 10 seconds are clamped to 10 seconds.
  Mono playback submits its double buffers
  directly to `audio.device`, so those buffers must be chip memory. Stereo
  playback keeps the interleaved decode work buffers in normal RAM where
  possible and uses chip memory only for the deinterleaved Paula left/right
  submission buffers. If the requested 22050 Hz or stereo buffer set is too
  large for available memory, playback automatically retries with smaller
  half-buffers and prints the reduced byte count instead of failing immediately.
  Playback prints the selected half-buffer duration and byte size at startup, and
  reports total underruns, per-buffer underruns, late-buffer count, and the
  minimum measured spare time before a playing buffer ended at exit.
- `--debug-play` prints startup diagnostics for Paula streaming, including the
  actual output rate, PAL period, requested buffer depth, selected half-buffer
  samples/bytes, chip submission buffer addresses/sizes, optional stereo work
  buffer addresses/sizes, buffer A/B fill samples/bytes, every A/B `CMD_WRITE`
  submit/complete milestone, underrun detections, and final cleanup counts for
  completed/aborted outstanding I/O, freed buffers, and closed audio devices. The
  streaming startup path
  allocates the playback buffers before pre-filling both A and B by decoded
  sample count (not amplitude), queues both non-empty buffers before rotation,
  refills the completed half while the queued half is playing, and never waits on
  an audio I/O request that has not been submitted. A silent first
  playback buffer is accepted so valid MP3 encoder delay, padding, or fade-ins
  can play normally; with `--debug-play`, an all-zero first buffer prints
  `first playback buffer is silent/near-silent`. Playback does not skip leading
  silence by default.
- `--decode-then-play` is a `--play` debug mode that decodes the whole MP3 to
  RAM as signed 8-bit PCM first (mono by default, or stereo with `--stereo`), then plays the resulting buffer via
  `audio.device`, which helps separate decoder/streaming issues from playback
  issues.
- `--play-lifecycle-test` opens playback, allocates the playback buffers, submits
  a short silent `CMD_WRITE`, cleans up, and repeats the sequence three times.
  Use it on real hardware with `--debug-play` to verify that repeated
  `audio.device` open/submit/abort-or-wait/close and buffer free cycles leave no
  stale requests or reply messages before testing longer MP3 streams.
- `--decode-only` decodes MP3 frames and skips PCM conversion plus all output.
  The output path argument is optional in this mode.
- `--no-output` runs PCM conversion/downsampling and 8SVX/Fibonacci compression
  paths but discards bytes instead of touching an output file.  The output path
  argument is optional in this mode.
- `--rate 22050`, `--rate 11025`, `--rate 8820`, or `--rate 8287` post-decode downsamples the
  output with a lightweight nearest-sample decimator when the MP3 sample rate is
  higher than the requested output rate.
- `--fast-lowrate` is an experimental, lower-quality Amiga conversion mode for
  speed experiments, not hi-fi playback. It requires one of the `--rate` values
  above. In `AMIGA_M68K` + `AMIGA_FAST_POLYPHASE` builds it writes only every
  second polyphase output sample for 22050 Hz, every fourth for 11025 Hz, and
  every fifth for 8820/8287 Hz. This skips discarded polyphase sample work,
  appends emitted samples through one cumulative low-rate output counter, and
  keeps the low-rate phase/stride state alive across granules and MP3 frames.
  For exact integer strides such as 44100 -> 11025, fast-lowrate selects the
  same source positions as normal `--rate` decimation. The 8287 Hz mode uses a
  fixed stride of 5 for Amiga-rate experiments, so 44100 Hz input emits at
  8820 Hz and reports/plays/writes metadata at that actual emitted rate instead
  of labeling it as the requested 8287 Hz. Huffman/dequant, IMDCT, and FDCT32
  still run at full MP3 rate for mono input; stereo input with `--mono` is
  collapsed in the decoder after required MPEG stereo reconstruction, so the
  right-channel IMDCT/FDCT32/polyphase work and full stereo PCM copy are skipped.
  `--bench` reports the huffman, dequant, stereo/post, imdct, subband/dct32,
  and polyphase buckets used to profile that path.
- `--debug-fastlowrate` prints one line per decoded frame/granule with the
  full-rate sample count, low-rate samples emitted, cumulative low-rate samples,
  and destination offset range used for contiguous placement.
- `--selftest-mulshift` compares the portable C `MULSHIFT32` reference with the
  optional 68020+ assembly helper over edge cases and 100,000 pseudo-random
  input pairs.
- `--selftest-fdct32` compares `FDCT32_C_REFERENCE` against the normal `FDCT32`
  entry point, so `AMIGA_M68K_ASM_FDCT32` builds can prove the optional asm
  multiply path preserves the C operation order and fixed-point outputs.
- `--selftest-imdct` compares the C IMDCT36 reference with the active IMDCT
  entry point over zero, random, edge-value, common long-window, and fallback
  window cases.
- `--selftest-fastlowrate` compares a synthetic ramp/impulse-like PCM sequence
  through normal 44100 -> 11025 `--rate` decimation and the stride-4
  fast-lowrate selector across chunk boundaries.
- `--checksum` prints a 32-bit checksum of the decoded 16-bit PCM stream before
  optional mixing, downsampling, or output-format conversion. With
  `--fast-lowrate`, it instead covers the low-rate output samples so experiments
  can verify deterministic fast-lowrate output. Use it to compare the default
  polyphase path with optional optimized builds.
- `--debug-argv` or `--show-argv` prints `argc` and `argv` after Amiga
  command-tail normalization.  The normalizer also handles Amiga C runtimes
  that pass the whole command tail as one argument, including CR/LF
  whitespace and quoted paths.

The program prints the first decoded frame's input sample rate, output sample
rate when it differs, channel count, and bitrate when available, followed by
decoded frame count and output sample count.

## Optimization roadmap

1. Keep the C helper block as the reference backend for 68000 and non-GCC builds.
2. Benchmark with `--bench --decode-only`, `--bench --no-output`, and normal
   output on target hardware to separate frame decode, conversion/compression,
   and filesystem cost.
3. `AMIGA_M68K_ASM` currently optimizes `MULSHIFT32` with optional 68020+
   `muls.l` inline assembly.  Build and prove it before touching additional
   helpers:

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_M68K_ASM -Ipub -Ireal \
     -o amiga_mp3dec.asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.asm --selftest-mulshift
   ```

4. `AMIGA_M68K_ASM_FDCT32` is an opt-in, exact FDCT32 arithmetic path for
   68020+ GNU m68k builds.  It keeps `FDCT32_C_REFERENCE` callable and routes
   the normal `FDCT32` entry point through an operation-order-preserving copy of
   the C transform that replaces only the 32x32 high multiply with `muls.l`.
   This flag is deliberately disabled by default; leave it disabled if any
   checksum or `--selftest-fdct32` comparison differs from the C reference.

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_M68K_ASM_FDCT32 -Ipub -Ireal \
     -o amiga_mp3dec.fdct32asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.fdct32asm --selftest-fdct32
   ```


5. `AMIGA_M68K_ASM_IMDCT` is an opt-in exact long-block IMDCT36 path for
   68020+ GNU m68k builds.  The C IMDCT remains the reference; the active
   entry point uses the asm path only for the common long-window case and
   falls back to C for short blocks, mixed/transition windows, start/stop
   windows, and anything else uncommon.  This flag is disabled by default;
   keep it disabled if `--selftest-imdct` or any required checksum differs.

   ```sh
   m68k-amigaos-gcc -m68030 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_FAST_POLYPHASE \
     -DAMIGA_M68K_ASM_FDCT32 -DAMIGA_M68K_ASM_IMDCT -Ipub -Ireal \
     -o amiga_mp3dec.imdctasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.imdctasm --selftest-imdct
   ```

6. Checksum the C and ASM FDCT32 builds with identical inputs and output modes
   before enabling the ASM binary in a release or local deployment.  The
   required regression set is: mono 56 kbps, stereo 160 kbps, stereo 256 kbps,
   fast-lowrate 11025 Hz, and fast-lowrate 8820 Hz.

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -Ipub -Ireal \
     -o amiga_mp3dec.c-ref amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_M68K_ASM_FDCT32 -Ipub -Ireal \
     -o amiga_mp3dec.fdct32asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c

   amiga_mp3dec.c-ref --bench --decode-only --checksum mono-56.mp3
   amiga_mp3dec.fdct32asm --bench --decode-only --checksum mono-56.mp3
   amiga_mp3dec.c-ref --bench --decode-only --checksum stereo-160.mp3
   amiga_mp3dec.fdct32asm --bench --decode-only --checksum stereo-160.mp3
   amiga_mp3dec.c-ref --bench --decode-only --checksum stereo-256.mp3
   amiga_mp3dec.fdct32asm --bench --decode-only --checksum stereo-256.mp3
   amiga_mp3dec.c-ref --bench --no-output --fast-lowrate --rate 11025 --checksum stereo-160.mp3
   amiga_mp3dec.fdct32asm --bench --no-output --fast-lowrate --rate 11025 --checksum stereo-160.mp3
   amiga_mp3dec.c-ref --bench --no-output --fast-lowrate --rate 8820 --checksum stereo-160.mp3
   amiga_mp3dec.fdct32asm --bench --no-output --fast-lowrate --rate 8820 --checksum stereo-160.mp3
   ```

   On a 68030, compare `elapsed seconds`, `decode speed`, and, when built with
   `AMIGA_PROFILE_DECODE`, `timing core subband/dct32` between the C reference
   and ASM binaries.  If any PCM checksum differs, do not define
   `AMIGA_M68K_ASM_FDCT32` for the default build.

7. `AMIGA_FAST_POLYPHASE` is an opt-in Amiga/m68k polyphase synthesis path
   for 68020+ builds.  It keeps the original implementation available when the
   flag is omitted, but replaces the 64-bit polyphase accumulator with 32-bit
   fixed-point high-multiply terms to reduce 68030 inner-loop overhead:

   ```sh
   m68k-amigaos-gcc -m68020 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   ```

8. Compare default and `AMIGA_FAST_POLYPHASE` builds with identical inputs,
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

8. Check final Amiga binaries for libgcc 64-bit helper calls before measuring:

   ```sh
   m68k-amigaos-nm -u amiga_mp3dec.asm | egrep '__muldi3|__ashrdi3|__lshrdi3'
   ```

   `MULSHIFT32` should not add those calls when `AMIGA_M68K_ASM` is enabled.
   `AMIGA_FAST_POLYPHASE` should remove the polyphase dependency on `MADD64` and
   `SAR64`; any remaining helper symbols should be attributed to non-polyphase
   paths before investigating them one at a time.
