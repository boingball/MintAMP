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
m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -Ipub -Ireal \
  -o amiga_mp3dec amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
```

Or use the checked-in convenience makefile so the include paths and `-D` flags
stay separated correctly:

```sh
make -f Makefile.amiga
```

For the experimental 68030 fast build, use:

```sh
make -f Makefile.amiga fast030
```

The equivalent expanded command is:

```sh
m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
  -Ipub -Ireal \
  -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_M68K_ASM_FDCT32 \
  -DAMIGA_FAST_POLYPHASE -DAMIGA_M68K_ASM_POLYPHASE \
  -DAMIGA_M68K_ASM_IMDCT -DAMIGA_M68K_ASM_MIDSIDE \
  -DAMIGA_M68K_ASM_HUFFMAN \
  -o amiga_mp3dec.fastexp amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c \
  real/amiga_m68k_polyphase.S
python3 tools/amiga_fast_preferred_hunks.py amiga_mp3dec.fastexp
```

The optional Huffman m68k ASM refill path is runtime-gated.  It is included in
the quality 0 preset and remains available as `--exp-huff` for targeted
profiling; builds without `AMIGA_M68K_ASM_HUFFMAN` silently stay on the portable
C Huffman decoder.

Keep a space between every `-D...` define and every `-I...` include path.  For
example, `-DAMIGA_M68K_ASM_MIDSIDE-Ipub` is parsed as one malformed macro
definition, so the compiler never receives the `pub` include path and then fails
with missing `mp3dec.h`/`mp3common.h` errors.  Likewise,
`DAMIGA_M68K_ASM_IMDCT` without the leading `-D` is treated as an input file name
instead of a preprocessor define.

After linking, `Makefile.amiga` runs `tools/amiga_fast_preferred_hunks.py` on the
Amiga executable.  The tool clears explicit chip-only or fast-only allocation
bits in the load-file hunk table, leaving the normal AmigaDOS "any memory, prefer
Fast RAM" setting.  This makes the program code/data load into Fast RAM when Fast
RAM is available while preserving compatibility with chip-only systems.


`--exp-poly` selects the additional experimental 68030 mono polyphase
assembly path in `AMIGA_M68K_ASM_POLYPHASE` builds when
`real/amiga_m68k_polyphase.S` is linked.  If the macro is set but the optional
assembly source is omitted, the weak asm reference resolves as unavailable and
the decoder falls back to the existing `AMIGA_FAST_POLYPHASE` C path instead of
failing at link time.  Without the runtime argument, the older experimental
polyphase remains in use so target profiling can compare both variants.

The command-line decoder embeds an AmigaOS `$STACK:250000` cookie, requesting a
minimum 250,000-byte stack without requiring users to run the `Stack` command
first.

For a native smoke build on a non-Amiga host, use a portable backend define such
as `-D__riscv` only if your compiler supports the helper assembly for that
architecture; otherwise build with `-DAMIGA_M68K` to exercise the plain-C
fallback.

On a real 68020+ target, the Huffman cache's full 16-bit refill uses one
potentially unaligned `move.w` through `LOADBE16`.  No `REV16` byte swap is
needed because m68k is already big-endian.  68000/68010 and non-m68k smoke
builds retain the safe two-byte C fallback.

### Playback cleanup diagnostics

Playback resources are owned by one audio-player lifecycle and released through a
single cleanup path. `--debug-cleanup` reports reaped/aborted writes, device and
Exec object deletion, Chip/work-buffer release, debug-build canary checks, and
input-file closure. `MINIAMP3_DEBUG` builds also emit AmigaDOS `Printf()`
cleanup trace lines for each submitted slot/channel, including `CheckIO`,
`AbortIO`, `WaitIO`, device close, request deletion, buffer frees, and cleanup
invocation count. `--selftest-play-cleanup` repeats a tiny silent
`audio.device` submission and complete teardown five times; the older
`--play-lifecycle-test` spelling remains as an alias.

The playback implementation does not call `CurrentDir()`, `Lock()`, `Forbid()`,
`Permit()`, `Disable()`, or `Enable()`, so playback cleanup does not own a current
directory lock or interrupt/task-switch nesting state.


## Minimal GadTools GUI build

A small Workbench 2.x/3.x GadTools frontend is available as `miniamp3`. It links
the same decoder object files as the command-line tool, so the decoder does not
have to be converted into a separate static library first:

```sh
make -f Makefile.amiga gui
# or
make -f Makefile.amiga miniamp3
```

`miniamp3` opens the MiniAMP3 window with an ASL file requester for choosing an
MP3, checkboxes for the fast-lowrate, fast-mem, and mono playback flags, sample
rate and quality cycle gadgets, a 1-30 second buffer slider, a read-only file
path display, a status bar, and Play/Stop buttons. The GUI uses
`gadtools.library` through the AmigaOS m68k pragmas, so no `-lauto` linker flag
is needed.

The quality cycle maps to the same playback code used by `amiga_mp3dec`:

- **Fast** enables the explicit `--play-fast-path` alias for the lowest-overhead
  playback setup.
- **Normal** and **Best** leave the memory preload choice to the Fast-mem
  checkbox, so unticking Fast-mem is always obeyed.

The buffer slider chooses the `--buffer-seconds` value from 1 to 30 seconds, and
the rate selector cycles through 8287, 8820, 11025, 22050, and 28600 Hz. Playback still
uses the Paula streaming implementation inside `amiga_mp3dec`. The GUI launches
the playback subprocess at normal priority so CPU-bound decoding does not starve
the Workbench/GadTools event loop. Stop requests set the same interrupt flag
used by Shell playback, signal the child,
and the audio wait path can abort/reap outstanding writes so the GUI remains
responsive. During playback the status bar keeps the change-only
`Playing - underruns: N` text visible instead of redrawing generic `Playing` on
each timer tick.

A separate library can still be added later if multiple frontends need to share
higher-level application code. For now, `amiga_mp3gui.c` includes the existing
CLI frontend with its `main` renamed internally, keeping a single compiled GUI
source while reusing the public decoder and Paula playback implementation.

## Usage

```sh
amiga_mp3dec [options] infile.mp3 outfile
amiga_mp3dec --info infile.mp3
amiga_mp3dec --play [--stereo] [--rate 8287|8820|11025|22050|28600] [--quality 0|1|2|3] [--buffer-seconds N] [--fast-mem] infile.mp3
amiga_mp3dec --selftest-play-cleanup [--debug-cleanup] [--buffer-seconds N]
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
- `--info` prints the input file size, ID3v2 text metadata and embedded-artwork
  size, first MPEG audio frame details, and ID3v1 metadata when present. Used as
  `--info infile.mp3`, it exits after inspection without decoding. It can also
  be combined with `--play` (or a normal decode command) to print the metadata
  before playback or decoding begins.
- `--play` is an experimental AmigaOS Paula streaming mode for CD32/TF330-style
  68030 testing. It opens `audio.device`, decodes to mono signed 8-bit PCM into
  Fast RAM work buffers, and bulk-copies each completed half-buffer into the
  chip-memory buffers submitted to `audio.device`. The default playback rate is
  8287 Hz for 030 safety; `--rate 8820` and `--rate 11025` are accepted,
  `--rate 22050` is accepted as an experimental/high-CPU mono-first mode that
  may underrun on 030 systems, and `--rate 28600` selects the PAL-top Paula
  mode. Playback rates up to 22050 Hz imply `--fast-lowrate`; 28600 Hz uses normal
  post-decode decimation because it is not an integer fast-lowrate stride. 22050 Hz fast-lowrate
  playback prints `22050 requires significantly more CPU and may underrun on
  030 systems.`
  Playback prints the requested and actual output rates when fixed stride output
  differs, and calculates the PAL audio period from the actual output rate using
  rounded `3546895 / actual_output_rate` ticks, so 22050 Hz uses period 161 and 28600 Hz uses period 124.
  Playback automatically uses the reduced-
  overhead fast path: checksums run only with `--checksum`, timing buckets and
  decode-core profiling run only with `--bench`, and export/8SVX/Fibonacci state
  is not touched while streaming to `audio.device`.
- `--stereo` is opt-in only and applies only with `--play`. It keeps the default
  mono path unchanged, writes signed 8-bit samples per channel, preserves decoded
  left/right channels for stereo MP3 input where possible, and duplicates mono
  MP3 input to both output channels. Streaming stereo playback opens separate
  `audio.device` allocations for one left Paula channel and one right Paula
  channel, converts decoded interleaved signed 16-bit PCM into planar signed
  8-bit Fast RAM work buffers, and uses one bulk copy per channel into the
  Paula chip-memory submission buffers when each half-buffer is submitted. Stereo
  supports `--rate 8820` and `--rate 11025` first; `--rate 22050` is allowed
  as an experimental/high-CPU stereo mode, and `--rate 28600` is available as
  the PAL-top Paula mode. `--rate 8287` is mono-only.
  Enabling stereo prints `Stereo playback needs significantly more CPU and may
  underrun on 030.`
- `--play-fast-path` is accepted as an explicit alias for `--play`; the normal
  `--play` mode already uses this reduced-overhead streaming path.
- `--buffer-seconds N` chooses the requested playback depth for each half of the
  `--play` double buffer; the default is 4 seconds for safer 030 playback. Values
  must be positive integers; values above 10 seconds are clamped to 10 seconds.
  Playback now keeps separate Fast RAM conversion buffers and chip-memory
  `audio.device` submission buffers; mono copies one completed half-buffer at
  submit time, while stereo copies the completed left and right planar buffers.
  If the requested 22050 Hz or stereo buffer set is too large for available
  memory, playback automatically retries with smaller
  half-buffers and prints the reduced byte count instead of failing immediately.
  Playback prints the selected half-buffer duration and byte size at startup.
  Each Paula channel submission is capped at 65534 bytes, so high-rate settings
  such as 22050 Hz or 28600 Hz automatically use shorter half-buffers in both
  streaming playback and `--decode-then-play` instead of decoding far more audio
  than one `CMD_WRITE` can play. When this cap shortens the requested depth,
  playback prints the maximum half-buffer duration for the selected rate; for
  example, mono 28600 Hz is limited to about 2291 ms per half-buffer. Playback
  reports total underruns,
  per-buffer underruns, late-buffer count, and the
  minimum measured spare time before a playing buffer ended at exit.
- `--fast-mem` preloads the complete compressed MP3 into Fast RAM before decoding
  or playback starts. On AmigaOS builds it requests `MEMF_FAST`, so the input
  does not consume chip RAM needed by Paula buffers. MiniAMP3 disables the
  checkbox when the selected file will not fit in available Fast RAM. During
  startup it reports that it is copying the input to Fast RAM. This removes
  filesystem and slow-HDD reads from the realtime decode/refill loop and prints
  the allocated byte count at startup. The option fails before playback if the complete input
  cannot be sized, read, or allocated; it never silently falls back to disk.
  Unlike `--decode-then-play`, it stores only the compressed MP3 and continues to
  decode into the normal two-slot playback queue while Stop cleanup is
  being validated on real hardware, so it normally needs less RAM than
  `--decode-then-play`. It intentionally preloads synchronously: background HDD I/O on a
  CPU-limited 68030 could still steal decode time at unpredictable points.
  For slow disks, start with `--play --fast-mem`; if decode-time spikes can still
  exhaust the queued audio, also increase `--buffer-seconds` toward 10.
- MiniAMP3 GUI startup diagnostics are compiled out by default so release builds
  do not write `T:MiniAMP3-startup.log` or show internal startup stages. Developers
  can enable the detailed GUI startup log, watchdog text, task/process pointers,
  and embedded audio-open diagnostics by building with `-DMINIAMP3_DEBUG`, for
  example `make -f Makefile.amiga gui EXTRA_CFLAGS=-DMINIAMP3_DEBUG`.
- `--debug-play` prints startup diagnostics for Paula streaming, including the
  actual output rate, PAL period, requested buffer depth, selected half-buffer
  samples/bytes, chip submission buffer addresses/sizes, optional stereo work
  buffer addresses/sizes, buffer A/B fill samples/bytes, queued A/B `CMD_WRITE` startup, every A/B `CMD_WRITE`
  submit/complete milestone, underrun detections, and final cleanup counts for
  completed/aborted outstanding I/O, freed buffers, and closed audio devices. The
  streaming startup path
  allocates the playback buffers before pre-filling A, B, and C by decoded
  sample count (not amplitude), queues the non-empty three-buffer ring before
  rotation, decodes ahead into the most recently queued slot while Paula plays
  the current slot and keeps the next slot queued, then waits for the active
  slot and re-commits the freshly decoded buffer. This enabled three-buffer
  ordering gives the decoder a full half-buffer period of slack and never waits
  on an audio I/O request that has not been submitted. A silent first
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
- `--rate 28600`, `--rate 22050`, `--rate 11025`, `--rate 8820`, or `--rate 8287` post-decode downsamples the
  output with a lightweight nearest-sample decimator when the MP3 sample rate is
  higher than the requested output rate.
- `--fast-lowrate` is a lower-quality Amiga conversion mode for speed-oriented
  playback and conversion. It requires one of the `--rate` values
  above. In `AMIGA_M68K` + `AMIGA_FAST_POLYPHASE` builds it writes only every
  second polyphase output sample for 22050 Hz, every fourth for 11025 Hz, and
  every fifth for 8820/8287 Hz. This skips discarded polyphase sample work,
  appends emitted samples through one cumulative low-rate output counter, and
  keeps the low-rate phase/stride state alive across granules and MP3 frames.
  For exact integer strides such as 44100 -> 11025, fast-lowrate selects the
  same source positions as normal `--rate` decimation. The 8287 Hz mode uses a
  fixed stride of 5 for Amiga-rate experiments, so 44100 Hz input emits at
  8820 Hz and reports/plays/writes metadata at that actual emitted rate instead
  of labeling it as the requested 8287 Hz. For stride-2 22050 Hz output, the
  fast-polyphase build computes only the 16 even synthesis rows: a half-width
  FDCT path skips the unused factored-DCT half and polyphase evaluates only those
  16 output samples. The emitted PCM remains bit-identical to selecting every
  second sample from the full synthesis path. Huffman/dequant and IMDCT still
  run at full MP3 rate; stride-4/5 output still uses full FDCT32. Stereo input
  with `--mono` is collapsed in the decoder after required MPEG stereo
  reconstruction, so the
  right-channel IMDCT/FDCT32/polyphase work and full stereo PCM copy are skipped.
  Pure mid/side joint-stereo mono output also advances over the unused coded
  side-channel Huffman payload, removing a bitrate-sensitive decode cost.
  `--bench` reports the huffman, dequant, stereo/post, imdct, subband/dct32,
  and polyphase buckets used to profile that path.
- `--debug-fastlowrate` prints one line per decoded frame/granule with the
  full-rate sample count, low-rate samples emitted, cumulative low-rate samples,
  and destination offset range used for contiguous placement.

### Quality/speed tradeoff

`--quality N` selects one of the verified fast-path combinations while leaving
the individual `--exp-*` flags available for fine-grained override. If
`--quality` is omitted, `--fast-lowrate --rate 11025` defaults to quality 1; all
other invocations default to quality 3.

- `--quality 0` (fastest) enables reduced taps, quarter-rate FDCT32, fast
  polyphase, and the optional Huffman ASM refill path. The
  Huffman shortcut is limited to this level because its ASM path has known
  bit-accounting edge cases; in builds without the Huffman m68k ASM path it
  silently falls back to the normal C Huffman decoder.
- `--quality 1` (fast) enables reduced taps and fast polyphase. This is the
  automatic default for `--fast-lowrate --rate 11025`.
- `--quality 2` (balanced) enables fast polyphase without reduced taps.
- `--quality 3` (accurate) enables no approximations and is equivalent to the
  original decoder behavior.

For 68030 realtime playback of 192 kbps and higher content, start with:

```sh
amiga_mp3dec --quality 0 --play --fast-mem --fast-lowrate --rate 11025 --mono song.mp3
```

- `--selftest-mulshift` compares the portable C `MULSHIFT32` reference with the
  optional 68020+ assembly helper over edge cases and 100,000 pseudo-random
  input pairs.
- `--selftest-clz` compares the portable C `CLZ` reference with the active
  helper over zero, one, every power of two, `0x7fffffff`, `0xffffffff`, and
  10,000 pseudo-random inputs.  In `AMIGA_M68K_ASM` 68020+ builds this proves
  the inline `bfffo` helper against the C reference.
- `--selftest-fdct32` compares `FDCT32_C_REFERENCE` against the normal `FDCT32`
  entry point, so `AMIGA_M68K_ASM_FDCT32` builds can prove the optional asm
  multiply path preserves the C operation order and fixed-point outputs.
- `--selftest-imdct` compares the C IMDCT36 reference with the active IMDCT
  entry point over zero, random, edge-value, common long-window, and fallback
  window cases.
- `--selftest-polyphase` compares the C fast mono polyphase path with the active
  mono polyphase entry point, so `AMIGA_M68K_ASM_POLYPHASE` builds can prove the
  optional 68030 assembly kernel.  `--exp-poly` no longer runs this selftest
  automatically; use this standalone flag when you want to re-verify the asm
  kernel.
- `--selftest-polyphase-stride2` compares the C stride-2 mono fast-lowrate
  polyphase selection with the optional contiguous-output m68k assembly kernel.
- `--selftest-polyphase-stride4` compares the C stride-4 mono fast-lowrate
  polyphase selection with the optional 11025 Hz m68k assembly kernel over 500
  pseudo-random vbuf states.
- `--selftest-polyphase-stride4-stereo` compares the stereo stride-4 compact
  fast-lowrate polyphase kernel with the generic stereo sample selection over
  every phase and 500 pseudo-random vbuf states.
- `--selftest-fastlowrate` compares a synthetic ramp/impulse-like PCM sequence
  through normal 44100 -> 11025 `--rate` decimation and the stride-4
  fast-lowrate selector across chunk boundaries.
- `--selftest-huffman` runs 1000 pseudo-random Huffman pair decode cases
  through the portable C reference and active test path to verify stable bit
  counts and coefficients.  `--exp-huff` enables the optional
  `AMIGA_M68K_ASM_HUFFMAN` decode path at runtime; it keeps the C Huffman state
  machine and bit extraction logic, but uses inline m68k refill code in the hot
  pair loops when compiled for 68020+.
- `--selftest-dequant` compares the C dequant block reference with the active
  optional 68030 dequant inner loop for scale values -47..0 and coefficient
  magnitudes 0..8206.
- `--selftest-quality` verifies the `--quality 0` through `--quality 3` flag
  combinations, explicit `--exp-*` override preservation, and the automatic
  `--fast-lowrate --rate 11025` quality 1 default.
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
   `muls.l` inline assembly, `CLZ` with optional 68020+ `bfffo` inline
   assembly, and the hot 4-byte bitstream refill with an optional 68020+
   `move.l (%a0)+,%d0` inline load.  Build and prove these helpers before
   enabling deeper asm paths:

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_M68K_ASM -Ipub -Ireal \
     -o amiga_mp3dec.asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.asm --selftest-mulshift
   amiga_mp3dec.asm --selftest-clz
   amiga_mp3dec.asm --selftest-bitstream
   ```

   Upstream GCC's m68k backend has a `clzsi2` pattern that emits `bfffo` when
   targeting 68020+ bit-field instructions, and marks CLZ-at-zero as 32 because
   `bfffo`/ColdFire `ff1` return 32 for zero.  This workspace did not include
   `m68k-amigaos-gcc`, so the local command-line selftest keeps the inline asm
   explicit until the exact Amiga cross-compiler build is disassembled.  Verify
   the installed compiler with:

   ```sh
   printf 'int f(unsigned x){return x?__builtin_clz(x):32;}\n' | \
     m68k-amigaos-gcc -m68020 -O2 -S -xc - -o - | grep bfffo
   ```

   If that command emits `bfffo`, a future pass can remove the handwritten
   `CLZ_AMIGA_M68K_ASM` wrapper and rely on the builtin guarded by the same zero
   check.

4. `AMIGA_M68K_ASM_FDCT32` is an opt-in, exact FDCT32 arithmetic path for
   68020+ GNU m68k builds, tuned for the 68030.  It keeps `FDCT32_C_REFERENCE`
   callable and routes the normal `FDCT32` entry point through an
   operation-order-preserving transform.  The fully unrolled first radix-4
   pass is one register-scheduled machine-code region: butterfly values remain
   in data registers, coefficients stream through an address register, and
   `muls.l` high words feed the next operation without C compiler spill/reload
   boundaries.  The second pass currently uses a compact four-iteration
   assembly kernel with stable data-register roles rather than four unrolled
   copies.  That keeps code size down, but it is not assumed to be fastest on
   a 68030: benchmark a fully unrolled variant on the target too, because
   eliminating loop and branch overhead may outweigh the extra code size.  The
   two output-shuffle halves use bounded assembly regions that retain each
   reused sum while issuing the paired stores directly.  The rare guard-bit
   clipping/scaling pass remains in C.  This flag is
   deliberately disabled by default; leave it disabled if any checksum or
   `--selftest-fdct32` comparison differs from the C reference.

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_M68K_ASM_FDCT32 -Ipub -Ireal \
     -o amiga_mp3dec.fdct32asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.fdct32asm --selftest-fdct32
   ```


5. `AMIGA_M68K_ASM_IMDCT` is an opt-in exact long-block IMDCT36 path for
   68020+ GNU m68k builds.  The C IMDCT remains the reference; the active
   entry point uses a compact nine-iteration asm window/overlap kernel only
   for the common long-window case and falls back to C for short blocks,
   mixed/transition windows, start/stop windows, and anything else uncommon.
   This flag is disabled by default; do not enable it by default unless real
   mono plus stereo/high-bitrate target benchmarks improve and
   `--selftest-imdct` plus every required checksum remain identical.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_FAST_POLYPHASE \
     -DAMIGA_M68K_ASM_FDCT32 -DAMIGA_M68K_ASM_IMDCT -Ipub -Ireal \
     -o amiga_mp3dec.imdctasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.imdctasm --selftest-imdct
   ```

6. `AMIGA_M68K_ASM_POLYPHASE` is an opt-in, experimental 68030 mono fast
   polyphase kernel.  The C fast mono path remains callable as the reference;
   `--selftest-polyphase` compares it against the active asm hook, and
   `--exp-poly` reruns that check automatically before playback or decode
   selects the assembly path.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_FAST_POLYPHASE \
     -DAMIGA_M68K_ASM_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.polyasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c \
     real/amiga_m68k_polyphase.S
   amiga_mp3dec.polyasm --selftest-polyphase
   ```

7. `AMIGA_M68K_ASM_MIDSIDE` is an opt-in 68020+ GNU m68k implementation of
   the joint-stereo mid/side reconstruction loop.  It keeps both channel
   pointers and guard-bit masks in registers, uses `dbf` for the bounded
   sample loop, and performs each absolute value with a branchless
   `asr`/`eor`/`sub` sequence.  The shift count is held in a data register
   because m68k immediate shifts cannot encode 31.  Compare PCM checksums and
   benchmark stereo joint-stereo inputs on the target before enabling it by
   default.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K_ASM_MIDSIDE -Ipub -Ireal \
     -o amiga_mp3dec.midsideasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.midsideasm --bench --decode-only --checksum stereo-joint.mp3
   ```


8. `AMIGA_M68K_ASM_HUFFMAN` is an opt-in experimental Huffman-pair refill
   shortcut for 68020+ GNU m68k builds, tuned for 68030.  It deliberately does
   not add a `.S` Huffman decoder and does not use `bfextu`; the C Huffman
   state machine remains authoritative while the hot pair-loop refill can use
   inline `move.l` on buffers with at least four safe bytes remaining, falling
   back to the existing 16-bit refill near the tail.  It is also gated at
   runtime by `--exp-huff`, so compiled-in support is inert unless explicitly
   requested.  Validate with `--selftest-huffman` and compare `--checksum`
   output against a reference decode before using it for playback benchmarks.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_M68K_ASM_HUFFMAN -Ipub -Ireal \
     -o amiga_mp3dec.huffasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.huffasm --selftest-huffman
   amiga_mp3dec.huffasm --exp-huff --bench --decode-only --checksum song.mp3
   ```

9. Checksum the C and ASM FDCT32 builds with identical inputs and output modes
   before enabling the ASM binary in a release or local deployment.  The
   required regression set is: mono 56 kbps, stereo 160 kbps, stereo 256 kbps,
   fast-lowrate 11025 Hz, and fast-lowrate 8820 Hz.

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -Ipub -Ireal \
     -o amiga_mp3dec.c-ref amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_M68K_ASM_FDCT32 -Ipub -Ireal \
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

10. `AMIGA_FAST_POLYPHASE` is an opt-in Amiga/m68k polyphase synthesis path
   for 68020+ builds.  It keeps the original implementation available when the
   flag is omitted, but replaces the 64-bit polyphase accumulator with 32-bit
   fixed-point high-multiply terms to reduce 68030 inner-loop overhead:

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   ```

11. Compare default and `AMIGA_FAST_POLYPHASE` builds with identical inputs,
   output modes, and checksum reporting:

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -Ipub -Ireal \
     -o amiga_mp3dec.c-ref amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.c-ref --bench --decode-only --checksum song.mp3
   amiga_mp3dec.fastpoly --bench --decode-only --checksum song.mp3
   amiga_mp3dec.c-ref --bench --no-output --fibdelta --rate 22050 --checksum song.mp3
   amiga_mp3dec.fastpoly --bench --no-output --fibdelta --rate 22050 --checksum song.mp3
   ```

12. Check final Amiga binaries for libgcc 64-bit helper calls before measuring:

   ```sh
   m68k-amigaos-nm -u amiga_mp3dec.asm | egrep '__muldi3|__ashrdi3|__lshrdi3'
   ```

   `MULSHIFT32` should not add those calls when `AMIGA_M68K_ASM` is enabled.
   `AMIGA_FAST_POLYPHASE` should remove the polyphase dependency on `MADD64` and
   `SAR64`; any remaining helper symbols should be attributed to non-polyphase
   paths before investigating them one at a time.
