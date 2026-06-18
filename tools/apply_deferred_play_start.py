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

rep(
'''\tint artValid;
\tint artLoading;
''',
'''\tint artValid;
\tint artLoading;
\tint artRestartPending;
''',
'artRestartPending;',
'GUI state fields not found')

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
\t\t/* Stop incremental artwork work before creating the playback child.
\t\t * Starting directly here avoids the timer-deferred launch getting stuck
\t\t * on an already-pending artwork timer.  The artwork decoder is restarted
\t\t * once audio reaches steady PLAYING. */
\t\tif (gui->artDecode.active || gui->artLoading) {
\t\t\tgui->artDecode.active = 0;
\t\t\tgui->artRestartPending = 1;
\t\t\tgui->artLoading = 1;
\t\t}
\t\tStartPlayback(gui);
\t\tbreak;
''',
'Starting directly here avoids the timer-deferred launch',
'GID_PLAY handler not found')

rep(
'''\tcase GUIPLAY_PHASE_PLAYING: {
''',
'''\tcase GUIPLAY_PHASE_PLAYING: {
\t\t\tif (gui->artRestartPending) {
\t\t\t\tgui->artRestartPending = 0;
\t\t\t\tStartArtDecode(gui);
\t\t\t}
''',
'if (gui->artRestartPending)',
'PLAYING phase block not found')

rep(
'''\tgGuiPlayer.stopRequested = 0;
\tgPlaybackInterrupted = 0;
''',
'''\tgGuiPlayer.stopRequested = 0;
\tgPlaybackInterrupted = 0;
\tif (gui->artRestartPending) {
\t\tgui->artRestartPending = 0;
\t\tStartArtDecode(gui);
\t}
''',
'if (gui->artRestartPending) {',
'FinalizePlayback reset point not found')

p.write_text(s)
