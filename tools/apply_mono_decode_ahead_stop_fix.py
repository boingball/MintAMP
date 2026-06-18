from pathlib import Path

p = Path('amiga_mp3dec.c')
s = p.read_text()

def rep(old, new, marker, error):
    global s
    if marker in s:
        return
    if old not in s:
        raise SystemExit(error)
    s = s.replace(old, new, 1)

rep(
'''/* Keep no more than two live audio.device writes per channel.  The earlier
 * three-request mono ring can leave Stop blocked while audio.device unwinds the
 * queued writes on real hardware.  Arrays remain sized for the stereo Fast-RAM
 * decode-ahead slot C, but only A/B are submitted to Paula. */
#define AMIGA_MONO_AUDIO_SLOTS 3
#define AMIGA_STEREO_AUDIO_SLOTS 2
#define AMIGA_STEREO_DECODE_SLOTS 3
#define AMIGA_AUDIO_PLAYBACK_SLOTS AMIGA_MONO_AUDIO_SLOTS
''',
'''/* Keep no more than two live audio.device writes per channel.  Both mono and
 * stereo submit only A/B to Paula.  Slot C remains a Fast RAM decode-ahead
 * buffer so Stop does not wait for a third live mono CMD_WRITE to unwind. */
#define AMIGA_MONO_AUDIO_SLOTS 2
#define AMIGA_STEREO_AUDIO_SLOTS 2
#define AMIGA_STEREO_DECODE_SLOTS 3
#define AMIGA_AUDIO_PLAYBACK_SLOTS AMIGA_STEREO_DECODE_SLOTS
''',
'#define AMIGA_MONO_AUDIO_SLOTS 2',
'audio slot constants block not found')

rep(
'''\t/* Fill decode buffers before the first CMD_WRITE starts playback.  Mono
\t * remains a true three-request audio.device ring.  Stereo queues only two
\t * live DMA pairs (A/B) and keeps C as a Fast RAM decode-ahead buffer; C is
\t * copied into whichever A/B chip pair has been WaitIO-reaped. */
\tgGuiPlaybackStatus.phase = GUIPLAY_PHASE_BUFFERING;
\tplaybackChannels = opt->stereo ? 2UL : 1UL;
\tliveSlots = AmigaAudioLiveSlots(opt->stereo);
\tdecodeAhead = opt->stereo ? 2 : -1;
\tinitialDecodeSlots = opt->stereo ? AMIGA_STEREO_DECODE_SLOTS : liveSlots;
''',
'''\t/* Fill decode buffers before the first CMD_WRITE starts playback.  Mono and
\t * stereo queue only two live DMA slots (A/B).  Slot C is used as decode-ahead
\t * whenever the decode path can refill independently of the live chip buffer. */
\tgGuiPlaybackStatus.phase = GUIPLAY_PHASE_BUFFERING;
\tplaybackChannels = opt->stereo ? 2UL : 1UL;
\tliveSlots = AmigaAudioLiveSlots(opt->stereo);
\tdecodeAhead = (opt->stereo || opt->fastLowrate) ? 2 : -1;
\tinitialDecodeSlots = decodeAhead >= 0 ? AMIGA_STEREO_DECODE_SLOTS : liveSlots;
''',
'decodeAhead = (opt->stereo || opt->fastLowrate) ? 2 : -1;',
'decode-ahead setup block not found')

rep(
'''\t\tif (opt->debugPlay) {
\t\tprintf("debug-play: CMD_WRITE queued initial ring depth %d\\n",
\t\t\tactive < liveSlots ? active : liveSlots);
\t\tif (opt->stereo)
\t\t\tprintf("debug-play: stereo decode-ahead buffer C prepared: %lu bytes\\n",
\t\t\t\tlen[decodeAhead]);
\t}
''',
'''\tif (opt->debugPlay) {
\t\tprintf("debug-play: CMD_WRITE queued initial ring depth %d\\n",
\t\t\tactive < liveSlots ? active : liveSlots);
\t\tif (decodeAhead >= 0)
\t\t\tprintf("debug-play: decode-ahead buffer C prepared: %lu bytes\\n",
\t\t\t\tlen[decodeAhead]);
\t}
''',
'debug-play: decode-ahead buffer C prepared',
'debug decode-ahead message block not found')

rep(
'''\t\tif (opt->stereo) {
\t\t\tactiveMilliseconds = PlaybackBufferDurationMilliseconds(opt,
\t\t\t\tlen[decodeAhead], playbackRate);
\t\t\tif (len[decodeAhead] == 0) {
\t\t\t\tactive = (active + 1) % liveSlots;
\t\t\t\tbreak;
\t\t\t}
\t\t\tif (AmigaAudioCopyStereoDecodeAheadToSlot(&player, justFreed,
\t\t\t\tdecodeAhead, len[decodeAhead]) != 0) {
\t\t\t\tfprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\\n",
\t\t\t\t\tPlaybackBufferName(justFreed));
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t\tlen[justFreed] = len[decodeAhead];
\t\t} else {
\t\t\tactiveMilliseconds = PlaybackBufferDurationMilliseconds(opt,
\t\t\t\tlen[justFreed], playbackRate);
\t\t\tif (gPlaybackInterrupted)
\t\t\t\tbreak;
\t\t\tlen[justFreed] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
\t\t\t\tjustFreed, buf[justFreed], bufBytes);
\t\t\tPrintPlaybackFillDebug(opt, justFreed, len[justFreed]);
\t\t\tif (stream.decodeError) {
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t\tif (len[justFreed] == 0) {
\t\t\t\tactive = (active + 1) % liveSlots;
\t\t\t\tbreak;
\t\t\t}
\t\t}
''',
'''\t\tif (opt->stereo) {
\t\t\tactiveMilliseconds = PlaybackBufferDurationMilliseconds(opt,
\t\t\t\tlen[decodeAhead], playbackRate);
\t\t\tif (len[decodeAhead] == 0) {
\t\t\t\tactive = (active + 1) % liveSlots;
\t\t\t\tbreak;
\t\t\t}
\t\t\tif (AmigaAudioCopyStereoDecodeAheadToSlot(&player, justFreed,
\t\t\t\tdecodeAhead, len[decodeAhead]) != 0) {
\t\t\t\tfprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\\n",
\t\t\t\t\tPlaybackBufferName(justFreed));
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t\tlen[justFreed] = len[decodeAhead];
\t\t} else if (decodeAhead >= 0) {
\t\t\tactiveMilliseconds = PlaybackBufferDurationMilliseconds(opt,
\t\t\t\tlen[decodeAhead], playbackRate);
\t\t\tif (len[decodeAhead] == 0) {
\t\t\t\tactive = (active + 1) % liveSlots;
\t\t\t\tbreak;
\t\t\t}
\t\t\tmemcpy(buf[justFreed], buf[decodeAhead], (size_t)len[decodeAhead]);
\t\t\tlen[justFreed] = len[decodeAhead];
\t\t} else {
\t\t\tactiveMilliseconds = PlaybackBufferDurationMilliseconds(opt,
\t\t\t\tlen[justFreed], playbackRate);
\t\t\tif (gPlaybackInterrupted)
\t\t\t\tbreak;
\t\t\tlen[justFreed] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
\t\t\t\tjustFreed, buf[justFreed], bufBytes);
\t\t\tPrintPlaybackFillDebug(opt, justFreed, len[justFreed]);
\t\t\tif (stream.decodeError) {
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t\tif (len[justFreed] == 0) {
\t\t\t\tactive = (active + 1) % liveSlots;
\t\t\t\tbreak;
\t\t\t}
\t\t}
''',
'} else if (decodeAhead >= 0) {',
'main-loop decode-ahead reuse block not found')

rep(
'''\t\tif (opt->stereo) {
\t\t\tif (gPlaybackInterrupted)
\t\t\t\tbreak;
\t\t\tlen[decodeAhead] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
\t\t\t\tdecodeAhead, buf[decodeAhead], bufBytes);
\t\t\tPrintPlaybackFillDebug(opt, decodeAhead, len[decodeAhead]);
\t\t\tif (stream.decodeError) {
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t}
''',
'''\t\tif (opt->stereo) {
\t\t\tif (gPlaybackInterrupted)
\t\t\t\tbreak;
\t\t\tlen[decodeAhead] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
\t\t\t\tdecodeAhead, buf[decodeAhead], bufBytes);
\t\t\tPrintPlaybackFillDebug(opt, decodeAhead, len[decodeAhead]);
\t\t\tif (stream.decodeError) {
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t} else if (decodeAhead >= 0) {
\t\t\tif (gPlaybackInterrupted)
\t\t\t\tbreak;
\t\t\tlen[decodeAhead] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
\t\t\t\tdecodeAhead, buf[decodeAhead], bufBytes);
\t\t\tPrintPlaybackFillDebug(opt, decodeAhead, len[decodeAhead]);
\t\t\tif (stream.decodeError) {
\t\t\t\terr = -1;
\t\t\t\tbreak;
\t\t\t}
\t\t}
''',
'} else if (decodeAhead >= 0) {\n\t\t\tif (gPlaybackInterrupted)',
'mono decode-ahead refill block not found')

p.write_text(s)
