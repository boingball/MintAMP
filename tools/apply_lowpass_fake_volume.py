from pathlib import Path

# Patch decoder
p = Path('amiga_mp3dec.c')
s = p.read_text()

def rep(old, new, marker, error):
    global s
    if marker in s:
        return
    if old not in s:
        raise SystemExit(error)
    s = s.replace(old, new, 1)

rep('''\tint fakeStereo;
\tint fakeStereoDelay;
\tint fakeStereoShift;
''','''\tint fakeStereo;
\tint fakeStereoDelay;
\tint fakeStereoShift;
\tint lowPass;
''','int lowPass;','DecodeOptions fake stereo block not found')

rep('''\tprintf("  --fake-stereo-shift K  fake-stereo cross-bleed >>K (0-8, default %d; higher=wider, 0=mono)\\n",
\t\tFAKE_STEREO_DEFAULT_SHIFT);
''','''\tprintf("  --fake-stereo-shift K  fake-stereo cross-bleed >>K (0-8, default %d; higher=wider, 0=mono)\\n",
\t\tFAKE_STEREO_DEFAULT_SHIFT);
\tprintf("  --low-pass   soften Paula output with a light one-pole low-pass filter\\n");
''','--low-pass   soften Paula output','usage fake stereo shift block not found')

rep('''\t\t} else if (!strcmp(argv[i], "--fake-stereo-shift")) {
\t\t\tif (i + 1 >= argc) {
\t\t\t\tfprintf(stderr, "--fake-stereo-shift requires a value (0-8)\\n");
\t\t\t\treturn -1;
\t\t\t}
\t\t\ti++;
\t\t\topt->fakeStereoShift = atoi(argv[i]);
\t\t} else if (!strcmp(argv[i], "--play-fast-path")) {
''','''\t\t} else if (!strcmp(argv[i], "--fake-stereo-shift")) {
\t\t\tif (i + 1 >= argc) {
\t\t\t\tfprintf(stderr, "--fake-stereo-shift requires a value (0-8)\\n");
\t\t\t\treturn -1;
\t\t\t}
\t\t\ti++;
\t\t\topt->fakeStereoShift = atoi(argv[i]);
\t\t} else if (!strcmp(argv[i], "--low-pass")) {
\t\t\topt->lowPass = 1;
\t\t} else if (!strcmp(argv[i], "--play-fast-path")) {
''','opt->lowPass = 1;','parse fake stereo shift block not found')

rep('''static signed char Sample16ToS8(short s)
{
\treturn (signed char)(s >> 8);
}
''','''static signed char Sample16ToS8(short s)
{
\treturn (signed char)(s >> 8);
}

static short ClipToS16(int v)
{
\tif (v > 32767)
\t\treturn 32767;
\tif (v < -32768)
\t\treturn -32768;
\treturn (short)v;
}
''','static short ClipToS16','Sample16ToS8 block not found')

rep('''\tl = mono + (d >> fs->shift);
\tr = d + (mono >> fs->shift);
\tif (l > 32767) l = 32767; else if (l < -32768) l = -32768;
\tif (r > 32767) r = 32767; else if (r < -32768) r = -32768;
\t*outL = (short)l;
\t*outR = (short)r;
''','''\tl = mono + (d >> fs->shift);
\tr = d + (mono >> fs->shift);
\t/* Pseudo-stereo sounds quieter than the real mono path because centre energy
\t * is spread across channels.  Give it a modest fixed-point makeup gain. */
\tl = (l * 3) / 2;
\tr = (r * 3) / 2;
\t*outL = ClipToS16(l);
\t*outR = ClipToS16(r);
''','Pseudo-stereo sounds quieter','FakeStereoProcess mix block not found')

rep('''\tint effectiveRate;
\tDecodeStats *stats;
\tTimingStats *timing;
\tRateState rateState;
\tFakeStereo fakeStereo;
''','''\tint effectiveRate;
\tDecodeStats *stats;
\tTimingStats *timing;
\tRateState rateState;
\tFakeStereo fakeStereo;
\tint lowPassReady;
\tint lowPassL;
\tint lowPassR;
''','int lowPassReady;','DecodeStream tail block not found')

helper = '''
static void DecodeStreamLowPassPair(DecodeStream *stream, short *left, short *right)
{
\tif (!stream->lowPassReady) {
\t\tstream->lowPassL = *left;
\t\tstream->lowPassR = *right;
\t\tstream->lowPassReady = 1;
\t} else {
\t\tstream->lowPassL += (((int)*left) - stream->lowPassL) >> 2;
\t\tstream->lowPassR += (((int)*right) - stream->lowPassR) >> 2;
\t}
\t*left = ClipToS16(stream->lowPassL);
\t*right = ClipToS16(stream->lowPassR);
}

'''
if 'DecodeStreamLowPassPair' not in s:
    marker = 'static int DecodeStreamCopySpill(DecodeStream *stream, signed char *dest,'
    pos = s.find(marker)
    if pos < 0:
        raise SystemExit('DecodeStreamCopySpill insertion point not found')
    s = s[:pos] + helper + s[pos:]

rep('''\t\tfor (i = 0; i < direct; i++) {
\t\t\tif (channels == 2) {
\t\t\t\tleft[produced + i] = Sample16ToS8(pcm[2 * i]);
\t\t\t\tright[produced + i] = Sample16ToS8(pcm[2 * i + 1]);
\t\t\t} else if (stream->fakeStereo.enabled) {
\t\t\t\tshort wl, wr;
\t\t\t\tFakeStereoProcess(&stream->fakeStereo, pcm[i], &wl, &wr);
\t\t\t\tleft[produced + i] = Sample16ToS8(wl);
\t\t\t\tright[produced + i] = Sample16ToS8(wr);
\t\t\t} else {
\t\t\t\tleft[produced + i] = Sample16ToS8(pcm[i]);
\t\t\t\tright[produced + i] = left[produced + i];
\t\t\t}
\t\t}
''','''\t\tfor (i = 0; i < direct; i++) {
\t\t\tshort wl, wr;
\t\t\tif (channels == 2) {
\t\t\t\twl = pcm[2 * i];
\t\t\t\twr = pcm[2 * i + 1];
\t\t\t} else if (stream->fakeStereo.enabled) {
\t\t\t\tFakeStereoProcess(&stream->fakeStereo, pcm[i], &wl, &wr);
\t\t\t} else {
\t\t\t\twl = pcm[i];
\t\t\t\twr = pcm[i];
\t\t\t}
\t\t\tif (opt->lowPass)
\t\t\t\tDecodeStreamLowPassPair(stream, &wl, &wr);
\t\t\tleft[produced + i] = Sample16ToS8(wl);
\t\t\tright[produced + i] = Sample16ToS8(wr);
\t\t}
''','if (opt->lowPass)\n\t\t\t\tDecodeStreamLowPassPair','planar direct loop not found')

rep('''\t\tfor (i = direct; i < frames; i++) {
\t\t\tint spill = i - direct;
\t\t\tif (channels == 2) {
\t\t\t\tstream->spill.planar[0][spill] = Sample16ToS8(pcm[2 * i]);
\t\t\t\tstream->spill.planar[1][spill] = Sample16ToS8(pcm[2 * i + 1]);
\t\t\t} else if (stream->fakeStereo.enabled) {
\t\t\t\tshort wl, wr;
\t\t\t\tFakeStereoProcess(&stream->fakeStereo, pcm[i], &wl, &wr);
\t\t\t\tstream->spill.planar[0][spill] = Sample16ToS8(wl);
\t\t\t\tstream->spill.planar[1][spill] = Sample16ToS8(wr);
\t\t\t} else {
\t\t\t\tstream->spill.planar[0][spill] = Sample16ToS8(pcm[i]);
\t\t\t\tstream->spill.planar[1][spill] = stream->spill.planar[0][spill];
\t\t\t}
\t\t}
''','''\t\tfor (i = direct; i < frames; i++) {
\t\t\tint spill = i - direct;
\t\t\tshort wl, wr;
\t\t\tif (channels == 2) {
\t\t\t\twl = pcm[2 * i];
\t\t\t\twr = pcm[2 * i + 1];
\t\t\t} else if (stream->fakeStereo.enabled) {
\t\t\t\tFakeStereoProcess(&stream->fakeStereo, pcm[i], &wl, &wr);
\t\t\t} else {
\t\t\t\twl = pcm[i];
\t\t\t\twr = pcm[i];
\t\t\t}
\t\t\tif (opt->lowPass)
\t\t\t\tDecodeStreamLowPassPair(stream, &wl, &wr);
\t\t\tstream->spill.planar[0][spill] = Sample16ToS8(wl);
\t\t\tstream->spill.planar[1][spill] = Sample16ToS8(wr);
\t\t}
''','stream->spill.planar[0][spill] = Sample16ToS8(wl);','planar spill loop not found')

p.write_text(s)

# Patch GUI
p = Path('amiga_mp3gui.c')
s = p.read_text()

def grep(marker):
    return marker in s

def repg(old, new, marker, error):
    global s
    if marker in s:
        return
    if old not in s:
        raise SystemExit(error)
    s = s.replace(old, new, 1)

repg('''#define HELIXAMP3_ARGC_MAX 26
''','''#define HELIXAMP3_ARGC_MAX 28
''','#define HELIXAMP3_ARGC_MAX 28','argc max not found')

repg('''#define STOP_X          (GUI_MARGIN_L + 306)
''','''#define STOP_X          (GUI_MARGIN_L + 306)
#define DOLBY_X         (GUI_MARGIN_L + 390)
#define DOLBY_W         54
''','#define DOLBY_X','transport geometry not found')

repg('''\tGID_STOP,
\tGID_STATUS,
''','''\tGID_STOP,
\tGID_LOWPASS,
\tGID_STATUS,
''','GID_LOWPASS','GID_STOP enum block not found')

repg('''\tstruct Gadget  *gadPlay;
\tstruct Gadget  *gadStop;
''','''\tstruct Gadget  *gadPlay;
\tstruct Gadget  *gadStop;
\tstruct Gadget  *gadLowPass;
''','gadLowPass','transport gadget fields not found')

repg('''\tint   fakeStereoDelayIndex;
\tint   rateIndex;
''','''\tint   fakeStereoDelayIndex;
\tint   lowPass;
\tint   rateIndex;
''','int   lowPass;','gui fake stereo fields not found')

repg('''\tSaveEnvInt("FakeStereoDelayIndex", gui->fakeStereoDelayIndex);
\tSaveEnvInt("RateIndex", gui->rateIndex);
''','''\tSaveEnvInt("FakeStereoDelayIndex", gui->fakeStereoDelayIndex);
\tSaveEnvInt("LowPass", gui->lowPass);
\tSaveEnvInt("RateIndex", gui->rateIndex);
''','SaveEnvInt("LowPass"','SaveGuiSettings fake stereo block not found')

repg('''\tgui->fakeStereoWidthIndex = LoadEnvInt("FakeStereoWidthIndex", 1, 0, 4);
\tgui->fakeStereoDelayIndex = LoadEnvInt("FakeStereoDelayIndex", 2, 0, 4);
\tgui->rateIndex = LoadEnvInt("RateIndex", 2, 0, 4);
''','''\tgui->fakeStereoWidthIndex = LoadEnvInt("FakeStereoWidthIndex", 1, 0, 4);
\tgui->fakeStereoDelayIndex = LoadEnvInt("FakeStereoDelayIndex", 2, 0, 4);
\tgui->lowPass = LoadEnvInt("LowPass", 0, 0, 1);
\tgui->rateIndex = LoadEnvInt("RateIndex", 2, 0, 4);
''','LoadEnvInt("LowPass"','GuiOpen load fake stereo block not found')

repg('''static void DrawTransportIcons(HelixAmp3Gui *gui)
{
''','''static void DrawTransportIcons(HelixAmp3Gui *gui)
{
''','static void DrawTransportIcons(HelixAmp3Gui *gui)','noop')
# Insert Dolby drawer after DrawTransportIcons
if 'static void DrawDolbyButton' not in s:
    pos = s.find('static void DrawArtPanel(HelixAmp3Gui *gui)')
    if pos < 0:
        raise SystemExit('DrawArtPanel insertion point not found')
    dolby = r'''
static void DrawDolbyButton(HelixAmp3Gui *gui)
{
	struct RastPort *rp;
	int x, y;

	if (!gui || !gui->win || !gui->gadLowPass)
		return;
	rp = gui->win->RPort;
	x = gui->gadLowPass->LeftEdge + 8;
	y = gui->gadLowPass->TopEdge + 5;
	SetAPen(rp, 1);
	Move(rp, x, y + 8);
	Draw(rp, x, y);
	Draw(rp, x + 4, y);
	Draw(rp, x + 8, y + 4);
	Draw(rp, x + 4, y + 8);
	Draw(rp, x, y + 8);
	Move(rp, x + 12, y + 8);
	Draw(rp, x + 12, y);
	Draw(rp, x + 16, y);
	Draw(rp, x + 20, y + 4);
	Draw(rp, x + 16, y + 8);
	Draw(rp, x + 12, y + 8);
	Move(rp, x + 25, y + 8);
	Draw(rp, x + 25, y);
	Draw(rp, x + 29, y);
	Move(rp, x + 31, y + 8);
	Draw(rp, x + 31, y);
	if (gui->lowPass) {
		RectFill(rp, gui->gadLowPass->LeftEdge + 2, gui->gadLowPass->TopEdge + 2,
			gui->gadLowPass->LeftEdge + 5, gui->gadLowPass->TopEdge + 5);
	}
}

'''
    s = s[:pos] + dolby + s[pos:]

repg('''\tDrawArtPanel(gui);
\tDrawTransportIcons(gui);
}
''','''\tDrawArtPanel(gui);
\tDrawTransportIcons(gui);
\tDrawDolbyButton(gui);
}
''','DrawDolbyButton(gui);\n}','GuiRefresh draw block not found')

repg('''\tDrawProgress(gui);
\tDrawArtPanel(gui);
\tDrawTransportIcons(gui);
\tif (gui->timerOpen)
''','''\tDrawProgress(gui);
\tDrawArtPanel(gui);
\tDrawTransportIcons(gui);
\tDrawDolbyButton(gui);
\tif (gui->timerOpen)
''','DrawDolbyButton(gui);\n\tif','GuiOpen draw block not found')

repg('''\tgui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
\t\tSTOP_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0);
\tif (!gad)
\t\treturn -1;

\tgui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
''','''\tgui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
\t\tSTOP_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0);
\tif (!gad)
\t\treturn -1;

\tgui->gadLowPass = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_LOWPASS,
\t\tDOLBY_X, ROW_BUTTONS, DOLBY_W, TRANSPORT_H, "",
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0);
\tif (!gad)
\t\treturn -1;

\tgui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
''','gui->gadLowPass = gad','Stop gadget block not found')

repg('''\tif (gui->decodeThenPlay)
\t\tAddArg(args, "--decode-then-play");
''','''\tif (gui->lowPass)
\t\tAddArg(args, "--low-pass");
\tif (gui->decodeThenPlay)
\t\tAddArg(args, "--decode-then-play");
''','AddArg(args, "--low-pass")','BuildPlaybackArgs decode then play block not found')

repg('''\tcase GID_STOP:
\t\tStopPlayback(gui);
\t\tbreak;
\t}
}
''','''\tcase GID_STOP:
\t\tStopPlayback(gui);
\t\tbreak;
\tcase GID_LOWPASS:
\t\tif (gui->playbackActive || gui->playbackDonePending) {
\t\t\tSetStatus(gui, "Stop playback before changing low-pass.");
\t\t\tbreak;
\t\t}
\t\tgui->lowPass = !gui->lowPass;
\t\tDrawDolbyButton(gui);
\t\tSetStatus(gui, gui->lowPass ? "Low-pass enabled." : "Low-pass disabled.");
\t\tSaveGuiSettings(gui);
\t\tbreak;
\t}
}
''','case GID_LOWPASS:','HandleGuiAction stop block not found')

repg('''\t\t\tDrawTransportIcons(gui);
''','''\t\t\tDrawTransportIcons(gui);
\t\t\tDrawDolbyButton(gui);
''','DrawDolbyButton(gui);\n\t\t} else if','Gadgetup redraw block not found')

p.write_text(s)
