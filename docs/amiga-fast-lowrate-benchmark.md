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
- Full-rate mono/stereo synthesis and normal `--rate` downsampling remain on the
  existing paths. Stereo fast-lowrate still uses the paired helper until mono
  target checksums have been verified on the Amiga benchmark machine.

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
safe duplicate-work case in this pass is pure mid/side joint stereo: Huffman must
still decode both channels for bitstream accounting, but mono output can skip the
side-channel dequant because `(L + R) / 2` is represented by the coded mid
channel after the existing scale adjustment.

| Metric | Before supplied profile | After this patch | Checksum/sample-count expectation |
| --- | ---: | ---: | --- |
| Elapsed | 92.160 s | _record on target_ | must preserve checksum |
| Decode speed | 0.44x realtime | _record on target_ | duration basis remains 40.724898 s |
| Output samples | 448,992 | 448,992 expected | one mono output channel |
| Huffman | 14.580 s | _record on target_ | bitstream position unchanged; table-0 regions now zero via `memset()` |
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
