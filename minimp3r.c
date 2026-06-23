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
#include <hardware/cia.h>

#define MR_ENV_PREFIX "ENVARC:MiniAMP3"
#define MR_SETTINGS_VERSION 1
#if !defined(__AROS__) && !defined(MR_DISABLE_CIA_FILTER)
#define MR_ENABLE_CIA_FILTER 1
#endif


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
	GID_STAR1,
	GID_STAR2,
	GID_STAR3,
	GID_STAR4,
	GID_STAR5,
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
	Object         *starGad[5];
	Object         *gaugeGad;
	Object         *statusGad;
	Object         *artGad;

	struct Window  *plWin;
	struct Gadget  *plGadgets;
	struct Gadget  *plGadContext;
	struct Gadget  *plGadList;
	struct List     plList;
	struct Node     plNodes[MR_PLAYLIST_MAX];
	char            plNames[MR_PLAYLIST_MAX][80];
	APTR            plVisualInfo;

	struct MsgPort   *timerPort;
	struct timerequest *timerReq;
	int               timerRunning;
	struct MsgPort   *donePort;

	char  inputName[MR_MAX_PATH];
	char  lastDrawer[MR_MAX_PATH];
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
	int   rating;
	int   totalSecs;
	int   elapsedSecs;
	unsigned long lastFrames;

	unsigned long playbackRunId;
	int   playbackActive;
	int   playbackDonePending;
	int   stoppedByUser;
	int   lastPhaseShown;
} MrApp;

static void UpdateTimeDisplay(MrApp *app);
static void RefreshFileInfoAndTags(MrApp *app);
static void ApplyHardwareAudioFilter(MrApp *app);
static void UpdateChannelGadgetState(MrApp *app);
static void SaveSettings(MrApp *app);
static void RefreshPlaylistView(MrApp *app);
static void ClosePlaylistWindow(MrApp *app);
static void OpenPlaylistWindow(MrApp *app);

static void SyncMenuChecks(MrApp *app);

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


static int ClampInt(int value, int minValue, int maxValue)
{
	if (value < minValue)
		return minValue;
	if (value > maxValue)
		return maxValue;
	return value;
}

static void FormatTime(int secs, char *buf)
{
	if (secs < 0) {
		SafeCopy(buf, 8, "--:--");
		return;
	}
	sprintf(buf, "%02d:%02d", secs / 60, secs % 60);
}


static void CopyDrawerFromPath(char *drawer, size_t drawerSize, const char *path)
{
	char *q;
	if (!drawer || drawerSize == 0) return;
	drawer[0] = '\0';
	if (!path || !path[0]) return;
	SafeCopy(drawer, drawerSize, path);
	q = drawer + strlen(drawer);
	while (q > drawer && *q != '/' && *q != ':') q--;
	if (*q == '/' || *q == ':') *(q + 1) = '\0';
	else drawer[0] = '\0';
}

static void EnvName(char *dst, size_t dstSize, const char *key)
{
	SafeCopy(dst, dstSize, MR_ENV_PREFIX);
	strncat(dst, "/", dstSize - strlen(dst) - 1);
	strncat(dst, key, dstSize - strlen(dst) - 1);
}

static int LoadEnvIntMaybe(const char *key, int *outValue, int minValue, int maxValue)
{
	char name[64], value[32]; long n; int v;
	if (!outValue) return 0;
	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)value, sizeof(value) - 1, 0);
	if (n <= 0) return 0;
	value[n] = '\0';
	v = atoi(value);
	*outValue = ClampInt(v, minValue, maxValue);
	return 1;
}

static int LoadEnvInt(const char *key, int fallback, int minValue, int maxValue)
{
	int v;
	return LoadEnvIntMaybe(key, &v, minValue, maxValue) ? v : fallback;
}

static void LoadEnvString(const char *key, char *dst, size_t dstSize)
{
	char name[64]; long n;
	if (!dst || dstSize == 0) return;
	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)dst, dstSize - 1, 0);
	if (n > 0) dst[n] = '\0'; else dst[0] = '\0';
}

static void SaveEnvString(const char *key, const char *value)
{
	char name[64];
	EnvName(name, sizeof(name), key);
	if (!value) value = "";
	SetVar((STRPTR)name, (STRPTR)value, strlen(value), GVF_GLOBAL_ONLY);
	SetVar((STRPTR)name, (STRPTR)value, strlen(value), GVF_SAVE_VAR);
}

static void SaveEnvInt(const char *key, int value)
{
	char text[16]; sprintf(text, "%d", value); SaveEnvString(key, text);
}

static void LoadSettings(MrApp *app)
{
	app->fastLowrate = LoadEnvInt("FastLowrate", app->fastLowrate, 0, 1);
	app->superfastLowrate = LoadEnvInt("SuperfastLowrate", app->superfastLowrate, 0, 1);
	app->fastMem = LoadEnvInt("FastMem", app->fastMem, 0, 1);
	app->mono = LoadEnvInt("Mono", app->mono, 0, 1);
	app->fakeStereo = LoadEnvInt("FakeStereo", app->fakeStereo, 0, 1);
	app->fakeStereoWidthIndex = LoadEnvInt("FakeStereoWidthIndex", app->fakeStereoWidthIndex, 0, 4);
	app->fakeStereoDelayIndex = LoadEnvInt("FakeStereoDelayIndex", app->fakeStereoDelayIndex, 0, 4);
	app->hardwareFilter = LoadEnvInt("HardwareFilter", app->hardwareFilter, 0, 1);
	app->rateIndex = LoadEnvInt("RateIndex", app->rateIndex, 0, MR_RATE_COUNT - 1);
	app->bufferSeconds = LoadEnvInt("BufferSeconds", app->bufferSeconds, 1, 10);
	app->volumePercent = LoadEnvInt("Volume", app->volumePercent, 0, 100);
	app->qualityIndex = LoadEnvInt("QualityIndex", app->qualityIndex, 0, 3);
	app->decodeThenPlay = LoadEnvInt("DecodeThenPlay", app->decodeThenPlay, 0, 1);
	app->bench = LoadEnvInt("Bench", app->bench, 0, 1);
	app->artEnabled = LoadEnvInt("Artwork", app->artEnabled, 0, 1);
	app->artCacheEnabled = LoadEnvInt("ArtworkCache", app->artCacheEnabled, 0, 1);
	app->artColorEnabled = LoadEnvInt("ArtworkColour", app->artColorEnabled, 0, 1);
	app->progressEnabled = LoadEnvInt("ProgressBar", app->progressEnabled, 0, 1);
	LoadEnvString("LastDrawer", app->lastDrawer, sizeof(app->lastDrawer));
}

static void SaveSettings(MrApp *app)
{
	SaveEnvInt("FastLowrate", app->fastLowrate);
	SaveEnvInt("SuperfastLowrate", app->superfastLowrate);
	SaveEnvInt("FastMem", app->fastMem);
	SaveEnvInt("Mono", app->mono);
	SaveEnvInt("FakeStereo", app->fakeStereo);
	SaveEnvInt("FakeStereoWidthIndex", app->fakeStereoWidthIndex);
	SaveEnvInt("FakeStereoDelayIndex", app->fakeStereoDelayIndex);
	SaveEnvInt("HardwareFilter", app->hardwareFilter);
	SaveEnvInt("RateIndex", app->rateIndex);
	SaveEnvInt("BufferSeconds", ClampInt(app->bufferSeconds, 1, 10));
	SaveEnvInt("Volume", ClampInt(app->volumePercent, 0, 100));
	SaveEnvInt("QualityIndex", app->qualityIndex);
	SaveEnvInt("SettingsVersion", MR_SETTINGS_VERSION);
	SaveEnvInt("DecodeThenPlay", app->decodeThenPlay);
	SaveEnvInt("Bench", app->bench);
	SaveEnvInt("Artwork", app->artEnabled);
	SaveEnvInt("ArtworkCache", app->artCacheEnabled);
	SaveEnvInt("ArtworkColour", app->artColorEnabled);
	SaveEnvInt("ProgressBar", app->progressEnabled);
	SaveEnvString("LastDrawer", app->lastDrawer);
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

static void UpdateChannelGadgetState(MrApp *app)
{
	if (app->win && app->channelGad)
		SetGadgetAttrs((struct Gadget *)app->channelGad, app->win, NULL,
			GA_Disabled, (ULONG)(app->fakeStereo ? TRUE : FALSE), TAG_DONE);
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
	sprintf(num, "%d", ClampInt(app->bufferSeconds, 1, 10));
	AddArg(args, num);
	AddArg(args, "--volume");
	sprintf(num, "%d", ClampInt(app->volumePercent, 0, 100));
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
	long audioSecs;

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

	if (rate > 0 && frames != app->lastFrames) {
		app->lastFrames = frames;
		audioSecs = (long)((frames * 1152UL) / (unsigned long)rate);
		audioSecs -= halfMs ? (long)((halfMs + 999UL) / 1000UL) : app->bufferSeconds;
		if (audioSecs < 0) audioSecs = 0;
		if (app->totalSecs > 0 && audioSecs > app->totalSecs) audioSecs = app->totalSecs;
		app->elapsedSecs = (int)audioSecs;
		UpdateTimeDisplay(app);
		SetGauge(app, (app->progressEnabled && app->totalSecs > 0) ? (app->elapsedSecs * 100) / app->totalSecs : 0);
	}
	(void)spareMs;
	(void)halfMs;

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
	                GA_Disabled, (ULONG)(app->fakeStereo ? TRUE : FALSE),
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
	                SLIDER_Max, 10,
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
	                CHOOSER_Selected, (ULONG)(app->superfastLowrate ? 1 : 0),
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
	                GA_ID, GID_PLAYLIST, GA_RelVerify, TRUE, GA_Text, (ULONG)"Playlist", TAG_DONE);

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
	                STRINGA_MaxChars, 80,
	                TAG_DONE);

	app->timeGad = ReadonlyString(GID_TIME, "00:00 / --:--", 32);
	app->fileInfoGad = ReadonlyString(GID_FILEINFO, "No file info", 56);
	app->titleGad = ReadonlyString(GID_TITLE, "-", 32);
	app->artistGad = ReadonlyString(GID_ARTIST, "-", 28);
	app->albumGad = ReadonlyString(GID_ALBUM, "-", 32);
	app->trackGad = ReadonlyString(GID_TRACK, "-", 12);
	app->genreGad = ReadonlyString(GID_GENRE, "-", 20);
	app->ratingGad = ReadonlyString(GID_RATING, "0/5", 16);
	/* ReAction artwork render target: keep this as a fixed right-hand box.
	 * TODO: reuse the GadTools picojpg artwork cache/decode path and render into
	 * this reserved rectangle when the ReAction rastport geometry is stable. */
	app->artGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_LAST,
	                GA_ReadOnly, TRUE,
	                GA_Disabled, TRUE,
	                GA_Text, (ULONG)"No art",
	                TAG_DONE);
	{ int i; for (i = 0; i < 5; i++) app->starGad[i] = (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, GID_STAR1 + i, GA_RelVerify, TRUE, GA_Text, (ULONG)"-", TAG_DONE); }

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
		!CheckGadget(app->genreGad, "genre") || !CheckGadget(app->ratingGad, "rating") ||
		!CheckGadget(app->starGad[0], "star1") || !CheckGadget(app->starGad[1], "star2") ||
		!CheckGadget(app->starGad[2], "star3") || !CheckGadget(app->starGad[3], "star4") ||
		!CheckGadget(app->starGad[4], "star5") || !CheckGadget(app->artGad, "artwork placeholder"))
		return 0;

	root = (Object *)NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_DeferLayout, TRUE,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,

				LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					ADD_LABELLED(app->fileGad, "File"),
					TAG_DONE),
				CHILD_WeightedHeight, 0,

				LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					ADD_LABELLED(app->titleGad, "Title"),
					ADD_LABELLED(app->artistGad, "Artist"),
					TAG_DONE),
				CHILD_WeightedHeight, 0,

				LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					ADD_LABELLED(app->albumGad, "Album"),
					ADD_LABELLED(app->genreGad, "Genre"),
					TAG_DONE),
				CHILD_WeightedHeight, 0,

				LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					ADD_LABELLED(app->ratingGad, "Rating"),
					ADD_LABELLED(app->trackGad, "Track"),
					LAYOUT_AddChild, (ULONG)app->starGad[0],
					LAYOUT_AddChild, (ULONG)app->starGad[1],
					LAYOUT_AddChild, (ULONG)app->starGad[2],
					LAYOUT_AddChild, (ULONG)app->starGad[3],
					LAYOUT_AddChild, (ULONG)app->starGad[4],
					TAG_DONE),
				CHILD_WeightedHeight, 0,
				TAG_DONE),
			LAYOUT_AddChild, (ULONG)app->artGad,
			CHILD_MinWidth, 96,
			CHILD_MinHeight, 96,
			CHILD_WeightedWidth, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->speedGad, "Speed"),
			ADD_LABELLED(app->widthGad, "Mode/width"),
			ADD_LABELLED(app->delayGad, "Delay"),
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->rateGad, "Rate"),
			ADD_LABELLED(app->qualityGad, "Quality"),
			ADD_LABELLED(app->channelGad, "Mono/Stereo"),
			LAYOUT_AddChild, (ULONG)app->fastMemGad,
			LAYOUT_AddChild, (ULONG)app->fastLowGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		ADD_LABELLED(app->bufferGad, "Buffer"),
		CHILD_WeightedHeight, 0,
		ADD_LABELLED(app->volumeGad, "Volume"),
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
		WA_Width, 620,
		WA_Height, 420,
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
		SyncMenuChecks(app);
		SetMenuStrip(app->win, app->menuStrip);
	} else {
		fprintf(stderr, "minimp3r: could not create menus.\n");
	}
	return 1;
}

static void MrCloseWindow(MrApp *app)
{
	ClosePlaylistWindow(app);
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


typedef struct MrMp3Info {
	char title[64], artist[64], album[64], track[16], genre[32];
	int bitrateKbps, sampleRate, channels, channelMode, durationSecs, rating;
	unsigned long fileSize;
} MrMp3Info;

static void CopyTrim(char *dst, size_t size, const unsigned char *src, int n)
{
	int end = n;
	while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == 0)) end--;
	if ((size_t)end >= size) end = (int)size - 1;
	memcpy(dst, src, end); dst[end] = 0;
}


static int Id3Synchsafe(const unsigned char *p)
{
	return ((p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) | ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static void CopyId3Text(char *dst, size_t size, const unsigned char *p, long n)
{
	if (n <= 1) return;
	if (p[0] == 0 || p[0] == 3) CopyTrim(dst, size, p + 1, (int)(n - 1));
	else if (n > 3) CopyTrim(dst, size, p + 3, (int)(n - 3));
}

static int RatingFromPopmByte(unsigned char b)
{
	if (b == 0) return 0;
	if (b < 32) return 1;
	if (b < 96) return 2;
	if (b < 160) return 3;
	if (b < 224) return 4;
	return 5;
}

static void ReadId3v2(FILE *f, MrMp3Info *info)
{
	unsigned char hdr[10], fh[10]; long end;
	if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3)) return;
	end = 10 + Id3Synchsafe(hdr + 6);
	while (ftell(f) + 10 <= end && fread(fh, 1, 10, f) == 10) {
		long sz = Id3Synchsafe(fh + 4); unsigned char *payload;
		if (fh[0] == 0 || sz <= 0 || ftell(f) + sz > end) break;
		payload = (unsigned char *)malloc((size_t)sz);
		if (!payload) { fseek(f, sz, SEEK_CUR); continue; }
		if (fread(payload, 1, (size_t)sz, f) != (size_t)sz) { free(payload); break; }
		if (!memcmp(fh, "TIT2", 4) && !info->title[0]) CopyId3Text(info->title, sizeof(info->title), payload, sz);
		else if (!memcmp(fh, "TPE1", 4) && !info->artist[0]) CopyId3Text(info->artist, sizeof(info->artist), payload, sz);
		else if (!memcmp(fh, "TALB", 4) && !info->album[0]) CopyId3Text(info->album, sizeof(info->album), payload, sz);
		else if (!memcmp(fh, "TRCK", 4) && !info->track[0]) CopyId3Text(info->track, sizeof(info->track), payload, sz);
		else if (!memcmp(fh, "TCON", 4) && !info->genre[0]) CopyId3Text(info->genre, sizeof(info->genre), payload, sz);
		else if (!memcmp(fh, "POPM", 4)) { long i; for (i = 0; i + 1 < sz; i++) if (payload[i] == 0) { info->rating = RatingFromPopmByte(payload[i + 1]); break; } }
		free(payload);
	}
}

static void ReadId3v1(FILE *f, MrMp3Info *info)
{
	unsigned char b[128];
	static const char *genres[] = { "Blues","Classic Rock","Country","Dance","Disco","Funk","Grunge","Hip-Hop","Jazz","Metal","New Age","Oldies","Other","Pop","R&B","Rap","Reggae","Rock" };
	if (fseek(f, -128, SEEK_END) != 0 || fread(b, 1, 128, f) != 128 || memcmp(b, "TAG", 3)) return;
	CopyTrim(info->title, sizeof(info->title), b + 3, 30);
	CopyTrim(info->artist, sizeof(info->artist), b + 33, 30);
	CopyTrim(info->album, sizeof(info->album), b + 63, 30);
	if (b[125] == 0 && b[126] != 0) sprintf(info->track, "%u", (unsigned)b[126]);
	if (b[127] < sizeof(genres) / sizeof(genres[0])) SafeCopy(info->genre, sizeof(info->genre), genres[b[127]]);
}

static void ReadMpegHeader(FILE *f, MrMp3Info *info, long *firstFrame)
{
	unsigned char h[4]; long pos = 0;
	static const int br[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
	static const int sr[4] = {44100,48000,32000,0};
	fseek(f, 0, SEEK_SET);
	while (fread(h, 1, 4, f) == 4 && pos < 65536) {
		if (h[0] == 0xff && (h[1] & 0xe0) == 0xe0) {
			int bi = (h[2] >> 4) & 15, si = (h[2] >> 2) & 3;
			info->bitrateKbps = br[bi]; info->sampleRate = sr[si];
			info->channelMode = (h[3] >> 6) & 3; info->channels = info->channelMode == 3 ? 1 : 2;
			*firstFrame = pos; return;
		}
		pos++; fseek(f, pos, SEEK_SET);
	}
}

static void ReadMp3Info(const char *path, MrMp3Info *info)
{
	FILE *f; long first = -1, sz;
	memset(info, 0, sizeof(*info));
	f = fopen(path, "rb"); if (!f) return;
	ReadId3v2(f, info);
	ReadMpegHeader(f, info, &first);
	if (fseek(f, 0, SEEK_END) == 0) { sz = ftell(f); if (sz > 0) info->fileSize = (unsigned long)sz; }
	if (info->bitrateKbps > 0 && first >= 0 && info->fileSize > (unsigned long)first)
		info->durationSecs = (int)(((info->fileSize - (unsigned long)first) * 8UL) / ((unsigned long)info->bitrateKbps * 1000UL));
	ReadId3v1(f, info);
	fclose(f);
}

static void UpdateTimeDisplay(MrApp *app)
{
	char e[8], t[8], buf[24];
	FormatTime(app->elapsedSecs, e); FormatTime(app->totalSecs > 0 ? app->totalSecs : -1, t);
	sprintf(buf, "%s / %s", e, t);
	if (app->timeGad && app->win) SetGadgetAttrs((struct Gadget *)app->timeGad, app->win, NULL, STRINGA_TextVal, (ULONG)buf, TAG_DONE);
}

static void UpdateRatingDisplay(MrApp *app)
{
	char buf[16]; int i;
	sprintf(buf, "%d/5", app->rating);
	if (app->ratingGad && app->win) SetGadgetAttrs((struct Gadget *)app->ratingGad, app->win, NULL, STRINGA_TextVal, (ULONG)buf, TAG_DONE);
	for (i = 0; i < 5; i++) if (app->starGad[i] && app->win) SetGadgetAttrs((struct Gadget *)app->starGad[i], app->win, NULL, GA_Text, (ULONG)(i < app->rating ? "*" : "-"), TAG_DONE);
}

static const char *MrChannelModeName(const MrMp3Info *info)
{
	if (!info || info->channels <= 0) return "-";
	if (info->channels == 1) return "mono";
	if (info->channelMode == 1) return "joint-stereo";
	return "stereo";
}

static void RefreshFileInfoAndTags(MrApp *app)
{
	MrMp3Info info; char fileInfo[128]; const char *ch; unsigned long kb;
	if (!app->inputName[0]) {
		if (app->fileInfoGad && app->win) SetGadgetAttrs((struct Gadget *)app->fileInfoGad, app->win, NULL, STRINGA_TextVal, (ULONG)"No file info", TAG_DONE);
		return;
	}
	ReadMp3Info(app->inputName, &info);
	app->rating = info.rating; app->totalSecs = info.durationSecs; app->elapsedSecs = 0; app->lastFrames = 0;
	ch = MrChannelModeName(&info);
	kb = (info.fileSize + 1023UL) / 1024UL;
	if (info.bitrateKbps > 0 || info.sampleRate > 0 || info.fileSize > 0)
		sprintf(fileInfo, "%d kbps, %s, %d Hz, %lu KB", info.bitrateKbps, ch, info.sampleRate, kb);
	else
		SafeCopy(fileInfo, sizeof(fileInfo), "-");
	if (app->titleGad && app->win) SetGadgetAttrs((struct Gadget *)app->titleGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.title[0] ? info.title : "-"), TAG_DONE);
	if (app->artistGad && app->win) SetGadgetAttrs((struct Gadget *)app->artistGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.artist[0] ? info.artist : "-"), TAG_DONE);
	if (app->albumGad && app->win) SetGadgetAttrs((struct Gadget *)app->albumGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.album[0] ? info.album : "-"), TAG_DONE);
	if (app->trackGad && app->win) SetGadgetAttrs((struct Gadget *)app->trackGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.track[0] ? info.track : "-"), TAG_DONE);
	if (app->genreGad && app->win) SetGadgetAttrs((struct Gadget *)app->genreGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.genre[0] ? info.genre : "-"), TAG_DONE);
	if (app->fileInfoGad && app->win) SetGadgetAttrs((struct Gadget *)app->fileInfoGad, app->win, NULL, STRINGA_TextVal, (ULONG)fileInfo, TAG_DONE);
	UpdateRatingDisplay(app); UpdateTimeDisplay(app); SetGauge(app, 0); SetStatus(app, "File ready (No art).");
}

static void ApplyHardwareAudioFilter(MrApp *app)
{
#ifdef MR_ENABLE_CIA_FILTER
	volatile UBYTE *ciapra = (volatile UBYTE *)0xbfe001;

	if (app && app->hardwareFilter)
		*ciapra &= (UBYTE)~CIAF_LED;
	else
		*ciapra |= (UBYTE)CIAF_LED;
#else
	(void)app;
#endif
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
ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
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
CopyDrawerFromPath(app->lastDrawer, sizeof(app->lastDrawer), app->inputName);
SaveSettings(app);

if (app->fileGad && app->win) {
SetGadgetAttrs((struct Gadget *)app->fileGad, app->win, NULL,
GETFILE_FullFile, (ULONG)app->inputName,
TAG_DONE);
}

RefreshFileInfoAndTags(app);
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
	RefreshFileInfoAndTags(app);
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
		RefreshFileInfoAndTags(app);
		RefreshPlaylistView(app);
		SetStatus(app, "Playlist loaded (No art).");
	} else {
		RefreshPlaylistView(app);
		SetStatus(app, "Playlist had no playable entries.");
	}
}

enum {
	PL_GID_LIST = 200,
	PL_GID_LOAD_M3U,
	PL_GID_PLAY,
	PL_GID_CLOSE
};

static const char *PlaylistBaseName(const char *path)
{
	const char *p = path;
	const char *last = path;
	while (*p) {
		if (*p == '/' || *p == ':')
			last = p + 1;
		p++;
	}
	return last;
}

static void RefreshPlaylistView(MrApp *app)
{
	int i;
	NewList(&app->plList);
	for (i = 0; i < app->playlistCount; i++) {
		SafeCopy(app->plNames[i], sizeof(app->plNames[i]), PlaylistBaseName(app->playlist[i]));
		app->plNodes[i].ln_Name = app->plNames[i];
		app->plNodes[i].ln_Type = NT_USER;
		app->plNodes[i].ln_Pri = 0;
		AddTail(&app->plList, &app->plNodes[i]);
	}
	if (app->plWin && app->plGadList)
		GT_SetGadgetAttrs(app->plGadList, app->plWin, NULL,
			GTLV_Labels, (ULONG)&app->plList,
			GTLV_Selected, app->playlistCurrent >= 0 ? (ULONG)app->playlistCurrent : (ULONG)~0,
			TAG_DONE);
}

static void ClosePlaylistWindow(MrApp *app)
{
	struct IntuiMessage *msg;
	if (!app->plWin)
		return;
	ModifyIDCMP(app->plWin, 0);
	while ((msg = GT_GetIMsg(app->plWin->UserPort)) != NULL)
		GT_ReplyIMsg(msg);
	if (app->plGadgets)
		RemoveGList(app->plWin, app->plGadgets, -1);
	CloseWindow(app->plWin);
	app->plWin = NULL;
	if (app->plGadgets) {
		FreeGadgets(app->plGadgets);
		app->plGadgets = NULL;
		app->plGadContext = NULL;
		app->plGadList = NULL;
	}
	if (app->plVisualInfo) {
		FreeVisualInfo(app->plVisualInfo);
		app->plVisualInfo = NULL;
	}
}

static void OpenPlaylistWindow(MrApp *app)
{
	struct NewWindow nw;
	struct NewGadget ng;
	struct Gadget *gad;
	if (app->plWin || !app->win || !GadToolsBase)
		return;
	app->plVisualInfo = GetVisualInfoA(app->win->WScreen, NULL);
	if (!app->plVisualInfo)
		return;
	app->plGadContext = CreateContext(&app->plGadgets);
	if (!app->plGadContext) {
		FreeVisualInfo(app->plVisualInfo);
		app->plVisualInfo = NULL;
		return;
	}
	gad = app->plGadContext;
	RefreshPlaylistView(app);
	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = 8; ng.ng_TopEdge = 20; ng.ng_Width = 344; ng.ng_Height = 120;
	ng.ng_GadgetID = PL_GID_LIST; ng.ng_Flags = 0; ng.ng_VisualInfo = app->plVisualInfo;
	app->plGadList = gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
		GTLV_Labels, (ULONG)&app->plList,
		GTLV_Selected, app->playlistCurrent >= 0 ? (ULONG)app->playlistCurrent : (ULONG)~0,
		GA_RelVerify, TRUE, TAG_DONE);
	if (!gad) goto fail;
	ng.ng_TopEdge = 148; ng.ng_Width = 112; ng.ng_Height = 18; ng.ng_Flags = PLACETEXT_IN;
	ng.ng_LeftEdge = 8; ng.ng_GadgetText = (UBYTE *)"Load M3U"; ng.ng_GadgetID = PL_GID_LOAD_M3U;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 128; ng.ng_GadgetText = (UBYTE *)"Play"; ng.ng_GadgetID = PL_GID_PLAY;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 248; ng.ng_GadgetText = (UBYTE *)"Close"; ng.ng_GadgetID = PL_GID_CLOSE;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = app->win->LeftEdge + 20; nw.TopEdge = app->win->TopEdge + 20;
	nw.Width = 368; nw.Height = 176;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH;
	nw.Title = (UBYTE *)"MiniAMP3 Playlist";
	nw.MinWidth = nw.MaxWidth = 368; nw.MinHeight = nw.MaxHeight = 176;
	nw.Type = WBENCHSCREEN;
	app->plWin = OpenWindowTags(&nw, TAG_DONE);
	if (!app->plWin) goto fail;
	AddGList(app->plWin, app->plGadgets, (UWORD)-1, -1, NULL);
	RefreshGList(app->plGadgets, app->plWin, NULL, -1);
	GT_RefreshWindow(app->plWin, NULL);
	RefreshPlaylistView(app);
	return;
fail:
	if (app->plGadgets) { FreeGadgets(app->plGadgets); app->plGadgets = NULL; app->plGadContext = NULL; app->plGadList = NULL; }
	if (app->plVisualInfo) { FreeVisualInfo(app->plVisualInfo); app->plVisualInfo = NULL; }
}

static void BrowseForPlaylist(MrApp *app)
{
	struct FileRequester *fr;
	char path[MR_MAX_PATH];
	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Choose an M3U playlist",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.(m3u|m3u8)",
		ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
		TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate playlist requester.");
		return;
	}
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)app->win, TAG_DONE)) {
		path[0] = '\0';
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
		if (fr->fr_File && fr->fr_File[0] && AddPart((STRPTR)path, fr->fr_File, sizeof(path))) {
			CopyDrawerFromPath(app->lastDrawer, sizeof(app->lastDrawer), path);
			SaveSettings(app);
			LoadPlaylistPath(app, path, (const char *)fr->fr_Drawer);
		}
	}
	FreeAslRequest(fr);
}

static void PlaylistLoadCurrent(MrApp *app, int index, int startPlayback)
{
	if (index < 0 || index >= app->playlistCount)
		return;
	app->playlistCurrent = index;
	SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[index]);
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	RefreshPlaylistView(app);
	SetStatus(app, "Playlist item selected.");
	if (startPlayback)
		StartPlayback(app);
}

static void HandlePlaylistWindow(MrApp *app)
{
	struct IntuiMessage *msg;
	if (!app->plWin)
		return;
	while ((msg = GT_GetIMsg(app->plWin->UserPort)) != NULL) {
		ULONG cls = msg->Class;
		struct Gadget *gad = (struct Gadget *)msg->IAddress;
		UWORD gid = gad ? gad->GadgetID : 0;
		UWORD code = msg->Code;
		GT_ReplyIMsg(msg);
		if (cls == IDCMP_CLOSEWINDOW) {
			ClosePlaylistWindow(app);
			return;
		}
		if (cls == IDCMP_GADGETUP) {
			if (gid == PL_GID_LIST) {
				PlaylistLoadCurrent(app, (int)code, 0);
			} else if (gid == PL_GID_LOAD_M3U) {
				BrowseForPlaylist(app);
			} else if (gid == PL_GID_PLAY) {
				PlaylistLoadCurrent(app, app->playlistCurrent, 1);
			} else if (gid == PL_GID_CLOSE) {
				ClosePlaylistWindow(app);
				return;
			}
		}
	}
}

static void SetMenuItemChecked(MrApp *app, int menuNum, int itemNum, int checked)
{
	struct MenuItem *item;
	if (!app->menuStrip) return;
	item = ItemAddress(app->menuStrip, SHIFTMENU(menuNum) | SHIFTITEM(itemNum));
	if (!item) return;
	if (checked) item->Flags |= CHECKED;
	else item->Flags &= ~CHECKED;
}

static void SyncMenuChecks(MrApp *app)
{
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_DTP, app->decodeThenPlay);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_BENCH, app->bench);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTWORK, app->artEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTCACHE, app->artCacheEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTCOLOR, app->artColorEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_PROGRESS, app->progressEnabled);
}

static void HandleMenu(MrApp *app, UWORD code, int *done)
{
	while (code != MENUNULL) {
		struct MenuItem *item = ItemAddress(app->menuStrip, code);
		if (item) {
			ULONG ud = (ULONG)GTMENUITEM_USERDATA(item);
			int mn = (int)(ud / 100), it = (int)(ud % 100);
			if (mn == MENUNUM_PROJECT && it == ITEMNUM_QUIT) *done = 1;
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ABOUT) {
				struct EasyStruct es;
				es.es_StructSize = sizeof(es);
				es.es_Flags = 0;
				es.es_Title = (UBYTE *)"About MiniAMP3";
				es.es_TextFormat = (UBYTE *)"MiniAMP3\nHelix fixed-point MP3 decoder\nAmigaOS ReAction frontend";
				es.es_GadgetFormat = (UBYTE *)"OK";
				EasyRequest(app->win, &es, NULL, TAG_DONE);
			}
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_DTP) app->decodeThenPlay = !app->decodeThenPlay;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_BENCH) app->bench = !app->bench;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTWORK) { app->artEnabled = !app->artEnabled; SetStatus(app, "No art placeholder."); }
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCACHE) app->artCacheEnabled = !app->artCacheEnabled;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCOLOR) app->artColorEnabled = !app->artColorEnabled;
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_PROGRESS) app->progressEnabled = !app->progressEnabled;
			else if (mn == MENUNUM_PLAYBACK) SetStatus(app, "Artwork action placeholder (No art).");
			SyncMenuChecks(app);
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
	if (app->volumeGad && GetAttr(SLIDER_Level, app->volumeGad, &v)) {
		int oldPercent = app->volumePercent;
		app->volumePercent = ClampInt((int)v, 0, 100);
		if (app->volumePercent != oldPercent) {
			gMiniAmp3RequestedVolume = (unsigned short)app->volumePercent;
			gMiniAmp3VolumeSequence++;
		}
	}
	if (app->bufferGad && GetAttr(SLIDER_Level, app->bufferGad, &v))
		app->bufferSeconds = ClampInt((int)v, 1, 10);
	if (app->fastMemGad && GetAttr(GA_Selected, app->fastMemGad, &v))
		app->fastMem = (v != 0);
	if (app->fastLowGad && GetAttr(GA_Selected, app->fastLowGad, &v))
		app->fastLowrate = (v != 0);
	if (app->speedGad && GetAttr(CHOOSER_Selected, app->speedGad, &v)) {
		app->superfastLowrate = ((int)v >= 1);
	}
	if (app->widthGad && GetAttr(CHOOSER_Selected, app->widthGad, &v)) {
		app->fakeStereo = ((int)v > 0);
		app->fakeStereoWidthIndex = app->fakeStereo ? (int)v - 1 : 0;
		UpdateChannelGadgetState(app);
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
	app.bufferSeconds = 10;
	app.fakeStereoDelayIndex = 0;
	app.artEnabled = 1;
	app.artCacheEnabled = 1;
	app.artColorEnabled = 0;
	app.progressEnabled = 0;
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

	LoadSettings(&app);

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
		SyncFromGadgets(&app);
		SaveSettings(&app);
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
		ULONG plSig = (app.plWin && app.plWin->UserPort) ? (1UL << app.plWin->UserPort->mp_SigBit) : 0;
		ULONG sigs = Wait(winSig | timerSig | doneSig | plSig | SIGBREAKF_CTRL_C);

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

		if (plSig && (sigs & plSig))
			HandlePlaylistWindow(&app);

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
						ApplyHardwareAudioFilter(&app);
						SetGadgetAttrs((struct Gadget *)app.filterGad, app.win, NULL, GA_Text, (ULONG)(app.hardwareFilter ? "FLT*" : "FLT"), TAG_DONE);
						SetStatus(&app, app.hardwareFilter ? "Hardware filter enabled." : "Hardware filter disabled.");
						break;
					case GID_PLAYLIST:
						if (app.plWin)
							ClosePlaylistWindow(&app);
						else
							OpenPlaylistWindow(&app);
						break;
					case GID_STAR1: case GID_STAR2: case GID_STAR3: case GID_STAR4: case GID_STAR5:
						app.rating = (int)(result & WMHI_GADGETMASK) - GID_STAR1 + 1;
						UpdateRatingDisplay(&app);
						SetStatus(&app, "Rating updated for this file only.");
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

	SyncFromGadgets(&app);
	SaveSettings(&app);
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
