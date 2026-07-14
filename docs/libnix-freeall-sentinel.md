# libnix `___free_all` sentinel compatibility note

The armed MiniAMP3 diagnostics traced the shutdown alerts to libnix
`___free_all` walking three internal allocation-list heads during CRT exit. Two
heads contained `0xffffffff` at shutdown. Stock `___free_all` only checked for
`NULL`, so it treated `0xffffffff` as a node, derived `base = 0xfffffffb`, read a
zero size, and called `FreeMem(0xfffffffb, 0)`, producing the recoverable alerts.

The known-good diagnostic run used the three head addresses taken from the exact
binary's `___free_all` disassembly. The compatibility runner has therefore been
returned to that diagnostic mode: it is not linked into normal GUI builds by
default, and `FREEALL_PROBE=1` supplies the three head addresses with linker
`--defsym` values from the exact binary under test.

A later production version should avoid executable-absolute addresses. Current
binary evidence suggests the heads are near libnix's `_errno` data symbol, but
that address calculation must be proven against the linked toolchain's libnix
headers/source and the current `___free_all` disassembly before becoming the
production default.
