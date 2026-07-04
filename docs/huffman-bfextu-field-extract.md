# Huffman codeword field extract via bfextu

Follow-up to `huffman-tablewalk-fastpath-analysis.md`.  That investigation
deliberately left the pair table-walk control flow untouched because
restructuring it (e.g. flattening root levels into generated tables) changes
Huffman semantics and needs generated side data plus broad equivalence
coverage before it can be trusted.

This change does not touch control flow, table data, or bit-cache/refill
logic at all.  It only replaces how the three 4-bit fields packed into each
16-bit codeword (`HLen`, `CWX`, `CWY`) are pulled out of the word.

## What changed

`GetHLen`/`GetCWX`/`GetCWY` in `real/huffman.c` are generic
`(x >> shift) & 0xf` shift+mask macros.  On a 68020+ target the same value
can be read with one `bfextu` bit-field extract instead of a shift and an
and.  Three tiny `__inline` helpers (`GetHLen_AMIGA_M68K_ASM`,
`GetCWX_AMIGA_M68K_ASM`, `GetCWY_AMIGA_M68K_ASM`) wrap
`bfextu %1{#offset:#4},%0` using the same register-operand form and `"cc"`
clobber already used by `CLZ_AMIGA_M68K_ASM`'s `bfffo` in
`real/assembly.h`. The codeword is passed in as a zero-extended
`unsigned int`, so its three nibbles sit at bit-field offsets 16, 20, and 24
counted from the register MSB.

`HUFF_GET_HLEN`/`HUFF_GET_CWX`/`HUFF_GET_CWY` pick the asm or C form at
*runtime*, keyed off the same `useAsmRefill` flag `DecodeHuffmanPairs_Impl`
already uses to pick the asm vs. C bit-refill primitive. That keeps
`DecodeHuffmanPairs_C_REFERENCE` exactly the portable C path used as the
comparison baseline, while `DecodeHuffmanPairs_TEST_ACTIVE`
(`--exp-huff` / quality-0 preset) gets the bfextu form. All three table-walk
shapes (`oneShot`, `loopNoLinbits`, `loopLinbits`) are covered; the count1
quad decoder (`DecodeHuffmanQuads`) uses an unrelated packed-byte codeword
layout and is not touched.

## Why this is safe to enable behind the existing gate

* Pure per-value arithmetic substitution — for every possible 16-bit `cw`,
  `GetHLen_AMIGA_M68K_ASM(cw) == GetHLen(cw)` (and likewise for CWX/CWY) by
  construction, since both compute the same bit-field of the same value.
* No change to loop structure, table data, sign handling, refill logic, or
  error returns.
* Already covered by the existing `--selftest-huffman` harness, which
  compares `DecodeHuffmanPairs_C_REFERENCE()` against the active path
  bit-exactly (decoded symbols, consumed-bit counts, error returns) on 1000
  deterministic pseudo-random cases, including truncated/malformed input.
* Build-fails-safe: if the `bfextu` operand syntax were wrong it would be a
  target-assembler error at build time (`fast030`/`AMIGA_M68K_ASM_HUFFMAN`),
  not a silent decode error.

## Validating on target

```sh
make -f Makefile.amiga fast030
amiga_mp3dec.fastexp --selftest-huffman --exp-huff
```

Expect `Huffman selftest failures: 0` with `Huffman forced by --exp-huff:
yes`. Then A/B a real file's decode time and `--checksum` output with and
without `--exp-huff` to confirm the bfextu path is both bit-exact and
measurably faster on 68020/68030 targets.

## Expected impact

Small and per-symbol: `bfextu` replaces a shift-by-immediate plus
`and`/`andi` for every codeword visited during the pair table walk (the
majority of `big_values` decode work). It does not touch the larger,
previously-identified cost centers in the table walk (variable-count cache
shifts, per-level metadata reads) — those remain the candidate for future
work under the equivalence-testing policy in
`huffman-tablewalk-fastpath-analysis.md`.
