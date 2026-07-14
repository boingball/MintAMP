# libnix `___free_all` sentinel compatibility note

The MiniAMP3 shutdown alerts were traced to libnix `___free_all` walking three
internal allocation-list heads during CRT exit. Runtime evidence from the armed
compatibility runner showed two heads containing `0xffffffff` at shutdown. The
stock libnix routine treated that value as a real node, derived
`base = 0xfffffffb`, read a zero size, and called `FreeMem(0xfffffffb, 0)`,
which produced the recoverable alerts.

The linked MiniAMP3 binary places the three heads at offsets from libnix's
`_errno` data symbol:

* `_errno + 0x0c`
* `_errno + 0x10`
* `_errno + 0x20`

The compatibility replacement therefore addresses the heads relative to `_errno`
instead of executable-absolute addresses. It treats both `NULL` and
`0xffffffff` as empty heads, normalises sentinel heads to `NULL`, and preserves
libnix traversal semantics for valid nodes.

No project source file assigns `0xffffffff` to these heads. The linked libnix
object/source used by the cross toolchain is external to this repository; the
binary evidence proves the sentinel value is present in libnix allocator state at
CRT shutdown, but this repository does not contain enough linked libnix source to
classify the assignment site beyond the observed runtime behaviour.
