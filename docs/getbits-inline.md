# Inlining GetBits()

`GetBits()` was a plain out-of-line function in `real/bitstream.c`, called
from 47 sites across `bitstream.c` (frame header / side info fields) and
`real/scalfact.c` (scale factor unpacking). Every one of those calls paid a
real `jsr`/`rts` plus register save/restore, for a function whose entire
body is a handful of shifts, an `and`, and an occasional cache refill -
called on the order of hundreds to a few thousand times per frame.

## What changed

`RefillBitstreamCache4_C_REFERENCE`, `RefillBitstreamCache4_TEST_ACTIVE`,
`RefillBitstreamCache`, and `GetBits` moved from `real/bitstream.c` into
`real/coder.h` as `static __inline`, right after the `BitStreamInfo`
struct definition. Every translation unit that includes `coder.h` (both
`bitstream.c` and `scalfact.c` already did) now gets its own inlined copy,
so GCC can fold the `& 0x1f` and fixed shifts away entirely at call sites
that pass a compile-time-constant `nBits` (the majority of them), and drops
the call/return overhead everywhere else.

No arithmetic, control flow, or the existing 68020+ `move.l` refill fast
path (`AMIGA_M68K_BITSTREAM_MOVEL`) changed - this is purely "move the
existing static-inline-eligible code from a .c file into the header it was
already effectively private to," the same way `RefillBitstreamCache` was
already `static __inline` inside `bitstream.c` before this change (it just
wasn't visible outside that one file).

`BitstreamRefillSelftest()` stays in `bitstream.c` and still exercises
`RefillBitstreamCache4_C_REFERENCE()` against `RefillBitstreamCache4_TEST_ACTIVE()`
directly - unaffected, since both are visible via `coder.h`.

`GetBits`'s entry in the `STATNAME()` multi-instance-linking rename table
was dropped: `STATNAME` exists to avoid symbol collisions when statically
linking multiple decoder instances, which only matters for externally
linked symbols. A `static` function has no external linkage, so each
translation unit already gets its own private copy without needing the
rename.

## Why this is safe

* Byte-for-byte the same C source as before, just relocated and given
  `static` linkage - no semantic change.
* Verified with a native x86 smoke compile (`-DAMIGA_M68K`) of
  `bitstream.c`, `scalfact.c`, and the rest of `real/*.c` plus `mp3dec.c`:
  all compile clean with no new warnings.
* `--selftest-huffman` and `BitstreamRefillSelftest()` remain the existing
  on-target oracles for the bit-cache primitives this touches.

## Validating on target

```sh
make -f Makefile.amiga fast030
amiga_mp3dec.fastexp --selftest-bitstream
amiga_mp3dec.fastexp --checksum <file.mp3>  # compare before/after for bit-exactness
```

Then A/B decode time on a representative file before/after this change to
measure the effect of removing the GetBits call overhead.
