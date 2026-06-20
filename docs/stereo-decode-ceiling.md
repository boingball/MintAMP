# Stereo 11025 decode ceiling investigation

Status: mapped on the MiniAMP3/libhelix side; SongPlayer-side behavior remains unobserved and explicitly unresolved.

## Hardware and benchmark context

The target system is CD32-class Amiga hardware with a 68030 at 50 MHz. The profile numbers below came from WinUAE benchmarking using a slightly conservative estimate of about 7 MIPS. Real hardware tends to beat the WinUAE estimate, so the WinUAE figures should be treated as a pessimistic lower bound rather than the final hardware result.

Real-hardware stereo 11025 figures still need to be measured on the 50 MHz 030. In particular, the current WinUAE stereo 11025 result of about 0.66x realtime may not be the real-hardware number.

## Measured profile buckets

Profiled configuration: stereo 11025, quality 0.

Approximate bucket breakdown:

- Huffman: ~56 s
- Dequant: ~51 s
- Stereo/post: ~19 s
- IMDCT: ~104 s
- Subband/DCT32: ~82 s
- Polyphase: ~114 s

Important context for these buckets:

- Polyphase is already using the reduced-tap assembly path where applicable.
- IMDCT is already capped to 8 of 32 subbands for this low-rate path, skipping about 75% of the full-band IMDCT work.
- The remaining cost is therefore not an untouched obvious hotspot; it is the residual cost after the verified low-rate shortcuts have already been applied.

## Levers tried and measured outcomes

### Reduced-tap dewindow

Reduced-tap dewindowing for low-rate stereo helped and is banked. It is a verified shipping win, including the reduced stride-2 stereo assembly path. It is not sufficient by itself to make stereo 11025 comfortably realtime in the current WinUAE measurement.

### Dequant input cap

The `AMIGA_FAST_SUBBAND_CAP` family is retained. It is correct and bit-exact, but measured close to a null effect in the stereo 11025 investigation because `nonZeroBound` already limits dequant work to the actual content extent. The subband cap therefore rarely binds in normal granules, though it remains harmless and can still help on spectrum-filling granules.

### 16-bit FDCT32 `muls.w`

The 16-bit FDCT32 experiment produced about 14.4% RMS error, but more importantly produced no useful speed win in the low-output-rate path. At low output rates the FDCT work is already shadowed by `FDCT32Quarter`, so the 16-bit FDCT32 path barely executes in the configuration that needed help. The code path has been shelved from shipping source to avoid preserving a dead branch.

### 16-bit IMDCT `muls.w`

The 16-bit IMDCT experiment had the right gross scale: measured magnitude ratio was about 0.9997. However, RMS error was 54.17% of signal, nearly four times worse than the 16-bit FDCT32 result. IMDCT36's deeper accumulation and overlap-add amplify the high-16 data-reduction error, making this path unusable quality. The code path has been shelved from shipping source.

### `muls.w` vs `muls.l` microbenchmark

The standalone `--selftest-mulsw-timing` diagnostic is retained. It measured only about a 1.3x `muls.w` advantage on WinUAE, pending confirmation on real hardware. This is much smaller than the kind of multiplier gap that made older MPEGA-era 16-bit tricks attractive on 68000/68020-class targets. On 68030, the word/long multiply gap is narrow enough that the quality cost of forcing true 16x16 math is not repaid.

## Fundamental finding

On 68030, the speed and quality goals conflict for these 16-bit transform experiments:

- A true `muls.w` path is a 16x16 multiply, so both operands must be narrowed. That produced unusable IMDCT quality: 54.17% RMS error relative to signal.
- MPEGA-style 32x16 math can keep data wide enough for quality, but it is not a single fast 16x16 instruction on 68030.

Those two requirements are therefore mutually exclusive on this CPU for the investigated transform paths. Narrow both operands and quality collapses; keep data wide and the desired single-instruction `muls.w` speedup disappears.

## Open SongPlayer question

How SongPlayer achieves stereo 11025 on similar hardware is not yet confirmed by direct observation. This should not be reopened as another source-reading exercise. The next investigation step is to observe SongPlayer's actual stereo 11025 output quality and exact hardware/RAM configuration, then compare A/B against MiniAMP3.

Candidate explanations to check by observation:

1. SongPlayer may decode mono plus fake/intensity stereo rather than true two-channel output.
2. SongPlayer may run at an MPEGA `QUALITY_LOW`-style setting with fewer taps and a full 16-bit pipeline, which may sound worse than MiniAMP3 quality 0.
3. The compared hardware, RAM, cache, or memory placement may differ.
4. SongPlayer may resample from a different internal decode rate.

Until that observation is done, SongPlayer remains an external comparison point, not proof that MiniAMP3 has an unexploited 68030-quality-preserving 16-bit path.

## Current shipping matrix

- Mono 11025: solid.
- Mono 22050: 1.07x realtime and bit-perfect with the reduced mono stride-2 assembly kernel.
- Stereo 11025: about 0.66x on WinUAE; real 50 MHz 030 hardware measurement is still TBD.
- Stereo 22050: supported for correctness/regression coverage, but not the current realtime target.
- Headline advantage versus SongPlayer: broader MP3 file compatibility, with verified low-rate wins kept and measured dead ends removed from the shipping path.
