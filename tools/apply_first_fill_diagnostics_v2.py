from pathlib import Path

p = Path('amiga_mp3dec.c')
s = p.read_text()

if 'GUISTART_FILL_PLANAR_ENTER' not in s:
    s = s.replace(
        '#define GUISTART_FILL_BUFFER_A         200\n',
        '#define GUISTART_FILL_BUFFER_A         200\n'
        '#define GUISTART_FILL_PLANAR_ENTER     201\n'
        '#define GUISTART_FILL_PLANAR_READ      202\n'
        '#define GUISTART_FILL_PLANAR_SYNC      203\n'
        '#define GUISTART_FILL_PLANAR_DECODE    204\n'
        '#define GUISTART_FILL_PLANAR_DECODED   205\n'
        '#define GUISTART_FILL_PLANAR_CONVERT   206\n'
        '#define GUISTART_FILL_PLANAR_COPIED    207\n', 1)

start = s.find('static int DecodeStreamFillPlanarS8(')
end = s.find('\n/* Shared status block', start)
if start < 0 or end < 0:
    raise SystemExit('planar fill function bounds not found')
f = s[start:end]

def add_before(needle, code, marker):
    global f
    if marker in f:
        return
    pos = f.find(needle)
    if pos < 0:
        raise SystemExit('missing marker: ' + marker)
    f = f[:pos] + code + f[pos:]

add_before(
    '\tproduced = 0;\n',
    '\tif (gGuiPlaybackStatus.startupStage == GUISTART_FILL_BUFFER_A)\n'
    '\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_ENTER;\n',
    'GUISTART_FILL_PLANAR_ENTER;')

add_before(
    '\t\t\tnRead = FillReadBuffer(',
    '\t\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)\n'
    '\t\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_READ;\n',
    'GUISTART_FILL_PLANAR_READ;')

add_before(
    '\t\toffset = FindValidatedMpegSync(',
    '\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)\n'
    '\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_SYNC;\n',
    'GUISTART_FILL_PLANAR_SYNC;')

add_before(
    '\t\tif (stream->timing) {\n',
    '\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)\n'
    '\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_DECODE;\n',
    'GUISTART_FILL_PLANAR_DECODE;')

needle = '\t\tif (gPlaybackInterrupted)\n\t\t\tbreak;\n'
add_before(
    needle,
    '\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)\n'
    '\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_DECODED;\n',
    'GUISTART_FILL_PLANAR_DECODED;')

add_before(
    '\t\tMP3GetLastFrameInfo(stream->decoder, &info);\n',
    '\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)\n'
    '\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_CONVERT;\n',
    'GUISTART_FILL_PLANAR_CONVERT;')

add_before(
    '\t\tif (stream->timing)\n\t\t\tstream->timing->pcmConvert += clock() - t0;\n',
    '\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)\n'
    '\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_COPIED;\n',
    'GUISTART_FILL_PLANAR_COPIED;')

s = s[:start] + f + s[end:]
p.write_text(s)
