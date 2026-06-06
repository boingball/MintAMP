# Amiga fast-lowrate optimization notes

## Target command

```sh
amiga_mp3dec_fastpoly --decode-only --bench --checksum --fast-lowrate --rate 11025 gs-16b-1c-44100hz.mp3
```

These passes are deliberately output-neutral: they change only fast-lowrate
selection, pointer movement, invariant setup, and avoided paired-side work for
samples that emit only one side of an existing polyphase pair. Coefficient
tables, clipping, emitted sample order, and the multiply/accumulator order for
each emitted sample are unchanged.

## FDCT32 second-pass and output-shuffle profile

This pass uses the already-confirmed first-pass `AMIGA_M68K_ASM_FDCT32` build
as its before baseline.  The transform's second pass currently uses one compact
four-iteration kernel instead of being unrolled four times, while the two
16-sample shuffle/store halves are separate bounded kernels.  The compact form
keeps code growth down, but a fully unrolled second pass must also be measured
on the 68030: avoiding the loop and branch overhead may be faster despite the
larger code footprint.  The rare clipping pass remains outside the assembly
fast path in either variant.

Use the same calibrated WinUAE configuration, fixture, and build flags for both
rows.  The workspace does not contain the required MP3 fixtures or a working
Amiga execution environment, so the after value must be recorded on that target;
do not treat a host timing as an FDCT32 result.

| Build | `timing core subband/dct32` | Overall speed | Required checksum |
| --- | ---: | ---: | --- |
| Before: first-pass ASM baseline | ~5.64 s | ~1.05x realtime | confirmed baseline |
| After: compact second pass + shuffle/store | _record on calibrated WinUAE_ | _record on calibrated WinUAE_ | must match before |

Profile and checksum both required fast-lowrate modes before accepting the
change:

```sh
amiga_mp3dec.fdct32.before --bench --no-output --checksum --fast-lowrate --rate 11025 --mono mono-56k-44100.mp3
amiga_mp3dec.fdct32.after  --bench --no-output --checksum --fast-lowrate --rate 11025 --mono mono-56k-44100.mp3
amiga_mp3dec.fdct32.before --bench --no-output --checksum --fast-lowrate --rate 11025 --mono stereo-44100.mp3
amiga_mp3dec.fdct32.after  --bench --no-output --checksum --fast-lowrate --rate 11025 --mono stereo-44100.mp3
```

For each pair, compare `timing core subband/dct32`, `decode speed`, PCM
checksum, and emitted sample count.  Reject the optimized build if either
checksum differs, even when `--selftest-fdct32` passes.

## Dedicated mono fixed-stride emitter pass

The mono `AMIGA_FAST_POLYPHASE` hot path now dispatches 11025 Hz stride-4 and
8820 Hz stride-5 blocks to dedicated emitters.  Each emitter writes the selected
samples in the same order as the previous phase/list walk, and calls a one-sided
low or high accumulator for paired positions.  It therefore does not calculate
the paired value that will not be emitted.  Stereo fast-lowrate, full-rate
polyphase, and normal `--rate` downsampling retain their existing paths.

The supplied target profiles for the comparison baseline are:

| Fixture/mode | Supplied before polyphase time | After |
| --- | ---: | ---: |
| mono 56 kbps, `--fast-lowrate --rate 11025 --mono` | ~6.5 s | _record on target_ |
| stereo input, `--fast-lowrate --rate 11025 --mono` | ~15.6 s | _record on target_ |

No MP3 fixtures or m68k cross-compiler are available in this workspace, so the
target after timings must be recorded on the Amiga build.  Use the confirmed
FDCT32 ASM build as the baseline; keep `AMIGA_M68K_ASM_IMDCT` omitted because
its profiling benefit remains unconfirmed:

```sh
amiga_mp3dec.fdct32 --decode-only --bench --checksum --fast-lowrate --rate 11025 --mono mono-56k-44100.mp3
amiga_mp3dec.fdct32 --decode-only --bench --checksum --fast-lowrate --rate 11025 --mono stereo-44100.mp3
amiga_mp3dec.fdct32 --decode-only --bench --checksum --fast-lowrate --rate 8820  --mono mono-56k-44100.mp3
```

For every fixture, compare the before/after PCM checksum and emitted sample
count before accepting the timing result.  Also checksum a full-rate decode and
a normal `--rate 11025` decode to confirm those unaffected paths remain exact.

### Compact-kernel follow-up

The fixed-stride mono emitters now share a compact eight-tap one-sided kernel
and phase/sample tables instead of using a switch containing an explicit call
for every emitted sample.  Stride 4 and stride 5 still have dedicated entry
points, calculate only selected samples, and preserve the multiply and
accumulator order for each selected low/high side.  This reduces the profiled
host executable text by 1,080 bytes and keeps the hot kernel small for the
68030 instruction cache.  Stereo fast-lowrate, full-rate synthesis, generic
fast-lowrate strides, and normal `--rate` remain on their existing paths.

The workspace has no m68k compiler/runtime and no mono MP3 fixture.  A repeated
44.1 kHz stereo fixture was therefore used only as a host-side checksum and
before/after timing smoke test for stereo-input-to-mono; final performance must
still be measured with both required fixtures on the target:

| Host fixture/mode | Before | Compact kernel | Checksum before/after |
| --- | ---: | ---: | --- |
| stereo input to mono, stride 4 | 1.715 s overall; 0.285 s polyphase | 1.697 s overall; 0.303 s polyphase | `53525b40` / `53525b40` |
| stereo input to mono, stride 5 | 1.734 s overall; 0.288 s polyphase | 1.678 s overall; 0.293 s polyphase | `d1a2a245` / `d1a2a245` |

Host timing is not predictive of 68030 performance.  The available short
fixture also confirmed unchanged checksums for full-rate mono output
(`70db07b7`) and the existing normal `--rate 11025` path (`70db07b7`).  A
10,000-block-per-stride randomized comparison against the pre-change kernels
produced the same aggregate checksum, `1e2da26d`.

## Before profile supplied for this pass

Build/profile: `AMIGA_PROFILE_DECODE + AMIGA_FAST_POLYPHASE + --fast-lowrate --rate 11025`.

| Bucket | Before elapsed |
| --- | ---: |
| polyphase | ~9.34 s |
| subband/dct32 | ~5.64 s |
| imdct | ~5.14 s |
| huffman | ~2.10 s |
| dequant | ~1.96 s |

## Loop-overhead changes made

- Fast-lowrate stride 4 and stride 5 now use precomputed phase-to-sample tables.
  This removes the per-output-block sample loop's phase test/modulo-style wrap
  from the common `--rate 11025` and `--rate 8287` paths.
- The fast-lowrate emitters now walk sample tables and output pointers directly,
  rather than recomputing `produced` indexes for each emitted mono/stereo sample.
- Subband now splits fast-lowrate and full-rate loops once per granule, hoists
  `stride`, `phase`, `fastLowrateOutputSamps`, and `vindex` into locals, and
  writes them back after the block loop.
- The DCT/polyphase block loop now computes the odd-block flag and active vbuf
  base once per block instead of repeating those expressions in each call site.


## One-sided paired-convolution pass

- The mono fast-lowrate sample helper now uses checksum-preserving one-sided
  variants of the paired `FAST_MC2` convolution when the selected low-rate
  sample is only one side of a pair. Samples below 16 compute only the low-side
  accumulator; samples above 16 compute only the high-side accumulator.
- The one-sided helpers copy the emitted side's coefficient loads, vbuf loads,
  multiply calls, and accumulator expression order from `FAST_MC2`; they only
  omit the paired accumulator whose PCM value is not emitted.
- Stereo fast-lowrate applies the same one-sided helpers independently to the
  left and right channels. Samples 0 and 16 remain on their existing special
  paths, and full-rate mono/stereo synthesis plus normal `--rate` downsampling
  remain unchanged.

## Checksum/benchmark log

The required MP3 fixture (`gs-16b-1c-44100hz.mp3`) was not present in this
workspace, so the exact before/after checksum and target timing lines must be
filled in on the Amiga benchmark machine that has the fixture. Local synthetic
verification compared mono fast-lowrate stride-4/stride-5 output against the
full fast-polyphase output selected at the same sample positions.

| Rate | Before checksum | After checksum | Before decode-only elapsed | After decode-only elapsed | Notes |
| --- | --- | --- | ---: | ---: | --- |
| 11025 | _record from pre-pass binary_ | _must match before_ | polyphase ~9.34 s from loop-overhead baseline | _record on target_ | stride-4 mono now avoids the unused side for paired samples 4, 8, 12, 20, 24, and 28 in phase 0 |
| 8287 | _record from pre-pass binary_ | _must match before_ | _record on target_ | _record on target_ | stride-5 mono now avoids unused paired sides for selected non-0/16 samples |

Suggested commands:

```sh
amiga_mp3dec_fastpoly.before --decode-only --bench --checksum --fast-lowrate --rate 11025 gs-16b-1c-44100hz.mp3
amiga_mp3dec_fastpoly.after  --decode-only --bench --checksum --fast-lowrate --rate 11025 gs-16b-1c-44100hz.mp3
amiga_mp3dec_fastpoly.before --decode-only --bench --checksum --fast-lowrate --rate 8287  gs-16b-1c-44100hz.mp3
amiga_mp3dec_fastpoly.after  --decode-only --bench --checksum --fast-lowrate --rate 8287  gs-16b-1c-44100hz.mp3
```

## `real/*.c` memset/memcpy audit

- `real/*.c` has no per-frame `memset`, `memcpy`, or `memmove` in the decode hot
  path.  The only local clearing helper is `ClearBuffer()` in `real/buffers.c`,
  used during decoder allocation/initialization, not per frame or granule.
- The per-frame bit reservoir movement lives in `mp3dec.c`: it preserves prior
  main-data bytes with `memmove()` only when `mainDataBegin > 0`, then appends
  new slot bytes with `memcpy()`.  That copy is format-required reservoir state
  and is not safe to remove without a ring-buffer rewrite.
- The Amiga front end uses `memmove()`/`memcpy()` for input-buffer compaction,
  rate-conversion scratch movement, and output formatting.  These are outside
  `real/*.c` and outside the requested decode-core loop pass; no safe reduction
  was made here.


## Stereo-to-mono synthesis optimization

This pass adds a decoder-level mono-output hint used by the Amiga command-line
front end whenever `--mono` is active.  Stereo and joint-stereo bitstream work is
still decoded normally, but after mandatory MPEG stereo reconstruction the
frequency-domain channels are collapsed to one mono channel before IMDCT/DCT32
and polyphase synthesis.  The full stereo PCM buffer and the later CLI
stereo-to-mono copy are therefore skipped for mono output.

For joint stereo with mid/side enabled and intensity stereo disabled, mono can be
collapsed more cheaply: `(L + R) / 2` after MPEG mid/side reconstruction equals
the coded mid channel, so the decoder keeps channel 0 and avoids the mid/side
sum/difference pass for the mono output path.  Joint-stereo frames that also use
intensity stereo still run the existing reconstruction first, then collapse to
mono.

`--bench` with `AMIGA_PROFILE_DECODE` now remains the profiling entry point for
stereo-to-mono investigation and prints the dominant core buckets required for
this benchmark: huffman, dequant, stereo/post, imdct, subband/dct32, and
polyphase.

### Stereo-to-mono benchmark log

Fixture unavailable in this workspace, so only the supplied before result is
recorded here.  Re-run the same command on the target Amiga fixture to fill the
after columns and capture the mono/stereo-to-mono checksums:

```sh
mp3dec_O3 --decode-only --bench --checksum --fast-lowrate --rate 11025 --mono "ZZ Top - Sharp Dressed Man.mp3"
```

| Build | Elapsed | Decode speed | Frame decode | Output samples | Mono checksum | Stereo-to-mono checksum | Notes |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| Before | 89.66 s | 0.45x | 86.40 s | 441504 | _record from mono fixture_ | _record from stereo fixture_ | Stereo synthesis plus CLI downmix still ran for mono output |
| After | _record on target_ | _record on target_ | _record on target_ | 441504 expected | _must match current mono test_ | _record new optimized checksum_ | Right-channel IMDCT/DCT32/polyphase and stereo PCM downmix skipped for `--mono` |

### Tougher 256 kbps stereo-to-mono profile

Supplied target profile for the tougher 44.1 kHz stereo, 256 kbps fixture
(duration used for realtime: 40.724898 s, 1559 decoded frames) is recorded as
the baseline for this pass.  The corrected mono fast-lowrate accounting target
is 448,992 emitted samples at one output channel.

The current decoder has already moved mono-output stereo fixtures onto a
one-channel IMDCT/DCT32/polyphase path: `MP3Decode()` limits the IMDCT loop to
one synthesis channel when `outputMono` is set, and `Subband()` selects the mono
synthesis branch for stereo input with mono output.  Therefore the main remaining
safe duplicate-work case in this pass is pure mid/side joint stereo. The side
information gives the exact channel payload length, so mono output can advance
over the unused side-channel Huffman payload without decoding it and can also
skip its dequant because `(L + R) / 2` is represented by the coded mid channel
after the existing scale adjustment.

| Metric | Before supplied profile | After this patch | Checksum/sample-count expectation |
| --- | ---: | ---: | --- |
| Elapsed | 92.160 s | _record on target_ | must preserve checksum |
| Decode speed | 0.44x realtime | _record on target_ | duration basis remains 40.724898 s |
| Output samples | 448,992 | 448,992 expected | one mono output channel |
| Huffman | 14.580 s | _record on target_ | pure M/S mono advances over the unused side-channel payload; other modes unchanged |
| Dequant | 9.340 s | _record on target_ | pure M/S mono skips side-channel dequant; other stereo modes unchanged |
| Stereo/post | 1.140 s | _record on target_ | pure M/S mono keeps the existing mid-channel shortcut |
| IMDCT | 11.820 s | _already one channel for mono output_ | no right-channel synthesis for stereo-to-mono |
| Subband/DCT32 | 15.060 s | _already one channel for mono output_ | no right-channel DCT32 for stereo-to-mono |
| Polyphase | 15.820 s | _already mono fast-lowrate branch_ | cumulative mono sample accounting unchanged |
| Bitstream/frame parsing | 3.500 s | _record on target_ | unchanged |

Validation command for the same fixture:

```sh
mp3dec_O3 --decode-only --bench --checksum --fast-lowrate --rate 11025 --mono "tougher-256k-stereo.mp3"
```

## Optional exact long-block IMDCT m68k ASM pass

`AMIGA_M68K_ASM_IMDCT` is an opt-in experiment for 68020+ GNU m68k builds.  It
keeps the C IMDCT as the reference and only routes the common long-block
`btCurr == 0 && btPrev == 0` IMDCT36 path through an operation-order-preserving
implementation.  Its common long-window/overlap stage is one compact
nine-iteration asm kernel, avoiding compiler spill/reload boundaries around
the three `muls.l` operations per iteration.  Short blocks, mixed-block
transition windows, start/stop windows, and any non-common block configuration
continue to use the C path.

Before recording speedups, run the synthetic guard test and checksum the existing
fast-lowrate fixtures against the safe build and the already-confirmed FDCT32 ASM
build.  If any checksum differs, leave `AMIGA_M68K_ASM_IMDCT` disabled in your
default build.

| Build | Suggested flags | Selftest | Required checksum status | Benchmark notes |
| --- | --- | --- | --- | --- |
| Safe build | `-DAMIGA_M68K -DAMIGA_FAST_POLYPHASE` | `--selftest-imdct` should pass with asm inactive | baseline | Record elapsed, realtime speed, and `timing core imdct` |
| FDCT32 ASM build | previous plus `-DAMIGA_M68K_ASM -DAMIGA_M68K_ASM_FDCT32` | `--selftest-fdct32 --selftest-imdct` should pass | must match safe build | Current confirmed fast build; use as the comparison point |
| FDCT32 + IMDCT ASM build | previous plus `-DAMIGA_M68K_ASM_IMDCT` | `--selftest-imdct` must pass with asm active on m68k | must match safe and FDCT32-only builds | Record any change in `timing core imdct` and overall realtime speed |

Checksum the following modes for each build:

```sh
amiga_mp3dec.safe --bench --no-output --checksum --fast-lowrate --rate 11025 --mono mono-56k-44100.mp3
amiga_mp3dec.safe --bench --no-output --checksum --fast-lowrate --rate 11025 --mono stereo-160k-44100.mp3
amiga_mp3dec.safe --bench --no-output --checksum --fast-lowrate --rate 11025 --mono stereo-256k-44100.mp3
amiga_mp3dec.safe --bench --no-output --8svx --rate 11025 --checksum mono-56k-44100.mp3
```

Repeat the same commands with the FDCT32 ASM binary and with the
FDCT32+IMDCT ASM binary.  The mono 56 kbps 44.1 kHz -> 11025 fast-lowrate,
stereo 160/256 kbps 44.1 kHz -> mono 11025 fast-lowrate, and 11025 Hz 8SVX
export checksums must remain identical across all three builds.

Do not add `AMIGA_M68K_ASM_IMDCT` to default build flags based on synthetic
selftests or host timings.  First demonstrate a repeatable improvement on real
68020+ hardware in both the mono fixture and the stereo/high-bitrate 160/256
kbps fixtures above; otherwise leave the option disabled.
