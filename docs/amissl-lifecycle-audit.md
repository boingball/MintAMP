# AmiSSL Lifecycle Audit

Audit of every AmiSSL open/init/cleanup/close path in this codebase against the
official AmiSSL v5 autodocs (`amissl.library/InitAmiSSLA`,
`amissl.library/CleanupAmiSSLA`, `amissl.library/--background--`) and the
project's ten lifecycle rules. Files audited: `radio_stream.c`,
`radio_stream_probe.c`, `radio_browser_http.c`, `amissl_https_get.c`,
`amiga_mp3gui.c` / `minimp3r.c` (playback child spawn sites).

Task model recap: the GUI/main task runs `Radio_NetworkInit()` →
`radio_amissl_open_shared()` → `OpenAmiSSLTags()` once and keeps the shared
`AmiSSLBase`/`AmiSSLExtBase` for the whole run. Station probes and favicon
fetches run **on the GUI/opener task** (`radio_stream_probe.c`). Each playback
session runs in a **child process** spawned with `CreateNewProcTags`
(`amiga_mp3gui.c:7148`, `minimp3r.c:1583`) that does its own
`InitAmiSSL()`/`CleanupAmiSSL()` via `radio_ssl_global_init()` /
`radio_ssl_global_cleanup()`.

> **Implementation status:** F1, F2 and F3 are implemented (plus the
> SSL_write fatal-fault quarantine in `radio_send_all()` /
> `rb_probe_send_all()`). F4 is deliberately deferred — every socket call in
> `radio_stream.c` goes through the one shared `SocketBase` global, so the
> per-child base needs its own change and soak test; a `TODO(F4, ...)` marks
> the `InitAmiSSL` call site in `radio_ssl_global_init()`. F5 remains open
> (low priority).

## Verdict summary

| Rule | Status |
|------|--------|
| 1. One `OpenAmiSSLTags()`, shared base kept by main task | PASS (edge case, F5) |
| 2. Opener task initialised before using `SSL_*` | **FAIL — F1** |
| 3. Child calls `InitAmiSSL` before `SSL_CTX_new`/`SSL_new`/`SSL_connect` | PASS |
| 4. Child pairs `InitAmiSSL` with `CleanupAmiSSL` unless poisoned | **FAIL — F2** |
| 5. No manual `CleanupAmiSSL` for an `AmiSSL_InitAmiSSL` opener | PASS (moot until F1 fixed) |
| 6. Never `CloseLibrary(AmiSSLExtBase)` directly | PASS |
| 7. `AmiSSL_ErrNoPtr` passed on every `InitAmiSSL` | PASS (see F4 note) |
| 8. `SocketBase` valid in the task calling `InitAmiSSL` | **AT RISK — F4** |
| 9. No AmiSSL cleanup/reset after MiniMem heap corruption | **FAIL — F3** |
| 10. TLS-error containment (stop/block/quarantine/disable) | PARTIAL — F3 |

---

## F1 — Opener task is never actually initialised (Rule 2) — HIGH

Both production `OpenAmiSSLTags()` call sites omit the `AmiSSL_InitAmiSSL`
tag:

- `radio_stream.c:736` (`radio_amissl_open_shared()`)
- `radio_stream_probe.c:659` (probe lazy fallback)

Yet the code assumes the opener task is auto-initialised and must skip
`InitAmiSSL()`:

- `radio_stream.c:805-812` — `Radio_AmiSslTaskIsOpener()` → "InitAmiSSL not
  needed", sets `radio_amissl_initialized = 1` without any init call.
- `radio_stream_probe.c:669-677` — same assumption for the probe path.

Per the autodocs this assumption only holds when `AmiSSL_InitAmiSSL` was
actually passed: `CleanupAmiSSLA` docs — "If using
amisslmaster.library/OpenSSLTags() **and the AmiSSL_InitAmiSSL tag** to
initialise AmiSSL ..."; `InitAmiSSLA` docs — if initialisation is delayed,
"this function must be called ... before you can use any of the available
OpenSSL functions." Without the tag, `OpenAmiSSLTags()` only *opens* the
library; the opener task has been *opened but not initialised*.

Consequence: every probe/favicon fetch on the GUI task runs `SSL_CTX_new()`,
`SSL_new()`, `SSL_connect()`, `SSL_read()` (`radio_stream_probe.c:882` onward,
shared `rb_probe_shared_ctx`) in a task AmiSSL was never initialised for —
timer port, errno wiring and per-task state are whatever uninitialised
defaults happen to be. This is exactly the class of "works mostly, corrupts
rarely" behaviour the crash log history in these files describes.

**Fix (recommended):** add `AmiSSL_InitAmiSSL, TRUE` to both
`OpenAmiSSLTags()` calls. `AmiSSL_SocketBase` and `AmiSSL_ErrNoPtr` are
already in the taglist, so the implicit init picks them up. With the tag in
place:

- the opener-skip logic in `radio_ssl_global_init()` /
  `rb_probe_ensure_amissl_locked()` becomes correct as written, and
- the opener-side `CleanupAmiSSL` skip (`radio_stream.c:860-866`,
  `radio_stream_probe.c:721-727`) becomes *mandatory* per Rule 5 — that code
  is already right and must stay.

(Alternative: keep the tag out and have the opener call `InitAmiSSL()` once
after `OpenAmiSSLTags()` + `CleanupAmiSSL()` once before `CloseAmiSSL()` —
the pattern `amissl_https_get.c` already implements correctly. But the
`AmiSSL_InitAmiSSL` route is smaller and matches the existing skip logic.)

## F2 — Every playback child unconditionally skips `CleanupAmiSSL` (Rule 4) — HIGH

`radio_ssl_close_stream_mode()` (`radio_stream.c:1087-1099`) and
`radio_ssl_free_ctx()` (`radio_stream.c:1120-1128`) skip `SSL_free()` /
`SSL_CTX_free()` for **every** session — healthy or poisoned ("Debug
containment") — and in doing so set `radio_amissl_task_poisoned = 1` and
`radio_tls_shutdown_quarantine = 1` unconditionally.

Knock-on effects:

1. `radio_ssl_global_cleanup()` (`radio_stream.c:847-859`) sees the poison
   flag and skips `CleanupAmiSSL()` for every exiting playback child. The
   autodocs are explicit: "each process ... where you made a successful
   InitAmiSSLA() call, you must pair it with a call to this function ...
   Failure to do so can cause AmiSSL to crash." Rule 4 allows the skip only
   for genuinely poisoned/quarantined sessions; here the exception has become
   the rule, and every station stop leaks that dead task's per-task state
   inside the shared instance — precisely the dangling state the
   quarantine comments blame for later crashes.
2. `Radio_NetworkShutdown()` (`radio_stream.c:2082-2102`) sees
   `radio_tls_shutdown_quarantine` and abandons the instance: **`CloseAmiSSL()`
   is never called in any run that played one HTTPS stream**, leaving
   `amissl.library` resident with a nonzero open count (the file's own
   comment at `radio_stream.c:346-355` explains why that then requires a
   reboot).
3. One leaked `SSL` + `SSL_CTX` (~100–150 KB per the file's own estimate) per
   station switch, even on clean stops.

**Fix:** restore the poison gate on those two paths — skip/leak and set the
quarantine flags only when `rs->sslStatePoisoned || Radio_IsTlsPoisoned()`
(the pre-containment logic the surrounding comments still describe); free
normally and let `radio_ssl_global_cleanup()` run `CleanupAmiSSL()` on the
healthy path. If the unconditional skip is retained deliberately as a
temporary experiment, it should at minimum not set
`radio_tls_shutdown_quarantine` on healthy closes, so a clean run still ends
with `CleanupAmiSSL`-less children but a proper `CloseAmiSSL()` — though that
still violates Rule 4 and the autodocs.

## F3 — MiniMem heap-corruption poison does not reach the AmiSSL teardown gates (Rules 9, 10) — MEDIUM

`Radio_MarkMemoryPoisoned()` (`radio_stream.c:333-339`) sets only
`radioMemoryPoisoned`. It does **not** set `radioAmiSslPoisoned` or
`radio_tls_shutdown_quarantine`. The ring-canary path happens to set both
(`radio_stream.c:1350-1364`), but the direct MiniMem detection path —
`Radio_CheckMiniMem()` (`radio_stream.c:457-463`), called from decoder
init/cleanup, artwork fetch, station switch and app exit
(`amiga_mp3dec.c:10299,10486,10846`, `minimp3r.c:3769,4400,4598,5560`) — does
not.

So after a MiniMem corruption report with no accompanying TLS fault:

- `Radio_NetworkShutdown()` (`radio_stream.c:2082`) checks only
  `Radio_IsTlsPoisoned() || radio_tls_shutdown_quarantine` and will still run
  `CleanupAmiSSL()`/`CloseAmiSSL()`/`CloseLibrary()` on a known-corrupt heap —
  a direct Rule 9 violation.
- `radio_ssl_global_cleanup()` (`radio_stream.c:847`) and
  `rb_probe_cleanup_amissl()` (`radio_stream_probe.c:713`) will still call
  `CleanupAmiSSL()` — same violation.
- Probe/favicon HTTPS keeps running: `rb_probe_ensure_amissl()`
  (`radio_stream_probe.c:586`) checks only `Radio_IsTlsPoisoned()`. Rule 10's
  "heap corrupt → disable radio until app restart" is enforced for playback
  (`Radio_OpenWithHostAddr`, `radio_stream.c:1748`) but not for the GUI-task
  probe transport.

**Fix:** make `Radio_MarkMemoryPoisoned()` also set the TLS quarantine (or
have every AmiSSL cleanup/close gate additionally check
`Radio_IsMemoryPoisoned()`), and add the memory-poison check to
`rb_probe_ensure_amissl()` so probes stop touching AmiSSL after corruption.

## F4 — One shared `SocketBase` across GUI task and playback children (Rule 8) — MEDIUM/HIGH

`SocketBase` is a single file-scope global (`radio_stream.c:79`) opened once
by the main task (`Radio_NetworkInit`, `radio_stream.c:2048`; lazy fallback
`radio_ssl_global_init`, `radio_stream.c:766`). Every playback child process
then reuses the parent's base for all socket calls *and* passes it to
`InitAmiSSL(AmiSSL_SocketBase, SocketBase, ...)` (`radio_stream.c:823`).

bsdsocket.library bases are per-opener: AmiTCP/Roadshow bind the base to the
opening process (signals, per-base errno pointer, DNS state), and their SDKs
state a base must not be shared between processes. Additionally, per the
`AmiSSL_ErrNoPtr` autodoc, each child's `InitAmiSSL()` will issue
`SBTM_SETVAL(SBTC_ERRNOLONGPTR)` against the *parent's* base — the GUI task's
probes and each child keep re-pointing the same base's errno pointer at the
same global `&errno` (one shared C global for all tasks in this hunk-format
binary), so errno reporting between tasks races. This works under WinUAE's
lenient bsdsocket emulation but is off-spec on real stacks, and is a credible
contributor to the unexplained heap corruption the debug comments chase.

**Fix:** have each playback child open its own `bsdsocket.library` base at
task start (stored per-`RadioStream`/per-task, not in the shared global),
pass *that* to its `InitAmiSSL()`, use it for the child's socket calls, and
close it before task exit. The GUI/opener keeps its own base for probes.

## F5 — Probe lazy fallback can create a second, never-closed shared instance (Rule 1) — LOW

Because the probe file's weak `AmiSSLBase`/`AmiSSLExtBase` do not merge with
`radio_stream.c`'s strong definitions under the m68k hunk linker
(`radio_stream.c:2005-2015`), the probe keeps private copies and adopts the
shared instance via `Radio_GetAmiSslShared()`. If `Radio_NetworkInit()`'s
open failed and a probe runs before any playback child, the probe opens its
own instance (`radio_stream_probe.c:659`, `rb_probe_opened_shared_here=1`) —
but `Radio_NetworkShutdown()` only closes `radio_stream.c`'s copy, so that
instance is never `CloseAmiSSL()`'d, and a later playback child can open a
*second* shared instance in parallel. Harmless in the normal startup order;
worth a guard (e.g. publish the probe-opened bases back through a setter, or
drop the probe fallback in GUI builds).

## Compliant areas

- **Rule 3:** the playback child always reaches `radio_ssl_global_init()`
  (which performs `InitAmiSSL` for non-opener tasks) via
  `radio_ssl_connect()` (`radio_stream.c:979`) before `SSLv23_client_method()`
  / `SSL_CTX_new()` / `SSL_new()` / `SSL_connect()`; `SSL_read` only after a
  completed handshake. Correct ordering throughout.
- **Rule 5:** opener-task `CleanupAmiSSL` is skipped
  (`radio_stream.c:860-866`, `radio_stream_probe.c:721-727`) — required once
  F1's `AmiSSL_InitAmiSSL` fix lands, and harmless today.
- **Rule 6:** no `CloseLibrary(AmiSSLExtBase)` anywhere; both shutdown paths
  only NULL the pointer after `CloseAmiSSL()` (`radio_stream.c:2111-2114`,
  `amissl_https_get.c:96-101`). Correct.
- **Rule 7:** `AmiSSL_ErrNoPtr` is passed at every `InitAmiSSL` call site
  (`radio_stream.c:823-825`, `radio_stream_probe.c:684-686`,
  `amissl_https_get.c:193-195`) and at both `OpenAmiSSLTags` sites. Correct
  (subject to the shared-errno caveat in F4).
- **Rule 10 (TLS-error path):** on `SSL_ERROR_SSL`/fatal error-queue faults
  from `SSL_connect`/`SSL_read` the code stops the stream, blocks the host
  until restart (`Radio_NoteTlsFaultHost` / `Radio_IsTlsFaultHost`,
  `radio_stream.c:953,974`), quarantines the session's objects, and keeps
  HTTPS enabled for fresh sessions. The optional "parent-side AmiSSL soft
  reset" is not implemented (allowed — it is optional). Hard-disable on heap
  corruption works for playback but not probes (see F3).
- **`amissl_https_get.c`** is fully compliant and is the correct reference
  pattern for the manual (no `AmiSSL_InitAmiSSL`) lifecycle:
  `OpenAmiSSLTags` → explicit `InitAmiSSL` → SSL use → `CleanupAmiSSL` →
  `CloseAmiSSL` → `CloseLibrary(master)`, never touching `AmiSSLExtBase`.

## Recommended fix order

1. **F1** — add `AmiSSL_InitAmiSSL, TRUE` to both `OpenAmiSSLTags()` sites
   (2-line change, removes an entire class of GUI-task undefined behaviour).
2. **F2** — re-gate the playback-close SSL_free/SSL_CTX_free/CleanupAmiSSL
   skip on actual poison state so clean runs clean up and `CloseAmiSSL()`
   runs at exit again.
3. **F3** — propagate MiniMem poison into the TLS quarantine gates.
4. **F4** — per-child `bsdsocket.library` base (larger change; do under a
   debug soak like the ones documented in the file comments).
5. **F5** — optional hardening of the probe lazy-fallback path.
