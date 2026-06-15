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

For the initial target (`44100 Hz -> 11025 Hz`, stride 4), playback starts with
only subbands 0-7 active.  Coefficients, IMDCT output, and overlap state for
subbands 8-31 must be zeroed and must not be consumed by sparse synthesis.

## IMDCT safety rule

The sparse path may skip permanently discarded subbands only after the active
subband count has capped `nBlocksLong`, `nBlocksTotal`, and `nBlocksPrev`.
That preserves long, short, mixed, and transition block safety: retained bands
run the normal long/short kernels, while discarded bands have no stale overlap
because their exposed IMDCT output is explicitly zeroed each granule.
Unsupported granules should fall back to the full path instead of reusing stale
`xPrev` data.

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
