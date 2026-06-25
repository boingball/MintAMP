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
#include "picojpeg.h"
#include "radio_stream.h"

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
#include <proto/graphics.h>

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
#define MR_ART_W        64
#define MR_ART_H        64
#define MR_ART_COLOR_CACHE 64
#define MR_MAX_JPEG_DIM 1024
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
	(STRPTR)"Ultrafast",
	(STRPTR)"22050 Mono Ultrafast",
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
	APTR            visualInfo;

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
	int   ultrafast;
	int   cd32Ultrafast;
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
	int   artCacheBypass;
	int   artPensBuilt;
	int   artPenCacheUsed;
	struct { unsigned long key; long pen; } artPenCache[MR_ART_COLOR_CACHE];
	unsigned char artRGBBuf[MR_ART_W * MR_ART_H * 3];
	unsigned char artPenIdx[MR_ART_W * MR_ART_H];
	int   playlistCount;
	int   playlistCurrent;
	int   playlistSelected;
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
	unsigned char artGreyBuf[MR_ART_W * MR_ART_H];
	int artValid;
} MrApp;

static void UpdateTimeDisplay(MrApp *app);
static void RefreshFileInfoAndTags(MrApp *app);
static void SaveSettings(MrApp *app);
static void ApplyHardwareAudioFilter(MrApp *app);
static void UpdateChannelGadgetState(MrApp *app);
static void UpdateNextButtonState(MrApp *app);
static void DrawArtPanel(MrApp *app);
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
	app->ultrafast = LoadEnvInt("Ultrafast", app->ultrafast, 0, 1);
	app->cd32Ultrafast = LoadEnvInt("CD32Ultrafast", app->cd32Ultrafast, 0, 1);
	if (app->cd32Ultrafast) {
		app->ultrafast = 0;
		app->fastLowrate = 1;
		app->superfastLowrate = 1;
	} else if (app->ultrafast) {
		app->fastLowrate = 0;
		app->superfastLowrate = 0;
	}
	app->fastMem = LoadEnvInt("FastMem", app->fastMem, 0, 1);
	app->mono = LoadEnvInt("Mono", app->mono, 0, 1);
	app->fakeStereo = LoadEnvInt("FakeStereo", app->fakeStereo, 0, 1);
	app->fakeStereoWidthIndex = LoadEnvInt("FakeStereoWidthIndex", app->fakeStereoWidthIndex, 0, 4);
	app->fakeStereoDelayIndex = LoadEnvInt("FakeStereoDelayIndex", app->fakeStereoDelayIndex, 0, 4);
	app->hardwareFilter = LoadEnvInt("HardwareFilter", app->hardwareFilter, 0, 1);
	app->rateIndex = LoadEnvInt("RateIndex", app->rateIndex, 0, MR_RATE_COUNT - 1);
	if (app->cd32Ultrafast) {
		app->mono = 1;
		app->rateIndex = 3;
	}
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
	SaveEnvInt("Ultrafast", app->ultrafast);
	SaveEnvInt("CD32Ultrafast", app->cd32Ultrafast);
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

static void UpdateNextButtonState(MrApp *app)
{
	int enabled = app->playlistCount > 0 && app->playlistCurrent >= 0 &&
		app->playlistCurrent + 1 < app->playlistCount &&
		!app->playbackDonePending && !gPlayer.stopRequested;
	if (app->win && app->nextGad)
		SetGadgetAttrs((struct Gadget *)app->nextGad, app->win, NULL,
			GA_Disabled, (ULONG)(enabled ? FALSE : TRUE), TAG_DONE);
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
	UpdateNextButtonState(app);
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
	if (!strncmp(app->inputName, "http://", 7))
		AddArg(args, "--radio-stream");
	if (app->fastMem)
		AddArg(args, "--fast-mem");
	if (app->cd32Ultrafast) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
		AddArg(args, "--exp-reduced-taps");
		AddArg(args, "--subband-cap");
		AddArg(args, "12");
	} else if (app->superfastLowrate ||
		(app->ultrafast && strcmp(kRates[app->rateIndex], "28600") != 0)) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
	} else if (app->fastLowrate && strcmp(kRates[app->rateIndex], "28600")) {
		AddArg(args, "--fast-lowrate");
	}
	if (app->ultrafast && strcmp(kRates[app->rateIndex], "28600") == 0)
		AddArg(args, "--ultrafast");
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
	app->elapsedSecs = 0;
	app->lastFrames = 0;
	UpdateTimeDisplay(app);
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
	UpdateNextButtonState(app);
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
	app->elapsedSecs = (stoppedByUser || app->totalSecs <= 0) ? 0 : app->totalSecs;
	UpdateTimeDisplay(app);
	SetGauge(app, app->progressEnabled && !stoppedByUser ? 100 : 0);
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

	if (rate > 0 && frames > 0 && frames != app->lastFrames) {
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
		if (frames > 0 && rate > 0) {
			sprintf(buf, "Playing - %lu frames @ %d Hz", frames, rate);
			SetStatus(app, buf);
		} else
			SetStatus(app, "Playing...");
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
		GETFILE_Pattern, (ULONG)"#?.(mp3|flac|aac|wav|aif|aiff)",
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
	                CHOOSER_Selected, (ULONG)(app->cd32Ultrafast ? 3 : (app->ultrafast ? 2 : (app->superfastLowrate ? 1 : 0))),
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
	                GA_ID, GID_NEXT, GA_RelVerify, TRUE, GA_Disabled, TRUE, GA_Text, (ULONG)"_Next", TAG_DONE);
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
	/* ReAction artwork render target: a fixed right-hand box.  The picojpeg
	 * decode/dither path (shared with the GadTools frontend) renders the 64x64
	 * thumbnail straight into this gadget's rectangle via DrawArtPanel(), which
	 * is re-stamped on every window refresh/resize. */
	app->artGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_LAST,
	                GA_ReadOnly, TRUE,
	                GA_Text, (ULONG)"",
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

		/* File chooser spans the full width at the top. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->fileGad, "File"),
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* Track metadata stacked on the left, artwork box on the right. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_SpaceInner, TRUE,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
				LAYOUT_BevelStyle, BVS_GROUP,
				LAYOUT_Label, (ULONG)"Track",
				LAYOUT_SpaceInner, TRUE,

				ADD_LABELLED(app->titleGad, "Title"),
				CHILD_WeightedHeight, 0,
				ADD_LABELLED(app->artistGad, "Artist"),
				CHILD_WeightedHeight, 0,
				ADD_LABELLED(app->albumGad, "Album"),
				CHILD_WeightedHeight, 0,
				ADD_LABELLED(app->genreGad, "Genre"),
				CHILD_WeightedHeight, 0,

				/* Compact rating row: read-out, five small stars, track no. */
				LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					ADD_LABELLED(app->ratingGad, "Rating"),
					CHILD_MaxWidth, 44,
					CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[0], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[1], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[2], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[3], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[4], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					ADD_LABELLED(app->trackGad, "Track"),
					CHILD_MaxWidth, 60,
					TAG_DONE),
				CHILD_WeightedHeight, 0,
				TAG_DONE),

			LAYOUT_AddChild, (ULONG)app->artGad,
			CHILD_MinWidth, 72,
			CHILD_MinHeight, 72,
			CHILD_MaxWidth, 72,
			CHILD_WeightedWidth, 0,
			CHILD_WeightedHeight, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* All decoder/playback controls grouped in one tidy block. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, (ULONG)"Decoder",
			LAYOUT_SpaceInner, TRUE,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				ADD_LABELLED(app->rateGad, "Rate"),
				ADD_LABELLED(app->qualityGad, "Quality"),
				ADD_LABELLED(app->channelGad, "Mono/Stereo"),
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
				ADD_LABELLED(app->bufferGad, "Buffer"),
				ADD_LABELLED(app->volumeGad, "Volume"),
				TAG_DONE),
			CHILD_WeightedHeight, 0,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				LAYOUT_AddChild, (ULONG)app->fastMemGad,
				LAYOUT_AddChild, (ULONG)app->fastLowGad,
				TAG_DONE),
			CHILD_WeightedHeight, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* File info readout, with the track time pinned to the right. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->fileInfoGad, "File info"),
			ADD_LABELLED(app->timeGad, "Time"),
			/* Wide enough for the full "00:00 / 00:00" read-out; the previous
			 * 96px clipped the elapsed half off the left. */
			CHILD_MinWidth, 120,
			CHILD_MaxWidth, 132,
			CHILD_WeightedWidth, 0,
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
			IDCMP_IDCMPUPDATE | IDCMP_MENUPICK | IDCMP_NEWSIZE,
		WA_NewLookMenus, TRUE,
		WA_Width, 440,
		WA_Height, 290,
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
	if (app->bufferGad)
		SetGadgetAttrs((struct Gadget *)app->bufferGad, app->win, NULL,
			GA_Disabled, app->decodeThenPlay,
			TAG_DONE);
	/* A visual-info handle is needed both to lay the menu strip out (without
	 * LayoutMenus the items have no size, so the drop-downs never render) and
	 * to draw the recessed artwork bevel. */
	app->visualInfo = GetVisualInfoA(app->win->WScreen, NULL);
	app->menuStrip = CreateMenus(kMenus, TAG_DONE);
	if (app->menuStrip && app->visualInfo &&
		LayoutMenus(app->menuStrip, app->visualInfo, TAG_DONE)) {
		SyncMenuChecks(app);
		SetMenuStrip(app->win, app->menuStrip);
	} else {
		fprintf(stderr, "minimp3r: could not create menus.\n");
		if (app->menuStrip) {
			FreeMenus(app->menuStrip);
			app->menuStrip = NULL;
		}
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
	if (app->visualInfo) {
		FreeVisualInfo(app->visualInfo);
		app->visualInfo = NULL;
	}
	if (app->winObj) {
		DisposeObject(app->winObj);	/* disposes the whole gadget tree too */
		app->winObj = NULL;
		app->win = NULL;
	}
}


typedef struct MrMp3Info {
	char title[64], artist[64], album[64], track[16], genre[32];
	int bitrateKbps, sampleRate, channels, channelMode, modeExtension, durationSecs, rating;
	unsigned long fileSize;
	unsigned char *artData;
	unsigned long artBytes;
	int artIsPng;
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

static long Id3BigEndian32(const unsigned char *p)
{
	return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | (long)p[3];
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

static int ContainsTextNoCase(const char *s, const char *needle)
{
	int i, j;
	if (!s || !needle || !needle[0]) return 0;
	for (i = 0; s[i]; i++) {
		for (j = 0; needle[j]; j++) {
			char a = s[i + j], b = needle[j];
			if (!a) return 0;
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) break;
		}
		if (!needle[j]) return 1;
	}
	return 0;
}

static unsigned long ApicImageOffset(const unsigned char *p, unsigned long n)
{
	unsigned long i = 1;
	if (!p || n < 4) return n;
	while (i < n && p[i]) i++; /* MIME */
	if (i < n) i++;
	if (i < n) i++;          /* picture type */
	while (i < n && p[i]) i++; /* description (ISO-8859-1/UTF-8 common case) */
	if (i < n) i++;
	return i;
}

static unsigned long PicImageOffset(const unsigned char *p, unsigned long n)
{
	unsigned long i = 5;
	if (!p || n < 6) return n;
	while (i < n && p[i]) i++;
	if (i < n) i++;
	return i;
}

static void DetectPictureMime(const unsigned char *payload,
	unsigned long payloadBytes, int version, int *isJpeg, int *isPng)
{
	char mime[40];
	unsigned long i;
	*isJpeg = 0; *isPng = 0;
	if (!payload || payloadBytes < 4) return;
	memset(mime, 0, sizeof(mime));
	if (version == 2) {
		for (i = 0; i < 3 && i + 1 < payloadBytes; i++) mime[i] = (char)payload[i + 1];
	} else {
		for (i = 1; i < payloadBytes && i < sizeof(mime); i++) {
			if (!payload[i]) break;
			mime[i - 1] = (char)payload[i];
		}
	}
	if (ContainsTextNoCase(mime, "jpeg") || ContainsTextNoCase(mime, "jpg")) *isJpeg = 1;
	else if (ContainsTextNoCase(mime, "png")) *isPng = 1;
}

static void FreeMp3Info(MrMp3Info *info)
{
	if (info && info->artData) {
		FreeMem(info->artData, info->artBytes);
		info->artData = NULL;
		info->artBytes = 0;
	}
}

static void ReadId3v2(FILE *f, MrMp3Info *info)
{
	unsigned char hdr[10], fh[10]; long end; int version;
	if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3)) {
		/* No ID3v2 tag: leave the file rewound so the MPEG scan starts at
		 * the top of the file. */
		fseek(f, 0, SEEK_SET);
		return;
	}
	version = hdr[3];
	end = 10 + Id3Synchsafe(hdr + 6);
	while (ftell(f) + 10 <= end && fread(fh, 1, 10, f) == 10) {
		long sz = version == 4 ? Id3Synchsafe(fh + 4) : Id3BigEndian32(fh + 4); unsigned char *payload;
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
		else if (!info->artData && (!memcmp(fh, "APIC", 4)) && sz > 4 && sz <= 512L * 1024L) {
			unsigned long off, bytes; int isJpeg, isPng;
			DetectPictureMime(payload, (unsigned long)sz, version, &isJpeg, &isPng);
			off = ApicImageOffset(payload, (unsigned long)sz);
			bytes = (unsigned long)sz - off;
			if (off < (unsigned long)sz && bytes > 4) {
				info->artData = (unsigned char *)AllocMem(bytes, MEMF_ANY);
				if (info->artData) {
					memcpy(info->artData, payload + off, bytes);
					info->artBytes = bytes;
					info->artIsPng = isPng || (!isJpeg && !isPng);
				}
			}
		}
		free(payload);
	}
	/* Position the file pointer just past the ID3v2 tag so the MPEG frame
	 * scan that follows does not mistake bytes inside the tag (e.g. an
	 * embedded JPEG's FFE0/FFD8 markers) for an MPEG frame sync. */
	fseek(f, end, SEEK_SET);
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

/* Strict MPEG-1/2 Layer III frame-sync test, matching the GadTools frontend.
 * The loose "(h[1] & 0xe0) == 0xe0" test used previously matched JPEG APPn
 * markers (0xFFEn) and Layer I/II frames, so embedded cover art or a stray
 * byte run produced bogus bitrate/sample-rate readouts. */
static int IsMpegSyncHeaderMr(const unsigned char *h)
{
	return h[0] == 0xff && (h[1] == 0xfb || h[1] == 0xfa ||
		h[1] == 0xf3 || h[1] == 0xf2 || h[1] == 0xe3 || h[1] == 0xe2);
}

static void ReadMpegHeader(FILE *f, MrMp3Info *info, long *firstFrame)
{
	static const int br[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
	static const int sr[4] = {44100,48000,32000,0};
	unsigned char h[4];
	int b;
	if (firstFrame) *firstFrame = -1;
	if (!f || !info) return;
	/* Scan forward from the current position (already advanced past any
	 * ID3v2 tag by ReadId3v2) using a 4-byte sliding window. */
	h[0] = h[1] = h[2] = h[3] = 0;
	while ((b = fgetc(f)) != EOF) {
		h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = (unsigned char)b;
		if (IsMpegSyncHeaderMr(h)) {
			long pos = ftell(f);
			int bi = (h[2] >> 4) & 15, si = (h[2] >> 2) & 3;
			if (firstFrame && pos >= 4) *firstFrame = pos - 4;
			info->bitrateKbps = br[bi];
			info->sampleRate = sr[si];
			info->channelMode = (h[3] >> 6) & 3;
			info->modeExtension = (h[3] >> 4) & 3;
			info->channels = info->channelMode == 3 ? 1 : 2;
			return;
		}
	}
}

static void TryFolderArt(const char *inputName, MrMp3Info *info)
{
	static const char *names[] = { "folder.jpg", "cover.jpg", "album.jpg", "front.jpg", NULL };
	char dirPath[MR_MAX_PATH], artPath[MR_MAX_PATH];
	int i;
	if (!inputName || !info || info->artData) return;
	SafeCopy(dirPath, sizeof(dirPath), inputName);
	{
		char *q = dirPath + strlen(dirPath);
		while (q > dirPath && *q != '/' && *q != ':') q--;
		if (*q == '/' || *q == ':') *(q + 1) = '\0';
		else dirPath[0] = '\0';
	}
	for (i = 0; names[i] && !info->artData; i++) {
		FILE *af;
		SafeCopy(artPath, sizeof(artPath), dirPath);
		strncat(artPath, names[i], sizeof(artPath) - strlen(artPath) - 1);
		af = fopen(artPath, "rb");
		if (af) {
			long sz;
			fseek(af, 0, SEEK_END); sz = ftell(af); fseek(af, 0, SEEK_SET);
			if (sz > 4 && sz <= 512L * 1024L) {
				info->artData = (unsigned char *)AllocMem((unsigned long)sz, MEMF_ANY);
				if (info->artData) {
					if (fread(info->artData, 1, (size_t)sz, af) == (size_t)sz) {
						info->artBytes = (unsigned long)sz;
						info->artIsPng = 0;
					} else {
						FreeMem(info->artData, (unsigned long)sz);
						info->artData = NULL;
					}
				}
			}
			fclose(af);
		}
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
	TryFolderArt(path, info);
}

typedef struct MrPjpegSrc {
	const unsigned char *data;
	unsigned long pos;
	unsigned long size;
} MrPjpegSrc;

static unsigned char MrPjpegCb(unsigned char *buf, unsigned char bufSize,
	unsigned char *bytesRead, void *ud)
{
	MrPjpegSrc *src = (MrPjpegSrc *)ud;
	unsigned long left = src->size - src->pos;
	unsigned char n = (unsigned char)(left < (unsigned long)bufSize ? left : (unsigned long)bufSize);
	if (n) {
		memcpy(buf, src->data + src->pos, n);
		src->pos += n;
	}
	*bytesRead = n;
	return 0;
}

static int MrJpegGreySample(const pjpeg_image_info_t *info, int off)
{
	unsigned long r, g, b;
	if (info->m_comps == 1) return info->m_pMCUBufR[off];
	r = info->m_pMCUBufR[off]; g = info->m_pMCUBufG[off]; b = info->m_pMCUBufB[off];
	return (int)((77UL * r + 150UL * g + 29UL * b + 128UL) >> 8);
}

static int MrJpegRgbSample(const pjpeg_image_info_t *info, int off,
	unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (info->m_comps == 1) {
		*r = *g = *b = info->m_pMCUBufR[off];
		return (int)*r;
	}
	*r = info->m_pMCUBufR[off];
	*g = info->m_pMCUBufG[off];
	*b = info->m_pMCUBufB[off];
	return MrJpegGreySample(info, off);
}

static int MrMcuSampleOffset(const pjpeg_image_info_t *info, int x, int y)
{
	int blockX = x / 8, blockY = y / 8, blocksPerRow = info->m_MCUWidth / 8;
	return (blockY * blocksPerRow + blockX) * 64 + (y & 7) * 8 + (x & 7);
}

static int DecodeJpegToGrey(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH, int isPng)
{
	pjpeg_image_info_t info;
	MrPjpegSrc src;
	unsigned char status;
	unsigned char xMap[MR_MAX_JPEG_DIM], yMap[MR_MAX_JPEG_DIM];
	static unsigned long greyAccum[MR_ART_W * MR_ART_H];
	static unsigned long rAccum[MR_ART_W * MR_ART_H];
	static unsigned long gAccum[MR_ART_W * MR_ART_H];
	static unsigned long bAccum[MR_ART_W * MR_ART_H];
	static unsigned short greyCount[MR_ART_W * MR_ART_H];
	int mcuIndex, i;
	if (isPng || !jpegData || jpegBytes <= 4 || !greyOut ||
		outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H)
		return -1;
	src.data = jpegData; src.pos = 0; src.size = jpegBytes;
	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(rAccum, 0, sizeof(rAccum));
	memset(gAccum, 0, sizeof(gAccum));
	memset(bAccum, 0, sizeof(bAccum));
	memset(greyCount, 0, sizeof(greyCount));
	status = pjpeg_decode_init(&info, MrPjpegCb, &src, 0);
	if (status != 0 || info.m_width <= 0 || info.m_height <= 0 ||
		info.m_width > MR_MAX_JPEG_DIM || info.m_height > MR_MAX_JPEG_DIM)
		return -1;
	for (i = 0; i < info.m_width; i++) xMap[i] = (unsigned char)((i * outW) / info.m_width);
	for (i = 0; i < info.m_height; i++) yMap[i] = (unsigned char)((i * outH) / info.m_height);
	for (mcuIndex = 0; mcuIndex < info.m_MCUSPerRow * info.m_MCUSPerCol; mcuIndex++) {
		int mcuX, mcuY, y;
		status = pjpeg_decode_mcu();
		if (status == PJPG_NO_MORE_BLOCKS) break;
		if (status != 0) return -1;
		mcuX = (mcuIndex % info.m_MCUSPerRow) * info.m_MCUWidth;
		mcuY = (mcuIndex / info.m_MCUSPerRow) * info.m_MCUHeight;
		for (y = 0; y < info.m_MCUHeight; y++) {
			int srcY = mcuY + y, dstY, x;
			if (srcY >= info.m_height) continue;
			dstY = yMap[srcY];
			for (x = 0; x < info.m_MCUWidth; x++) {
				int srcX = mcuX + x, dst;
				unsigned char r, g, b;
				if (srcX >= info.m_width) continue;
				dst = dstY * outW + xMap[srcX];
				greyAccum[dst] += (unsigned long)MrJpegRgbSample(&info, MrMcuSampleOffset(&info, x, y), &r, &g, &b);
				rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
				if (greyCount[dst] != 0xffff) greyCount[dst]++;
			}
		}
	}
	for (i = 0; i < outW * outH; i++)
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			greyOut[i] = (unsigned char)((greyAccum[i] + (c / 2)) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + (c / 2)) / c);
			}
		}
	return 0;
}

static void ReleaseArtColorPens(MrApp *app)
{
	if (app && app->artPensBuilt && app->win) {
		struct ColorMap *cm = app->win->WScreen->ViewPort.ColorMap;
		int i;
		if (cm)
			for (i = 0; i < app->artPenCacheUsed; i++)
				if (app->artPenCache[i].pen >= 0)
					ReleasePen(cm, app->artPenCache[i].pen);
	}
	if (app) {
		app->artPensBuilt = 0;
		app->artPenCacheUsed = 0;
	}
}

static void BuildArtColorPens(MrApp *app)
{
	struct ColorMap *cm;
	int i;
	static const unsigned char bayer[8][8] = {
		{0,32,8,40,2,34,10,42},{48,16,56,24,50,18,58,26},{12,44,4,36,14,46,6,38},{60,28,52,20,62,30,54,22},
		{3,35,11,43,1,33,9,41},{51,19,59,27,49,17,57,25},{15,47,7,39,13,45,5,37},{63,31,55,23,61,29,53,21}
	};
	ReleaseArtColorPens(app);
	if (!app || !app->win || !app->artValid)
		return;
	cm = app->win->WScreen->ViewPort.ColorMap;
	if (!cm)
		return;

	/* Pass 1: cache the first unique source colours we can obtain pens for.
	 * The artwork usually has many more than 64 colours, so later pixels must
	 * be matched to the nearest cached colour instead of falling back to grey. */
	for (i = 0; i < MR_ART_W * MR_ART_H; i++) {
		const unsigned char *p = &app->artRGBBuf[i * 3];
		unsigned long key = ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
		int j;
		for (j = 0; j < app->artPenCacheUsed; j++)
			if (app->artPenCache[j].key == key)
				break;
		if (j == app->artPenCacheUsed && app->artPenCacheUsed < MR_ART_COLOR_CACHE) {
			ULONG r32 = (ULONG)p[0] | ((ULONG)p[0] << 8) | ((ULONG)p[0] << 16) | ((ULONG)p[0] << 24);
			ULONG g32 = (ULONG)p[1] | ((ULONG)p[1] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[1] << 24);
			ULONG b32 = (ULONG)p[2] | ((ULONG)p[2] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[2] << 24);
			app->artPenCache[j].key = key;
			app->artPenCache[j].pen = ObtainBestPen(cm, r32, g32, b32,
				OBP_FailIfBad, (Tag)FALSE, TAG_DONE);
			app->artPenCacheUsed++;
		}
	}
	if (!app->artPenCacheUsed)
		return;

	/* Pass 2: match every pixel to the closest cached colour after a small
	 * Bayer dither offset, mirroring the GadTools colour-art path. */
	for (i = 0; i < MR_ART_W * MR_ART_H; i++) {
		const unsigned char *p = &app->artRGBBuf[i * 3];
		int dv = (int)bayer[(i / MR_ART_W) & 7][i & 7] - 32;
		int dscale = dv * 3 / 4;
		int rd = (int)p[0] + dscale;
		int gd = (int)p[1] + dscale;
		int bd = (int)p[2] + dscale;
		int bestj = 0;
		unsigned long bestDist = 0xffffffffUL;
		int j;
		if (rd < 0) rd = 0; else if (rd > 255) rd = 255;
		if (gd < 0) gd = 0; else if (gd > 255) gd = 255;
		if (bd < 0) bd = 0; else if (bd > 255) bd = 255;
		for (j = 0; j < app->artPenCacheUsed; j++) {
			unsigned long k = app->artPenCache[j].key;
			int dr = (int)((k >> 16) & 0xff) - rd;
			int dg = (int)((k >>  8) & 0xff) - gd;
			int db = (int)( k        & 0xff) - bd;
			unsigned long dist = (unsigned long)(dr * dr + dg * dg + db * db);
			if (dist < bestDist) { bestDist = dist; bestj = j; }
		}
		app->artPenIdx[i] = (unsigned char)bestj;
	}
	app->artPensBuilt = 1;
}

static void ArtworkCacheName(MrApp *app, char *dst, size_t dstSize)
{
	const char *base;
	char safe[80];
	int i, j;
	EnvName(dst, dstSize, "ArtCache");
	base = app->inputName + strlen(app->inputName);
	while (base > app->inputName && base[-1] != '/' && base[-1] != ':')
		base--;
	for (i = 0, j = 0; base[i] && j < (int)sizeof(safe) - 1; i++) {
		unsigned char c = (unsigned char)base[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			safe[j++] = (char)c;
		else if (c == '.')
			safe[j++] = '_';
	}
	safe[j] = '\0';
	if (!safe[0])
		SafeCopy(safe, sizeof(safe), "art");
	strncat(dst, "/", dstSize - strlen(dst) - 1);
	strncat(dst, safe, dstSize - strlen(dst) - 1);
	strncat(dst, ".grey64", dstSize - strlen(dst) - 1);
}

static int LoadArtworkCache(MrApp *app)
{
	char path[MR_MAX_PATH], hdr[8];
	FILE *f;
	if (!app->artCacheEnabled || app->artCacheBypass || !app->inputName[0])
		return 0;
	ArtworkCacheName(app, path, sizeof(path));
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
		memcmp(hdr, "M3AG64\0", 7) == 0 && (hdr[7] == 1 || hdr[7] == 2) &&
		fread(app->artGreyBuf, 1, MR_ART_W * MR_ART_H, f) == MR_ART_W * MR_ART_H) {
		int i;
		if (hdr[7] != 2 ||
			fread(app->artRGBBuf, 1, MR_ART_W * MR_ART_H * 3, f) != MR_ART_W * MR_ART_H * 3)
			for (i = 0; i < MR_ART_W * MR_ART_H; i++)
				app->artRGBBuf[i * 3] = app->artRGBBuf[i * 3 + 1] = app->artRGBBuf[i * 3 + 2] = app->artGreyBuf[i];
		fclose(f);
		app->artValid = 1;
		return 1;
	}
	fclose(f);
	return 0;
}

static void SaveArtworkCache(MrApp *app)
{
	char dir[64], path[MR_MAX_PATH];
	FILE *f;
	static const unsigned char hdr[8] = { 'M','3','A','G','6','4','\0', 2 };
	if (!app->artCacheEnabled || !app->inputName[0] || !app->artValid)
		return;
	EnvName(dir, sizeof(dir), "ArtCache");
	CreateDir((STRPTR)dir);
	ArtworkCacheName(app, path, sizeof(path));
	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite(app->artGreyBuf, 1, MR_ART_W * MR_ART_H, f);
	fwrite(app->artRGBBuf, 1, MR_ART_W * MR_ART_H * 3, f);
	fclose(f);
}

static void CleanArtworkCache(MrApp *app)
{
	char dir[64];
	BPTR lock;
	struct FileInfoBlock *fib;
	int removed = 0;
	EnvName(dir, sizeof(dir), "ArtCache");
	lock = Lock((STRPTR)dir, ACCESS_READ);
	if (!lock) { SetStatus(app, "Artwork cache is empty."); return; }
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(lock, fib)) {
		while (ExNext(lock, fib)) {
			char path[MR_MAX_PATH];
			int len = strlen(fib->fib_FileName);
			if (fib->fib_DirEntryType >= 0 || len < 7 || strcmp(fib->fib_FileName + len - 7, ".grey64") != 0)
				continue;
			SafeCopy(path, sizeof(path), dir);
			strncat(path, "/", sizeof(path) - strlen(path) - 1);
			strncat(path, fib->fib_FileName, sizeof(path) - strlen(path) - 1);
			if (DeleteFile((STRPTR)path))
				removed++;
		}
	}
	if (fib) FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	if (removed) {
		char msg[64];
		sprintf(msg, "Removed %d cached artwork file(s).", removed);
		SetStatus(app, msg);
	} else
		SetStatus(app, "No cached artwork files to remove.");
}

static void UpdateArtwork(MrApp *app, MrMp3Info *info)
{
	ReleaseArtColorPens(app);
	app->artValid = 0;
	if (app->artEnabled && LoadArtworkCache(app)) {
		/* cache hit */
	} else if (app->artEnabled && info && info->artData && info->artBytes > 4 &&
		DecodeJpegToGrey(info->artData, info->artBytes, app->artGreyBuf, app->artRGBBuf, MR_ART_W, MR_ART_H, info->artIsPng) == 0) {
		app->artValid = 1;
		SaveArtworkCache(app);
	}
	if (app->artColorEnabled && app->artValid)
		BuildArtColorPens(app);
	/* The panel (frame + thumbnail or "No art") is hand-drawn over the
	 * placeholder gadget rather than via the button's own text. */
	DrawArtPanel(app);
}

static void DrawArtPanel(MrApp *app)
{
	struct Gadget *gad;
	struct RastPort *rp;
	int availW, availH, w, h, ox, oy, x, y;
	static const unsigned char bayer[8][8] = {
		{0,32,8,40,2,34,10,42},{48,16,56,24,50,18,58,26},{12,44,4,36,14,46,6,38},{60,28,52,20,62,30,54,22},
		{3,35,11,43,1,33,9,41},{51,19,59,27,49,17,57,25},{15,47,7,39,13,45,5,37},{63,31,55,23,61,29,53,21}
	};
	if (!app || !app->win || !app->artGad) return;
	gad = (struct Gadget *)app->artGad;
	rp = app->win->RPort;
	/* The artwork placeholder button reserves the rectangle in the layout;
	 * its laid-out bounds tell us where to paint.  Bail out until the layout
	 * has actually sized it (early refreshes can fire with a zero rect). */
	if (gad->Width <= 8 || gad->Height <= 8) return;
	availW = gad->Width - 8; availH = gad->Height - 8;
	w = availW < MR_ART_W ? availW : MR_ART_W;
	h = availH < MR_ART_H ? availH : MR_ART_H;
	if (w <= 0 || h <= 0) return;
	ox = gad->LeftEdge + 4 + (availW - w) / 2;
	oy = gad->TopEdge + 4 + (availH - h) / 2;

	/* Recessed frame, exactly like the GadTools frontend, so the panel is
	 * always visible even before any artwork has decoded. */
	if (app->visualInfo)
		DrawBevelBox(rp, ox - 2, oy - 2, w + 4, h + 4,
			GT_VisualInfo, (ULONG)app->visualInfo,
			GTBB_Recessed, TRUE,
			TAG_DONE);

	if (app->artValid) {
		int pens[3] = {0, 1, 1};
		struct DrawInfo *dri = GetScreenDrawInfo(app->win->WScreen);
		if (dri) {
			pens[0] = dri->dri_Pens[SHADOWPEN];
			pens[1] = dri->dri_Pens[BACKGROUNDPEN];
			pens[2] = dri->dri_Pens[SHINEPEN];
			FreeScreenDrawInfo(app->win->WScreen, dri);
		}
		for (y = 0; y < h; y++) {
			int yy = (y * MR_ART_H) / h;
			for (x = 0; x < w; x++) {
				int xx = (x * MR_ART_W) / w;
				int idx = yy * MR_ART_W + xx;
				if (app->artColorEnabled && app->artPensBuilt &&
					app->artPenIdx[idx] < app->artPenCacheUsed &&
					app->artPenCache[app->artPenIdx[idx]].pen >= 0) {
					SetAPen(rp, (UWORD)app->artPenCache[app->artPenIdx[idx]].pen);
				} else {
					int g = app->artGreyBuf[idx] + (((int)bayer[yy & 7][xx & 7] - 32) * 3 / 4);
					SetAPen(rp, (UWORD)pens[g >= 171 ? 2 : (g >= 85 ? 1 : 0)]);
				}
				WritePixel(rp, ox + x, oy + y);
			}
		}
	} else {
		/* Paint our own "No art" panel rather than relying on the button's
		 * text, which the layout can repaint over (leaving it blank). */
		SetAPen(rp, 0);
		RectFill(rp, ox, oy, ox + w - 1, oy + h - 1);
		if (w >= 48 && h >= 8) {
			SetAPen(rp, 1);
			Move(rp, ox + (w - 48) / 2, oy + h / 2 + 2);
			Text(rp, "No art", 6);
		}
	}
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
	if (info->channelMode == 1) return (info->modeExtension & 0x02) ? "M/S" : "joint-stereo";
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
	/* Same one-line summary the GadTools frontend shows; the track length is
	 * presented separately in the Time field rather than crammed in here. */
	fileInfo[0] = '\0';
	if (info.bitrateKbps > 0 || info.sampleRate > 0 || info.fileSize > 0)
		sprintf(fileInfo, "%d kbps, %s, %d Hz, %lu KB",
			info.bitrateKbps, ch, info.sampleRate, kb);
	else
		SafeCopy(fileInfo, sizeof(fileInfo), "-");
	if (app->titleGad && app->win) SetGadgetAttrs((struct Gadget *)app->titleGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.title[0] ? info.title : "-"), TAG_DONE);
	if (app->artistGad && app->win) SetGadgetAttrs((struct Gadget *)app->artistGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.artist[0] ? info.artist : "-"), TAG_DONE);
	if (app->albumGad && app->win) SetGadgetAttrs((struct Gadget *)app->albumGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.album[0] ? info.album : "-"), TAG_DONE);
	if (app->trackGad && app->win) SetGadgetAttrs((struct Gadget *)app->trackGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.track[0] ? info.track : "-"), TAG_DONE);
	if (app->genreGad && app->win) SetGadgetAttrs((struct Gadget *)app->genreGad, app->win, NULL, STRINGA_TextVal, (ULONG)(info.genre[0] ? info.genre : "-"), TAG_DONE);
	if (app->fileInfoGad && app->win) SetGadgetAttrs((struct Gadget *)app->fileInfoGad, app->win, NULL, STRINGA_TextVal, (ULONG)fileInfo, TAG_DONE);
	UpdateRatingDisplay(app); UpdateTimeDisplay(app); SetGauge(app, 0); UpdateArtwork(app, &info); SetStatus(app, app->artValid ? "File ready." : "File ready (No art).");
	FreeMp3Info(&info);
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
ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|wav|aif|aiff)",
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

app->playlistCount = 0;
app->playlistCurrent = -1;
RefreshPlaylistView(app);
UpdateNextButtonState(app);
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
	if (app->playbackDonePending || gPlayer.stopRequested) {
		SetStatus(app, "Previous playback is still exiting.");
		UpdateNextButtonState(app);
		return;
	}
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
	app->playlistSelected = -1;
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
		app->playlistSelected = 0;
		SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[0]);
		UpdateFileGadget(app);
		RefreshFileInfoAndTags(app);
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		SetStatus(app, app->artValid ? "Playlist loaded." : "Playlist loaded (No art).");
	} else {
		app->playlistSelected = -1;
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		SetStatus(app, "Playlist had no playable entries.");
	}
}

enum {
	PL_GID_LIST = 200,
	PL_GID_ADD,
	PL_GID_REMOVE,
	PL_GID_CLEAR,
	PL_GID_PLAY,
	PL_GID_LOAD_M3U,
	PL_GID_SAVE_M3U,
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
	if (app->plWin && app->plGadList) {
		int sel = app->playlistSelected >= 0 ? app->playlistSelected : app->playlistCurrent;
		GT_SetGadgetAttrs(app->plGadList, app->plWin, NULL,
			GTLV_Labels, (ULONG)&app->plList,
			GTLV_Selected, sel >= 0 ? (ULONG)sel : (ULONG)~0,
			TAG_DONE);
	}
}

/* Append one or more files (ASL multi-select) to the playlist. */
static void PlaylistAddFiles(MrApp *app)
{
	struct FileRequester *fr;
	char path[MR_MAX_PATH];
	int added = 0;

	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Add to playlist",
		ASLFR_DoMultiSelect, TRUE,
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|wav|aif|aiff)",
		ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
		TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate file requester.");
		return;
	}
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)(app->plWin ? app->plWin : app->win),
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(app->lastDrawer, sizeof(app->lastDrawer), (const char *)fr->fr_Drawer);
		if (fr->fr_NumArgs > 0 && fr->fr_ArgList) {
			int i;
			for (i = 0; i < (int)fr->fr_NumArgs && app->playlistCount < MR_PLAYLIST_MAX; i++) {
				path[0] = '\0';
				if (fr->fr_Drawer && fr->fr_Drawer[0]) {
					SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
					AddPart((STRPTR)path, fr->fr_ArgList[i].wa_Name, sizeof(path));
				} else {
					SafeCopy(path, sizeof(path), (const char *)fr->fr_ArgList[i].wa_Name);
				}
				if (!path[0]) continue;
				SafeCopy(app->playlist[app->playlistCount++], MR_MAX_PATH, path);
				added++;
			}
		} else if (fr->fr_File && fr->fr_File[0] && app->playlistCount < MR_PLAYLIST_MAX) {
			path[0] = '\0';
			if (fr->fr_Drawer && fr->fr_Drawer[0])
				SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
			if (AddPart((STRPTR)path, fr->fr_File, sizeof(path))) {
				SafeCopy(app->playlist[app->playlistCount++], MR_MAX_PATH, path);
				added++;
			}
		}
	}
	FreeAslRequest(fr);
	if (added > 0) {
		char msg[48];
		if (app->playlistSelected < 0)
			app->playlistSelected = app->playlistCount - added;
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		sprintf(msg, "Added %d track%s to playlist.", added, added == 1 ? "" : "s");
		SetStatus(app, msg);
	} else {
		SetStatus(app, app->playlistCount >= MR_PLAYLIST_MAX ?
			"Playlist is full." : "No tracks added.");
	}
}

/* Remove the currently selected entry from the playlist. */
static void PlaylistRemoveSelected(MrApp *app)
{
	int sel = app->playlistSelected;
	int i;
	if (sel < 0 || sel >= app->playlistCount) {
		SetStatus(app, "Select a track to remove first.");
		return;
	}
	for (i = sel; i < app->playlistCount - 1; i++)
		SafeCopy(app->playlist[i], MR_MAX_PATH, app->playlist[i + 1]);
	app->playlistCount--;
	if (app->playlistCurrent > sel) app->playlistCurrent--;
	else if (app->playlistCurrent == sel) app->playlistCurrent = -1;
	if (app->playlistSelected >= app->playlistCount)
		app->playlistSelected = app->playlistCount - 1;
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
	SetStatus(app, "Track removed from playlist.");
}

static void PlaylistClearAll(MrApp *app)
{
	app->playlistCount = 0;
	app->playlistCurrent = -1;
	app->playlistSelected = -1;
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
	SetStatus(app, "Playlist cleared.");
}

/* Write the current playlist out as a simple #EXTM3U file. */
static void PlaylistSaveM3U(MrApp *app)
{
	struct FileRequester *fr;
	char m3uPath[MR_MAX_PATH];
	char line[MR_MAX_PATH + 2];
	BPTR fh;
	int i, len;

	if (app->playlistCount <= 0) {
		SetStatus(app, "Playlist is empty - nothing to save.");
		return;
	}
	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Save M3U playlist",
		ASLFR_DoSaveMode, TRUE,
		ASLFR_InitialFile, (ULONG)"playlist.m3u",
		ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
		TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate file requester.");
		return;
	}
	m3uPath[0] = '\0';
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)(app->plWin ? app->plWin : app->win),
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(m3uPath, sizeof(m3uPath), (const char *)fr->fr_Drawer);
		if (fr->fr_File && fr->fr_File[0])
			AddPart((STRPTR)m3uPath, fr->fr_File, sizeof(m3uPath));
	}
	FreeAslRequest(fr);
	if (!m3uPath[0])
		return;
	fh = Open((STRPTR)m3uPath, MODE_NEWFILE);
	if (!fh) {
		SetStatus(app, "Cannot create M3U file.");
		return;
	}
	Write(fh, (APTR)"#EXTM3U\n", 8);
	for (i = 0; i < app->playlistCount; i++) {
		SafeCopy(line, sizeof(line) - 1, app->playlist[i]);
		len = (int)strlen(line);
		line[len++] = '\n';
		if (Write(fh, (APTR)line, len) != len) {
			Close(fh);
			SetStatus(app, "Error writing M3U file.");
			return;
		}
	}
	Close(fh);
	SetStatus(app, "Playlist saved as M3U.");
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
	{
	int sel = app->playlistSelected >= 0 ? app->playlistSelected : app->playlistCurrent;
	ng.ng_LeftEdge = 8; ng.ng_TopEdge = 20; ng.ng_Width = 344; ng.ng_Height = 120;
	ng.ng_GadgetID = PL_GID_LIST; ng.ng_Flags = 0; ng.ng_VisualInfo = app->plVisualInfo;
	app->plGadList = gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
		GTLV_Labels, (ULONG)&app->plList,
		GTLV_Selected, sel >= 0 ? (ULONG)sel : (ULONG)~0,
		GA_RelVerify, TRUE, TAG_DONE);
	}
	if (!gad) goto fail;
	/* Row 1: Add | Remove | Clear | Play */
	ng.ng_TopEdge = 148; ng.ng_Width = 84; ng.ng_Height = 18; ng.ng_Flags = PLACETEXT_IN;
	ng.ng_LeftEdge = 8; ng.ng_GadgetText = (UBYTE *)"Add"; ng.ng_GadgetID = PL_GID_ADD;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 96; ng.ng_GadgetText = (UBYTE *)"Remove"; ng.ng_GadgetID = PL_GID_REMOVE;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 184; ng.ng_GadgetText = (UBYTE *)"Clear"; ng.ng_GadgetID = PL_GID_CLEAR;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 272; ng.ng_GadgetText = (UBYTE *)"Play"; ng.ng_GadgetID = PL_GID_PLAY;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	/* Row 2: Load M3U | Save M3U | Close */
	ng.ng_TopEdge = 170; ng.ng_Width = 114;
	ng.ng_LeftEdge = 8; ng.ng_GadgetText = (UBYTE *)"Load M3U"; ng.ng_GadgetID = PL_GID_LOAD_M3U;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 126; ng.ng_GadgetText = (UBYTE *)"Save M3U"; ng.ng_GadgetID = PL_GID_SAVE_M3U;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 244; ng.ng_GadgetText = (UBYTE *)"Close"; ng.ng_GadgetID = PL_GID_CLOSE;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = app->win->LeftEdge + 20; nw.TopEdge = app->win->TopEdge + 20;
	nw.Width = 368; nw.Height = 200;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH;
	nw.Title = (UBYTE *)"MiniAMP3 Playlist";
	nw.MinWidth = nw.MaxWidth = 368; nw.MinHeight = nw.MaxHeight = 200;
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
	app->playlistSelected = index;
	SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[index]);
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
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
		if (cls == IDCMP_REFRESHWINDOW) {
			GT_BeginRefresh(app->plWin);
			GT_EndRefresh(app->plWin, TRUE);
			continue;
		}
		if (cls == IDCMP_GADGETUP) {
			switch (gid) {
			case PL_GID_LIST:
				/* Single click just selects; the Play button (or a second
				 * click via the main window) starts playback. */
				app->playlistSelected = (int)code;
				break;
			case PL_GID_ADD:
				PlaylistAddFiles(app);
				break;
			case PL_GID_REMOVE:
				PlaylistRemoveSelected(app);
				break;
			case PL_GID_CLEAR:
				PlaylistClearAll(app);
				break;
			case PL_GID_PLAY: {
				int idx = app->playlistSelected >= 0 ?
					app->playlistSelected : app->playlistCurrent;
				if (idx >= 0 && idx < app->playlistCount)
					PlaylistLoadCurrent(app, idx, 1);
				else
					SetStatus(app, "Select a track to play first.");
				break;
			}
			case PL_GID_LOAD_M3U:
				BrowseForPlaylist(app);
				break;
			case PL_GID_SAVE_M3U:
				PlaylistSaveM3U(app);
				break;
			case PL_GID_CLOSE:
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

static void SetDecodeThenPlay(MrApp *app, int enabled)
{
	app->decodeThenPlay = enabled ? 1 : 0;
	if (app->bufferGad && app->win)
		SetGadgetAttrs((struct Gadget *)app->bufferGad, app->win, NULL,
			GA_Disabled, app->decodeThenPlay,
			TAG_DONE);
	SetStatus(app, app->decodeThenPlay ?
		"Decode-then-play enabled; Buffer slider disabled." :
		"Streaming playback mode enabled.");
	SaveSettings(app);
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
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_DTP)
				SetDecodeThenPlay(app, !app->decodeThenPlay);
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_BENCH) {
				app->bench = !app->bench;
				SetStatus(app, app->bench ? "Bench mode enabled." : "Bench mode disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTWORK) {
				app->artEnabled = !app->artEnabled;
				app->artCacheBypass = 0;
				RefreshFileInfoAndTags(app);
				SetStatus(app, app->artEnabled ? (app->artValid ? "Artwork enabled." : "No artwork.") : "Artwork disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCACHE) {
				app->artCacheEnabled = !app->artCacheEnabled;
				SetStatus(app, app->artCacheEnabled ? "Artwork cache enabled." : "Artwork cache disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCOLOR) {
				app->artColorEnabled = !app->artColorEnabled;
				if (app->artColorEnabled && app->artValid) BuildArtColorPens(app);
				else ReleaseArtColorPens(app);
				DrawArtPanel(app);
				SetStatus(app, app->artColorEnabled ? "Colour artwork enabled." : "Colour artwork disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_PROGRESS) {
				app->progressEnabled = !app->progressEnabled;
				SetGauge(app, app->progressEnabled && app->totalSecs > 0 ? (app->elapsedSecs * 100) / app->totalSecs : 0);
				SetStatus(app, app->progressEnabled ? "Progress bar enabled." : "Progress bar disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTREFRESH) {
				DrawArtPanel(app);
				SetStatus(app, app->artValid ? "Artwork refreshed." : "No artwork.");
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTRELOAD) {
				app->artCacheBypass = 1;
				RefreshFileInfoAndTags(app);
				app->artCacheBypass = 0;
				SetStatus(app, app->artValid ? "Artwork refreshed." : "No artwork.");
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCLEAN) {
				CleanArtworkCache(app);
			}
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
		app->cd32Ultrafast = ((int)v == 3);
		app->ultrafast = ((int)v == 2);
		app->superfastLowrate = ((int)v == 1 || app->cd32Ultrafast);
		if (app->cd32Ultrafast) {
			app->fastLowrate = 1;
			app->mono = 1;
			app->rateIndex = 3;
		} else if (app->ultrafast)
			app->fastLowrate = 0;
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
	app.ultrafast = 0;
	app.cd32Ultrafast = 0;
	app.volumePercent = 100;
	app.bufferSeconds = 10;
	app.fakeStereoDelayIndex = 0;
	app.artEnabled = 1;
	app.artCacheEnabled = 1;
	app.artColorEnabled = 0;
	app.progressEnabled = 0;
	app.playlistCurrent = -1;
	app.playlistSelected = -1;
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

	/* Paint the (empty) artwork panel once now that the layout has sized the
	 * placeholder, so the recessed box is shown before the first file loads. */
	DrawArtPanel(&app);

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
			/* window.class repaints its gadgets on refresh/resize but it does
			 * not know about our hand-drawn artwork, so re-stamp the thumbnail
			 * after every batch of window input. */
			DrawArtPanel(&app);
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
