from pathlib import Path

path = Path('amiga_mp3dec.c')
text = path.read_text()

# Add diagnostic stage values beside the existing startup stage definitions.
anchor = '#define GUISTART_FILL_BUFFER_A         200\n'
extra = '''#define GUISTART_FILL_BUFFER_A         200
#define GUISTART_FILL_PLANAR_ENTER     201
#define GUISTART_FILL_PLANAR_READ      202
#define GUISTART_FILL_PLANAR_SYNC      203
#define GUISTART_FILL_PLANAR_DECODE    204
#define GUISTART_FILL_PLANAR_DECODED   205
#define GUISTART_FILL_PLANAR_CONVERT   206
#define GUISTART_FILL_PLANAR_COPIED    207
'''
if 'GUISTART_FILL_PLANAR_ENTER' not in text:
    if anchor not in text:
        raise SystemExit('startup stage anchor not found')
    text = text.replace(anchor, extra, 1)

start_marker = 'static int DecodeStreamFillPlanarS8(DecodeStream *stream, const DecodeOptions *opt,'
end_marker = '\nstatic int DecodeStreamFill'
start = text.find(start_marker)
if start < 0:
    raise SystemExit('planar fill function not found')
end = text.find(end_marker, start + len(start_marker))
if end < 0:
    # The next function has a different name in some revisions; use the next top-level static.
    end = text.find('\nstatic ', start + len(start_marker))
if end < 0:
    raise SystemExit('end of planar fill function not found')

func = text[start:end]


def replace_once_in_func(old, new, marker, error):
    global func
    if marker in func:
        return
    if old not in func:
        raise SystemExit(error)
    func = func.replace(old, new, 1)

replace_once_in_func(
'''static int DecodeStreamFillPlanarS8(DecodeStream *stream, const DecodeOptions *opt,
\tsigned char *left, signed char *right, int maxFrames)
{
\tMP3FrameInfo info;
\tint produced;

\tproduced = 0;
''',
'''static int DecodeStreamFillPlanarS8(DecodeStream *stream, const DecodeOptions *opt,
\tsigned char *left, signed char *right, int maxFrames)
{
\tMP3FrameInfo info;
\tint produced;

\tif (gGuiPlaybackStatus.startupStage == GUISTART_FILL_BUFFER_A)
\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_ENTER;
\tproduced = 0;
''',
'GUISTART_FILL_PLANAR_ENTER;',
'planar entry block not found')

replace_once_in_func(
'''\t\tif (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
\t\t\tnRead = FillReadBuffer(stream->readBuf, stream->readPtr,
\t\t\t\tREADBUF_SIZE, stream->bytesLeft, stream->input);
\t\t\tstream->bytesLeft += nRead;
\t\t\tstream->readPtr = stream->readBuf;
\t\t\tif (nRead == 0)
\t\t\t\tstream->eofReached = 1;
\t\t}

\t\toffset = FindValidatedMpegSync(stream->readPtr, stream->bytesLeft);
''',
'''\t\tif (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
\t\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)
\t\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_READ;
\t\t\tnRead = FillReadBuffer(stream->readBuf, stream->readPtr,
\t\t\t\tREADBUF_SIZE, stream->bytesLeft, stream->input);
\t\t\tstream->bytesLeft += nRead;
\t\t\tstream->readPtr = stream->readBuf;
\t\t\tif (nRead == 0)
\t\t\t\tstream->eofReached = 1;
\t\t}

\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)
\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_SYNC;
\t\toffset = FindValidatedMpegSync(stream->readPtr, stream->bytesLeft);
''',
'GUISTART_FILL_PLANAR_READ;',
'planar read/sync block not found')

replace_once_in_func(
'''\t\tif (stream->timing) {
\t\t\tt0 = clock();
\t\t\terr = MP3Decode(stream->decoder, &stream->readPtr,
\t\t\t\t&stream->bytesLeft, stream->decodeBuf, 0);
\t\t\tstream->timing->frameDecode += clock() - t0;
\t\t} else {
\t\t\terr = MP3Decode(stream->decoder, &stream->readPtr,
\t\t\t\t&stream->bytesLeft, stream->decodeBuf, 0);
\t\t}
''',
'''\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)
\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_DECODE;
\t\tif (stream->timing) {
\t\t\tt0 = clock();
\t\t\terr = MP3Decode(stream->decoder, &stream->readPtr,
\t\t\t\t&stream->bytesLeft, stream->decodeBuf, 0);
\t\t\tstream->timing->frameDecode += clock() - t0;
\t\t} else {
\t\t\terr = MP3Decode(stream->decoder, &stream->readPtr,
\t\t\t\t&stream->bytesLeft, stream->decodeBuf, 0);
\t\t}
\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)
\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_DECODED;
''',
'GUISTART_FILL_PLANAR_DECODE;',
'planar MP3Decode block not found')

replace_once_in_func(
'''\t\tMP3GetLastFrameInfo(stream->decoder, &info);
\t\tUpdateFirstFrameStats(stream->stats, &info);
''',
'''\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)
\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_CONVERT;
\t\tMP3GetLastFrameInfo(stream->decoder, &info);
\t\tUpdateFirstFrameStats(stream->stats, &info);
''',
'GUISTART_FILL_PLANAR_CONVERT;',
'planar conversion block not found')

replace_once_in_func(
'''\t\tstream->stats->outputSamples += (unsigned long)(frames * 2);
\t\tstream->stats->decodedFrames++;
''',
'''\t\tstream->stats->outputSamples += (unsigned long)(frames * 2);
\t\tstream->stats->decodedFrames++;
\t\tif (gGuiPlaybackStatus.startupStage < GUISTART_FILL_BUFFER_A_DONE)
\t\t\tgGuiPlaybackStatus.startupStage = GUISTART_FILL_PLANAR_COPIED;
''',
'GUISTART_FILL_PLANAR_COPIED;',
'planar copied marker not found')

text = text[:start] + func + text[end:]
path.write_text(text)
