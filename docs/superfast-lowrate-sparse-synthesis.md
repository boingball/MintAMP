# Experimental `--superfast-lowrate` sparse synthesis plan

This mode is intentionally disabled by default.  It is a separate experiment from
`--fast-lowrate` quality presets until 68030 hardware testing proves that the
sound quality and FIFO safety are acceptable.

## Active subband dependency map

The explicit low-pass model is derived from fast-lowrate stride:

| stride | active IMDCT subbands | permanently discarded subbands |
| --- | ---: | --- |
| 1 | 32 | none |
| 2 | 16 | 16-31 |
| 4 | 8 | 8-31 |
| 5 | 6 | 6-31 |

For `44100 Hz -> 22050 Hz` (stride 2), playback retains subbands 0-15 and permanently discards subbands 16-31.  For `44100 Hz -> 11025 Hz` (stride 4), playback retains subbands 0-7 and permanently discards subbands 8-31.  For `44100 Hz -> 8820/8287 Hz` (stride 5), playback retains subbands 0-5 and permanently discards subbands 6-31.  Coefficients, IMDCT output, and overlap state for discarded subbands must be zeroed and must not be consumed by sparse synthesis.

## IMDCT safety rule

The sparse path may skip permanently discarded subbands only after the active
subband count has capped `nBlocksLong`, `nBlocksTotal`, and `nBlocksPrev`.
That preserves long, short, mixed, and transition block safety: retained bands
run the normal long/short kernels, while discarded bands have no stale overlap
because their exposed IMDCT output is explicitly zeroed each granule.
Unsupported granules should fall back to the full path instead of reusing stale
`xPrev` data.

## Stride-2 polyphase rows

Stride 2 is the only sparse mode supported for 22,050 Hz output. It must use
`FDCT32Half`; it must never call the stride-4 phase model or `FDCT32Quarter`,
because doing so would expose the wrong FIFO rows and label half the required
sample count as 22,050 Hz audio.

| fastLowratePhase at entry | consumed rows |
| ---: | --- |
| 0 | 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30 |
| 1 | 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31 |

`oddBlock`, `offset`, and `vindex` still select the visible synthesis FIFO half.
A stride-2 sparse FDCT must preserve the same FIFO rows as the existing
half-rate FDCT path for every `vindex` and both `oddBlock` states.

## Supported sparse output rates

`--superfast-lowrate` supports 8,287 Hz, 8,820 Hz, 11,025 Hz, and 22,050 Hz.  If
no rate is specified, the command-line default remains 11,025 Hz for
compatibility.  The GUI must not silently change a selected rate when Superfast
is enabled; the user-visible status/debug output should report the active stride,
active subband count, and FDCT choice.  Unsupported rates such as 28,600 Hz or
arbitrary values must be rejected instead of falling through to sparse mode.

## Stride-5 (8,820 / 8,287 Hz) note

8,820 Hz and 8,287 Hz both map to the stride-5 fast-lowrate selector.  Unlike
stride 2 and stride 4, stride 5's five phases together consume every synthesis
FIFO row (rows 0-16), so there is no bit-exact "skip whole rows" sparse FDCT for
it: the full `FDCT32` is still required for exact output.  Superfast stride 5
therefore takes its win from the IMDCT subband cap, not the FDCT.

The active subband count for stride 5 is 6 (output Nyquist ~4.4 kHz / 689 Hz per
subband).  Superfast caps the IMDCT to those 6 subbands and zeroes 6-31, down
from the default fast-lowrate cap of 16 for stride >= 4.  This both reduces IMDCT
work and removes the high-band aliasing that plain decimation-by-5 produces, so
8,820/8,287 superfast output can be cleaner as well as faster.  The synthesis
FDCT runs the full `FDCT32` on the band-limited (subband 6-31 zeroed) input, so
the result is bit-identical to running the full pipeline with those subbands
zeroed.  A dedicated stride-5 sparse FDCT that also skips the now-zero first-pass
folds is possible but would save only the first-pass multiplies (~10% of the
transform) because all output rows are still needed; it is left as a future
micro-optimisation.

## Performance trade-off

Stride 2 executes up to 16 IMDCT subbands per granule and uses `FDCT32Half`, so
it saves less work than stride 4 but preserves the exact 22,050 Hz duration and
pitch.  Stride 4 executes up to 8 IMDCT subbands and may use `FDCT32Quarter`,
which gives the largest 11,025 Hz saving but is not valid for 22,050 Hz.

## Stride-4 polyphase rows

The stride-4 mono polyphase selector consumes these PCM row numbers from each
32-row synthesis block:

| fastLowratePhase at entry | consumed rows |
| ---: | --- |
| 0 | 0, 4, 8, 12, 16, 20, 24, 28 |
| 1 | 3, 7, 11, 15, 19, 23, 27, 31 |
| 2 | 2, 6, 10, 14, 18, 22, 26, 30 |
| 3 | 1, 5, 9, 13, 17, 21, 25, 29 |

`oddBlock`, `offset`, and `vindex` choose which half of the synthesis FIFO is
exposed to the following polyphase call.  For even blocks (`oddBlock == 0`) the
primary lane is `dest + offset`; for odd blocks the primary lane is
`dest + VBUF_LENGTH + offset`.  The companion delay lane uses
`delayOff = (offset - oddBlock) & 7` on the opposite half.  A sparse FDCT must
write every FIFO location reachable through those lanes for the phase table
above and explicitly clear unused exposed locations before polyphase runs.

## Reference-test contract

Reference tests should:

1. Generate deterministic capped input with subbands 8-31 zero.
2. Run normal full IMDCT plus FDCT32.
3. Run the sparse path with the same cap.
4. Compare the selected stride-4 PCM samples sample-for-sample for all four
   phases, both `oddBlock` values, all eight `vindex` offsets, and varied guard
   bit counts.
5. Repeat over consecutive granules for long, short, mixed, and transition
   blocks, first mono and then stereo.

Success means exact output match against the capped reference, or a documented
and bounded difference for any intentionally lossy sparse FDCT approximation.
