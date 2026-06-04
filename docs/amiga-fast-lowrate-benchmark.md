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
