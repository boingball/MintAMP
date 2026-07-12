# libnix exit allocation diagnostics

Hardware run inputs used for this note:

- Runtime load bias: `0x4037F140`.
- Raw survivor caller `0x40456A80` symbolises to link-time `0x000D7940`, the instruction after the wrapped `calloc(4, 4)` inside `___init_fh` / `__init_fh`; the 16-byte result is stored in global `__fh`.
- Raw survivor caller `0x40446446` symbolises to link-time `0x000C7306`, the instruction after the wrapped `malloc(0x50)` inside `strtok`; the 80-byte result is stored at `_impure_ptr + 0xE8`.
- `___free_all` at link-time `0x000D083E` loads `SysBase`, derives the Exec allocation base/size from the libnix allocation header, and calls Exec vector `jsr -210(a6)` directly.

## Exit-list order

The pushed map/objdump show the CRT teardown path runs the registered exit list via `callfuncs` / `__EXIT_LIST__` before `___free_all` walks the remaining libnix malloc list.  The relevant consequence is that library destructors can release backing Exec allocations without going through the link-wrapped `free` symbol, leaving stale nodes for the later `___free_all` pass.

For the two observed survivors, the effective order is:

1. application returns from `main`;
2. exit-list dispatch through `callfuncs` / `__EXIT_LIST__`;
3. file-handle cleanup (`___exit_fh`) runs for the table allocated by `___init_fh`;
4. reentrancy cleanup runs for lazily allocated newlib/libnix per-thread state such as `strtok` state;
5. `___free_all` walks libnix's malloc list and frees each remaining node by calling Exec `FreeMem` directly.

## Survivor ownership

### `__fh`

`__fh` is CRT-owned.  It is allocated by `___init_fh` / `__init_fh` with `calloc(4, 4)` and stored in global `__fh`.  Its intended cleanup owner is `___exit_fh`, not application shutdown.  Application code must not free this pointer.

### `strtok` state

The 80-byte block allocated by `strtok` is reentrancy state stored at `_impure_ptr + 0xE8`.  Its intended cleanup owner is the CRT/newlib reentrancy cleanup, not application shutdown.  Application code must not free this pointer.

## FreeMem boundary

`___free_all` does not call a linkable `FreeMem` symbol.  The observed disassembly is an Exec library-vector call:

```asm
movea.l SysBase,a6
lea -4(a0),a1
move.l -4(a0),d0
jsr -210(a6)
```

Because this is a direct Exec vector call, a normal linker `--wrap=FreeMem` interposition cannot observe it.  Do not use `SetFunction`, do not patch Exec globally, and do not try to intercept unrelated OS `FreeMem` calls.

## Pointer lifecycle producing the recoverable alert

Both survivors are observed by the linker-level malloc-family audit at `main` return because their allocation went through libnix `calloc`/`malloc` and no wrapped `free` occurred before application exit.

The failure pattern is:

1. CRT allocates a block through libnix malloc-family (`__fh` or lazy `strtok` state), so the block is present on libnix's internal malloc list.
2. CRT teardown releases the owned object through a cleanup path that bypasses the wrapped `free` symbol and calls Exec `FreeMem` directly.
3. The stale libnix malloc-list node remains visible to `___free_all`.
4. `___free_all` later walks that list and calls Exec `FreeMem` directly again for the same block, producing the recoverable alert on the main/CRT task.

This is not a radio/TLS ownership defect and should not be fixed by freeing CRT-owned survivors in application shutdown.

## Source-level mitigation in this branch

The only application-controlled survivor from the observed pair is the lazy `strtok` allocation.  The stream header parser now uses a small in-place CR/LF tokenizer instead of `strtok`, avoiding creation of the `_impure_ptr + 0xE8` CRT reentrancy block during radio header parsing.

The remaining `__fh` survivor is CRT-owned and should be addressed with a CRT/build-level fix, not by application cleanup.  Acceptable next steps are to use a libnix/newlib variant whose file-handle/reent destructors unlink via `free`, or to adjust the CRT teardown ordering/implementation so a block released by `___exit_fh` cannot remain on the malloc list subsequently walked by `___free_all`.
