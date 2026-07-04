# AmiSSL TLS lifecycle manual regression tests

Use these tests after changing AmiSSL cleanup, poison, or quarantine logic.
They are intended for an Amiga/AmiSSL environment because the failure mode is
resident AmiSSL state corruption across stream stops and app relaunches.

## Test A: same app run

1. Start an HTTPS station.
2. Stop playback.
3. Start another HTTPS station.
4. Stop playback.
5. Repeat the start/stop cycle at least 4 times.
6. Confirm there is no crash, freeze, or unexpected AmiSSL quarantine log.

Expected clean-stop log characteristics:

- `SSL_shutdown` may be skipped for abort-mode stops.
- `SSL_free` runs unless the session was explicitly poisoned.
- `SSL_CTX_free` runs when the playback session closes unless the session was explicitly poisoned.
- `CleanupAmiSSL skipped (poisoned)` is not logged for clean stops.
- `shutdown_quarantine=0` remains true if no real TLS fault occurred.

## Test B: app close and relaunch

1. Start an HTTPS station.
2. Stop playback.
3. Exit the app normally.
4. Relaunch the app without rebooting Amiga.
5. Start an HTTPS station.
6. Confirm it does not crash or freeze at the `rb-probe TLS` line.

Expected shutdown log characteristics after a clean run:

- `tls_fault_count=0`.
- `tls_poisoned=0`.
- No message claiming AmiSSL was quarantined after TLS faults.
- Final AmiSSL shutdown is allowed to run normally.

## Test C: fault path

1. Force a bad or invalid HTTPS host that produces a real SSL/AmiSSL error.
2. Confirm the failing session records the TLS fault host.
3. Confirm the failing session is marked poisoned/quarantined.
4. Confirm unsafe `SSL_free`, `SSL_CTX_free`, and per-task `CleanupAmiSSL` are skipped for that bad session.
5. Confirm quarantine is attributed to the real TLS fault, not to a clean user stop.
6. Confirm other non-faulting hosts remain usable during the same app run.

## Probe quarantine regression tests

### Test 1: HTTPS AACP station JOE

1. Start `https://audio-streaming.joe.nl/Joe_nl_high.aac`.
2. Confirm there is no `CORRUPT bad chunk size` after probe close.
3. Confirm playback either starts cleanly or fails gracefully without a recoverable alert.
4. Confirm AACP stays enabled when the heap remains clean.

### Test 2: HTTPS sequence JOE / AAC / Dance Wave

1. Start an HTTPS AAC station.
2. Stop playback.
3. Start the HTTPS AACP JOE station.
4. Stop playback.
5. Start Dance Wave.
6. Confirm there is no recoverable alert loop.

### Test 3: corrupt-after-probe gate

1. Force or reproduce heap corruption immediately after HTTPS probe close.
2. Confirm the UI reports that playback is blocked and MiniAMP3 should be restarted.
3. Confirm no `PlaybackEntry` child task is spawned after the corrupt probe.
4. Confirm artwork is not loaded after the corrupt probe.
