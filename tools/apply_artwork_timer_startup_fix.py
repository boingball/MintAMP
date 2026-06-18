from pathlib import Path

p = Path('amiga_mp3gui.c')
s = p.read_text()

old = '''\t/* Let audio startup own the process/DOS runtime until the child has entered
\t * steady playback.  Artwork decoding resumes once PLAYING is published. */
\tif (!gui->playbackActive ||
\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN)
\t\tPumpArtDecode(gui);
\tSendTimerRequest(gui, gui->artDecode.active ? ART_TIMER_MICROS :
\t\tTIMER_TICK_MICROS);
'''
new = '''\t/* Let audio startup own the process/DOS runtime until the child has entered
\t * steady playback.  While artwork is paused, use the normal timer cadence:
\t * repeatedly scheduling ART_TIMER_MICROS would make every expiry look like
\t * an artwork tick, skip playback-status polling, and can starve the child. */
\t{
\t\tint artCanPump = !gui->playbackActive ||
\t\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
\t\t\tgGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN;

\t\tif (artCanPump)
\t\t\tPumpArtDecode(gui);
\t\tSendTimerRequest(gui, gui->artDecode.active && artCanPump ?
\t\t\tART_TIMER_MICROS : TIMER_TICK_MICROS);
\t}
'''
if new not in s:
    if old not in s:
        raise SystemExit('artwork startup timer block not found')
    s = s.replace(old, new, 1)

p.write_text(s)
