/*
 * minimp3r - ReAction/ClassAct mini-player frontend for the Helix fixed-point
 * MP3 decoder, aimed at AmigaOS 3.3/3.5/3.9 (and 68k boards with ReAction or
 * the older ClassAct distribution installed).
 *
 * Like the GadTools frontend (amiga_mp3gui.c) this wraps the existing
 * amiga_mp3dec playback engine: the decoder source is compiled straight into
 * this translation unit with main() renamed to HelixAmp3CliMain(), and a small
 * child process feeds it the same --play/--rate/--buffer-seconds argument set
 * the Shell command would use.  All of the decode/Paula-streaming code is the
 * proven path; only the user interface differs.
 *
 * Build it from the Makefile with:  make -f Makefile.amiga guir
 *
 * The window is assembled entirely from BOOPSI gadget classes (window.class,
 * layout.gadget, getfile.gadget, chooser.gadget, slider.gadget,
 * checkbox.gadget, fuelgauge.gadget, string.gadget and label.image) so it gets
 * a native ReAction look and resizes cleanly on a 3.x/3.9 Workbench.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AMIGA_M68K)

/* Pull the whole decoder/playback engine into this object, with its command
 * line entry point renamed.  Mirrors the trick used by amiga_mp3gui.c so we
 * share gGuiPlaybackStatus, gMiniAmp3EmbeddedPlayback and gPlaybackInterrupted
 * with the playback child without any extra glue. */
#define main HelixAmp3CliMain
#include "amiga_mp3dec.c"
#undef main
#undef printf
#undef fprintf
#undef fputs
#undef puts
#undef putchar
#undef fflush
#undef fwrite

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <libraries/gadtools.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/getfile.h>
#include <gadgets/chooser.h>
#include <gadgets/slider.h>
#include <gadgets/checkbox.h>
#include <gadgets/fuelgauge.h>
#include <gadgets/string.h>
#include <images/label.h>

#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/asl.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/getfile.h>
#include <proto/chooser.h>
#include <proto/slider.h>
#include <proto/checkbox.h>
#include <proto/fuelgauge.h>
#include <proto/string.h>
#include <proto/label.h>
#include <proto/gadtools.h>

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

/* OpenLibrary version for the ReAction classes.  V44 is the OS 3.5/3.9
 * ReAction baseline; users running the older ClassAct 2.x distribution on
 * OS 3.1/3.3 can drop this to the version their classes report (typically
 * 41-43) and rebuild. */
#ifndef MINIMP3R_CLASS_VERSION
#define MINIMP3R_CLASS_VERSION 44
#endif

#define MR_MAX_PATH      256
#define MR_ARGC_MAX      40
#define MR_PLAYLIST_MAX  128
#define MR_QUALITY_MIN   0
#define MR_QUALITY_MAX   3

/* How often we poll the shared playback status block while a track plays. */
#define MR_TICK_MICROS   500000UL

/* Mirror the phase/startup constants the decoder publishes.  They are defined
 * inside amiga_mp3dec.c only for non-AMIGA builds, so re-declare the few we use
 * here for the m68k path. */
#ifndef GUIPLAY_PHASE_IDLE
#define GUIPLAY_PHASE_IDLE      0
#define GUIPLAY_PHASE_BUFFERING 1
#define GUIPLAY_PHASE_PLAYING   2
#define GUIPLAY_PHASE_UNDERRUN  3
#define GUIPLAY_PHASE_DONE      4
#define GUIPLAY_PHASE_STOPPING  5
#define GUIPLAY_PHASE_ERROR     6
#endif

/* ------------------------------------------------------------------------- */
/* Gadget IDs                                                                */
/* ------------------------------------------------------------------------- */

enum {
	GID_FILE = 1,
	GID_RATE,
	GID_QUALITY,
	GID_CHANNEL,
	GID_VOLUME,
	GID_BUFFER,
	GID_FASTMEM,
	GID_FASTLOW,
	GID_SPEED,
	GID_WIDTH,
	GID_DELAY,
	GID_PLAY,
	GID_NEXT,
	GID_STOP,
	GID_FILTER,
	GID_PLAYLIST,
	GID_TIME,
	GID_FILEINFO,
	GID_TITLE,
	GID_ARTIST,
	GID_ALBUM,
	GID_TRACK,
	GID_GENRE,
	GID_RATING,
	GID_LAST
};

/* ------------------------------------------------------------------------- */
/* Option tables (shared with the CLI flag set the decoder understands)      */
/* ------------------------------------------------------------------------- */

static const char * const kRates[] = {
	"8287", "8820", "11025", "22050", "28600"
};
#define MR_RATE_COUNT  ((int)(sizeof(kRates) / sizeof(kRates[0])))

static const STRPTR kRateLabels[] = {
	(STRPTR)"8287 Hz",
	(STRPTR)"8820 Hz",
	(STRPTR)"11025 Hz",
	(STRPTR)"22050 Hz",
	(STRPTR)"28600 Hz",
	NULL
};

static const STRPTR kQualityLabels[] = {
	(STRPTR)"Faster",
	(STRPTR)"Fast",
	(STRPTR)"Normal",
	(STRPTR)"Best",
	NULL
};

static const STRPTR kChannelLabels[] = {
	(STRPTR)"Stereo",
	(STRPTR)"Mono",
	NULL
};
static const STRPTR kSpeedLabels[] = {
	(STRPTR)"Normal",
	(STRPTR)"Fast low-rate",
	(STRPTR)"Superfast low-rate",
	NULL
};

static const STRPTR kWidthLabels[] = {
	(STRPTR)"Normal stereo",
	(STRPTR)"Fake stereo 1",
	(STRPTR)"Fake stereo 2",
	(STRPTR)"Fake stereo 3",
	(STRPTR)"Fake stereo 4",
	(STRPTR)"Fake stereo 5",
	NULL
};

static const int kFakeStereoShifts[] = { 1, 2, 3, 4, 5 };

static const STRPTR kDelayLabels[] = {
	(STRPTR)"48", (STRPTR)"64", (STRPTR)"96", (STRPTR)"128", (STRPTR)"192", NULL
};

static const int kFakeStereoDelays[] = { 48, 64, 96, 128, 192 };

#define MENUNUM_PROJECT   0
#define MENUNUM_PLAYBACK  1
#define ITEMNUM_ABOUT     0
#define ITEMNUM_QUIT      1
#define ITEMNUM_DTP       0
#define ITEMNUM_BENCH     1
#define ITEMNUM_ARTWORK   2
#define ITEMNUM_ARTCACHE  3
#define ITEMNUM_ARTCOLOR  4
#define ITEMNUM_ARTREFRESH 5
#define ITEMNUM_ARTRELOAD  6
#define ITEMNUM_ARTCLEAN   7
#define ITEMNUM_PROGRESS   8

static struct NewMenu kMenus[] = {
	{ NM_TITLE, (STRPTR)"Project", 0, 0, 0, 0 },
	{ NM_ITEM, (STRPTR)"About MiniAMP3...", 0, 0, 0, (APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_ABOUT) },
	{ NM_ITEM, (STRPTR)"Quit", 0, 0, 0, (APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_QUIT) },
	{ NM_TITLE, (STRPTR)"Playback", 0, 0, 0, 0 },
	{ NM_ITEM, (STRPTR)"Decode-then-play", 0, CHECKIT | MENUTOGGLE, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_DTP) },
	{ NM_ITEM, (STRPTR)"Bench mode", 0, CHECKIT | MENUTOGGLE, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_BENCH) },
	{ NM_ITEM, (STRPTR)"Artwork", 0, CHECKIT | MENUTOGGLE, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTWORK) },
	{ NM_ITEM, (STRPTR)"Artwork Cache", 0, CHECKIT | MENUTOGGLE, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTCACHE) },
	{ NM_ITEM, (STRPTR)"Colour Artwork", 0, CHECKIT | MENUTOGGLE, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTCOLOR) },
	{ NM_ITEM, (STRPTR)"Refresh Artwork", 0, 0, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTREFRESH) },
	{ NM_ITEM, (STRPTR)"Reload Art from File", 0, 0, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTRELOAD) },
	{ NM_ITEM, (STRPTR)"Clear Artwork Cache", 0, 0, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTCLEAN) },
	{ NM_ITEM, (STRPTR)"Progress Bar", 0, CHECKIT | MENUTOGGLE, 0, (APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_PROGRESS) },
	{ NM_END, NULL, 0, 0, 0, 0 }
};


/* ------------------------------------------------------------------------- */
/* Library / class bases                                                     */
/* ------------------------------------------------------------------------- */

struct IntuitionBase *IntuitionBase;
struct Library *UtilityBase;
struct Library *AslBase;
struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *GetFileBase;
struct Library *ChooserBase;
struct Library *SliderBase;
struct Library *CheckBoxBase;
struct Library *FuelGaugeBase;
struct Library *StringBase;
struct Library *LabelBase;
struct Library *GadToolsBase;

/* ------------------------------------------------------------------------- */
/* Playback child plumbing (a trimmed-down copy of the amiga_mp3gui logic)   */
/* ------------------------------------------------------------------------- */

typedef struct MrPlayArgs {
	int   argc;
	char *argv[MR_ARGC_MAX];
	char  storage[MR_ARGC_MAX][MR_MAX_PATH];
} MrPlayArgs;

typedef struct MrPlayer {
	volatile int    stopRequested;
	int             argc;
	char          **argv;
	struct Process *process;
} MrPlayer;

static MrPlayer        gPlayer;
static MrPlayArgs      gArgs;
static struct Message  gDoneMsg;
static struct MsgPort *gDonePort;
static volatile unsigned long gRunCounter;
static volatile unsigned long gEntryRunId;
static volatile unsigned long gDoneRunId;

/* ------------------------------------------------------------------------- */
/* Application state                                                         */
/* ------------------------------------------------------------------------- */

typedef struct MrApp {
	Object         *winObj;
	struct Window  *win;
	struct Menu    *menuStrip;

	Object         *fileGad;
	Object         *rateGad;
	Object         *qualityGad;
	Object         *channelGad;
	Object         *volumeGad;
	Object         *bufferGad;
	Object         *fastMemGad;
	Object         *fastLowGad;
	Object         *speedGad;
	Object         *widthGad;
	Object         *delayGad;
	Object         *playGad;
	Object         *nextGad;
	Object         *stopGad;
	Object         *filterGad;
	Object         *playlistGad;
	Object         *timeGad;
	Object         *fileInfoGad;
	Object         *titleGad;
	Object         *artistGad;
	Object         *albumGad;
	Object         *trackGad;
	Object         *genreGad;
	Object         *ratingGad;
	Object         *gaugeGad;
	Object         *statusGad;

	struct MsgPort   *timerPort;
	struct timerequest *timerReq;
	int               timerRunning;
	struct MsgPort   *donePort;

	char  inputName[MR_MAX_PATH];
	char  playlist[MR_PLAYLIST_MAX][MR_MAX_PATH];
	int   rateIndex;
	int   qualityIndex;
	int   mono;
	int   fastMem;
	int   fastLowrate;
	int   superfastLowrate;
	int   fakeStereo;
	int   fakeStereoWidthIndex;
	int   fakeStereoDelayIndex;
	int   hardwareFilter;
	int   decodeThenPlay;
	int   bench;
	int   artEnabled;
	int   artCacheEnabled;
	int   artColorEnabled;
	int   progressEnabled;
	int   playlistCount;
	int   playlistCurrent;
	int   playlistNextPending;
	int   volumePercent;
	int   bufferSeconds;

	unsigned long playbackRunId;
	int   playbackActive;
	int   playbackDonePending;
	int   stoppedByUser;
	int   lastPhaseShown;
} MrApp;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void SafeCopy(char *dst, size_t size, const char *src)
{
	if (!size)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, size - 1);
	dst[size - 1] = '\0';
}

static void SetStatus(MrApp *app, const char *text)
{
	if (app->statusGad && app->win)
		SetGadgetAttrs((struct Gadget *)app->statusGad, app->win, NULL,
			STRINGA_TextVal, (ULONG)text,
			TAG_DONE);
}

static void SetGauge(MrApp *app, int level)
{
	if (level < 0)
		level = 0;
	if (level > 100)
		level = 100;
	if (app->gaugeGad && app->win)
		SetGadgetAttrs((struct Gadget *)app->gaugeGad, app->win, NULL,
			FUELGAUGE_Level, (ULONG)level,
			TAG_DONE);
}

static void EnablePlayStop(MrApp *app, int playing)
{
	if (app->win) {
		if (app->playGad)
			SetGadgetAttrs((struct Gadget *)app->playGad, app->win, NULL,
				GA_Disabled, (ULONG)(playing ? TRUE : FALSE), TAG_DONE);
		if (app->stopGad)
			SetGadgetAttrs((struct Gadget *)app->stopGad, app->win, NULL,
				GA_Disabled, (ULONG)(playing ? FALSE : TRUE), TAG_DONE);
	}
}

/* ------------------------------------------------------------------------- */
/* Build the playback argument vector (same flags as the Shell command)      */
/* ------------------------------------------------------------------------- */

static void AddArg(MrPlayArgs *args, const char *text)
{
	if (args->argc >= MR_ARGC_MAX)
		return;
	SafeCopy(args->storage[args->argc], MR_MAX_PATH, text);
	args->argv[args->argc] = args->storage[args->argc];
	args->argc++;
}

static void BuildPlaybackArgs(MrApp *app, MrPlayArgs *args)
{
	char num[16];

	memset(args, 0, sizeof(*args));
	AddArg(args, "minimp3r");
	AddArg(args, "--play");
	if (app->fastMem)
		AddArg(args, "--fast-mem");
	if (app->superfastLowrate) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
	} else if (app->fastLowrate && strcmp(kRates[app->rateIndex], "28600")) {
		AddArg(args, "--fast-lowrate");
	}
	if (app->fakeStereo) {
		AddArg(args, "--fake-stereo");
		AddArg(args, "--fake-stereo-delay");
		sprintf(num, "%d", kFakeStereoDelays[app->fakeStereoDelayIndex]);
		AddArg(args, num);
		AddArg(args, "--fake-stereo-shift");
		sprintf(num, "%d", kFakeStereoShifts[app->fakeStereoWidthIndex]);
		AddArg(args, num);
	} else if (app->mono)
		AddArg(args, "--mono");
	else
		AddArg(args, "--stereo");
	AddArg(args, "--rate");
	AddArg(args, kRates[app->rateIndex]);
	AddArg(args, "--buffer-seconds");
	sprintf(num, "%d", app->bufferSeconds);
	AddArg(args, num);
	AddArg(args, "--volume");
	sprintf(num, "%d", app->volumePercent);
	AddArg(args, num);
	AddArg(args, "--quality");
	sprintf(num, "%d", app->qualityIndex);
	AddArg(args, num);
	if (app->decodeThenPlay)
		AddArg(args, "--decode-then-play");
	if (app->bench)
		AddArg(args, "--bench");
	AddArg(args, app->inputName);
	args->argv[args->argc] = NULL;
}

/* ------------------------------------------------------------------------- */
/* CLI parser reset (the C runtime getopt state is process-global)           */
/* ------------------------------------------------------------------------- */

extern int optind;
extern int opterr;
extern int optopt;
extern char *optarg;

static void ResetCliParser(void)
{
	optind = 1;
	opterr = 0;
	optopt = 0;
	optarg = NULL;
}

/* ------------------------------------------------------------------------- */
/* The playback child process                                                */
/* ------------------------------------------------------------------------- */

static int PlaybackProcessStillExists(void)
{
	struct Task *task;

	Forbid();
	task = FindTask((STRPTR)"minimp3r playback");
	Permit();
	return task != NULL;
}

static void PlaybackEntry(void)
{
	struct MsgPort *donePort;
	ULONG pending;
	int earlyStop;
	int ranDecoder = 0;

	pending = SetSignal(0, 0);
	earlyStop = gPlayer.stopRequested || gPlaybackInterrupted ||
		(pending & SIGBREAKF_CTRL_C);
	if (earlyStop)
		gPlaybackInterrupted = 1;

	ResetCliParser();
	if (!earlyStop) {
		MP3ResetStatics();
		ResetCliParser();
	}

	gGuiPlaybackStatus.runId = gEntryRunId;

	if (!earlyStop && !gPlayer.stopRequested && !gPlaybackInterrupted) {
		ranDecoder = 1;
		gMiniAmp3EmbeddedPlayback = 1;
		HelixAmp3CliMain(gPlayer.argc, gPlayer.argv);
		gMiniAmp3EmbeddedPlayback = 0;
	}

	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	gGuiPlaybackStatus.cleanupComplete = 1;
	(void)ranDecoder;

	gDoneRunId = gGuiPlaybackStatus.runId;
	donePort = gDonePort;
	if (donePort) {
		gDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
		PutMsg(donePort, &gDoneMsg);
	}
}

static void StartPlayback(MrApp *app)
{
	struct Process *thisProc;
	BPTR dirLock;
	BPTR nilOut;
	struct Message *stale;

	if (!app->inputName[0]) {
		SetStatus(app, "Pick an audio file first.");
		return;
	}
	if (app->playbackActive || app->playbackDonePending) {
		SetStatus(app, "Already playing - press Stop first.");
		return;
	}
	if (PlaybackProcessStillExists()) {
		SetStatus(app, "Previous playback is still exiting.");
		return;
	}
	if (!app->donePort) {
		SetStatus(app, "No reply port; cannot start playback.");
		return;
	}

	/* Drain any stale done message and re-arm the static message node. */
	while ((stale = GetMsg(app->donePort)) != NULL)
		;
	memset(&gDoneMsg, 0, sizeof(gDoneMsg));
	gDoneMsg.mn_Length = sizeof(gDoneMsg);
	gDoneMsg.mn_Node.ln_Type = NT_MESSAGE;

	memset((void *)&gGuiPlaybackStatus, 0, sizeof(gGuiPlaybackStatus));
	app->playbackRunId = ++gRunCounter;
	gGuiPlaybackStatus.runId = app->playbackRunId;
	gEntryRunId = app->playbackRunId;

	BuildPlaybackArgs(app, &gArgs);
	gPlayer.argc = gArgs.argc;
	gPlayer.argv = gArgs.argv;
	gPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gDonePort = app->donePort;
	gDoneRunId = 0;

	thisProc = (struct Process *)FindTask(NULL);
	dirLock = DupLock(thisProc ? thisProc->pr_CurrentDir : (BPTR)0);
	nilOut = Open((STRPTR)"NIL:", MODE_NEWFILE);
	if (!nilOut) {
		if (dirLock)
			UnLock(dirLock);
		SetStatus(app, "Could not open NIL: for the player.");
		return;
	}

	gPlayer.process = CreateNewProcTags(
		NP_Entry,      (ULONG)PlaybackEntry,
		NP_Name,       (ULONG)"minimp3r playback",
		NP_Priority,   0,
		NP_StackSize,  262144,
		NP_CurrentDir, dirLock,
		NP_Output,     nilOut,
		NP_CloseOutput, TRUE,
		NP_CopyVars,   FALSE,
		TAG_DONE);

	if (!gPlayer.process) {
		Close(nilOut);
		if (dirLock)
			UnLock(dirLock);
		gDonePort = NULL;
		SetStatus(app, "Could not launch the playback process.");
		return;
	}

	app->playbackActive = 1;
	app->playbackDonePending = 0;
	app->stoppedByUser = 0;
	app->lastPhaseShown = -1;
	EnablePlayStop(app, 1);
	SetStatus(app, "Starting playback...");
	SetGauge(app, 0);
}

static void StopPlayback(MrApp *app)
{
	struct Task *child;

	if (!app->playbackActive) {
		SetStatus(app, "Nothing is playing.");
		return;
	}
	if (gPlayer.stopRequested) {
		SetStatus(app, "Stopping...");
		return;
	}
	app->stoppedByUser = 1;
	gPlayer.stopRequested = 1;
	gPlaybackInterrupted = 1;

	/* Wake the child immediately so it does not sit in WaitIO for the rest of
	 * a multi-second audio buffer.  Forbid()/FindTask() guards against the
	 * child already being torn down by DOS. */
	Forbid();
	child = FindTask((STRPTR)"minimp3r playback");
	if (child)
		Signal(child, SIGBREAKF_CTRL_C);
	Permit();

	SetStatus(app, "Stopping...");
}

static void FinalizePlayback(MrApp *app)
{
	int stoppedByUser = app->stoppedByUser;

	app->playbackActive = 0;
	app->playbackDonePending = 0;
	app->stoppedByUser = 0;
	gPlayer.process = NULL;
	gPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gDonePort = NULL;
	ResetCliParser();

	EnablePlayStop(app, 0);
	SetGauge(app, stoppedByUser ? 0 : 100);
	SetStatus(app, stoppedByUser ? "Stopped." : "Finished.");
	if (app->playlistNextPending) {
		app->playlistNextPending = 0;
		StartPlayback(app);
	}
}

static void HandleDoneSignal(MrApp *app)
{
	struct Message *msg;
	int gotDone = 0;

	if (!app->donePort)
		return;
	while ((msg = GetMsg(app->donePort)) != NULL)
		gotDone = 1;
	if (!gotDone)
		return;

	app->playbackDonePending = 1;
	/* The child posts its done message just before returning from
	 * PlaybackEntry(); wait for DOS to actually reap the task before we let a
	 * new decoder start. */
	if (!PlaybackProcessStillExists())
		FinalizePlayback(app);
}

/* ------------------------------------------------------------------------- */
/* Status polling (driven by the timer tick)                                 */
/* ------------------------------------------------------------------------- */

static void PollPlaybackStatus(MrApp *app)
{
	int phase;
	unsigned long frames;
	int rate;
	long spareMs;
	unsigned long halfMs;
	char buf[96];

	/* A late done where the child had already vanished before we drained the
	 * port: finalize now. */
	if (app->playbackDonePending && !PlaybackProcessStillExists()) {
		FinalizePlayback(app);
		return;
	}
	if (!app->playbackActive)
		return;

	phase   = gGuiPlaybackStatus.phase;
	frames  = gGuiPlaybackStatus.decodedFrames;
	rate    = gGuiPlaybackStatus.sampleRate;
	spareMs = gGuiPlaybackStatus.spareMs;
	halfMs  = gGuiPlaybackStatus.halfBufferMs;

	if (halfMs > 0) {
		long level = (spareMs * 100L) / (long)(halfMs * 2UL);
		SetGauge(app, (int)level);
	}

	if (phase == app->lastPhaseShown)
		return;
	app->lastPhaseShown = phase;

	switch (phase) {
	case GUIPLAY_PHASE_BUFFERING:
		SetStatus(app, "Buffering...");
		break;
	case GUIPLAY_PHASE_PLAYING:
		sprintf(buf, "Playing - %lu frames @ %d Hz", frames, rate);
		SetStatus(app, buf);
		break;
	case GUIPLAY_PHASE_UNDERRUN:
		SetStatus(app, "Playing (buffer low)...");
		break;
	case GUIPLAY_PHASE_STOPPING:
		SetStatus(app, "Stopping...");
		break;
	case GUIPLAY_PHASE_ERROR:
		SetStatus(app, "Playback error.");
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Timer device                                                              */
/* ------------------------------------------------------------------------- */

static void ArmTimer(MrApp *app, ULONG micros)
{
	if (!app->timerReq)
		return;
	if (app->timerRunning) {
		AbortIO((struct IORequest *)app->timerReq);
		WaitIO((struct IORequest *)app->timerReq);
	}
	app->timerReq->tr_node.io_Command = TR_ADDREQUEST;
	app->timerReq->tr_time.tv_secs  = micros / 1000000UL;
	app->timerReq->tr_time.tv_micro = micros % 1000000UL;
	SendIO((struct IORequest *)app->timerReq);
	app->timerRunning = 1;
}

static int OpenTimer(MrApp *app)
{
	app->timerPort = CreateMsgPort();
	if (!app->timerPort)
		return 0;
	app->timerReq = (struct timerequest *)CreateIORequest(app->timerPort,
		sizeof(struct timerequest));
	if (!app->timerReq)
		return 0;
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
			(struct IORequest *)app->timerReq, 0) != 0)
		return 0;
	ArmTimer(app, MR_TICK_MICROS);
	return 1;
}

static void CloseTimer(MrApp *app)
{
	if (app->timerReq) {
		if (app->timerRunning) {
			AbortIO((struct IORequest *)app->timerReq);
			WaitIO((struct IORequest *)app->timerReq);
			app->timerRunning = 0;
		}
		if (app->timerReq->tr_node.io_Device)
			CloseDevice((struct IORequest *)app->timerReq);
		DeleteIORequest((struct IORequest *)app->timerReq);
		app->timerReq = NULL;
	}
	if (app->timerPort) {
		DeleteMsgPort(app->timerPort);
		app->timerPort = NULL;
	}
}

/* ------------------------------------------------------------------------- */
/* Library / class open / close                                              */
/* ------------------------------------------------------------------------- */

static void CloseLibs(void)
{
	if (GadToolsBase)  { CloseLibrary(GadToolsBase);  GadToolsBase = NULL; }
	if (LabelBase)     { CloseLibrary(LabelBase);     LabelBase = NULL; }
	if (StringBase)    { CloseLibrary(StringBase);    StringBase = NULL; }
	if (FuelGaugeBase) { CloseLibrary(FuelGaugeBase); FuelGaugeBase = NULL; }
	if (CheckBoxBase)  { CloseLibrary(CheckBoxBase);  CheckBoxBase = NULL; }
	if (SliderBase)    { CloseLibrary(SliderBase);    SliderBase = NULL; }
	if (ChooserBase)   { CloseLibrary(ChooserBase);   ChooserBase = NULL; }
	if (GetFileBase)   { CloseLibrary(GetFileBase);   GetFileBase = NULL; }
	if (ButtonBase)    { CloseLibrary(ButtonBase);    ButtonBase = NULL; }
	if (LayoutBase)    { CloseLibrary(LayoutBase);    LayoutBase = NULL; }
	if (WindowBase)    { CloseLibrary(WindowBase);    WindowBase = NULL; }
	if (UtilityBase)   { CloseLibrary(UtilityBase);   UtilityBase = NULL; }
	if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

static int OpenLibs(void)
{
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
	if (!IntuitionBase) {
		fprintf(stderr, "minimp3r needs intuition.library V39+.\n");
		return 0;
	}
	GadToolsBase = OpenLibrary("gadtools.library", 39);
	if (!GadToolsBase) {
		fprintf(stderr, "minimp3r needs gadtools.library V39+.\n");
		return 0;
	}
	UtilityBase = OpenLibrary("utility.library", 39);
	if (!UtilityBase) {
		fprintf(stderr, "minimp3r needs utility.library V39+.\n");
		return 0;
	}
	AslBase = OpenLibrary("asl.library", 39);
	if (!AslBase) {
		fprintf(stderr, "minimp3r needs asl.library V39+.\n");
		return 0;
	}

	WindowBase    = OpenLibrary("window.class",            MINIMP3R_CLASS_VERSION);
	LayoutBase    = OpenLibrary("gadgets/layout.gadget",   MINIMP3R_CLASS_VERSION);
	ButtonBase    = OpenLibrary("gadgets/button.gadget",   MINIMP3R_CLASS_VERSION);
	GetFileBase   = OpenLibrary("gadgets/getfile.gadget",  MINIMP3R_CLASS_VERSION);
	ChooserBase   = OpenLibrary("gadgets/chooser.gadget",  MINIMP3R_CLASS_VERSION);
	SliderBase    = OpenLibrary("gadgets/slider.gadget",   MINIMP3R_CLASS_VERSION);
	CheckBoxBase  = OpenLibrary("gadgets/checkbox.gadget", MINIMP3R_CLASS_VERSION);
	FuelGaugeBase = OpenLibrary("gadgets/fuelgauge.gadget",MINIMP3R_CLASS_VERSION);
	StringBase    = OpenLibrary("gadgets/string.gadget",   MINIMP3R_CLASS_VERSION);
	LabelBase     = OpenLibrary("images/label.image",      MINIMP3R_CLASS_VERSION);

	if (!WindowBase || !LayoutBase || !ButtonBase || !GetFileBase ||
		!ChooserBase || !SliderBase || !CheckBoxBase || !FuelGaugeBase ||
		!StringBase || !LabelBase) {
		fprintf(stderr,
			"minimp3r needs the ReAction (or ClassAct) classes V%d+ installed.\n",
			MINIMP3R_CLASS_VERSION);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------------- */
/* Window build / teardown                                                   */
/* ------------------------------------------------------------------------- */

/* A labelled child gadget, added to a (vertical) layout group. */
#define ADD_LABELLED(gadget, labeltext) \
        LAYOUT_AddChild, (ULONG)(gadget), \
        CHILD_Label, (ULONG)NewObject(LABEL_GetClass(), NULL, LABEL_Text, (ULONG)(labeltext), TAG_DONE)

static Object *ReadonlyString(ULONG id, const char *text, ULONG max)
{
	return (Object *)NewObject(STRING_GetClass(), NULL,
		GA_ID, id, GA_ReadOnly, TRUE, STRINGA_TextVal, (ULONG)text,
		STRINGA_MaxChars, max, TAG_DONE);
}

static int CheckGadget(Object *obj, const char *name)
{
	if (obj)
		return 1;
	fprintf(stderr, "minimp3r: could not create %s gadget.\n", name);
	return 0;
}

static int MrOpenWindow(MrApp *app)
{
	Object *root;

	app->fileGad = (Object *)NewObject(GETFILE_GetClass(), NULL,
		GA_ID, GID_FILE,
		GA_RelVerify, TRUE,
		GETFILE_TitleText, (ULONG)"Choose an audio file",
		GETFILE_Pattern, (ULONG)"#?.(mp3|flac|wav|aif|aiff)",
		GETFILE_DoPatterns, TRUE,
		GETFILE_FullFile, (ULONG)(app->inputName[0] ? app->inputName : ""),
		TAG_DONE);

	app->rateGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_RATE,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kRateLabels,
	                CHOOSER_Selected, (ULONG)app->rateIndex,
	                TAG_DONE);

	app->qualityGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_QUALITY,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kQualityLabels,
	                CHOOSER_Selected, (ULONG)app->qualityIndex,
	                TAG_DONE);

	app->channelGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_CHANNEL,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kChannelLabels,
	                CHOOSER_Selected, (ULONG)(app->mono ? 1 : 0),
	                TAG_DONE);

	app->volumeGad = (Object *)NewObject(SLIDER_GetClass(), NULL,
	                GA_ID, GID_VOLUME,
	                GA_RelVerify, TRUE,
	                SLIDER_Min, 0,
	                SLIDER_Max, 100,
	                SLIDER_Level, (ULONG)app->volumePercent,
	                SLIDER_Orientation, SORIENT_HORIZ,
	                SLIDER_LevelFormat, (ULONG)"%ld%%",
	                TAG_DONE);

	app->bufferGad = (Object *)NewObject(SLIDER_GetClass(), NULL,
	                GA_ID, GID_BUFFER,
	                GA_RelVerify, TRUE,
	                SLIDER_Min, 1,
	                SLIDER_Max, 30,
	                SLIDER_Level, (ULONG)app->bufferSeconds,
	                SLIDER_Orientation, SORIENT_HORIZ,
	                SLIDER_LevelFormat, (ULONG)"%ld s",
	                TAG_DONE);

	app->fastMemGad = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
	                GA_ID, GID_FASTMEM,
	                GA_RelVerify, TRUE,
	                GA_Text, (ULONG)"Decode from Fast RAM",
	                GA_Selected, (ULONG)(app->fastMem ? TRUE : FALSE),
	                TAG_DONE);

	app->fastLowGad = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
	                GA_ID, GID_FASTLOW,
	                GA_RelVerify, TRUE,
	                GA_Text, (ULONG)"Fast low-rate decode",
	                GA_Selected, (ULONG)(app->fastLowrate ? TRUE : FALSE),
	                TAG_DONE);

	app->speedGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_SPEED,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kSpeedLabels,
	                CHOOSER_Selected, (ULONG)(app->superfastLowrate ? 2 : (app->fastLowrate ? 1 : 0)),
	                TAG_DONE);

	app->widthGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_WIDTH,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kWidthLabels,
	                CHOOSER_Selected, (ULONG)(app->fakeStereo ? app->fakeStereoWidthIndex + 1 : 0),
	                TAG_DONE);

	app->delayGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_DELAY,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kDelayLabels,
	                CHOOSER_Selected, (ULONG)app->fakeStereoDelayIndex,
	                TAG_DONE);

	app->playGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_PLAY,
	                GA_RelVerify, TRUE,
	                GA_Text, (ULONG)"_Play",
	                TAG_DONE);

	app->stopGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_STOP,
	                GA_RelVerify, TRUE,
	                GA_Disabled, TRUE,
	                GA_Text, (ULONG)"_Stop",
	                TAG_DONE);

	app->nextGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_NEXT, GA_RelVerify, TRUE, GA_Text, (ULONG)"_Next", TAG_DONE);
	app->filterGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_FILTER, GA_RelVerify, TRUE, GA_Text, (ULONG)"FLT", TAG_DONE);
	app->playlistGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_PLAYLIST, GA_RelVerify, TRUE, GA_Text, (ULONG)"M3U", TAG_DONE);

	app->gaugeGad = (Object *)NewObject(FUELGAUGE_GetClass(), NULL,
	                FUELGAUGE_Min, 0,
	                FUELGAUGE_Max, 100,
	                FUELGAUGE_Level, 0,
	                FUELGAUGE_Percent, TRUE,
	                FUELGAUGE_Justification, FGJ_CENTER,
	                TAG_DONE);

	app->statusGad = (Object *)NewObject(STRING_GetClass(), NULL,
	                GA_ReadOnly, TRUE,
	                STRINGA_TextVal, (ULONG)"Ready.",
	                STRINGA_MaxChars, 128,
	                TAG_DONE);

	app->timeGad = ReadonlyString(GID_TIME, "00:00 / 00:00", 32);
	app->fileInfoGad = ReadonlyString(GID_FILEINFO, "No file info", 128);
	app->titleGad = ReadonlyString(GID_TITLE, "", 64);
	app->artistGad = ReadonlyString(GID_ARTIST, "", 64);
	app->albumGad = ReadonlyString(GID_ALBUM, "", 64);
	app->trackGad = ReadonlyString(GID_TRACK, "", 16);
	app->genreGad = ReadonlyString(GID_GENRE, "", 32);
	app->ratingGad = ReadonlyString(GID_RATING, "Rating: -", 16);

	if (!CheckGadget(app->fileGad, "file") || !CheckGadget(app->rateGad, "rate") ||
		!CheckGadget(app->qualityGad, "quality") || !CheckGadget(app->channelGad, "channel") ||
		!CheckGadget(app->volumeGad, "volume") || !CheckGadget(app->bufferGad, "buffer") ||
		!CheckGadget(app->fastMemGad, "fast memory") || !CheckGadget(app->fastLowGad, "fast low-rate") ||
		!CheckGadget(app->speedGad, "speed") || !CheckGadget(app->widthGad, "width") ||
		!CheckGadget(app->delayGad, "delay") || !CheckGadget(app->playGad, "play") ||
		!CheckGadget(app->nextGad, "next") || !CheckGadget(app->stopGad, "stop") ||
		!CheckGadget(app->filterGad, "filter") || !CheckGadget(app->playlistGad, "playlist") ||
		!CheckGadget(app->gaugeGad, "progress") || !CheckGadget(app->statusGad, "status") ||
		!CheckGadget(app->timeGad, "time") || !CheckGadget(app->fileInfoGad, "file info") ||
		!CheckGadget(app->titleGad, "title") || !CheckGadget(app->artistGad, "artist") ||
		!CheckGadget(app->albumGad, "album") || !CheckGadget(app->trackGad, "track") ||
		!CheckGadget(app->genreGad, "genre") || !CheckGadget(app->ratingGad, "rating"))
		return 0;

	root = (Object *)NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_DeferLayout, TRUE,

		ADD_LABELLED(app->fileGad, "Audio file"),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			ADD_LABELLED(app->rateGad, "Output rate"),
			ADD_LABELLED(app->qualityGad, "Quality"),
			ADD_LABELLED(app->channelGad, "Channels"),
                  TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			ADD_LABELLED(app->speedGad, "Speed"),
			ADD_LABELLED(app->widthGad, "Playback mode / width"),
			ADD_LABELLED(app->delayGad, "Delay"),
                  TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			ADD_LABELLED(app->titleGad, "Title"),
			ADD_LABELLED(app->artistGad, "Artist"),
			ADD_LABELLED(app->albumGad, "Album"),
                  TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			ADD_LABELLED(app->trackGad, "Track"),
			ADD_LABELLED(app->genreGad, "Genre"),
			ADD_LABELLED(app->ratingGad, "Rating"),
                  TAG_DONE),
		CHILD_WeightedHeight, 0,

		ADD_LABELLED(app->volumeGad, "Volume"),
		CHILD_WeightedHeight, 0,

		ADD_LABELLED(app->bufferGad, "Buffer"),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_AddChild, (ULONG)app->fastMemGad,
			LAYOUT_AddChild, (ULONG)app->fastLowGad,
                  TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->fileInfoGad, "File info"),
			ADD_LABELLED(app->timeGad, "Time"),
                  TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)app->gaugeGad,
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)app->statusGad,
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			LAYOUT_AddChild, (ULONG)app->playGad,
			LAYOUT_AddChild, (ULONG)app->nextGad,
			LAYOUT_AddChild, (ULONG)app->stopGad,
			LAYOUT_AddChild, (ULONG)app->filterGad,
			LAYOUT_AddChild, (ULONG)app->playlistGad,
                  TAG_DONE),
		CHILD_WeightedHeight, 0,
                TAG_DONE);

        if (!root) {
		fprintf(stderr, "minimp3r: could not build the gadget layout.\n");
		return 0;
	}

	app->winObj = (Object *)NewObject(WINDOW_GetClass(), NULL,
		WA_Title, (ULONG)"minimp3r",
		WA_ScreenTitle, (ULONG)"minimp3r - Helix MP3 player",
		WA_Activate, TRUE,
		WA_DepthGadget, TRUE,
		WA_DragBar, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			IDCMP_IDCMPUPDATE | IDCMP_MENUPICK,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_ParentGroup, (ULONG)root,
                TAG_DONE);

        if (!app->winObj) {
		fprintf(stderr, "minimp3r: could not create the window object.\n");
		return 0;
	}

	app->win = (struct Window *)RA_OpenWindow(app->winObj);
	if (!app->win) {
		fprintf(stderr, "minimp3r: could not open the window.\n");
		return 0;
	}
	app->menuStrip = CreateMenus(kMenus, TAG_DONE);
	if (app->menuStrip) {
		SetMenuStrip(app->win, app->menuStrip);
	} else {
		fprintf(stderr, "minimp3r: could not create menus.\n");
	}
	return 1;
}

static void MrCloseWindow(MrApp *app)
{
	if (app->win && app->menuStrip) {
		ClearMenuStrip(app->win);
	}
	if (app->menuStrip) {
		FreeMenus(app->menuStrip);
		app->menuStrip = NULL;
	}
	if (app->winObj) {
		DisposeObject(app->winObj);	/* disposes the whole gadget tree too */
		app->winObj = NULL;
		app->win = NULL;
	}
}

/* ------------------------------------------------------------------------- */
/* Reading the current gadget values back into app state                     */
/* ------------------------------------------------------------------------- */


static void BrowseForFile(MrApp *app)
{
struct FileRequester *fr;
char path[MR_MAX_PATH];

fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
ASLFR_TitleText, (ULONG)"Choose an audio file",
ASLFR_DoPatterns, TRUE,
ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|wav|aif|aiff)",
TAG_DONE);

if (!fr) {
SetStatus(app, "Could not allocate file requester.");
return;
}

if (AslRequestTags(fr,
ASLFR_Window, (ULONG)app->win,
TAG_DONE)) {
path[0] = '\0';

if (fr->fr_Drawer && fr->fr_Drawer[0])
SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);

if (fr->fr_File && fr->fr_File[0]) {
if (!AddPart((STRPTR)path, fr->fr_File, sizeof(path))) {
SetStatus(app, "Selected path is too long.");
FreeAslRequest(fr);
return;
}

SafeCopy(app->inputName, sizeof(app->inputName), path);

if (app->fileGad && app->win) {
SetGadgetAttrs((struct Gadget *)app->fileGad, app->win, NULL,
GETFILE_FullFile, (ULONG)app->inputName,
TAG_DONE);
}

SetStatus(app, "File selected.");
}
}

FreeAslRequest(fr);
}


static void UpdateFileGadget(MrApp *app)
{
	if (app->fileGad && app->win)
		SetGadgetAttrs((struct Gadget *)app->fileGad, app->win, NULL,
			GETFILE_FullFile, (ULONG)app->inputName, TAG_DONE);
}

static void PlaylistNext(MrApp *app)
{
	int wasPlaying = app->playbackActive;
	if (app->playlistCount <= 0 || app->playlistCurrent + 1 >= app->playlistCount) {
		SetStatus(app, "No next playlist item.");
		return;
	}
	app->playlistCurrent++;
	SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[app->playlistCurrent]);
	UpdateFileGadget(app);
	if (wasPlaying) {
		app->playlistNextPending = 1;
		StopPlayback(app);
	} else {
		SetStatus(app, "Playlist item selected.");
		StartPlayback(app);
	}
}

static void TrimLine(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		memmove(s, s + 1, strlen(s));
	e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
}

static void LoadPlaylistPath(MrApp *app, const char *m3uPath, const char *drawer)
{
	BPTR fh;
	char line[MR_MAX_PATH];
	char full[MR_MAX_PATH];
	app->playlistCount = 0;
	app->playlistCurrent = -1;
	fh = Open((STRPTR)m3uPath, MODE_OLDFILE);
	if (!fh) {
		SetStatus(app, "Could not open playlist.");
		return;
	}
	while (FGets(fh, line, sizeof(line)) && app->playlistCount < MR_PLAYLIST_MAX) {
		TrimLine(line);
		if (!line[0] || line[0] == '#')
			continue;
		if (strchr(line, ':') || line[0] == '/') {
			SafeCopy(full, sizeof(full), line);
		} else {
			SafeCopy(full, sizeof(full), drawer ? drawer : "");
			AddPart((STRPTR)full, (STRPTR)line, sizeof(full));
		}
		SafeCopy(app->playlist[app->playlistCount++], MR_MAX_PATH, full);
	}
	Close(fh);
	if (app->playlistCount > 0) {
		app->playlistCurrent = 0;
		SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[0]);
		UpdateFileGadget(app);
		SetStatus(app, "Playlist loaded (No art).");
	} else {
		SetStatus(app, "Playlist had no playable entries.");
	}
}

static void BrowseForPlaylist(MrApp *app)
{
	struct FileRequester *fr;
	char path[MR_MAX_PATH];
	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Choose an M3U playlist",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.(m3u|m3u8)", TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate playlist requester.");
		return;
	}
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)app->win, TAG_DONE)) {
		path[0] = '\0';
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
		if (fr->fr_File && fr->fr_File[0] && AddPart((STRPTR)path, fr->fr_File, sizeof(path)))
			LoadPlaylistPath(app, path, (const char *)fr->fr_Drawer);
	}
	FreeAslRequest(fr);
}

static void HandleMenu(MrApp *app, UWORD code, int *done)
{
	while (code != MENUNULL) {
		struct MenuItem *item = ItemAddress(app->menuStrip, code);
		if (item) {
			ULONG ud = (ULONG)GTMENUITEM_USERDATA(item);
			int mn = (int)(ud / 100), it = (int)(ud % 100);
			if (mn == MENUNUM_PROJECT && it == ITEMNUM_QUIT) *done = 1;
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ABOUT) SetStatus(app, "MiniAMP3 ReAction frontend.");
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_DTP) app->decodeThenPlay = !app->decodeThenPlay;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_BENCH) app->bench = !app->bench;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTWORK) { app->artEnabled = !app->artEnabled; SetStatus(app, "No art placeholder."); }
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCACHE) app->artCacheEnabled = !app->artCacheEnabled;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCOLOR) app->artColorEnabled = !app->artColorEnabled;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_PROGRESS) app->progressEnabled = !app->progressEnabled;
			else if (mn == MENUNUM_PLAYBACK) SetStatus(app, "Artwork action placeholder (No art).");
			code = item->NextSelect;
		} else code = MENUNULL;
	}
}

static void SyncFromGadgets(MrApp *app)
{
	ULONG v;
	STRPTR path = NULL;

	if (app->fileGad) {
		GetAttr(GETFILE_FullFile, app->fileGad, (ULONG *)(void *)&path);
		if (path)
			SafeCopy(app->inputName, sizeof(app->inputName), (const char *)path);
	}
	if (app->rateGad && GetAttr(CHOOSER_Selected, app->rateGad, &v)) {
		if ((int)v >= 0 && (int)v < MR_RATE_COUNT)
			app->rateIndex = (int)v;
	}
	if (app->qualityGad && GetAttr(CHOOSER_Selected, app->qualityGad, &v))
		app->qualityIndex = (int)v;
	if (app->channelGad && GetAttr(CHOOSER_Selected, app->channelGad, &v))
		app->mono = ((int)v == 1);
	if (app->volumeGad && GetAttr(SLIDER_Level, app->volumeGad, &v))
		app->volumePercent = (int)v;
	if (app->bufferGad && GetAttr(SLIDER_Level, app->bufferGad, &v))
		app->bufferSeconds = (int)v;
	if (app->fastMemGad && GetAttr(GA_Selected, app->fastMemGad, &v))
		app->fastMem = (v != 0);
	if (app->fastLowGad && GetAttr(GA_Selected, app->fastLowGad, &v))
		app->fastLowrate = (v != 0);
	if (app->speedGad && GetAttr(CHOOSER_Selected, app->speedGad, &v)) {
		app->fastLowrate = ((int)v >= 1);
		app->superfastLowrate = ((int)v >= 2);
	}
	if (app->widthGad && GetAttr(CHOOSER_Selected, app->widthGad, &v)) {
		app->fakeStereo = ((int)v > 0);
		app->fakeStereoWidthIndex = app->fakeStereo ? (int)v - 1 : 0;
	}
	if (app->delayGad && GetAttr(CHOOSER_Selected, app->delayGad, &v)) {
		if ((int)v >= 0 && (int)v < 5)
			app->fakeStereoDelayIndex = (int)v;
	}
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	static MrApp app;
	ULONG winSig = 0;
	ULONG timerSig = 0;
	ULONG doneSig = 0;
	int done = 0;

	(void)argc;
	(void)argv;

	/* Defaults that match a typical 030 setup. */
	app.rateIndex = 2;		/* 11025 Hz */
	app.qualityIndex = 2;		/* Normal   */
	app.mono = 0;
	app.fastMem = 0;
	app.fastLowrate = 0;
	app.volumePercent = 100;
	app.bufferSeconds = 8;
	app.fakeStereoDelayIndex = 0;
	app.artEnabled = 1;
	app.artCacheEnabled = 1;
	app.progressEnabled = 1;
	app.playlistCurrent = -1;
	app.lastPhaseShown = -1;

	/* Let the playback child find any installed *.decoder modules, exactly as
	 * the GadTools frontend does. */
	if (!gDecoderModulesPath[0])
		SafeCopy(gDecoderModulesPath, sizeof(gDecoderModulesPath),
			"PROGDIR:decoders/");

	if (!OpenLibs()) {
		CloseLibs();
		return 1;
	}

	app.donePort = CreateMsgPort();
	if (!app.donePort) {
		fprintf(stderr, "minimp3r: could not create the reply port.\n");
		CloseLibs();
		return 1;
	}

	if (!OpenTimer(&app)) {
		fprintf(stderr, "minimp3r: could not open timer.device.\n");
		CloseTimer(&app);
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

	if (!MrOpenWindow(&app)) {
		MrCloseWindow(&app);
		CloseTimer(&app);
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

	GetAttr(WINDOW_SigMask, app.winObj, &winSig);
	timerSig = 1UL << app.timerPort->mp_SigBit;
	doneSig  = 1UL << app.donePort->mp_SigBit;

	while (!done) {
		ULONG sigs = Wait(winSig | timerSig | doneSig | SIGBREAKF_CTRL_C);

		if (sigs & SIGBREAKF_CTRL_C)
			done = 1;

		if (sigs & doneSig)
			HandleDoneSignal(&app);

		if (sigs & timerSig) {
			struct Message *tmsg;
			while ((tmsg = GetMsg(app.timerPort)) != NULL)
				;
			app.timerRunning = 0;
			PollPlaybackStatus(&app);
			ArmTimer(&app, MR_TICK_MICROS);
		}

		if (sigs & winSig) {
			ULONG result;
			UWORD code = 0;
			while ((result = RA_HandleInput(app.winObj, &code)) != WMHI_LASTMSG) {
				switch (result & WMHI_CLASSMASK) {
				case WMHI_CLOSEWINDOW:
					done = 1;
					break;
				case WMHI_MENUPICK:
					if (app.menuStrip)
						HandleMenu(&app, code, &done);
					break;
				case WMHI_GADGETUP:
					switch (result & WMHI_GADGETMASK) {
					case GID_FILE:
						BrowseForFile(&app);
						break;
					case GID_PLAY:
						SyncFromGadgets(&app);
						StartPlayback(&app);
						break;
					case GID_NEXT:
						SyncFromGadgets(&app);
						PlaylistNext(&app);
						break;
					case GID_STOP:
						StopPlayback(&app);
						break;
					case GID_FILTER:
						app.hardwareFilter = !app.hardwareFilter;
						SetStatus(&app, app.hardwareFilter ? "Hardware filter on." : "Hardware filter off.");
						break;
					case GID_PLAYLIST:
						BrowseForPlaylist(&app);
						break;
					default:
						/* Keep app state current for the other controls. */
						SyncFromGadgets(&app);
						break;
					}
					break;
				default:
					break;
				}
				if (done)
					break;
			}
		}
	}

	/* Make sure any running child is stopped and reaped before we tear the
	 * window (and its shared status block) down. */
	if (app.playbackActive) {
		StopPlayback(&app);
		while (PlaybackProcessStillExists())
			Delay(5);
		HandleDoneSignal(&app);
	}

	MrCloseWindow(&app);
	CloseTimer(&app);
	if (app.donePort) {
		struct Message *m;
		while ((m = GetMsg(app.donePort)) != NULL)
			;
		DeleteMsgPort(app.donePort);
		app.donePort = NULL;
	}
	CloseLibs();
	return 0;
}

#else	/* !AMIGA_M68K */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr,
		"minimp3r is an AmigaOS ReAction/ClassAct frontend and needs an "
		"AMIGA_M68K build.\n");
	return 1;
}

#endif
