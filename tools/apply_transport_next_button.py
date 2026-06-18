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

rep('''#define PLAY_X          (GUI_MARGIN_L + 154)
#define STOP_X          (GUI_MARGIN_L + 306)
#define FILTER_X         (GUI_MARGIN_L + 390)
#define FILTER_W         54
''','''#define PLAY_X          (GUI_MARGIN_L + 154)
#define NEXT_X          (GUI_MARGIN_L + 214)
#define STOP_X          (GUI_MARGIN_L + 306)
#define FILTER_X        (GUI_MARGIN_L + 390)
#define FILTER_W        54
''','#define NEXT_X','transport x constants not found')

rep('''#define ROW_BUTTONS     (GUI_TOP_Y + 13 * GUI_ROW_H + 12)
''','''#define ROW_BUTTONS     (GUI_TOP_Y + 13 * GUI_ROW_H + 4)
''','+ 13 * GUI_ROW_H + 4)','ROW_BUTTONS constant not found')

rep('''\tGID_PLAY,
\tGID_STOP,
''','''\tGID_PLAY,
\tGID_NEXT,
\tGID_STOP,
''','GID_NEXT','GID_PLAY enum block not found')

rep('''\tstruct Gadget  *gadPlay;
\tstruct Gadget  *gadStop;
''','''\tstruct Gadget  *gadPlay;
\tstruct Gadget  *gadNext;
\tstruct Gadget  *gadStop;
''','gadNext','transport gadget fields not found')

rep('''\tint   playbackStoppedByUser;
\tunsigned long playbackRunId;
''','''\tint   playbackStoppedByUser;
\tint   queuedAutoPlay;
\tunsigned long playbackRunId;
''','queuedAutoPlay','playback state fields not found')

# Insert next-file helpers before ChooseMp3.
if 'static int FindNextMp3Path' not in s:
    pos = s.find('static void ChooseMp3(HelixAmp3Gui *gui)')
    if pos < 0:
        raise SystemExit('ChooseMp3 insertion point not found')
    helper = r'''
static int GuiNameCmpNoCase(const char *a, const char *b)
{
	unsigned char ca;
	unsigned char cb;

	if (!a)
		a = "";
	if (!b)
		b = "";
	while (*a || *b) {
		ca = AmigaAsciiLower((unsigned char)*a);
		cb = AmigaAsciiLower((unsigned char)*b);
		if (ca != cb)
			return (int)ca - (int)cb;
		if (*a)
			a++;
		if (*b)
			b++;
	}
	return 0;
}

static int GuiNameHasMp3Suffix(const char *name)
{
	int len;

	if (!name)
		return 0;
	len = strlen(name);
	return len > 4 && name[len - 4] == '.' &&
		AmigaAsciiLower((unsigned char)name[len - 3]) == 'm' &&
		AmigaAsciiLower((unsigned char)name[len - 2]) == 'p' &&
		AmigaAsciiLower((unsigned char)name[len - 1]) == '3';
}

static void CopyFileFromPath(char *dst, size_t dstSize, const char *path)
{
	const char *p;
	const char *base;

	if (!dst || dstSize == 0)
		return;
	dst[0] = '\0';
	if (!path || !path[0])
		return;
	base = path;
	for (p = path; *p; p++) {
		if (*p == '/' || *p == ':')
			base = p + 1;
	}
	SafeCopy(dst, dstSize, base);
}

static int FindNextMp3Path(HelixAmp3Gui *gui, char *outPath, size_t outSize)
{
	char drawer[HELIXAMP3_MAX_PATH];
	char current[HELIXAMP3_MAX_PATH];
	char first[HELIXAMP3_MAX_PATH];
	char best[HELIXAMP3_MAX_PATH];
	BPTR lock;
	struct FileInfoBlock *fib;

	if (!gui || !gui->inputName[0] || !outPath || outSize == 0)
		return 0;
	outPath[0] = '\0';
	CopyDrawerFromPath(drawer, sizeof(drawer), gui->inputName);
	CopyFileFromPath(current, sizeof(current), gui->inputName);
	if (!drawer[0] || !current[0])
		return 0;
	first[0] = '\0';
	best[0] = '\0';
	lock = Lock((STRPTR)drawer, ACCESS_READ);
	if (!lock)
		return 0;
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(lock, fib)) {
		while (ExNext(lock, fib)) {
			const char *name = fib->fib_FileName;
			if (fib->fib_DirEntryType >= 0 || !GuiNameHasMp3Suffix(name))
				continue;
			if (!first[0] || GuiNameCmpNoCase(name, first) < 0)
				SafeCopy(first, sizeof(first), name);
			if (GuiNameCmpNoCase(name, current) > 0 &&
				(!best[0] || GuiNameCmpNoCase(name, best) < 0))
				SafeCopy(best, sizeof(best), name);
		}
	}
	if (fib)
		FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	if (!best[0]) {
		if (!first[0] || GuiNameCmpNoCase(first, current) == 0)
			return 0;
		SafeCopy(best, sizeof(best), first);
	}
	SafeCopy(outPath, outSize, drawer);
	AddPart(outPath, best, outSize);
	return 1;
}

static void LoadSelectedMp3(HelixAmp3Gui *gui, const char *path)
{
	CancelArtDecode(gui);
	SafeCopy(gui->inputName, sizeof(gui->inputName), path);
	SetFileDisplay(gui, gui->inputName);
	ReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
	gui->totalSecs = gui->tags.durationSecs;
	gui->elapsedSecs = 0;
	gui->launchBufferSecs = 0;
	UpdateTagDisplay(gui);
	UpdateArtDisplay(gui);
	DrawProgress(gui);
	if (gui->artDecode.active)
		SendTimerRequest(gui, ART_TIMER_MICROS);
	GuiDisableFastMemIfTooSmall(gui);
}

static void PlayNextMp3(HelixAmp3Gui *gui)
{
	char nextPath[HELIXAMP3_MAX_PATH];

	if (!FindNextMp3Path(gui, nextPath, sizeof(nextPath))) {
		SetStatus(gui, "No next MP3 found in this drawer.");
		return;
	}
	if (gui->playbackActive || gui->playbackDonePending) {
		SafeCopy(gui->queuedInputName, sizeof(gui->queuedInputName), nextPath);
		gui->queuedAutoPlay = 1;
		SetStatus(gui, "Skipping to next track...");
		StopPlayback(gui);
		return;
	}
	LoadSelectedMp3(gui, nextPath);
	SetStatus(gui, "Next track selected.");
	StartPlayback(gui);
}

'''
    s = s[:pos] + helper + s[pos:]

# Use LoadSelectedMp3 in ChooseMp3 normal path and clear queuedAutoPlay for browse queue.
old = '''\t\tif (gui->playbackActive || gui->playbackDonePending) {
\t\t\tSafeCopy(gui->queuedInputName, sizeof(gui->queuedInputName), path);
\t\t\tSetStatus(gui, "Selected for next Play.");
\t\t} else {
\t\t\tCancelArtDecode(gui);
\t\t\tSafeCopy(gui->inputName, sizeof(gui->inputName), path);
\t\t\tSetFileDisplay(gui, gui->inputName);
\t\t\tReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
\t\t\tgui->totalSecs = gui->tags.durationSecs;
\t\t\tgui->elapsedSecs = 0;
\t\t\tUpdateTagDisplay(gui);
\t\t\tUpdateArtDisplay(gui);
\t\t\tDrawProgress(gui);
\t\t\tif (gui->artDecode.active)
\t\t\t\tSendTimerRequest(gui, ART_TIMER_MICROS);
\t\t\tif (!gui->artDecode.active) {
\t\t\t\tFormatReadyStatus(&gui->tags, gui->statusText, sizeof(gui->statusText));
\t\t\t\tSetStatus(gui, gui->statusText);
\t\t\t}
\t\t\tGuiDisableFastMemIfTooSmall(gui);
\t\t}
'''
new = '''\t\tif (gui->playbackActive || gui->playbackDonePending) {
\t\t\tSafeCopy(gui->queuedInputName, sizeof(gui->queuedInputName), path);
\t\t\tgui->queuedAutoPlay = 0;
\t\t\tSetStatus(gui, "Selected for next Play.");
\t\t} else {
\t\t\tLoadSelectedMp3(gui, path);
\t\t\tif (!gui->artDecode.active) {
\t\t\t\tFormatReadyStatus(&gui->tags, gui->statusText, sizeof(gui->statusText));
\t\t\t\tSetStatus(gui, gui->statusText);
\t\t\t}
\t\t}
'''
rep(old, new, 'gui->queuedAutoPlay = 0;', 'ChooseMp3 selection block not found')

# Add next button gadget between play and stop.
old = '''\tgui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
\t\tSTOP_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
'''
new = '''\tgui->gadNext = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_NEXT,
\t\tNEXT_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0,
\t\tTAG_IGNORE, 0);
\tif (!gad)
\t\treturn -1;

\tgui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
\t\tSTOP_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
'''
rep(old, new, 'gui->gadNext = gad', 'stop gadget block not found')

# Enhance icon drawer for next button. Need replace current function bits.
old = '''\tint stopX;
\tint stopY;
\tint i;

\tif (!gui || !gui->win || !gui->gadPlay || !gui->gadStop)
\t\treturn;
'''
new = '''\tint nextX;
\tint nextY;
\tint stopX;
\tint stopY;
\tint i;

\tif (!gui || !gui->win || !gui->gadPlay || !gui->gadNext || !gui->gadStop)
\t\treturn;
'''
rep(old, new, 'int nextX;', 'DrawTransportIcons declarations not found')

old = '''\tstopX = gui->gadStop->LeftEdge + (gui->gadStop->Width / 2) - 5;
\tstopY = gui->gadStop->TopEdge + (gui->gadStop->Height / 2) - 5;
\tRectFill(rp, stopX, stopY, stopX + 9, stopY + 9);
'''
new = '''\tnextX = gui->gadNext->LeftEdge + (gui->gadNext->Width / 2) - 8;
\tnextY = gui->gadNext->TopEdge + (gui->gadNext->Height / 2) - 5;
\tfor (i = 0; i < 8; i++) {
\t\tint half = (7 - i) / 2;
\t\tRectFill(rp, nextX + i, nextY + 5 - half, nextX + i,
\t\t\tnextY + 5 + half);
\t\tRectFill(rp, nextX + 7 + i, nextY + 5 - half,
\t\t\tnextX + 7 + i, nextY + 5 + half);
\t}
\tRectFill(rp, nextX + 17, nextY, nextX + 18, nextY + 10);
\tstopX = gui->gadStop->LeftEdge + (gui->gadStop->Width / 2) - 5;
\tstopY = gui->gadStop->TopEdge + (gui->gadStop->Height / 2) - 5;
\tRectFill(rp, stopX, stopY, stopX + 9, stopY + 9);
'''
rep(old, new, 'nextX = gui->gadNext->LeftEdge', 'DrawTransportIcons stop block not found')

# Finalize queued behavior: load selected, optionally autoplay when it came from Next.
old = '''\t} else if (gui->queuedInputName[0]) {
\t\tchar queued[HELIXAMP3_MAX_PATH];
\t\tSafeCopy(queued, sizeof(queued), gui->queuedInputName);
\t\tgui->queuedInputName[0] = '\0';
\t\tCancelArtDecode(gui);
\t\tSafeCopy(gui->inputName, sizeof(gui->inputName), queued);
\t\tSetFileDisplay(gui, gui->inputName);
\t\tReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
\t\tgui->totalSecs = gui->tags.durationSecs;
\t\tgui->elapsedSecs = 0;
\t\tgui->launchBufferSecs = 0;
\t\tUpdateTagDisplay(gui);
\t\tUpdateArtDisplay(gui);
\t\tDrawProgress(gui);
\t\tif (gui->artDecode.active)
\t\t\tSendTimerRequest(gui, ART_TIMER_MICROS);
\t\telse
\t\t\tSetStatus(gui, "Next file ready.");
\t}
'''
new = '''\t} else if (gui->queuedInputName[0]) {
\t\tchar queued[HELIXAMP3_MAX_PATH];
\t\tint autoPlay = gui->queuedAutoPlay;
\t\tSafeCopy(queued, sizeof(queued), gui->queuedInputName);
\t\tgui->queuedInputName[0] = '\0';
\t\tgui->queuedAutoPlay = 0;
\t\tLoadSelectedMp3(gui, queued);
\t\tif (autoPlay) {
\t\t\tSetStatus(gui, "Starting next track...");
\t\t\tStartPlayback(gui);
\t\t} else if (gui->artDecode.active) {
\t\t\tSendTimerRequest(gui, ART_TIMER_MICROS);
\t\t} else {
\t\t\tSetStatus(gui, "Next file ready.");
\t\t}
\t}
'''
rep(old, new, 'int autoPlay = gui->queuedAutoPlay;', 'FinalizePlayback queued block not found')

# Add action.
old = '''\tcase GID_PLAY:
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
\tcase GID_STOP:
'''
new = '''\tcase GID_PLAY:
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
\tcase GID_NEXT:
\t\tPlayNextMp3(gui);
\t\tbreak;
\tcase GID_STOP:
'''
rep(old, new, 'case GID_NEXT:', 'GID_PLAY action block not found')

# Clear autoplay on close.
s = s.replace("gui->queuedInputName[0] = '\\0';\n\t} else if", "gui->queuedInputName[0] = '\\0';\n\t\tgui->queuedAutoPlay = 0;\n\t} else if")

p.write_text(s)
