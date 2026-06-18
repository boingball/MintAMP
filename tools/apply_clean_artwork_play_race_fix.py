from pathlib import Path

p = Path('amiga_mp3gui.c')
s = p.read_text()

def rep(old, new, marker, error):
    global s
    if marker in s:
        return
    if old not in s:
        raise SystemExit(error)
    s = s.replace(old, new, 1)

# Track artwork work that has been paused while the playback child starts, and
# defer cache writes until playback has left the shared DOS/runtime critical path.
rep(
'''\tint artValid;
\tint artLoading;
''',
'''\tint artValid;
\tint artLoading;
\tint artRestartPending;
\tint artCacheSavePending;
''',
'artRestartPending;',
'GUI artwork state fields not found')

# Do not write the artwork cache while the playback child is opening/reading.
rep(
'''\t\tmemcpy(gui->artGreyBuf, st->greyOut, ART_W * ART_H);
\t\tgui->artValid = 1;
\t\tSaveArtworkCache(gui);
''',
'''\t\tmemcpy(gui->artGreyBuf, st->greyOut, ART_W * ART_H);
\t\tgui->artValid = 1;
\t\t/* The GUI and playback child share the same AmigaDOS/C runtime state.
\t\t * Avoid overlapping artwork-cache writes with playback startup. */
\t\tif (gui->playbackActive)
\t\t\tgui->artCacheSavePending = 1;
\t\telse
\t\t\tSaveArtworkCache(gui);
''',
'artCacheSavePending = 1;',
'FinishArtDecode cache save block not found')

# On Play, pause incremental artwork work first, then launch playback immediately.
rep(
'''\tcase GID_PLAY:
\t\tStartPlayback(gui);
\t\tbreak;
''',
'''\tcase GID_PLAY:
\t\tif (gui->playbackActive || gui->playbackDonePending) {
\t\t\tSetStatus(gui, "Playback is already starting or active.");
\t\t\tbreak;
\t\t}
\t\t/* If artwork is still decoding, pause it before the playback child is
\t\t * created.  Rapid Browse->Play can otherwise overlap GUI artwork work
\t\t * with the child task's first file reads on shared AmigaDOS/C runtime state. */
\t\tif (gui->artDecode.active || gui->artLoading) {
\t\t\tgui->artDecode.active = 0;
\t\t\tgui->artRestartPending = 1;
\t\t\tgui->artLoading = 1;
\t\t}
\t\tStartPlayback(gui);
\t\tbreak;
''',
'Rapid Browse->Play can otherwise overlap GUI artwork work',
'GID_PLAY handler not found')

# While playback startup is active, do not run artwork decode slices.  Also avoid
# keeping the GUI on the fast artwork timer when artwork is paused.
rep(
'''\tPumpArtDecode(gui);
\tSendTimerRequest(gui, gui->artDecode.active ? ART_TIMER_MICROS :
\t\tTIMER_TICK_MICROS);
''',
'''\t{
\t\tint artCanPump = !gui->playbackActive ||
\t\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
\t\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN;

\t\tif (artCanPump)
\t\t\tPumpArtDecode(gui);
\t\tSendTimerRequest(gui, gui->artDecode.active && artCanPump ?
\t\t\tART_TIMER_MICROS : TIMER_TICK_MICROS);
\t}
''',
'artCanPump = !gui->playbackActive',
'HandleTimerSignal artwork pump block not found')

# Restart paused artwork once audio is steady.
rep(
'''\t\tcase GUIPLAY_PHASE_PLAYING: {
''',
'''\t\tcase GUIPLAY_PHASE_PLAYING: {
\t\t\tif (gui->artRestartPending) {
\t\t\t\tgui->artRestartPending = 0;
\t\t\t\tStartArtDecode(gui);
\t\t\t}
''',
'gui->artRestartPending = 0;',
'PLAYING phase block not found')

# Flush deferred artwork work after playback teardown.
rep(
'''\tgGuiPlayer.stopRequested = 0;
\tgPlaybackInterrupted = 0;
\tgui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
''',
'''\tgGuiPlayer.stopRequested = 0;
\tgPlaybackInterrupted = 0;
\tif (gui->artCacheSavePending) {
\t\tgui->artCacheSavePending = 0;
\t\tSaveArtworkCache(gui);
\t}
\tif (gui->artRestartPending) {
\t\tgui->artRestartPending = 0;
\t\tStartArtDecode(gui);
\t}
\tgui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
''',
'if (gui->artCacheSavePending)',
'FinalizePlayback reset block not found')

p.write_text(s)
