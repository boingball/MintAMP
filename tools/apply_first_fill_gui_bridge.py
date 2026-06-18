from pathlib import Path

p = Path('amiga_mp3gui.c')
s = p.read_text()

old = '\t\t\tint stage = gGuiPlaybackStatus.startupStage;\n'
new = ('\t\t\tint stage = gFirstFillDiagnosticStage ?\n'
       '\t\t\t\tgFirstFillDiagnosticStage : gGuiPlaybackStatus.startupStage;\n')
if new not in s:
    if old not in s:
        raise SystemExit('GUI startup stage read not found')
    s = s.replace(old, new, 1)

old = '\tgui->playbackRunId = ++gPlaybackRunCounter;\n'
new = ('\tgFirstFillDiagnosticStage = 0;\n'
       '\tgui->playbackRunId = ++gPlaybackRunCounter;\n')
if new not in s:
    if old not in s:
        raise SystemExit('playback run-id reset point not found')
    s = s.replace(old, new, 1)

p.write_text(s)
