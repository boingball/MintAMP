# libnix `___free_all` sentinel compatibility note

The armed MintAMP diagnostics traced the shutdown alerts to libnix
`___free_all` walking three internal allocation-list heads during CRT exit. Two
heads contained `0xffffffff` at shutdown. Stock `___free_all` only checked for
`NULL`, so it treated `0xffffffff` as a node, derived `base = 0xfffffffb`, read a
zero size, and called `FreeMem(0xfffffffb, 0)`, producing the recoverable alerts.

The accepted fix is the sentinel-safe replacement installed into libnix's exit
list. It preserves stock traversal for valid nodes but treats both `NULL` and
`0xffffffff` as empty list values, normalising sentinel heads/next pointers to
`NULL` without dereferencing them.

The build must not rely on manually maintained executable-absolute list-head
addresses. `Makefile.amiga` first links a discovery binary with the replacement
present and zero head symbols, extracts the three list-head operands from that
binary's stock `___free_all` disassembly, then relinks the final binary with
those exact addresses supplied as linker `--defsym` symbols. A post-link
verification step disassembles the final binary and fails if the operands differ.

The earlier `_errno`-relative experiment is not used as an ABI. Any future
source-level explanation for why libnix stores `0xffffffff` in these heads should
come from the exact linked toolchain source/objects, not from inferred offsets.
