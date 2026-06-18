from pathlib import Path

p = Path('amiga_mp3gui.c')
s = p.read_text()

old = '''static void DrawArtPanel(HelixAmp3Gui *gui);
static void HandleDoneSignal(HelixAmp3Gui *gui);
static void SaveArtworkCache(HelixAmp3Gui *gui);
'''
new = '''static void DrawArtPanel(HelixAmp3Gui *gui);
static void DrawTransportIcons(HelixAmp3Gui *gui);
static void HandleDoneSignal(HelixAmp3Gui *gui);
static void SaveArtworkCache(HelixAmp3Gui *gui);
'''
if new not in s:
    if old not in s:
        raise SystemExit('forward declaration block not found')
    s = s.replace(old, new, 1)

p.write_text(s)
