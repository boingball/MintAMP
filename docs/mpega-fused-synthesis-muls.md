# MPEGA fused synthesis multiply estimate (CLOSED)

Status: **CLOSED**. The abandoned `--exp-fused-synth` / `AMIGA_FUSED_SYNTHESIS`
experiment is retired and is not part of the shipping synthesis path.

The experiment was superseded because the proposed fused butterfly duplicated
work that `FDCT32Quarter` already performs: a reduced-multiply quarter-rate
butterfly in the correct libhelix FIFO layout. Its projected 3.36x saving was
measured against a full-`FDCT32` plus 16-tap polyphase baseline, but the actual
stride-4 quality-0 path does not use that baseline.

The real 11025 Hz true-stereo quality-0 performance gap was the missing
reduced-tap stereo dewindow dispatch. That gap is addressed by wiring the
existing `PolyphaseStereoFastLowrateStride4Reduced()` implementation into the
stereo stride-4 fast-lowrate path before the full 8-tap assembly/C paths can
shadow it.
