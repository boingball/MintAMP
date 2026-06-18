from pathlib import Path

p = Path('amiga_mp3gui.c')
s = p.read_text()

def replace_once(old, new, marker, error):
    global s
    if marker in s:
        return
    if old not in s:
        raise SystemExit(error)
    s = s.replace(old, new, 1)

replace_once(
'''\tint artValid;
\tint artLoading;
''',
'''\tint artValid;
\tint artLoading;
\tint artCacheSavePending;
''',
'artCacheSavePending;',
'GUI artwork fields not found')

replace_once(
'''\t\tmemcpy(gui->artGreyBuf, st->greyOut, ART_W * ART_H);
\t\tgui->artValid = 1;
\t\tSaveArtworkCache(gui);
''',
'''\t\tmemcpy(gui->artGreyBuf, st->greyOut, ART_W * ART_H);
\t\tgui->artValid = 1;
\t\t/* Do not perform stdio/DOS cache writes while the playback child is
\t\t * opening or reading its MP3.  The Amiga C/DOS runtime is shared by both
\t\t * tasks and rapid track changes can otherwise overlap the first audio read
\t\t * with artwork-cache IO. */
\t\tif (gui->playbackActive)
\t\t\tgui->artCacheSavePending = 1;
\t\telse
\t\t\tSaveArtworkCache(gui);
''',
'artCacheSavePending = 1;',
'FinishArtDecode cache-save block not found')

replace_once(
'''\tPumpArtDecode(gui);
\tSendTimerRequest(gui, gui->artDecode.active ? ART_TIMER_MICROS :
\t\tTIMER_TICK_MICROS);
''',
'''\t/* Let audio startup own the process/DOS runtime until the child has entered
\t * steady playback.  Artwork decoding resumes once PLAYING is published. */
\tif (!gui->playbackActive ||
\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN)
\t\tPumpArtDecode(gui);
\tSendTimerRequest(gui, gui->artDecode.active ? ART_TIMER_MICROS :
\t\tTIMER_TICK_MICROS);
''',
'Let audio startup own the process/DOS runtime',
'HandleTimerSignal artwork pump block not found')

replace_once(
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
\tgui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
''',
'if (gui->artCacheSavePending)',
'FinalizePlayback reset block not found')

p.write_text(s)
