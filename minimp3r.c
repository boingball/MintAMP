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
#include "radio_debug.h"

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
#include "lodepng.h"
#include "radio_stream.h"
#include "radio_browser_controller.h"
#include "radio_browser_http.h"

#define MR_ENV_PREFIX "ENVARC:MiniAMP3"
#define MR_SETTINGS_VERSION 1
#define MR_RADIO_FAV_MAX 20
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

/* Optional Radio Browser station-favicon artwork.  Disable by building with
 * -DENABLE_RADIO_ARTWORK=0; it never touches stream playback either way. */
#ifndef ENABLE_RADIO_ARTWORK
#define ENABLE_RADIO_ARTWORK 1
#endif
/* PNG favicon support via lodepng (most Radio Browser favicons are PNG).
 * Disable by building with -DENABLE_PNG_ARTWORK=0 -- note this only skips
 * the decode calls; lodepng.c must also be dropped from the Makefile's
 * source list to actually shrink the binary. */
#ifndef ENABLE_PNG_ARTWORK
#define ENABLE_PNG_ARTWORK 1
#endif
#define MR_FAVICON_MAX_BYTES (256L * 1024L)

/* How often we poll the shared playback status block while a track plays.
 * Keep the heartbeat responsive, but throttle expensive text redraws below. */
#define MR_TICK_MICROS   250000UL
#define MR_METADATA_TICKS 4
#define MR_TIME_TICKS     2

#ifdef REACTION_POLL_DEBUG
#define MR_POLL_DBG(args) do { printf args; } while (0)
#else
#define MR_POLL_DBG(args) do { } while (0)
#endif

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
	GID_RADIO,
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

/* 8287 Hz removed: it failed often enough in practice to not be worth
 * offering, and 8820 Hz already covers the same "lowest available rate"
 * role.  kRates[MR_RATE_22050_INDEX] must stay "22050" -- several speed/
 * ultrafast/CD32 code paths below key off that specific rate. */
static const char * const kRates[] = {
	"8820", "11025", "22050", "28600"
};
#define MR_RATE_COUNT  ((int)(sizeof(kRates) / sizeof(kRates[0])))
#define MR_RATE_22050_INDEX 2

static const STRPTR kRateLabels[] = {
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

static const STRPTR kSpeedLabelsNo22050[] = {
	(STRPTR)"Normal",
	(STRPTR)"Superfast low-rate",
	(STRPTR)"Ultrafast",
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
#define ITEMNUM_RADIO     1
#define ITEMNUM_QUIT      2
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
	{ NM_ITEM, (STRPTR)"Internet Radio", 0, 0, 0, (APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_RADIO) },
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

typedef enum MrStreamState {
	MR_STREAM_IDLE = 0,
	MR_STREAM_STARTING,
	MR_STREAM_PLAYING,
	MR_STREAM_STOP_REQUESTED,
	MR_STREAM_STOPPING,
	MR_STREAM_EXITED,
	MR_STREAM_ERROR,
	MR_STREAM_STOP_TIMEOUT
} MrStreamState;

typedef struct MrPlayer {
	volatile int    stopRequested;
	int             argc;
	char          **argv;
	struct Process *process;
	struct Task    *task;
	unsigned long   sessionId;
	char            url[MR_MAX_PATH];
	char            codec[16];
	volatile const char *stage;
	volatile const char *startupStage;
	volatile const char *cleanupStage;
	volatile const char *lastIoState;
	volatile int    donePosted;
} MrPlayer;

static MrPlayer        gPlayer;
static MrPlayArgs      gArgs;
typedef struct MrDoneMessage {
	struct Message msg;
	unsigned long magic;
	unsigned long runId;
	int posted;
} MrDoneMessage;

static MrDoneMessage gDoneMsg;
static struct MsgPort *gDonePort;
static volatile unsigned long gRunCounter;
static volatile unsigned long gEntryRunId;
static volatile unsigned long gDoneRunId;
#define MR_APP_MAGIC 0x4d523047UL
#define MR_DONE_MAGIC 0x4d52444fUL
#define MR_WINDOW_TITLE "Amiga MP3 Player"

/* ------------------------------------------------------------------------- */
/* Application state                                                         */
/* ------------------------------------------------------------------------- */

typedef struct MrApp {
	unsigned long magic;
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
	Object         *radioGad;
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

	struct Window  *rbWin;
	struct Gadget  *rbGadgets;
	struct Gadget  *rbGadContext;
	struct Gadget  *rbGadList;
	struct List     rbList;
	struct Node     rbNodes[RB_CONTROLLER_MAX_STATIONS];
	char            rbNames[RB_CONTROLLER_MAX_STATIONS][96];
	int             rbVisibleToController[RB_CONTROLLER_MAX_STATIONS];
	int             rbVisibleCount;
	int             rbShowHttps;
	int             rbSchemeMode;
	int             rbCountryMode;
	int             rbShowingFavourites;
	int             rbFavouriteCount;
	int             rbSelectedFavourite;
	int             rbSearchInProgress;
	char            rbFavouriteNames[MR_RADIO_FAV_MAX][RB_MAX_NAME];
	char            rbFavouriteUrls[MR_RADIO_FAV_MAX][RB_MAX_URL];
	char            currentRadioStationName[RB_MAX_NAME];
	char            currentRadioFavicon[RB_MAX_FAVICON];
	APTR            rbVisualInfo;
	RadioBrowserController rbController;

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
	int   haveRadioHostAddr;
	unsigned long radioHostAddrBe;
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
	MrStreamState streamState;
	char  currentStreamUrl[MR_MAX_PATH];
	char  queuedStreamUrl[MR_MAX_PATH];
	int   lastCompletedWasHttps;
	char  currentStreamCodec[16];
	unsigned long activeChildCount;
	unsigned long activeStreamSessions;
	unsigned long activeStreamTasks;
	int   stoppedByUser;
	char  lastChildExitReason[32];
	char  lastChildError[128];
	char  lastRadioError[256];
	int   parentDoneHandled;
	int   lastChildEverPlayed;
	int   lastChildFirstData;
	int   lastPhaseShown;
	unsigned char artGreyBuf[MR_ART_W * MR_ART_H];
	int artValid;

	char  shownTitle[128];
	char  shownArtist[128];
	char  shownAlbum[128];
	char  fullAlbum[128];
	char  shownTrack[32];
	char  shownGenre[64];
	char  shownFileInfo[128];
	char  shownStatus[128];
	int   albumHover;
	int   albumScrollPos;
	unsigned long pollTick;
	unsigned long lastRadioMetaTick;
	unsigned long lastRadioStatusTick;
	unsigned long lastTimeTick;
	int   shownGaugeLevel;
	int   shownChannelDisabled;
	int   shownNextDisabled;
	int   lastRadioStatusShown;
	int   shuttingDown;
} MrApp;

static void UpdateTimeDisplay(MrApp *app);
static void RefreshFileInfoAndTags(MrApp *app);
static void SaveSettings(MrApp *app);
static void ApplyHardwareAudioFilter(MrApp *app);
static void UpdateChannelGadgetState(MrApp *app);
static void UpdateSpeedGadgetChoices(MrApp *app);
static void UpdateNextButtonState(MrApp *app);
static void DrawArtPanel(MrApp *app);
static void SaveSettings(MrApp *app);
static void RefreshPlaylistView(MrApp *app);
static void ClosePlaylistWindow(MrApp *app);
static void OpenPlaylistWindow(MrApp *app);
static void CloseRadioWindow(MrApp *app);
static void OpenRadioWindow(MrApp *app);
static void HandleRadioWindow(MrApp *app);
static void HandleDoneSignal(MrApp *app);
static void RadioSetStatus(MrApp *app, const char *text);
static void RadioDoProbeAndPlay(MrApp *app);
static void RadioSelectResult(MrApp *app, ULONG eventSelected);

static void SyncMenuChecks(MrApp *app);

static const char *MrStreamStateName(MrStreamState state)
{
	switch (state) {
	case MR_STREAM_IDLE: return "IDLE";
	case MR_STREAM_STARTING: return "STARTING";
	case MR_STREAM_PLAYING: return "PLAYING";
	case MR_STREAM_STOP_REQUESTED: return "STOP_REQUESTED";
	case MR_STREAM_STOPPING: return "STOPPING";
	case MR_STREAM_EXITED: return "EXITED";
	case MR_STREAM_ERROR: return "ERROR";
	case MR_STREAM_STOP_TIMEOUT: return "STOP_TIMEOUT";
	}
	return "UNKNOWN";
}

static void MrDebugSession(const char *event, const MrApp *app)
{
	RADIO_DBG(printf("radio-session: %s session=%lu state=%s childTask=%p childProc=%p url=\"%s\" codec=%s stop=%d done=%d active_child_count=%lu active_stream_sessions=%lu active_stream_tasks=%lu stage=\"%s\" startup=\"%s\" cleanup=\"%s\" io=\"%s\"\n",
		event ? event : "event",
		gPlayer.sessionId,
		app ? MrStreamStateName(app->streamState) : "(no-app)",
		gPlayer.task, gPlayer.process,
		gPlayer.url[0] ? gPlayer.url : (app ? app->inputName : ""),
		gPlayer.codec[0] ? gPlayer.codec : "unknown",
		gPlayer.stopRequested, gPlayer.donePosted,
		app ? app->activeChildCount : 0,
		app ? app->activeStreamSessions : 0,
		app ? app->activeStreamTasks : 0,
		gPlayer.stage ? (const char *)gPlayer.stage : "",
		gPlayer.startupStage ? (const char *)gPlayer.startupStage : "",
		gPlayer.cleanupStage ? (const char *)gPlayer.cleanupStage : "",
		gPlayer.lastIoState ? (const char *)gPlayer.lastIoState : "");)
}

static int PlaybackProcessStillExists(void);
static int StopPlaybackAndWait(MrApp *app, int ticks, const char *timeoutStatus);
static void HandleDoneSignal(MrApp *app);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */


#ifdef RADIO_DEBUG
static void AppCloseDebug(const char *stage, const MrApp *app)
{
	long active_stream_sessions = 0;
	long active_stream_tasks = 0;
	long open_socket_count = 0;
	long active_ssl_count = 0;
	long active_ssl_ctx_count = 0;
	void *socket_base = 0;
	void *amissl_base = 0;
	void *amissl_master_base = 0;
	Radio_GetNetworkStats(&active_stream_sessions, &active_stream_tasks,
		&open_socket_count, &active_ssl_count, &active_ssl_ctx_count);
	Radio_GetNetworkBases(&socket_base, &amissl_base, &amissl_master_base);
	printf("APP_CLOSE: %s streamState=%s playbackActive=%d playbackDonePending=%d activeChildCount=%lu gPlayer.process=%p gPlayer.task=%p gPlayer.stopRequested=%d active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld browser_probe_socket_counts=unavailable SocketBase=%p AmiSSLBase=%p AmiSSLMasterBase=%p\n",
		stage ? stage : "stage",
		app ? MrStreamStateName(app->streamState) : "(no-app)",
		app ? app->playbackActive : 0,
		app ? app->playbackDonePending : 0,
		app ? app->activeChildCount : 0,
		gPlayer.process, gPlayer.task, gPlayer.stopRequested,
		active_stream_sessions, active_stream_tasks, open_socket_count,
		active_ssl_count, active_ssl_ctx_count,
		socket_base, amissl_base, amissl_master_base
	);
}
#else
#define AppCloseDebug(stage, app) do { (void)(stage); (void)(app); } while (0)
#endif

static int AppHasActivePlaybackChild(const MrApp *app)
{
	return app && app->activeChildCount > 0 &&
		(app->playbackActive || app->playbackDonePending ||
		 gPlayer.process || gPlayer.task || PlaybackProcessStillExists());
}

static void AppCloseShutdown(MrApp *app)
{
	if (!app)
		return;
	app->shuttingDown = 1;
	AppCloseDebug("begin", app);
	AppCloseDebug("playback state", app);
	if (AppHasActivePlaybackChild(app)) {
		AppCloseDebug("stop active playback if needed", app);
		if (!StopPlaybackAndWait(app, 500, "Failed to stop previous stream")) {
			while (PlaybackProcessStillExists())
				Delay(5);
			HandleDoneSignal(app);
		}
	} else {
		AppCloseDebug("playback already idle, skip stop", app);
		AppCloseDebug("playback idle, no stop needed", app);
	}
	AppCloseDebug("dispose radio browser controller", app);
	AppCloseDebug("free favourites/search results", app);
	AppCloseDebug("dispose GUI objects", app);
}

static void SafeCopy(char *dst, size_t size, const char *src)
{
	if (!size)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, size - 1);
	dst[size - 1] = '\0';
}

static int MrVerifyAppMagic(MrApp *app, const char *where)
{
	if (!app || app->magic != MR_APP_MAGIC) {
		RADIO_DBG(printf("radio-guard: WARNING app magic corrupt before %s app=%p magic=%08lx expected=%08lx\n",
			where ? where : "operation", app, app ? app->magic : 0UL, (unsigned long)MR_APP_MAGIC);)
		return 0;
	}
	return 1;
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
		app->rateIndex = MR_RATE_22050_INDEX;
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
	{
		int i;
		char key[32];
		app->rbFavouriteCount = LoadEnvInt("RadioFavCount", app->rbFavouriteCount, 0, MR_RADIO_FAV_MAX);
		for (i = 0; i < MR_RADIO_FAV_MAX; i++) {
			sprintf(key, "RadioFavName%d", i);
			LoadEnvString(key, app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]));
			sprintf(key, "RadioFavUrl%d", i);
			LoadEnvString(key, app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]));
		}
	}
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
	{
		int i;
		char key[32];
		SaveEnvInt("RadioFavCount", ClampInt(app->rbFavouriteCount, 0, MR_RADIO_FAV_MAX));
		for (i = 0; i < MR_RADIO_FAV_MAX; i++) {
			sprintf(key, "RadioFavName%d", i);
			SaveEnvString(key, app->rbFavouriteNames[i]);
			sprintf(key, "RadioFavUrl%d", i);
			SaveEnvString(key, app->rbFavouriteUrls[i]);
		}
	}
}

static int SetReadonlyString(Object *gad, struct Window *win, char *cache, size_t cacheSize, const char *text)
{
	if (!text)
		text = "";
	if (cache && cacheSize > 0 && !strcmp(cache, text))
		return 0;
	if (cache && cacheSize > 0)
		SafeCopy(cache, cacheSize, text);
	if (gad && win) {
		SetGadgetAttrs((struct Gadget *)gad, win, NULL,
			STRINGA_TextVal, (ULONG)text,
			TAG_DONE);
		/* STRINGA_TextVal parks the cursor/view at the END of the new string, so
		 * a long value (e.g. the album/station name) renders with its left edge
		 * clipped off.  Reset the view to the start in a SEPARATE SetGadgetAttrs
		 * pass - doing it in the same tag list as STRINGA_TextVal gets overridden
		 * when the gadget re-derives its display offset from the cursor. */
		SetGadgetAttrs((struct Gadget *)gad, win, NULL,
			STRINGA_BufferPos, 0,
			STRINGA_DispPos, 0,
			TAG_DONE);
	}
	return 1;
}

static int SetStatusIfChanged(MrApp *app, const char *text)
{
	const char *safeText = text ? text : "";
	if (!app)
		return 0;
	SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), safeText);
	return SetReadonlyString(app->statusGad, app->win, app->shownStatus,
		sizeof(app->shownStatus), app->lastRadioError);
}

static void SetStatus(MrApp *app, const char *text)
{
	(void)SetStatusIfChanged(app, text);
}

static int PointInGadget(struct Gadget *gad, int x, int y)
{
	return gad && x >= gad->LeftEdge && y >= gad->TopEdge &&
		x < gad->LeftEdge + gad->Width && y < gad->TopEdge + gad->Height;
}

/* How many characters of the album/station name fit in the gadget at the
 * current font.  A ReAction read-only string gadget that overflows renders its
 * tail (clipping the left), so we truncate the text to fit and it then always
 * shows the start, left-aligned.  The full name is still available on hover via
 * the status line. */
static int AlbumVisibleChars(MrApp *app)
{
	int txw, px, chars;
	if (!app || !app->win || !app->albumGad)
		return 0;
	txw = app->win->RPort ? app->win->RPort->TxWidth : 8;
	if (txw <= 0)
		txw = 8;
	/* Gadget Width includes the recessed border; leave a couple of chars of
	 * slack so the last glyph never spills past the right edge. */
	px = ((struct Gadget *)app->albumGad)->Width - 8;
	if (px < txw)
		return 1;
	chars = px / txw - 1;
	return chars > 0 ? chars : 1;
}

/* Set the album gadget to a left-aligned, fit-to-width copy, keeping the full
 * string in app->fullAlbum for the hover status hint. */
static int SetAlbumDisplay(MrApp *app, const char *full)
{
	char shown[128];
	int fit;
	if (!app)
		return 0;
	if (!full)
		full = "";
	SafeCopy(app->fullAlbum, sizeof(app->fullAlbum), full);
	fit = AlbumVisibleChars(app);
	if (fit > (int)sizeof(shown) - 1)
		fit = (int)sizeof(shown) - 1;
	if ((int)strlen(full) > fit) {
		memcpy(shown, full, (size_t)fit);
		shown[fit] = 0;
	} else {
		SafeCopy(shown, sizeof(shown), full);
	}
	return SetReadonlyString(app->albumGad, app->win, app->shownAlbum,
		sizeof(app->shownAlbum), shown);
}

static void UpdateAlbumHover(MrApp *app)
{
	int over;
	if (!app || !app->win || !app->albumGad)
		return;
	over = PointInGadget((struct Gadget *)app->albumGad, app->win->MouseX, app->win->MouseY);
	if (over == app->albumHover)
		return;
	app->albumHover = over;
	/* On hover show the full album/station name in the status line (the gadget
	 * itself only has room for the left part). */
	if (over && app->fullAlbum[0] && strcmp(app->fullAlbum, "-")) {
		char buf[160];
		SafeCopy(buf, sizeof(buf), "Album: ");
		strncat(buf, app->fullAlbum, sizeof(buf) - strlen(buf) - 1);
		SetStatus(app, buf);
	}
}

/* The album text is truncated to fit, so there is nothing to scroll; kept as a
 * no-op so the main loop call site stays simple. */
static void ScrollAlbumHover(MrApp *app)
{
	(void)app;
}


static int MrIsRadioInput(const char *name)
{
	return name && (!strncmp(name, "http://", 7) ||
		!strncmp(name, "https://", 8));
}

static void MrCopyVolatileString(char *dst, unsigned long dstSize, volatile const char *src)
{
	unsigned long i;
	if (!dst || dstSize == 0) return;
	if (!src) { dst[0] = 0; return; }
	for (i = 0; i + 1 < dstSize && src[i]; i++) dst[i] = (char)src[i];
	dst[i] = 0;
}

static void MrSplitStreamTitle(const char *streamTitle, char *artist, unsigned long artistSize, char *title, unsigned long titleSize)
{
	const char *sep; char tmp[128];
	if (artist && artistSize) artist[0] = 0;
	if (title && titleSize) title[0] = 0;
	if (!streamTitle || !streamTitle[0]) return;
	sep = strstr(streamTitle, " - ");
	if (!sep) { SafeCopy(title, titleSize, streamTitle); return; }
	SafeCopy(tmp, sizeof(tmp), streamTitle);
	sep = strstr(tmp, " - ");
	if (sep) { ((char *)sep)[0] = 0; SafeCopy(artist, artistSize, tmp); SafeCopy(title, titleSize, sep + 3); }
}

static const char *MrRadioCodecName(const char *contentType)
{
	if (!contentType) return "";
	if (strstr(contentType, "aac") || strstr(contentType, "AAC") ||
		strstr(contentType, "aach") || strstr(contentType, "AACH"))
		return "AAC+";
	if (strstr(contentType, "mpeg") || strstr(contentType, "MPEG") ||
		strstr(contentType, "mp3") || strstr(contentType, "MP3"))
		return "MP3";
	return "";
}


static int MrRadioPlaybackHasStarted(void)
{
	return gGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
		gGuiPlaybackStatus.decodedFrames > 0;
}

static void MrFormatRadioStreamingStatus(MrApp *app, const char *station, char *status, unsigned long statusSize)
{
	const char *name = station;
	char streamUrl[128];

	if (!status || statusSize == 0)
		return;
	if (!name || !name[0]) {
		MrCopyVolatileString(streamUrl, sizeof(streamUrl), gGuiPlaybackStatus.radioStreamUrl);
		name = (app && app->currentRadioStationName[0]) ? app->currentRadioStationName :
			(streamUrl[0] ? streamUrl : (app && app->inputName[0] ? app->inputName : "Internet Radio"));
	}
	sprintf(status, "Streaming %.100s", name);
}

static int MrSetRadioMetadata(MrApp *app, int updateStatus)
{
	char streamTitle[128], station[128], genre[64], contentType[64], radioError[128], artist[64], title[64], fileInfo[128], status[128];
	const char *codec;
	int bitrate;
	int updates = 0;
	int radioStatus;

	MrCopyVolatileString(streamTitle, sizeof(streamTitle), gGuiPlaybackStatus.radioTitle);
	MrCopyVolatileString(station, sizeof(station), gGuiPlaybackStatus.radioStationName);
	MrCopyVolatileString(genre, sizeof(genre), gGuiPlaybackStatus.radioGenre);
	MrCopyVolatileString(contentType, sizeof(contentType), gGuiPlaybackStatus.radioContentType);
	MrCopyVolatileString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
	radioStatus = gGuiPlaybackStatus.radioStatus;

	MrSplitStreamTitle(streamTitle, artist, sizeof(artist), title, sizeof(title));
	codec = MrRadioCodecName(contentType);
	bitrate = gGuiPlaybackStatus.radioBitrateKbps;
	if (codec[0] && bitrate > 0)
		sprintf(fileInfo, "Internet Stream - %s %dkbps", codec, bitrate);
	else if (codec[0])
		sprintf(fileInfo, "Internet Stream - %s", codec);
	else if (bitrate > 0)
		sprintf(fileInfo, "Internet Stream - %dkbps", bitrate);
	else
		sprintf(fileInfo, "Internet Stream");
	updates += SetReadonlyString(app->titleGad, app->win, app->shownTitle, sizeof(app->shownTitle), title[0] ? title : "-");
	updates += SetReadonlyString(app->artistGad, app->win, app->shownArtist, sizeof(app->shownArtist), artist[0] ? artist : "-");
	updates += SetAlbumDisplay(app, station[0] ? station : "Internet Radio");
	updates += SetReadonlyString(app->trackGad, app->win, app->shownTrack, sizeof(app->shownTrack), "Live");
	updates += SetReadonlyString(app->genreGad, app->win, app->shownGenre, sizeof(app->shownGenre), genre[0] ? genre : "-");
	updates += SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), fileInfo);
	if (updateStatus) {
		if (radioStatus == RADIO_STATUS_ERROR)
			sprintf(status, "Stream failed: %s", radioError[0] ? radioError : "radio error");
		else if (radioStatus == RADIO_STATUS_RECONNECTING)
			sprintf(status, "Stream dropped - reconnecting");
		else if (radioStatus == RADIO_STATUS_CONNECTING)
			sprintf(status, "Connecting stream...");
		else if (radioStatus == RADIO_STATUS_BUFFERING && !MrRadioPlaybackHasStarted())
			sprintf(status, "Buffering - %.100s", station[0] ? station :
				(app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio"));
		else if (radioStatus == RADIO_STATUS_PLAYING ||
			(radioStatus == RADIO_STATUS_BUFFERING && MrRadioPlaybackHasStarted())) {
			if (gGuiPlaybackStatus.decodedFrames > 0)
				RADIO_DBG(printf("radio-ui: UI state set to PLAYING after first audio frame\n");)
			MrFormatRadioStreamingStatus(app, station, status, sizeof(status));
		}
		else
			sprintf(status, "Connecting stream...");
		updates += SetReadonlyString(app->statusGad, app->win, app->shownStatus,
			sizeof(app->shownStatus), status);
	}
	return updates;
}

static void SetGauge(MrApp *app, int level)
{
	if (level < 0)
		level = 0;
	if (level > 100)
		level = 100;
	if (level == app->shownGaugeLevel)
		return;
	app->shownGaugeLevel = level;
	if (app->gaugeGad && app->win)
		SetGadgetAttrs((struct Gadget *)app->gaugeGad, app->win, NULL,
			FUELGAUGE_Level, (ULONG)level,
			TAG_DONE);
}

static int SpeedChoiceFromApp(const MrApp *app)
{
	if (app->rateIndex == MR_RATE_22050_INDEX && app->cd32Ultrafast)
		return 3;
	if (app->ultrafast)
		return 2;
	if (app->superfastLowrate)
		return 1;
	return 0;
}

static void UpdateSpeedGadgetChoices(MrApp *app)
{
	if (app->rateIndex != MR_RATE_22050_INDEX && app->cd32Ultrafast) {
		app->cd32Ultrafast = 0;
		app->ultrafast = 0;
		app->superfastLowrate = 0;
	}
	if (app->win && app->speedGad)
		SetGadgetAttrs((struct Gadget *)app->speedGad, app->win, NULL,
			CHOOSER_LabelArray, (ULONG)(app->rateIndex == MR_RATE_22050_INDEX ? kSpeedLabels : kSpeedLabelsNo22050),
			CHOOSER_Selected, (ULONG)SpeedChoiceFromApp(app),
			TAG_DONE);
}

static void UpdateChannelGadgetState(MrApp *app)
{
	int disabled = app->fakeStereo ? TRUE : FALSE;
	if (disabled == app->shownChannelDisabled)
		return;
	app->shownChannelDisabled = disabled;
	if (app->win && app->channelGad)
		SetGadgetAttrs((struct Gadget *)app->channelGad, app->win, NULL,
			GA_Disabled, (ULONG)disabled, TAG_DONE);
}

static void UpdateNextButtonState(MrApp *app)
{
	int enabled = app->playlistCount > 0 && app->playlistCurrent >= 0 &&
		app->playlistCurrent + 1 < app->playlistCount &&
		!app->playbackDonePending && !gPlayer.stopRequested;
	int disabled = enabled ? FALSE : TRUE;
	if (disabled == app->shownNextDisabled)
		return;
	app->shownNextDisabled = disabled;
	if (app->win && app->nextGad)
		SetGadgetAttrs((struct Gadget *)app->nextGad, app->win, NULL,
			GA_Disabled, (ULONG)disabled, TAG_DONE);
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
	int isRadio;
	int useCd32Ultrafast;
	int useUltrafast;
	int useSuperfast;
	int useFastLowrate;
	int useMono;
	int useFakeStereo;
	int rateIndex;

	memset(args, 0, sizeof(*args));
	isRadio = MrIsRadioInput(app->inputName);
	useCd32Ultrafast = app->cd32Ultrafast;
	useUltrafast = app->ultrafast;
	useSuperfast = app->superfastLowrate;
	useFastLowrate = app->fastLowrate;
	useMono = app->mono;
	useFakeStereo = app->fakeStereo;
	rateIndex = app->rateIndex;
	if (isRadio && (useCd32Ultrafast || (useUltrafast && useMono && rateIndex == MR_RATE_22050_INDEX))) {
		useCd32Ultrafast = 0;
		useUltrafast = 0;
		useSuperfast = 0;
		useFastLowrate = 0;
	}
	AddArg(args, "minimp3r");
	AddArg(args, "--play");
	if (isRadio) {
		AddArg(args, "--radio-stream");
		if (app->haveRadioHostAddr) {
			AddArg(args, "--radio-host-addr-be");
			sprintf(num, "%lu", app->radioHostAddrBe);
			AddArg(args, num);
		}
	}
	/* --fast-mem preloads the *whole input* into Fast RAM up front
	 * (InputSourcePreloadFastMemory() in amiga_mp3dec.c does an
	 * end-of-file Seek()/ftell() to size the preload buffer) -- that
	 * assumes a finite, seekable local file.  A radio stream is neither;
	 * passing --fast-mem through for radio input pointed that preload
	 * logic at a live stream socket/handle it was never designed for. */
	if (app->fastMem && !isRadio)
		AddArg(args, "--fast-mem");
	if (useCd32Ultrafast) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
		AddArg(args, "--exp-reduced-taps");
		AddArg(args, "--subband-cap");
		AddArg(args, "12");
	} else if (useSuperfast ||
		(useUltrafast && strcmp(kRates[rateIndex], "28600") != 0)) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
	} else if (useFastLowrate && strcmp(kRates[rateIndex], "28600")) {
		AddArg(args, "--fast-lowrate");
	}
	if (useUltrafast && strcmp(kRates[rateIndex], "28600") == 0)
		AddArg(args, "--ultrafast");
	if (useFakeStereo) {
		AddArg(args, "--fake-stereo");
		AddArg(args, "--fake-stereo-delay");
		sprintf(num, "%d", kFakeStereoDelays[app->fakeStereoDelayIndex]);
		AddArg(args, num);
		AddArg(args, "--fake-stereo-shift");
		sprintf(num, "%d", kFakeStereoShifts[app->fakeStereoWidthIndex]);
		AddArg(args, num);
	} else if (useMono)
		AddArg(args, "--mono");
	else
		AddArg(args, "--stereo");
	AddArg(args, "--rate");
	AddArg(args, kRates[rateIndex]);
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
	int postedDone = 0;

	gPlayer.task = FindTask(NULL);
	gPlayer.stage = "STARTING";
	gPlayer.startupStage = "child entered";
	RADIO_DBG(printf("radio-session: child entered session=%lu childTask=%p childProc=%p url=\"%s\" codec=%s state=STARTING\n", gPlayer.sessionId, gPlayer.task, gPlayer.process, gPlayer.url, gPlayer.codec);)
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
		gPlayer.stage = "PLAYING";
		gPlayer.startupStage = "decoder main";
		RADIO_DBG(printf("radio-teardown: child entering HelixAmp3CliMain session=%lu\n", gPlayer.sessionId);)
		HelixAmp3CliMain(gPlayer.argc, gPlayer.argv);
		RADIO_DBG(printf("radio-teardown: child returned from HelixAmp3CliMain session=%lu stop=%d\n", gPlayer.sessionId, gPlayer.stopRequested);)
		gMiniAmp3EmbeddedPlayback = 0;
	}

	gPlayer.stage = "EXITING";
	gPlayer.cleanupStage = "cleanup enter";
	RADIO_DBG(printf("radio-session: child begins cleanup session=%lu reason=%s\n", gPlayer.sessionId, gPlayer.stopRequested ? "stop" : "normal");)
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	gGuiPlaybackStatus.cleanupComplete = 1;
	gPlayer.cleanupStage = "cleanup complete";
	RADIO_DBG(printf("radio-session: child cleanup complete session=%lu\n", gPlayer.sessionId);)
	(void)ranDecoder;

	gDoneRunId = gGuiPlaybackStatus.runId;
	donePort = gDonePort;
	if (donePort && !gPlayer.donePosted && !gDoneMsg.posted) {
		gDoneMsg.msg.mn_Node.ln_Type = NT_MESSAGE;
		gDoneMsg.magic = MR_DONE_MAGIC;
		gDoneMsg.runId = gGuiPlaybackStatus.runId;
		gDoneMsg.posted = 1;
		RADIO_DBG(printf("radio-teardown: child posting done message session=%lu done_magic=%08lx error_ptr=%p\n", gPlayer.sessionId, gDoneMsg.magic, (void *)gGuiPlaybackStatus.radioError);)
		gPlayer.donePosted = 1;
		postedDone = 1;
		PutMsg(donePort, &gDoneMsg.msg);
	} else if (donePort) {
		RADIO_DBG(printf("radio-teardown: duplicate child done suppressed session=%lu donePosted=%d msgPosted=%d\n", gPlayer.sessionId, gPlayer.donePosted, gDoneMsg.posted);)
	}
	if (!postedDone) RADIO_DBG(printf("radio-session: child no done port session=%lu done message not posted\n", gPlayer.sessionId);)
	gPlayer.stage = "EXITED";
	RADIO_DBG(printf("child exiting session=%lu reason=%s\n", gPlayer.sessionId, gPlayer.stopRequested ? "stop" : "normal");)
	RADIO_DBG(printf("radio-teardown: child PlaybackEntry exiting (task will terminate) session=%lu\n", gPlayer.sessionId);)
}

static void StartPlayback(MrApp *app)
{
	struct Process *thisProc;
	BPTR dirLock;
	BPTR nilOut;
	struct Message *stale;

	if (!MrVerifyAppMagic(app, "StartPlayback"))
		return;
	if (!app->inputName[0]) {
		SetStatus(app, "Pick an audio file first.");
		return;
	}
	if (app->streamState == MR_STREAM_STARTING) {
		SetStatus(app, "Playback is already starting.");
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
	gDoneMsg.msg.mn_Length = sizeof(gDoneMsg);
	gDoneMsg.msg.mn_Node.ln_Type = NT_MESSAGE;
	gDoneMsg.magic = MR_DONE_MAGIC;

	memset((void *)&gGuiPlaybackStatus, 0, sizeof(gGuiPlaybackStatus));
	app->streamState = MR_STREAM_STARTING;
	app->playbackRunId = ++gRunCounter;
	gGuiPlaybackStatus.runId = app->playbackRunId;
	gEntryRunId = app->playbackRunId;

	BuildPlaybackArgs(app, &gArgs);
	gPlayer.argc = gArgs.argc;
	gPlayer.argv = gArgs.argv;
	gPlayer.stopRequested = 0;
	gPlayer.sessionId = app->playbackRunId;
	SafeCopy(gPlayer.url, sizeof(gPlayer.url), app->inputName);
	SafeCopy(gPlayer.codec, sizeof(gPlayer.codec), MrIsRadioInput(app->inputName) ? "radio" : "file");
	gPlayer.stage = "STARTING";
	gPlayer.startupStage = "CreateNewProc";
	gPlayer.cleanupStage = "";
	gPlayer.lastIoState = "";
	gPlayer.donePosted = 0;
	app->lastChildExitReason[0] = '\0';
	app->lastChildError[0] = '\0';
	app->lastRadioError[0] = '\0';
	app->parentDoneHandled = 0;
	app->lastChildEverPlayed = 0;
	app->lastChildFirstData = 0;
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
	app->streamState = MR_STREAM_PLAYING;
	SafeCopy(app->currentStreamUrl, sizeof(app->currentStreamUrl), app->inputName);
	SafeCopy(app->currentStreamCodec, sizeof(app->currentStreamCodec), gPlayer.codec);
	gPlayer.task = (struct Task *)gPlayer.process;
	app->activeChildCount = 1; app->activeStreamSessions = 1; app->activeStreamTasks = 1;
	MrDebugSession("parent starts stream", app);
	app->stoppedByUser = 0;
	app->lastPhaseShown = -1;
	app->elapsedSecs = 0;
	app->lastFrames = 0;
	app->lastRadioMetaTick = 0;
	app->lastRadioStatusTick = 0;
	app->lastTimeTick = 0;
	app->lastRadioStatusShown = -1;
	UpdateTimeDisplay(app);
	EnablePlayStop(app, 1);
	if (MrIsRadioInput(app->inputName)) {
		char status[160];
		sprintf(status, "Buffering - %.140s", app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio");
		SetStatus(app, status);
		RadioSetStatus(app, status);
	} else
		SetStatus(app, "Starting playback...");
	SetGauge(app, 0);
}

static void StopPlayback(MrApp *app)
{
	struct Task *child;
	int canStop;

	if (!MrVerifyAppMagic(app, "StopPlayback"))
		return;
	RADIO_DBG(printf("radio-guard: before StopPlayback\n");)
	RADIO_DBG(printf("radio-stop: button streamState=%s playbackActive=%d playbackDonePending=%d activeChildCount=%lu process=%p task=%p stopRequested=%d queuedStreamUrl=\"%s\" playlistNextPending=%d session=%lu lastExit=\"%s\" lastError=\"%s\"\n",
		MrStreamStateName(app->streamState), app->playbackActive,
		app->playbackDonePending, app->activeChildCount, gPlayer.process,
		gPlayer.task, gPlayer.stopRequested, app->queuedStreamUrl,
		app->playlistNextPending, gPlayer.sessionId, app->lastChildExitReason,
		app->lastChildError);)
	canStop = app->playbackActive &&
		app->streamState != MR_STREAM_IDLE &&
		app->streamState != MR_STREAM_EXITED &&
		app->streamState != MR_STREAM_ERROR &&
		app->activeChildCount > 0 &&
		(gPlayer.process || gPlayer.task || PlaybackProcessStillExists());
	if (!canStop) {
		RADIO_DBG(printf("Stop ignored: no active playback child\n");)
		SetStatus(app, "Stopped.");
		RADIO_DBG(printf("radio-guard: after StopPlayback\n");)
		return;
	}
	if (gPlayer.stopRequested) {
		SetStatus(app, "Stopping...");
		RADIO_DBG(printf("radio-guard: after StopPlayback\n");)
		return;
	}
	app->stoppedByUser = 1;
	app->streamState = MR_STREAM_STOP_REQUESTED;
	gPlayer.stage = "STOP_REQUESTED";
	gPlayer.stopRequested = 1;
	MrDebugSession("parent stop request sent", app);
	gPlaybackInterrupted = 1;

	/* Radio teardown safety (parity with the GadTools front-end): mark the
	 * shared radio status as stopping and clear the active flag straight away.
	 * Otherwise the timer poll below keeps calling MrSetRadioMetadata() against
	 * a stream the playback child is in the middle of closing, refreshing the
	 * GUI from radio state that is being torn down. */
	if (MrIsRadioInput(app->inputName)) {
		gGuiPlaybackStatus.radioStatus = (int)RADIO_STATUS_STOPPING;
		gGuiPlaybackStatus.radioActive = 0;
		gGuiPlaybackStatus.radioBufferedBytes = 0;
	}

	/* Wake the child immediately so it does not sit in WaitIO for the rest of
	 * a multi-second audio buffer.  Forbid()/FindTask() guards against the
	 * child already being torn down by DOS. */
	Forbid();
	child = FindTask((STRPTR)"minimp3r playback");
	if (child)
		Signal(child, SIGBREAKF_CTRL_C);
	Permit();

	app->streamState = MR_STREAM_STOPPING;
	gPlayer.stage = "STOPPING";
	SetStatus(app, "Stopping...");
	UpdateNextButtonState(app);
	RADIO_DBG(printf("radio-guard: after StopPlayback\n");)
}

static int StopPlaybackAndWait(MrApp *app, int ticks, const char *timeoutStatus)
{
	int waited;

	if (!app)
		return 1;
	if (!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists())
		return 1;
	StopPlayback(app);
	for (waited = 0; waited < ticks; waited++) {
		HandleDoneSignal(app);
		if (!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists())
			return 1;
		Delay(1);
	}
	app->streamState = MR_STREAM_STOP_TIMEOUT;
	if (timeoutStatus && timeoutStatus[0])
		SetStatus(app, timeoutStatus);
	MrDebugSession("parent stop timeout - queued stream not started", app);
	RADIO_DBG(printf("radio-session: stop timeout details childTask=%p session=%lu currentStage=\"%s\" lastStartup=\"%s\" lastCleanup=\"%s\" currentUrl=\"%s\" lastReadDecode=\"%s\" stopFlag=%d donePosted=%d\n", gPlayer.task, gPlayer.sessionId, gPlayer.stage ? (const char *)gPlayer.stage : "", gPlayer.startupStage ? (const char *)gPlayer.startupStage : "", gPlayer.cleanupStage ? (const char *)gPlayer.cleanupStage : "", gPlayer.url, gPlayer.lastIoState ? (const char *)gPlayer.lastIoState : "", gPlayer.stopRequested, gPlayer.donePosted);)
	return 0;
}
static void FinalizePlayback(MrApp *app)
{
	int stoppedByUser = app->stoppedByUser;
	char radioError[128];
	int failedStart;
	const char *finalStatus;

	if (!MrVerifyAppMagic(app, "FinalizePlayback"))
		return;
	RADIO_DBG(printf("radio-guard: before FinalizePlayback\n");)
	MrCopyVolatileString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
	failedStart = (!stoppedByUser && MrIsRadioInput(gPlayer.url) && gGuiPlaybackStatus.decodedFrames == 0 &&
		(gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR ||
		 gGuiPlaybackStatus.radioStatus == RADIO_STATUS_CONNECTING ||
		 gGuiPlaybackStatus.radioStatus == RADIO_STATUS_BUFFERING ||
		 gGuiPlaybackStatus.radioStatus == RADIO_STATUS_CLOSED));
	SafeCopy(app->lastChildExitReason, sizeof(app->lastChildExitReason),
		stoppedByUser ? "stop" : (failedStart ? "error" : "normal"));
	SafeCopy(app->lastChildError, sizeof(app->lastChildError),
		failedStart ? (radioError[0] ? radioError : "radio stream failed") : "");
	app->lastChildEverPlayed = gGuiPlaybackStatus.decodedFrames > 0 ? 1 : 0;
	app->lastChildFirstData = gGuiPlaybackStatus.radioBufferedBytes > 0 ? 1 : 0;
	app->lastCompletedWasHttps = (gPlayer.url[0] && strncmp(gPlayer.url, "https://", 8) == 0);
	app->playbackActive = 0;
	app->playbackDonePending = 0;
	app->stoppedByUser = 0;
	app->streamState = failedStart ? MR_STREAM_ERROR : MR_STREAM_EXITED;
	MrDebugSession("parent receives done and clears child pointer", app);
	RADIO_DBG(printf("radio-done: parent done received childExit=\"%s\" childError=\"%s\" everPlayed=%d firstData=%d streamStateBeforeFinalize=%s\n",
		app->lastChildExitReason, app->lastChildError,
		app->lastChildEverPlayed, app->lastChildFirstData,
		failedStart ? "ERROR" : "EXITED");)
	gPlayer.process = NULL;
	gPlayer.task = NULL;
	gPlayer.stopRequested = 0;
	app->activeChildCount = 0; app->activeStreamSessions = 0; app->activeStreamTasks = 0;
	gPlaybackInterrupted = 0;
	gDonePort = NULL;
	ResetCliParser();

	EnablePlayStop(app, 0);
	app->elapsedSecs = (stoppedByUser || app->totalSecs <= 0) ? 0 : app->totalSecs;
	UpdateTimeDisplay(app);
	SetGauge(app, app->progressEnabled && !stoppedByUser ? 100 : 0);
	if (failedStart) {
		char status[160];
		sprintf(status, "Stream failed: %s", app->lastChildError[0] ? app->lastChildError : "radio stream failed");
		SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), status);
		finalStatus = app->lastRadioError;
		SetStatus(app, app->lastRadioError);
	} else {
		finalStatus = stoppedByUser ? "Stopped." : "Finished.";
		SetStatus(app, finalStatus);
		app->streamState = MR_STREAM_IDLE;
	}
	RADIO_DBG(printf("radio-done: streamStateAfterFinalize=%s guiStatus=\"%s\"\n",
		MrStreamStateName(app->streamState), finalStatus);)
	RADIO_DBG(printf("radio-guard: after FinalizePlayback\n");)
	if (app->queuedStreamUrl[0]) {
		app->queuedStreamUrl[0] = '\0';
		RadioSetStatus(app, "Starting queued stream...");
		RadioDoProbeAndPlay(app);
		return;
	}
	if (app->playlistNextPending) {
		app->playlistNextPending = 0;
		StartPlayback(app);
	}
}

static void HandleDoneSignal(MrApp *app)
{
	struct Message *msg;
	int gotDone = 0;
	char doneError[128];
	const char *doneReason;
	int doneEverPlayed;
	int doneFirstData;

	if (!app->donePort)
		return;
	while ((msg = GetMsg(app->donePort)) != NULL) {
		gotDone = 1;
		if (((MrDoneMessage *)msg)->magic != MR_DONE_MAGIC)
			RADIO_DBG(printf("radio-done: bad done message magic msg=%p magic=%08lx\n", (void *)msg, ((MrDoneMessage *)msg)->magic);)
	}
	if (!gotDone)
		return;
	if (app->parentDoneHandled) {
		RADIO_DBG(printf("radio-done: duplicate parent done ignored session=%lu handled=%d pending=%d active=%d\n", gPlayer.sessionId, app->parentDoneHandled, app->playbackDonePending, app->playbackActive);)
		return;
	}
	app->parentDoneHandled = 1;

	MrCopyVolatileString(doneError, sizeof(doneError), gGuiPlaybackStatus.radioError);
	doneReason = gPlayer.stopRequested ? "stop" :
		(gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR ? "error" : "normal");
	doneEverPlayed = gGuiPlaybackStatus.decodedFrames > 0 ? 1 : 0;
	doneFirstData = gGuiPlaybackStatus.radioBufferedBytes > 0 ? 1 : 0;
	RADIO_DBG(printf("radio-session: parent receives done message session=%lu doneRun=%lu active=%d pending=%d childExit=\"%s\" childError=\"%s\" everPlayed=%d firstData=%d streamStateBefore=%s\n",
		gPlayer.sessionId, gDoneRunId, app->playbackActive,
		app->playbackDonePending, doneReason, doneError,
		doneEverPlayed, doneFirstData, MrStreamStateName(app->streamState));)
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
	int updates = 0;
	int radioStatus;
	int radioActive;
	unsigned long tick;

	if (!app)
		return;
	tick = ++app->pollTick;
	MR_POLL_DBG(("minimp3r: poll %lu start\n", tick));

	/* A late done where the child had already vanished before we drained the
	 * port: finalize now.  This is only a quick task lookup and never asks the
	 * playback child to respond synchronously. */
	if (app->playbackDonePending && !PlaybackProcessStillExists()) {
		FinalizePlayback(app);
		MR_POLL_DBG(("minimp3r: poll %lu finalized done, updates=%d\n", tick, updates));
		return;
	}
	if (!app->playbackActive) {
		MR_POLL_DBG(("minimp3r: poll %lu idle, updates=%d\n", tick, updates));
		return;
	}

	radioStatus = gGuiPlaybackStatus.radioStatus;
	radioActive = gGuiPlaybackStatus.radioActive;
	if (MrIsRadioInput(app->inputName)) {
		if (radioStatus == RADIO_STATUS_ERROR) {
			char radioError[128], status[160];
			MrCopyVolatileString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
			gGuiPlaybackStatus.radioActive = 0;
			gGuiPlaybackStatus.radioBufferedBytes = 0;
			sprintf(status, "Stream failed: %s", radioError[0] ? radioError : "radio error");
			SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), status);
			updates += SetStatusIfChanged(app, app->lastRadioError);
			RadioSetStatus(app, app->lastRadioError);
		} else if (radioActive &&
			radioStatus != RADIO_STATUS_STOPPING &&
			radioStatus != RADIO_STATUS_CLOSED) {
			int updateMeta = (app->lastRadioMetaTick == 0) ||
				(tick - app->lastRadioMetaTick >= MR_METADATA_TICKS);
			int updateStatus = (app->lastRadioStatusTick == 0) ||
				radioStatus != app->lastRadioStatusShown ||
				((radioStatus == RADIO_STATUS_BUFFERING || radioStatus == RADIO_STATUS_PLAYING) &&
				 MrRadioPlaybackHasStarted() && strncmp(app->shownStatus, "Streaming ", 10));
			if (updateMeta || updateStatus) {
				updates += MrSetRadioMetadata(app, updateStatus);
				if (updateMeta) app->lastRadioMetaTick = tick;
				if (updateStatus) { app->lastRadioStatusTick = tick; app->lastRadioStatusShown = radioStatus; }
			}
		}
	}

	phase   = gGuiPlaybackStatus.phase;
	frames  = gGuiPlaybackStatus.decodedFrames;
	rate    = gGuiPlaybackStatus.sampleRate;
	spareMs = gGuiPlaybackStatus.spareMs;
	halfMs  = gGuiPlaybackStatus.halfBufferMs;

	if (!MrIsRadioInput(app->inputName) && rate > 0 && frames > 0 &&
		frames != app->lastFrames &&
		(app->lastTimeTick == 0 || tick - app->lastTimeTick >= MR_TIME_TICKS)) {
		app->lastFrames = frames;
		app->lastTimeTick = tick;
		audioSecs = (long)((frames * 1152UL) / (unsigned long)rate);
		audioSecs -= halfMs ? (long)((halfMs + 999UL) / 1000UL) : app->bufferSeconds;
		if (audioSecs < 0) audioSecs = 0;
		if (app->totalSecs > 0 && audioSecs > app->totalSecs) audioSecs = app->totalSecs;
		if (app->elapsedSecs != (int)audioSecs) {
			app->elapsedSecs = (int)audioSecs;
			UpdateTimeDisplay(app);
			updates++;
		}
		SetGauge(app, (app->progressEnabled && app->totalSecs > 0) ? (app->elapsedSecs * 100) / app->totalSecs : 0);
	}
	(void)spareMs;
	(void)halfMs;

	if (phase != app->lastPhaseShown) {
		int isRadioInput = MrIsRadioInput(app->inputName);
		app->lastPhaseShown = phase;
		switch (phase) {
		case GUIPLAY_PHASE_BUFFERING:
			if (!isRadioInput) { SetStatus(app, "Buffering..."); updates++; }
			break;
		case GUIPLAY_PHASE_PLAYING:
			if (!isRadioInput) {
				if (frames > 0 && rate > 0) {
					sprintf(buf, "Playing - %lu frames @ %d Hz", frames, rate);
					SetStatus(app, buf);
				} else
					SetStatus(app, "Playing...");
				updates++;
			}
			break;
		case GUIPLAY_PHASE_UNDERRUN:
			if (!isRadioInput) { SetStatus(app, "Playing (buffer low)..."); updates++; }
			break;
		case GUIPLAY_PHASE_STOPPING:
			SetStatus(app, "Stopping..."); updates++;
			break;
		case GUIPLAY_PHASE_ERROR:
			SetStatus(app, "Playback error."); updates++;
			break;
		default:
			break;
		}
	}
	MR_POLL_DBG(("minimp3r: poll %lu end updates=%d phase=%d radio=%d/%d fullrefresh=0\n",
		tick, updates, phase, radioActive, radioStatus));
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
		STRINGA_BufferPos, 0,
		STRINGA_DispPos, 0,
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
		GETFILE_Pattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|aif|aiff)",
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
	                CHOOSER_LabelArray, (ULONG)(app->rateIndex == MR_RATE_22050_INDEX ? kSpeedLabels : kSpeedLabelsNo22050),
	                CHOOSER_Selected, (ULONG)SpeedChoiceFromApp(app),
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
	app->radioGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_RADIO, GA_RelVerify, TRUE, GA_Text, (ULONG)"Internet Radio", TAG_DONE);

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

	app->timeGad = ReadonlyString(GID_TIME, "00:00 / --:--", 32);
	app->fileInfoGad = ReadonlyString(GID_FILEINFO, "No file info", 128);
	app->titleGad = ReadonlyString(GID_TITLE, "-", 96);
	app->artistGad = ReadonlyString(GID_ARTIST, "-", 96);
	app->albumGad = ReadonlyString(GID_ALBUM, "-", 128);
	app->trackGad = ReadonlyString(GID_TRACK, "-", 32);
	app->genreGad = ReadonlyString(GID_GENRE, "-", 64);
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
		!CheckGadget(app->radioGad, "internet radio") ||
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
			LAYOUT_AddChild, (ULONG)app->radioGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		TAG_DONE);

        if (!root) {
		fprintf(stderr, "minimp3r: could not build the gadget layout.\n");
		return 0;
	}

	app->winObj = (Object *)NewObject(WINDOW_GetClass(), NULL,
		WA_Title, (ULONG)MR_WINDOW_TITLE,
		WA_ScreenTitle, (ULONG)MR_WINDOW_TITLE,
		WA_Activate, TRUE,
		WA_DepthGadget, TRUE,
		WA_DragBar, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			IDCMP_IDCMPUPDATE | IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_MOUSEMOVE,
		WA_NewLookMenus, TRUE,
		/* Match the GadTools frontend's larger default footprint now that
		 * ReAction window sizing is stable, so long metadata and file-info
		 * strings get the same breathing room. */
		WA_Width, 560,
		WA_Height, 340,
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
	CloseRadioWindow(app);
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

static const char *PjpegStatusName(int status)
{
	switch (status) {
	case 0: return "OK";
	case PJPG_NO_MORE_BLOCKS: return "NO_MORE_BLOCKS";
	case PJPG_BAD_DHT_COUNTS: return "BAD_DHT_COUNTS";
	case PJPG_BAD_DHT_INDEX: return "BAD_DHT_INDEX";
	case PJPG_BAD_DHT_MARKER: return "BAD_DHT_MARKER";
	case PJPG_BAD_DQT_MARKER: return "BAD_DQT_MARKER";
	case PJPG_BAD_DQT_TABLE: return "BAD_DQT_TABLE";
	case PJPG_BAD_PRECISION: return "BAD_PRECISION";
	case PJPG_BAD_HEIGHT: return "BAD_HEIGHT";
	case PJPG_BAD_WIDTH: return "BAD_WIDTH";
	case PJPG_TOO_MANY_COMPONENTS: return "TOO_MANY_COMPONENTS";
	case PJPG_BAD_SOF_LENGTH: return "BAD_SOF_LENGTH";
	case PJPG_BAD_VARIABLE_MARKER: return "BAD_VARIABLE_MARKER";
	case PJPG_BAD_DRI_LENGTH: return "BAD_DRI_LENGTH";
	case PJPG_BAD_SOS_LENGTH: return "BAD_SOS_LENGTH";
	case PJPG_BAD_SOS_COMP_ID: return "BAD_SOS_COMP_ID";
	case PJPG_W_EXTRA_BYTES_BEFORE_MARKER: return "EXTRA_BYTES_BEFORE_MARKER";
	case PJPG_NO_ARITHMITIC_SUPPORT: return "NO_ARITHMETIC_SUPPORT";
	case PJPG_UNEXPECTED_MARKER: return "UNEXPECTED_MARKER";
	case PJPG_NOT_JPEG: return "NOT_JPEG";
	case PJPG_UNSUPPORTED_MARKER: return "UNSUPPORTED_MARKER";
	case PJPG_BAD_DQT_LENGTH: return "BAD_DQT_LENGTH";
	case PJPG_TOO_MANY_BLOCKS: return "TOO_MANY_BLOCKS";
	case PJPG_UNDEFINED_QUANT_TABLE: return "UNDEFINED_QUANT_TABLE";
	case PJPG_UNDEFINED_HUFF_TABLE: return "UNDEFINED_HUFF_TABLE";
	case PJPG_NOT_SINGLE_SCAN: return "NOT_SINGLE_SCAN (progressive JPEG unsupported)";
	case PJPG_UNSUPPORTED_COLORSPACE: return "UNSUPPORTED_COLORSPACE";
	case PJPG_UNSUPPORTED_SAMP_FACTORS: return "UNSUPPORTED_SAMP_FACTORS";
	case PJPG_DECODE_ERROR: return "DECODE_ERROR";
	case PJPG_BAD_RESTART_MARKER: return "BAD_RESTART_MARKER";
	case PJPG_ASSERTION_ERROR: return "ASSERTION_ERROR";
	case PJPG_BAD_SOS_SPECTRAL: return "BAD_SOS_SPECTRAL";
	case PJPG_BAD_SOS_SUCCESSIVE: return "BAD_SOS_SUCCESSIVE";
	case PJPG_STREAM_READ_ERROR: return "STREAM_READ_ERROR";
	case PJPG_NOTENOUGHMEM: return "NOTENOUGHMEM";
	case PJPG_UNSUPPORTED_COMP_IDENT: return "UNSUPPORTED_COMP_IDENT";
	case PJPG_UNSUPPORTED_QUANT_TABLE: return "UNSUPPORTED_QUANT_TABLE";
	case PJPG_UNSUPPORTED_MODE: return "UNSUPPORTED_MODE";
	default: return "UNKNOWN";
	}
}


static int MrJpegHasSof2(const unsigned char *data, unsigned long bytes)
{
	unsigned long p = 2;
	if (!data || bytes < 4 || data[0] != 0xff || data[1] != 0xd8) return 0;
	while (p + 3 < bytes) {
		unsigned int m, len;
		while (p < bytes && data[p] != 0xff) p++;
		while (p < bytes && data[p] == 0xff) p++;
		if (p >= bytes) break;
		m = data[p++];
		if (m == 0xc2) return 1;
		if (m == 0xd9 || m == 0xda) break;
		if (m >= 0xd0 && m <= 0xd7) continue;
		if (p + 1 >= bytes) break;
		len = ((unsigned int)data[p] << 8) | data[p + 1];
		if (len < 2 || p + len > bytes) break;
		p += len;
	}
	return 0;
}

typedef struct MrJpegHuffTab {
	unsigned char valid;
	unsigned char size[256];
	unsigned short code[256];
	unsigned char sym[256];
	int count;
} MrJpegHuffTab;

typedef struct MrProgComp {
	unsigned char id, h, v, tq, td;
	int bw, bh, prevDc;
	unsigned char haveDc;
} MrProgComp;

typedef struct MrProgBits {
	const unsigned char *p, *end;
	unsigned int bits;
	int bitcnt;
} MrProgBits;

static int MrJpegReadBits(MrProgBits *br, int n)
{
	int v = 0;
	while (n-- > 0) {
		if (br->bitcnt <= 0) {
			int c;
			if (br->p >= br->end) return -1;
			c = *br->p++;
			if (c == 0xff) {
				if (br->p < br->end && *br->p == 0x00) br->p++;
				else return -1;
			}
			br->bits = (unsigned int)c;
			br->bitcnt = 8;
		}
		v = (v << 1) | (int)((br->bits >> 7) & 1);
		br->bits <<= 1;
		br->bitcnt--;
	}
	return v;
}

static int MrJpegDecodeHuff(MrProgBits *br, const MrJpegHuffTab *ht)
{
	unsigned short code = 0;
	int len, i;
	if (!ht || !ht->valid) return -1;
	for (len = 1; len <= 16; len++) {
		int b = MrJpegReadBits(br, 1);
		if (b < 0) return -1;
		code = (unsigned short)((code << 1) | b);
		for (i = 0; i < ht->count; i++)
			if (ht->size[i] == len && ht->code[i] == code)
				return ht->sym[i];
	}
	return -1;
}

static int MrJpegReceiveExtend(MrProgBits *br, int s)
{
	int v;
	if (s == 0) return 0;
	if (s < 0 || s > 11) return -40999;
	v = MrJpegReadBits(br, s);
	if (v < 0) return -40999;
	if (v < (1 << (s - 1))) v += ((-1) << s) + 1;
	return v;
}

static int MrJpegFindEntropyEnd(const unsigned char *p, const unsigned char *end)
{
	const unsigned char *s = p;
	while (s + 1 < end) {
		if (s[0] == 0xff && s[1] != 0x00) {
			if (s[1] >= 0xd0 && s[1] <= 0xd7) { s += 2; continue; }
			break;
		}
		s++;
	}
	return (int)(s - p);
}

static int DecodeProgressiveJpegDcPreviewToGrey(const unsigned char *jpg, unsigned long bytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	static short dc[3][(MR_MAX_JPEG_DIM / 8) * (MR_MAX_JPEG_DIM / 8)];
	static unsigned long greyAccum[MR_ART_W * MR_ART_H], rAccum[MR_ART_W * MR_ART_H], gAccum[MR_ART_W * MR_ART_H], bAccum[MR_ART_W * MR_ART_H];
	static unsigned short greyCount[MR_ART_W * MR_ART_H];
	MrJpegHuffTab hdc[4]; MrProgComp comp[3];
	unsigned short qdc[4];
	unsigned long p = 2; int width = 0, height = 0, comps = 0, maxh = 1, maxv = 1, sof2 = 0, anyDc = 0, anyDcScan = 0, lumaDcReady = 0, laterScanFailed = 0, i;
	const char *failReason = NULL;
	memset(hdc, 0, sizeof(hdc)); memset(qdc, 0, sizeof(qdc)); memset(comp, 0, sizeof(comp)); memset(dc, 0, sizeof(dc));
	if (!jpg || bytes < 4 || !greyOut || outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H || jpg[0] != 0xff || jpg[1] != 0xd8) { failReason = "bad entropy decode"; goto fail; }
	while (p + 3 < bytes) {
		unsigned int m, len;
		while (p < bytes && jpg[p] != 0xff) p++;
		while (p < bytes && jpg[p] == 0xff) p++;
		if (p >= bytes) break;
		m = jpg[p++];
		if (m == 0xd9) break;
		if (m >= 0xd0 && m <= 0xd7) continue;
		if (p + 1 >= bytes) { failReason = "bad entropy decode"; goto fail; }
		len = ((unsigned int)jpg[p] << 8) | jpg[p + 1];
		if (len < 2 || p + len > bytes) { failReason = "bad entropy decode"; goto fail; }
		if (m == 0xdb) {
			unsigned long e = p + len; p += 2;
			while (p < e) { unsigned int t = jpg[p++]; if (t >> 4) { failReason = "unsupported quant precision"; goto fail; } if ((t & 15) > 3 || p + 64 > e) { failReason = "bad entropy decode"; goto fail; } qdc[t & 15] = jpg[p]; p += 64; }
			continue;
		}
		if (m == 0xc2) {
			unsigned long s = p + 2; int c;
			sof2 = 1; if (len < 8 || jpg[s] != 8) { failReason = (len >= 8 && jpg[s] != 8) ? "unsupported quant precision" : "bad entropy decode"; goto fail; }
			height = ((int)jpg[s+1] << 8) | jpg[s+2]; width = ((int)jpg[s+3] << 8) | jpg[s+4]; comps = jpg[s+5];
			RADIO_DBG(printf("radio-art: SOF2 width=%d height=%d components=%d\n", width, height, comps);)
			if (width <= 0 || height <= 0 || width > MR_MAX_JPEG_DIM || height > MR_MAX_JPEG_DIM || (comps != 1 && comps != 3) || len != (unsigned)(8 + comps * 3)) { failReason = "bad entropy decode"; goto fail; }
			for (c = 0, s += 6; c < comps; c++, s += 3) { comp[c].id = jpg[s]; comp[c].h = jpg[s+1] >> 4; comp[c].v = jpg[s+1] & 15; comp[c].tq = jpg[s+2]; RADIO_DBG(printf("radio-art: SOF2 component id=%u h=%u v=%u tq=%u\n", comp[c].id, comp[c].h, comp[c].v, comp[c].tq);) if (!comp[c].h || !comp[c].v || comp[c].tq > 3) { failReason = "bad entropy decode"; goto fail; } if (comp[c].h > maxh) maxh = comp[c].h; if (comp[c].v > maxv) maxv = comp[c].v; }
			for (c = 0; c < comps; c++) { comp[c].bw = (width * comp[c].h + maxh * 8 - 1) / (maxh * 8); comp[c].bh = (height * comp[c].v + maxv * 8 - 1) / (maxv * 8); if (comp[c].bw * comp[c].bh > (MR_MAX_JPEG_DIM / 8) * (MR_MAX_JPEG_DIM / 8)) { failReason = "bad entropy decode"; goto fail; } }
		} else if (m == 0xc4) {
			unsigned long e = p + len; p += 2;
			while (p < e) { unsigned int tc, th, n = 0, k; unsigned short code = 0; const unsigned char *counts; if (p + 17 > e) { failReason = "bad entropy decode"; goto fail; } tc = jpg[p] >> 4; th = jpg[p] & 15; p++; counts = jpg + p; for (i = 0; i < 16; i++) n += jpg[p+i]; RADIO_DBG(printf("radio-art: DHT class=%u table=%u symbols=%u\n", tc, th, n);) if (th > 3 || tc > 1 || p + 16 + n > e || n > 256) { failReason = "bad entropy decode"; goto fail; } p += 16; if (tc == 1) { RADIO_DBG(printf("radio-art: skipped AC DHT table=%u symbols=%u\n", th, n);) p += n; continue; } hdc[th].count = 0; for (i = 1; i <= 16; i++) { int cnt = counts[i - 1]; for (k = 0; k < (unsigned)cnt; k++) { int idx = hdc[th].count++; hdc[th].size[idx] = i; hdc[th].code[idx] = code++; hdc[th].sym[idx] = jpg[p++]; } code <<= 1; } hdc[th].valid = 1; }
			continue;
		} else if (m == 0xda) {
			unsigned long s = p + 2; int ns, Ss, Se, Ah, Al, scanComp[3], scanOk = 1, scanHasLuma = 0, c, mi, mx, my, by, bx; const unsigned char *ep; MrProgBits br;
			if (!sof2 || len < 6) { failReason = !sof2 ? "no SOF2" : "bad entropy decode"; goto fail; } ns = jpg[s++]; if (ns < 1 || ns > comps || len != (unsigned)(6 + ns * 2)) { failReason = "bad entropy decode"; goto fail; }
			for (i = 0; i < ns; i++, s += 2) { for (c = 0; c < comps && comp[c].id != jpg[s]; c++); if (c >= comps || (jpg[s+1] & 15)) { failReason = "bad entropy decode"; goto fail; } comp[c].td = jpg[s+1] >> 4; scanComp[i] = c; if (c == 0) scanHasLuma = 1; RADIO_DBG(printf("radio-art: SOS component id=%u td=%u\n", comp[c].id, comp[c].td);) }
			Ss = jpg[s++]; Se = jpg[s++]; Ah = jpg[s] >> 4; Al = jpg[s] & 15;
			RADIO_DBG(printf("radio-art: SOS ns=%d Ss=%d Se=%d Ah=%d Al=%d\n", ns, Ss, Se, Ah, Al);)
			ep = jpg + p + len; i = MrJpegFindEntropyEnd(ep, jpg + bytes); br.p = ep; br.end = ep + i; br.bits = 0; br.bitcnt = 0; p += len + i;
			if (Ss == 0 && Se == 0 && Ah == 0) {
				anyDcScan = 1;
				mx = (width + maxh * 8 - 1) / (maxh * 8); my = (height + maxv * 8 - 1) / (maxv * 8);
				for (mi = 0; scanOk && mi < mx * my; mi++) for (i = 0; scanOk && i < ns; i++) { c = scanComp[i]; for (by = 0; scanOk && by < comp[c].v; by++) for (bx = 0; bx < comp[c].h; bx++) { int x = (mi % mx) * comp[c].h + bx, y = (mi / mx) * comp[c].v + by, sym, diff; if (x >= comp[c].bw || y >= comp[c].bh) continue; if (!hdc[comp[c].td].valid) { failReason = "no DC Huffman table"; scanOk = 0; break; } sym = MrJpegDecodeHuff(&br, &hdc[comp[c].td]); diff = MrJpegReceiveExtend(&br, sym); if (sym < 0 || diff == -40999) { failReason = "bad entropy decode"; scanOk = 0; break; } comp[c].prevDc += diff; dc[c][y * comp[c].bw + x] = (short)(comp[c].prevDc << Al); comp[c].haveDc = 1; anyDc = 1; } }
				if (scanHasLuma && comp[0].haveDc && !lumaDcReady) { lumaDcReady = 1; RADIO_DBG(printf("radio-art: progressive JPEG luma DC ready\n");) }
				if (!scanOk) {
					if (comp[0].haveDc) { laterScanFailed = 1; goto render_preview; }
					goto fail;
				}
			} else {
				/* AC scans and refinement scans are not needed for the DC preview. */
			}
			continue;
		}
		p += len;
	}
	if (!sof2) { failReason = "no SOF2"; goto fail; }
	if (!anyDcScan) { failReason = "no DC scan found"; goto fail; }
	if (!anyDc) { failReason = "no DC Huffman table"; goto fail; }
	if (!lumaDcReady || !comp[0].haveDc) { failReason = "no luma DC"; goto fail; }

render_preview:
	if (!comp[0].haveDc) { failReason = failReason ? failReason : "no luma DC"; goto fail; }
	memset(greyOut, 0x80, (size_t)(outW * outH)); if (rgbOut) memset(rgbOut, 0x80, (size_t)(outW * outH * 3)); memset(greyAccum,0,sizeof(greyAccum)); memset(rAccum,0,sizeof(rAccum)); memset(gAccum,0,sizeof(gAccum)); memset(bAccum,0,sizeof(bAccum)); memset(greyCount,0,sizeof(greyCount));
	for (i = 0; i < width * height; i++) { int x = i % width, y = i / width, dst = (y * outH / height) * outW + (x * outW / width); int yb = (y * comp[0].v * comp[0].bh) / height / comp[0].v, xb = (x * comp[0].h * comp[0].bw) / width / comp[0].h; int Y = 128 + (dc[0][yb * comp[0].bw + xb] * (int)qdc[comp[0].tq] + 4) / 8; int R = Y, G = Y, B = Y; if (Y < 0) Y = 0; else if (Y > 255) Y = 255; if (comps == 3 && comp[1].haveDc && comp[2].haveDc) { int cbx = x * comp[1].bw / width, cby = y * comp[1].bh / height, crx = x * comp[2].bw / width, cry = y * comp[2].bh / height; int Cb = 128 + (dc[1][cby * comp[1].bw + cbx] * (int)qdc[comp[1].tq] + 4) / 8; int Cr = 128 + (dc[2][cry * comp[2].bw + crx] * (int)qdc[comp[2].tq] + 4) / 8; R = Y + ((359 * (Cr - 128)) >> 8); G = Y - ((88 * (Cb - 128) + 183 * (Cr - 128)) >> 8); B = Y + ((454 * (Cb - 128)) >> 8); if (R<0)R=0; else if(R>255)R=255; if(G<0)G=0; else if(G>255)G=255; if(B<0)B=0; else if(B>255)B=255; }
		greyAccum[dst] += (77UL * R + 150UL * G + 29UL * B + 128UL) >> 8; rAccum[dst] += R; gAccum[dst] += G; bAccum[dst] += B; if (greyCount[dst] != 0xffff) greyCount[dst]++; }
	for (i = 0; i < outW * outH; i++) if (greyCount[i]) { unsigned short c = greyCount[i]; greyOut[i] = (unsigned char)((greyAccum[i] + c / 2) / c); if (rgbOut) { rgbOut[i*3] = (unsigned char)((rAccum[i] + c / 2) / c); rgbOut[i*3+1] = (unsigned char)((gAccum[i] + c / 2) / c); rgbOut[i*3+2] = (unsigned char)((bAccum[i] + c / 2) / c); } }
	if (laterScanFailed) RADIO_DBG(printf("radio-art: progressive JPEG later scan failed, keeping luma preview\n");)
	if (comps == 3 && comp[1].haveDc && comp[2].haveDc) RADIO_DBG(printf("radio-art: progressive JPEG colour DC preview used\n");)
	else RADIO_DBG(printf("radio-art: progressive JPEG greyscale DC preview used\n");)
	return 0;

fail:
	if (comp[0].haveDc && failReason && !strcmp(failReason, "bad entropy decode")) { laterScanFailed = 1; goto render_preview; }
	RADIO_DBG(printf("radio-art: progressive JPEG DC preview unsupported: %s\n", failReason ? failReason : "bad entropy decode");)
	return -1;
}

static int DecodeJpegToGrey(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH, int isPng)
{
	pjpeg_image_info_t info;
	MrPjpegSrc src;
	unsigned char status;
	static unsigned char xMap[MR_MAX_JPEG_DIM], yMap[MR_MAX_JPEG_DIM];
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
	if (status != 0) {
		if (MrJpegHasSof2(jpegData, jpegBytes)) {
			RADIO_DBG(printf("radio-art: progressive JPEG detected\n");)
			if (DecodeProgressiveJpegDcPreviewToGrey(jpegData, jpegBytes, greyOut, rgbOut, outW, outH) == 0)
				return 0;
			RADIO_DBG(printf("radio-art: progressive JPEG DC preview unsupported\n");)
		}
		RADIO_DBG(printf("radio-art: pjpeg_decode_init failed status=%d (%s)\n",
			(int)status, PjpegStatusName((int)status));)
		return -1;
	}
	if (info.m_width <= 0 || info.m_height <= 0 ||
		info.m_width > MR_MAX_JPEG_DIM || info.m_height > MR_MAX_JPEG_DIM) {
		RADIO_DBG(printf("radio-art: jpeg dimensions out of range %dx%d (max %d)\n",
			(int)info.m_width, (int)info.m_height, MR_MAX_JPEG_DIM);)
		return -1;
	}
	RADIO_DBG(printf("radio-art: jpeg %dx%d comps=%d scan-type=%d\n",
		(int)info.m_width, (int)info.m_height, (int)info.m_comps, (int)info.m_scanType);)
	for (i = 0; i < info.m_width; i++) xMap[i] = (unsigned char)((i * outW) / info.m_width);
	for (i = 0; i < info.m_height; i++) yMap[i] = (unsigned char)((i * outH) / info.m_height);
	for (mcuIndex = 0; mcuIndex < info.m_MCUSPerRow * info.m_MCUSPerCol; mcuIndex++) {
		int mcuX, mcuY, y;
		status = pjpeg_decode_mcu();
		if (status == PJPG_NO_MORE_BLOCKS) break;
		if (status != 0) {
			RADIO_DBG(printf("radio-art: pjpeg_decode_mcu failed at mcu=%d status=%d (%s)\n",
				mcuIndex, (int)status, PjpegStatusName((int)status));)
			return -1;
		}
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
	const char *source, *base, *end;
	char safe[80];
	int i, j;
	EnvName(dst, dstSize, "ArtCache");
	source = (MrIsRadioInput(app->inputName) && app->currentRadioFavicon[0]) ?
		app->currentRadioFavicon : app->inputName;
	end = strchr(source, '?');
	if (!end) end = source + strlen(source);
	base = end;
	while (base > source && base[-1] != '/' && base[-1] != ':')
		base--;
	for (i = 0, j = 0; base + i < end && base[i] && j < (int)sizeof(safe) - 1; i++) {
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


static int MrUrlIsJpeg(const char *url)
{
	const char *q, *dot;
	int len;
	if (!url || !url[0]) return 0;
	q = strchr(url, '?');
	len = (int)(q ? (q - url) : (int)strlen(url));
	dot = url + len;
	while (dot > url && dot[-1] != '/' && dot[-1] != ':') dot--;
	while (*dot && dot < url + len && *dot != '.') dot++;
	return (dot < url + len) &&
		(ContainsTextNoCase(dot, ".jpg") || ContainsTextNoCase(dot, ".jpeg"));
}

static int MrUrlIsPng(const char *url)
{
	const char *q, *dot;
	int len;
	if (!url || !url[0]) return 0;
	q = strchr(url, '?');
	len = (int)(q ? (q - url) : (int)strlen(url));
	dot = url + len;
	while (dot > url && dot[-1] != '/' && dot[-1] != ':') dot--;
	while (*dot && dot < url + len && *dot != '.') dot++;
	return (dot < url + len) && ContainsTextNoCase(dot, ".png");
}

/* Uppercased file extension (without the dot) of a URL's path, e.g. "PNG"
 * for ".../icon.png?x=1".  Empty if there's no recognizable extension.
 * Used only to label the artwork placeholder so it's visible at a glance
 * which favicon type was rejected (".ico", ".png", ...). */
static void MrUrlExtensionUpper(const char *url, char *out, int outSize)
{
	const char *q, *dot;
	int len, i, j;
	if (!out || outSize <= 0) return;
	out[0] = '\0';
	if (!url || !url[0]) return;
	q = strchr(url, '?');
	len = (int)(q ? (q - url) : (int)strlen(url));
	dot = url + len;
	while (dot > url && dot[-1] != '/' && dot[-1] != ':') dot--;
	while (*dot && dot < url + len && *dot != '.') dot++;
	if (dot >= url + len) return;
	dot++;
	for (i = 0, j = 0; dot + i < url + len && dot[i] && j < outSize - 1; i++) {
		unsigned char c = (unsigned char)dot[i];
		if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			out[j++] = (char)c;
		else
			break;
	}
	out[j] = '\0';
}

#if ENABLE_RADIO_ARTWORK
static int MrContentTypeIsJpeg(const char *contentType)
{
	return contentType && (ContainsTextNoCase(contentType, "image/jpeg") ||
		ContainsTextNoCase(contentType, "image/jpg"));
}

static int MrIsJpegMagic(const unsigned char *data, int bytes)
{
	return bytes >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

#if ENABLE_PNG_ARTWORK
static int MrContentTypeIsPng(const char *contentType)
{
	return contentType && ContainsTextNoCase(contentType, "image/png");
}

static int MrIsPngMagic(const unsigned char *data, int bytes)
{
	static const unsigned char sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	return bytes >= 8 && memcmp(data, sig, sizeof(sig)) == 0;
}

/* Decodes a PNG favicon via lodepng into the same downsampled grey/RGB
 * thumbnail buffers DecodeJpegToGrey() produces, so the rest of the
 * artwork pipeline (cache, dithered/colour rendering) doesn't need to
 * know which decoder ran.  lodepng_inspect() reads just the IHDR header
 * first so an oversized PNG is rejected before lodepng_decode32() would
 * malloc width*height*4 bytes for it -- the 256KB download cap bounds the
 * compressed input, but a small PNG can still declare huge dimensions. */
static int DecodePngToGrey(const unsigned char *pngData, unsigned long pngBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	LodePNGState state;
	unsigned pw = 0, ph = 0, err;
	unsigned char *image;
	static unsigned char xMap[MR_MAX_JPEG_DIM], yMap[MR_MAX_JPEG_DIM];
	static unsigned long greyAccum[MR_ART_W * MR_ART_H];
	static unsigned long rAccum[MR_ART_W * MR_ART_H];
	static unsigned long gAccum[MR_ART_W * MR_ART_H];
	static unsigned long bAccum[MR_ART_W * MR_ART_H];
	static unsigned short greyCount[MR_ART_W * MR_ART_H];
	unsigned x, y;
	int i;

	if (!pngData || pngBytes <= 8 || !greyOut ||
		outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H)
		return -1;

	lodepng_state_init(&state);
	err = lodepng_inspect(&pw, &ph, &state, pngData, (size_t)pngBytes);
	lodepng_state_cleanup(&state);
	if (err) {
		RADIO_DBG(printf("radio-art: lodepng_inspect failed err=%u (%s)\n",
			err, lodepng_error_text(err));)
		return -1;
	}
	if (pw == 0 || ph == 0 || pw > MR_MAX_JPEG_DIM || ph > MR_MAX_JPEG_DIM) {
		RADIO_DBG(printf("radio-art: png dimensions out of range %ux%u (max %d)\n",
			pw, ph, MR_MAX_JPEG_DIM);)
		return -1;
	}

	image = NULL;
	err = lodepng_decode32(&image, &pw, &ph, pngData, (size_t)pngBytes);
	if (err || !image) {
		RADIO_DBG(printf("radio-art: lodepng_decode32 failed err=%u (%s)\n",
			err, lodepng_error_text(err));)
		if (image) free(image);
		return -1;
	}
	RADIO_DBG(printf("radio-art: png %ux%u decoded ok\n", pw, ph);)

	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(rAccum, 0, sizeof(rAccum));
	memset(gAccum, 0, sizeof(gAccum));
	memset(bAccum, 0, sizeof(bAccum));
	memset(greyCount, 0, sizeof(greyCount));

	for (x = 0; x < pw; x++) xMap[x] = (unsigned char)(((unsigned long)x * (unsigned long)outW) / pw);
	for (y = 0; y < ph; y++) yMap[y] = (unsigned char)(((unsigned long)y * (unsigned long)outH) / ph);

	/* Alpha is ignored (treated as opaque), matching DecodeJpegToGrey's
	 * source format -- favicons are rarely meaningfully transparent. */
	for (y = 0; y < ph; y++) {
		const unsigned char *row = image + 4UL * (unsigned long)y * (unsigned long)pw;
		int dstY = yMap[y];
		for (x = 0; x < pw; x++) {
			const unsigned char *px = row + 4 * x;
			unsigned char r = px[0], g = px[1], b = px[2];
			int dst = dstY * outW + xMap[x];
			greyAccum[dst] += (77UL * r + 150UL * g + 29UL * b + 128UL) >> 8;
			rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
			if (greyCount[dst] != 0xffff) greyCount[dst]++;
		}
	}
	free(image);

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
#endif /* ENABLE_PNG_ARTWORK */

/* Favicon artwork is fetched from the Radio Browser station's "favicon"
 * field only, never from the MP3/ICY stream, and only once playback has
 * already started picking a station (see RadioDoProbeAndPlay).  The fetch
 * goes through rb_probe_fetch_binary(), which shares its HTTP/HTTPS/AmiSSL
 * connection handling with the stream probe used to start playback but is
 * a separate code path that the probe's stream-playback logic never calls
 * into -- so this never touches the MP3 stream's SSL read loop.  Any
 * failure here (bad URL, unsupported TLS, oversized body, unsupported
 * format, broken decode) just leaves artValid 0 and never affects
 * playback.  JPEG and PNG are tried in that order based on URL extension
 * or Content-Type, each independently confirmed via magic bytes before
 * decoding; anything else (ICO, WebP, SVG, ...) is silently rejected. */
static int LoadRadioFaviconImage(MrApp *app)
{
	char contentType[64];
	static unsigned char response[MR_FAVICON_MAX_BYTES];
	int bytes = 0;
	int rc;
	if (!app || !app->currentRadioFavicon[0]) {
		RADIO_DBG(printf("radio-art: no favicon URL for current station\n");)
		return 0;
	}
	RADIO_DBG(printf("radio-art: fetching favicon url=%s\n", app->currentRadioFavicon);)
	rc = rb_probe_fetch_binary(app->currentRadioFavicon, response, (int)sizeof(response),
		&bytes, contentType, (int)sizeof(contentType));
	if (rc != RB_STREAM_PROBE_OK) {
		RADIO_DBG(printf("radio-art: fetch failed rc=%d (%s)\n", rc, rb_probe_error_text(rc));)
		return 0;
	}
	RADIO_DBG(printf("radio-art: fetched %d bytes content-type=\"%s\"\n", bytes, contentType);)
	if (bytes <= 8)
		return 0;

	if (MrUrlIsJpeg(app->currentRadioFavicon) || MrContentTypeIsJpeg(contentType)) {
		if (!MrIsJpegMagic(response, bytes)) {
			RADIO_DBG(printf("radio-art: rejected, claimed JPEG but first bytes are not FF D8 FF (%02X %02X %02X)\n",
				response[0], response[1], response[2]);)
			return 0;
		}
		if (DecodeJpegToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H, 0) != 0) {
			RADIO_DBG(printf("radio-art: picojpeg decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#if ENABLE_PNG_ARTWORK
	if (MrUrlIsPng(app->currentRadioFavicon) || MrContentTypeIsPng(contentType)) {
		if (!MrIsPngMagic(response, bytes)) {
			RADIO_DBG(printf("radio-art: rejected, claimed PNG but signature bytes don't match\n");)
			return 0;
		}
		if (DecodePngToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H) != 0) {
			RADIO_DBG(printf("radio-art: lodepng decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#endif
	RADIO_DBG(printf("radio-art: rejected, unsupported favicon type (url and content-type don't look like JPEG or PNG)\n");)
	return 0;
}
#endif /* ENABLE_RADIO_ARTWORK */

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
#if ENABLE_RADIO_ARTWORK
	} else if (app->artEnabled && MrIsRadioInput(app->inputName) && LoadRadioFaviconImage(app)) {
		SaveArtworkCache(app);
#endif
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
		/* Paint our own placeholder panel rather than relying on the button's
		 * text, which the layout can repaint over (leaving it blank).  For
		 * radio input, label it with what we actually tried so it's visible
		 * at a glance whether the favicon pipeline ran at all: "Blank" when
		 * the station has no favicon URL, or "No art" plus the rejected file
		 * type, so a station icon swap is visibly noticed even when nothing
		 * renders.  The panel is only MR_ART_W (64px) wide, too narrow for
		 * "No art (WEBP)" on one line, so fall back to a second line for the
		 * extension rather than silently dropping the label when it
		 * wouldn't fit. */
		char line1[16], line2[16];
		int line1Len, line2Len, line1W, line2W;
		line2[0] = '\0';
		if (MrIsRadioInput(app->inputName)) {
			if (!app->currentRadioFavicon[0]) {
				SafeCopy(line1, sizeof(line1), "Blank");
			} else {
				char ext[16];
				MrUrlExtensionUpper(app->currentRadioFavicon, ext, sizeof(ext));
				SafeCopy(line1, sizeof(line1), "No art");
				if (ext[0]) sprintf(line2, "(%s)", ext);
			}
		} else {
			SafeCopy(line1, sizeof(line1), "No art");
		}
		SetAPen(rp, 0);
		RectFill(rp, ox, oy, ox + w - 1, oy + h - 1);
		line1Len = (int)strlen(line1);
		line2Len = (int)strlen(line2);
		line1W = line1Len > 0 ? TextLength(rp, line1, line1Len) : 0;
		line2W = line2Len > 0 ? TextLength(rp, line2, line2Len) : 0;
		SetAPen(rp, 1);
		if (line2Len > 0 && h >= 18) {
			/* Two centered lines: label on top, extension underneath. */
			if (line1W <= w) {
				Move(rp, ox + (w - line1W) / 2, oy + h / 2 - 2);
				Text(rp, line1, line1Len);
			}
			if (line2W <= w) {
				Move(rp, ox + (w - line2W) / 2, oy + h / 2 + 9);
				Text(rp, line2, line2Len);
			}
		} else if (line1Len > 0 && h >= 8 && line1W <= w) {
			Move(rp, ox + (w - line1W) / 2, oy + h / 2 + 2);
			Text(rp, line1, line1Len);
		}
	}
}

static void UpdateTimeDisplay(MrApp *app)
{
	char e[8], t[8], buf[24];
	if (MrIsRadioInput(app->inputName)) {
		sprintf(buf, "Live");
	} else {
		FormatTime(app->elapsedSecs, e); FormatTime(app->totalSecs > 0 ? app->totalSecs : -1, t);
		sprintf(buf, "%s / %s", e, t);
	}
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
		SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), "No file info");
		return;
	}
	if (MrIsRadioInput(app->inputName)) {
		app->rating = 0; app->totalSecs = 0; app->elapsedSecs = 0; app->lastFrames = 0;
		SetReadonlyString(app->titleGad, app->win, app->shownTitle, sizeof(app->shownTitle), "Internet Radio");
		SetReadonlyString(app->artistGad, app->win, app->shownArtist, sizeof(app->shownArtist), "-");
		SetAlbumDisplay(app, "Internet Radio");
		SetReadonlyString(app->trackGad, app->win, app->shownTrack, sizeof(app->shownTrack), "Live");
		SetReadonlyString(app->genreGad, app->win, app->shownGenre, sizeof(app->shownGenre), "-");
		SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), "Internet Stream");
		UpdateRatingDisplay(app); UpdateTimeDisplay(app); SetGauge(app, 0);
		/* Radio has no local MP3/ID3 art, only an optional station favicon,
		 * so this is the only place that drives LoadRadioFaviconImage() for
		 * radio input -- without it, switching stations (or "Reload Art
		 * from File") never re-attempts the favicon fetch, and any layout
		 * repaint of the placeholder gadget is never redrawn. */
		UpdateArtwork(app, NULL);
		SetStatus(app, app->artValid ? "Internet Radio ready." : "Internet Radio ready (No art).");
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
	SetReadonlyString(app->titleGad, app->win, app->shownTitle, sizeof(app->shownTitle), info.title[0] ? info.title : "-");
	SetReadonlyString(app->artistGad, app->win, app->shownArtist, sizeof(app->shownArtist), info.artist[0] ? info.artist : "-");
	SetAlbumDisplay(app, info.album[0] ? info.album : "-");
	SetReadonlyString(app->trackGad, app->win, app->shownTrack, sizeof(app->shownTrack), info.track[0] ? info.track : "-");
	SetReadonlyString(app->genreGad, app->win, app->shownGenre, sizeof(app->shownGenre), info.genre[0] ? info.genre : "-");
	SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), fileInfo);
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
ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|aif|aiff)",
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

enum {
	RB_GID_SEARCH_TEXT = 300,
	RB_GID_CODEC,
	RB_GID_COUNTRY,
	RB_GID_COUNTRY_CODE,
	RB_GID_RADIO_RESULTS,
	RB_GID_SEARCH,
	RB_GID_PROBE,
	RB_GID_ADD_FAV,
	RB_GID_FAVOURITES,
	RB_GID_CLOSE,
	RB_GID_SCHEME,
	RB_GID_LIMIT,
	RB_GID_BITRATE,
	RB_GID_UP,
	RB_GID_DOWN,
	RB_GID_STATUS
};


static const int kRadioSearchLimits[] = { 10, 25, 50, 100 };
static STRPTR kRadioSearchLimitLabels[] = { (STRPTR)"10", (STRPTR)"25", (STRPTR)"50", (STRPTR)"100", NULL };
#define MR_RADIO_SEARCH_LIMIT_COUNT ((int)(sizeof(kRadioSearchLimits) / sizeof(kRadioSearchLimits[0])))

/* Maps a limit value back to its kRadioSearchLimits[] cycle-gadget index;
 * picks the closest entry rather than the first/last bucket so adding more
 * limit choices doesn't need a hand-maintained chain of >=/<= comparisons. */
static int RadioSearchLimitIndex(int limit)
{
	int best = 0, bestDist, i;
	bestDist = limit > kRadioSearchLimits[0] ? limit - kRadioSearchLimits[0] : kRadioSearchLimits[0] - limit;
	for (i = 1; i < MR_RADIO_SEARCH_LIMIT_COUNT; i++) {
		int dist = limit > kRadioSearchLimits[i] ? limit - kRadioSearchLimits[i] : kRadioSearchLimits[i] - limit;
		if (dist < bestDist) { bestDist = dist; best = i; }
	}
	return best;
}
static const int kRadioBitrateMax[] = { -1, 56, 64, 96, 128 };
static STRPTR kRadioBitrateLabels[] = { (STRPTR)"Any", (STRPTR)"<=56", (STRPTR)"<=64", (STRPTR)"<=96", (STRPTR)"<=128", NULL };
static STRPTR kRadioSchemeLabels[] = { (STRPTR)"HTTP", (STRPTR)"HTTPS", (STRPTR)"All", NULL };
static STRPTR kRadioCountryLabels[] = { (STRPTR)"All", (STRPTR)"GB", (STRPTR)"US", (STRPTR)"FR", (STRPTR)"ZA", (STRPTR)"DE", (STRPTR)"NL", NULL };

static const char *RadioCountryFromIndex(int idx)
{
	switch (idx) {
	case 1: return "GB";
	case 2: return "US";
	case 3: return "FR";
	case 4: return "ZA";
	case 5: return "DE";
	case 6: return "NL";
	default: return "";
	}
}

static int RadioCountryToIndex(const char *countrycode)
{
	int i;
	if (!countrycode || !countrycode[0]) return 0;
	for (i = 1; kRadioCountryLabels[i]; i++)
		if (!strcmp(countrycode, (const char *)kRadioCountryLabels[i])) return i;
	return 0;
}

static const char *RadioBitrateFilterLabel(int max_bitrate)
{
	static char label[16];

	if (max_bitrate <= 0) return "Any";
	sprintf(label, "<=%d", max_bitrate);
	return label;
}

static const char *RadioCodecFromIndex(int idx)
{
	switch (idx) {
	case 1: return "MP3";
	case 2: return "AAC";
	case 3: return "AAC+";
	default: return "";
	}
}

static int RadioCodecToIndex(const char *codec)
{
	if (!codec || !codec[0]) return 0;
	if (!strcmp(codec, "MP3")) return 1;
	if (!strcmp(codec, "AAC")) return 2;
	if (!strcmp(codec, "AAC+")) return 3;
	return 0;
}

static const char *ProbeCodecName(RbStreamCodec codec)
{
	if (codec == RB_STREAM_CODEC_MP3) return "MP3";
	if (codec == RB_STREAM_CODEC_AAC) return "AAC";
	return "unknown";
}

static void RadioSetStatus(MrApp *app, const char *text)
{
	struct Gadget *gad;
	if (!app) return;
	SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), text ? text : "");
	if (!app->rbWin || !app->rbGadgets) return;
	gad = app->rbGadgets;
	while (gad && gad->GadgetID != RB_GID_STATUS) gad = gad->NextGadget;
	if (gad) {
		GT_SetGadgetAttrs(gad, app->rbWin, NULL,
			GTST_String, (ULONG)app->lastRadioError, TAG_DONE);
	}
}

static int RadioStationMatchesScheme(MrApp *app, const RadioBrowserStation *st)
{
	const char *url = rb_station_play_url(st);
	int isHttp, isHttps;
	if (!url) return 0;
	isHttp = strncmp(url, "http://", 7) == 0;
	isHttps = strncmp(url, "https://", 8) == 0;
	if (app && app->rbSchemeMode == 1) {
#if defined(HAVE_AMISSL)
		return isHttps;
#else
		return 0;
#endif
	}
	if (app && app->rbSchemeMode == 2) {
#if defined(HAVE_AMISSL)
		return isHttp || isHttps;
#else
		return isHttp;
#endif
	}
	return isHttp;
}

static void RadioRefreshResults(MrApp *app)
{
	int i, row;
	int selectedRow = -1;
	int wantedController = -1;
	int wantedFavourite = -1;
	char display[RB_MAX_NAME];
	const RadioBrowserStation *st;
	const char *url;
	const char *reason;
	if (!app) return;
	if (app->rbShowingFavourites)
		wantedFavourite = app->rbSelectedFavourite;
	else
		wantedController = app->rbController.selected_index;
	if (app->rbWin && app->rbGadList)
		GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL,
			GTLV_Labels, (ULONG)~0,
			GTLV_Selected, (ULONG)~0,
			TAG_DONE);
	NewList(&app->rbList);
	app->rbVisibleCount = 0;
	app->rbSelectedFavourite = -1;
	if (app->rbShowingFavourites) {
		for (i = 0; i < app->rbFavouriteCount && app->rbVisibleCount < RB_CONTROLLER_MAX_STATIONS; i++) {
			if (!app->rbFavouriteNames[i][0] || !app->rbFavouriteUrls[i][0]) continue;
			row = app->rbVisibleCount++;
			app->rbVisibleToController[row] = i;
			sprintf(app->rbNames[row], "%.48s | favourite", app->rbFavouriteNames[i]);
			if (i == wantedFavourite) selectedRow = row;
			memset(&app->rbNodes[row], 0, sizeof(app->rbNodes[row]));
			app->rbNodes[row].ln_Name = app->rbNames[row];
			app->rbNodes[row].ln_Type = NT_USER;
			app->rbNodes[row].ln_Pri = (BYTE)i;
			AddTail(&app->rbList, &app->rbNodes[row]);
		}
	} else {
		for (i = 0; i < app->rbController.station_count; i++) {
			st = rb_controller_get_station(&app->rbController, i);
			if (!st) continue;
			url = rb_station_play_url(st);
			reason = "show";
			if (!RadioStationMatchesScheme(app, st)) { reason = "hidden_scheme"; }
			else if (st->hls) { reason = "hidden_hls"; }
			else if (st->lastcheckok == 0) { reason = "hidden_offline"; }
			else if (st->ssl_error != 0) { reason = "hidden_ssl_error"; }
			else if (app->rbController.max_bitrate > 0 && st->bitrate == 0) { reason = "hidden_bitrate_unknown"; }
			else if (app->rbController.max_bitrate > 0 && st->bitrate > app->rbController.max_bitrate) { reason = "hidden_bitrate"; }
#ifdef MINIAMP3_DEBUG
			RADIO_DBG(printf("Radio Browser filter: name=\"%s\" scheme=%s codec=%s bitrate=%d max=%d country=%s hls=%d lastcheckok=%d ssl_error=%d reason=%s\n",
				st->name, url && strncmp(url, "https://", 8) == 0 ? "https" : (url && strncmp(url, "http://", 7) == 0 ? "http" : "other"),
				st->codec, st->bitrate, app->rbController.max_bitrate, st->countrycode,
				st->hls, st->lastcheckok, st->ssl_error, reason);)
#endif
			if (reason[0] != 's') continue;
			row = app->rbVisibleCount++;
			app->rbVisibleToController[row] = i;
			if (i == wantedController) selectedRow = row;
			rb_station_display_name(st, display, (int)sizeof(display));
			sprintf(app->rbNames[row], "%.48s | %s | %d | %s",
				display, st->codec, st->bitrate, st->countrycode);
			memset(&app->rbNodes[row], 0, sizeof(app->rbNodes[row]));
			app->rbNodes[row].ln_Name = app->rbNames[row];
			app->rbNodes[row].ln_Type = NT_USER;
			app->rbNodes[row].ln_Pri = (BYTE)i;
			AddTail(&app->rbList, &app->rbNodes[row]);
		}
	}
	if (app->rbVisibleCount <= 0) {
		app->rbController.selected_index = -1;
		app->rbSelectedFavourite = -1;
		selectedRow = -1;
	} else if (selectedRow < 0) {
		selectedRow = 0;
		if (app->rbShowingFavourites)
			app->rbSelectedFavourite = app->rbVisibleToController[0];
		else
			rb_controller_set_selected(&app->rbController, app->rbVisibleToController[0]);
	} else if (app->rbShowingFavourites) {
		app->rbSelectedFavourite = app->rbVisibleToController[selectedRow];
	} else {
		rb_controller_set_selected(&app->rbController, app->rbVisibleToController[selectedRow]);
	}
	if (app->rbWin && app->rbGadList) {
		GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL,
			GTLV_Labels, (ULONG)&app->rbList,
			GTLV_Selected, selectedRow >= 0 ? (ULONG)selectedRow : (ULONG)~0,
			TAG_DONE);
		RefreshGList(app->rbGadList, app->rbWin, NULL, 1);
	}
}

static struct Gadget *FindRadioGadget(MrApp *app, UWORD id)
{
	struct Gadget *gad = app->rbGadgets;
	while (gad) {
		if (gad->GadgetID == id) return gad;
		gad = gad->NextGadget;
	}
	return NULL;
}

static void RadioDoSearch(MrApp *app)
{
	struct Gadget *nameGad = FindRadioGadget(app, RB_GID_SEARCH_TEXT);
	struct Gadget *codecGad = FindRadioGadget(app, RB_GID_CODEC);
	struct Gadget *countryGad = FindRadioGadget(app, RB_GID_COUNTRY);
	struct Gadget *countryCodeGad = FindRadioGadget(app, RB_GID_COUNTRY_CODE);
	struct Gadget *limitGad = FindRadioGadget(app, RB_GID_LIMIT);
	struct Gadget *bitrateGad = FindRadioGadget(app, RB_GID_BITRATE);
	STRPTR text;
	ULONG v;
	int rc;
	char filterMsg[192];

	if (app->rbSearchInProgress) {
		RadioSetStatus(app, "Search already running.");
		return;
	}
	app->rbSearchInProgress = 1;
	RadioSetStatus(app, "Searching Radio Browser...");
	text = NULL;
	GT_GetGadgetAttrs(nameGad, app->rbWin, NULL, GTST_String, (ULONG)(void *)&text, TAG_DONE);
	SafeCopy(app->rbController.name, sizeof(app->rbController.name), text ? (const char *)text : "");
	v = 0;
	GT_GetGadgetAttrs(codecGad, app->rbWin, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
	SafeCopy(app->rbController.codec, sizeof(app->rbController.codec), RadioCodecFromIndex((int)v));
	text = NULL;
	GT_GetGadgetAttrs(countryGad, app->rbWin, NULL, GTST_String, (ULONG)(void *)&text, TAG_DONE);
	SafeCopy(app->rbController.countrycode, sizeof(app->rbController.countrycode), text ? (const char *)text : "");
	v = 0;
	if (countryCodeGad)
		GT_GetGadgetAttrs(countryCodeGad, app->rbWin, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
	app->rbCountryMode = ClampInt((int)v, 0, 6);
	if (app->rbCountryMode > 0)
		SafeCopy(app->rbController.countrycode, sizeof(app->rbController.countrycode), RadioCountryFromIndex(app->rbCountryMode));
	v = 1;
	if (limitGad)
		GT_GetGadgetAttrs(limitGad, app->rbWin, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
	app->rbController.limit = kRadioSearchLimits[ClampInt((int)v, 0, MR_RADIO_SEARCH_LIMIT_COUNT - 1)];
	v = 0;
	if (bitrateGad)
		GT_GetGadgetAttrs(bitrateGad, app->rbWin, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
	app->rbController.max_bitrate = kRadioBitrateMax[ClampInt((int)v, 0, 4)];
	sprintf(filterMsg, "Search filters: name=\"%.40s\" codec=%s country=%s max bitrate=%s limit=%d",
		app->rbController.name[0] ? app->rbController.name : "Any",
		app->rbController.codec[0] ? app->rbController.codec : "Any",
		app->rbController.countrycode[0] ? app->rbController.countrycode : "Any",
		RadioBitrateFilterLabel(app->rbController.max_bitrate),
		app->rbController.limit);
	RadioSetStatus(app, filterMsg);
#ifdef MINIAMP3_DEBUG
	printf("%s\n", filterMsg);
#endif
	rc = rb_controller_search(&app->rbController);
	app->rbSearchInProgress = 0;
	app->rbShowingFavourites = FALSE;
	RadioRefreshResults(app);
	if (rc < 0)
		RadioSetStatus(app, app->rbController.last_error);
	else {
		char msg[128];
		int hidden = app->rbController.raw_station_count - app->rbVisibleCount;
		if (app->rbVisibleCount == 0 && app->rbController.raw_station_count > 0)
			sprintf(msg, "No stations found after filters");
		else if (app->rbVisibleCount == 0 && app->rbController.raw_station_count == 0)
			sprintf(msg, "No stations found");
		else
			sprintf(msg, "Found %d stations, showing %d playable (%d hidden)",
				app->rbController.raw_station_count, app->rbVisibleCount, hidden < 0 ? 0 : hidden);
		RadioSetStatus(app, msg);
	}
}

static int RadioCurrentSelectedRow(MrApp *app)
{
	int i, wanted;
	if (!app || app->rbVisibleCount <= 0) return -1;
	wanted = app->rbShowingFavourites ? app->rbSelectedFavourite : app->rbController.selected_index;
	for (i = 0; i < app->rbVisibleCount; i++)
		if (app->rbVisibleToController[i] == wanted)
			return i;
	return -1;
}

static void RadioMoveSelection(MrApp *app, int delta)
{
	int row;
	if (!app || !app->rbWin || !app->rbGadList) return;
	if (app->rbVisibleCount <= 0) {
		RadioSetStatus(app, "No stations to select.");
		return;
	}
	row = RadioCurrentSelectedRow(app);
	if (row < 0) row = 0;
	else row += delta;
	if (row < 0) row = 0;
	if (row >= app->rbVisibleCount) row = app->rbVisibleCount - 1;
	RadioSelectResult(app, (ULONG)row);
}

static void RadioSelectResult(MrApp *app, ULONG eventSelected)
{
	ULONG selected = eventSelected;
	ULONG row;
	const RadioBrowserStation *st;
	char display[RB_MAX_NAME];
	char msg[RB_MAX_NAME + 16];

	if (!app->rbWin || !app->rbGadList) return;
	if (selected == (ULONG)~0)
		GT_GetGadgetAttrs(app->rbGadList, app->rbWin, NULL,
			GTLV_Selected, (ULONG)&selected, TAG_DONE);
#ifdef MINIAMP3_DEBUG
	RADIO_DBG(printf("radio results selection event row/index: %ld\n", (long)selected);)
#endif
	if (selected == (ULONG)~0 || selected >= (ULONG)app->rbVisibleCount) {
		app->rbSelectedFavourite = -1;
		rb_controller_set_selected(&app->rbController, -1);
#ifdef MINIAMP3_DEBUG
		RADIO_DBG(printf("radio results controller selected_index: %d\n", app->rbController.selected_index);)
#endif
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	row = selected;
	selected = (ULONG)app->rbVisibleToController[row];
	if (app->rbShowingFavourites) {
		app->rbSelectedFavourite = (int)selected;
		GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL, GTLV_Selected, (ULONG)row, TAG_DONE);
		RefreshGList(app->rbGadList, app->rbWin, NULL, 1);
		sprintf(msg, "Selected favourite: %.120s", app->rbFavouriteNames[app->rbSelectedFavourite]);
		RadioSetStatus(app, msg);
		return;
	}
	app->rbSelectedFavourite = -1;
	if (rb_controller_set_selected(&app->rbController, (int)selected) < 0) {
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL,
		GTLV_Selected, (ULONG)row, TAG_DONE);
	RefreshGList(app->rbGadList, app->rbWin, NULL, 1);
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	rb_station_display_name(st, display, (int)sizeof(display));
#ifdef MINIAMP3_DEBUG
	RADIO_DBG(printf("radio results controller selected_index: %d\n", app->rbController.selected_index);)
	RADIO_DBG(printf("radio results station display name: %s\n", display);)
#endif
	sprintf(msg, "Selected: %.120s", display);
	RadioSetStatus(app, msg);
}

static void RadioAddFavourite(MrApp *app)
{
	const RadioBrowserStation *st;
	const char *url;
	char display[RB_MAX_NAME];
	char msg[160];
	int i;
	if (app->rbController.selected_index < 0) {
		RadioSetStatus(app, "Select a search result to favourite.");
		return;
	}
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a search result to favourite.");
		return;
	}
	url = rb_station_play_url(st);
	if (!url || !url[0]) {
		RadioSetStatus(app, "Selected station has no URL.");
		return;
	}
	rb_station_display_name(st, display, (int)sizeof(display));
	for (i = 0; i < app->rbFavouriteCount; i++) {
		if (!strcmp(app->rbFavouriteUrls[i], url)) {
			SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), display);
			SaveSettings(app);
			RadioSetStatus(app, "Favourite updated.");
			return;
		}
	}
	if (app->rbFavouriteCount >= MR_RADIO_FAV_MAX) {
		RadioSetStatus(app, "Radio favourites are full.");
		return;
	}
	i = app->rbFavouriteCount++;
	SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), display);
	SafeCopy(app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]), url);
	SaveSettings(app);
	sprintf(msg, "Added favourite: %.120s", display);
	RadioSetStatus(app, msg);
}

static void RadioToggleFavourites(MrApp *app)
{
	app->rbShowingFavourites = app->rbShowingFavourites ? FALSE : TRUE;
	RadioRefreshResults(app);
	RadioSetStatus(app, app->rbShowingFavourites ? "Showing radio favourites." : "Showing search results.");
}

static void RadioDoProbeAndPlay(MrApp *app)
{
	static unsigned char peek[512];
	RbStreamInfo info;
	int peekLen = 0;
	int rc;
	const RadioBrowserStation *st;
	char msg[512];
	RADIO_DBG(printf("radio-ui: play requested currentActive=%d donePending=%d stopRequested=%d state=%s input=\"%s\"\n",
		app->playbackActive, app->playbackDonePending, gPlayer.stopRequested, MrStreamStateName(app->streamState), app->inputName);)
	if (app->streamState == MR_STREAM_STARTING) { RadioSetStatus(app, "Still starting previous stream"); return; }
	if (app->streamState == MR_STREAM_STOP_REQUESTED || app->streamState == MR_STREAM_STOPPING || app->streamState == MR_STREAM_STOP_TIMEOUT) {
		SafeCopy(app->queuedStreamUrl, sizeof(app->queuedStreamUrl), "radio-selection");
		RadioSetStatus(app, "Queued stream; waiting for previous stream to stop...");
		return;
	}
	if (app->rbShowingFavourites) {
		if (app->rbSelectedFavourite >= 0 && app->rbSelectedFavourite < app->rbFavouriteCount &&
			app->playbackActive && MrIsRadioInput(app->inputName) &&
			!strcmp(app->inputName, app->rbFavouriteUrls[app->rbSelectedFavourite])) {
			RadioSetStatus(app, "Selected stream is already playing.");
			MrDebugSession("parent play same URL ignored", app);
			return;
		}
	} else if (app->rbController.selected_index >= 0) {
		st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
		if (st && rb_station_play_url(st) && app->playbackActive && MrIsRadioInput(app->inputName) &&
			!strcmp(app->inputName, rb_station_play_url(st))) {
			RadioSetStatus(app, "Selected stream is already playing.");
			MrDebugSession("parent play same URL ignored", app);
			return;
		}
	}
	if ((app->playbackActive || app->playbackDonePending || PlaybackProcessStillExists()) &&
		MrIsRadioInput(app->inputName)) {
		SafeCopy(app->queuedStreamUrl, sizeof(app->queuedStreamUrl), "radio-selection");
		RadioSetStatus(app, "Queued stream; stopping previous stream...");
		RADIO_DBG(printf("radio-ui: queued stream while stopping old stream\n");)
		if (!gPlayer.stopRequested)
			StopPlayback(app);
		return;
	}
	if (app->rbShowingFavourites) {
		if (app->rbSelectedFavourite < 0 || app->rbSelectedFavourite >= app->rbFavouriteCount) {
			RadioSetStatus(app, "Select a favourite first.");
			return;
		}
		SafeCopy(app->inputName, sizeof(app->inputName), app->rbFavouriteUrls[app->rbSelectedFavourite]);
		app->currentRadioFavicon[0] = '\0';
		app->haveRadioHostAddr = 0;
		app->radioHostAddrBe = 0;
		UpdateFileGadget(app);
		RefreshFileInfoAndTags(app);
		SafeCopy(app->currentRadioStationName, sizeof(app->currentRadioStationName), app->rbFavouriteNames[app->rbSelectedFavourite]);
		sprintf(msg, "Buffering - %.120s", app->rbFavouriteNames[app->rbSelectedFavourite]);
		RadioSetStatus(app, msg);
		StartPlayback(app);
		return;
	}
	if (app->rbController.selected_index < 0) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
#if !defined(HAVE_AMISSL)
	if (rb_station_play_url(st) && strncmp(rb_station_play_url(st), "https://", 8) == 0) {
		RadioSetStatus(app, "HTTPS not supported in this build");
		return;
	}
#endif
	memset(&info, 0, sizeof(info));
	RadioSetStatus(app, "Connecting...");
	RADIO_DBG(printf("radio-ui: new stream probe start url=\"%s\"\n", rb_station_play_url(st));)
	rc = rb_controller_probe_selected(&app->rbController, &info, peek, (int)sizeof(peek), &peekLen);
	RADIO_DBG(printf("radio-ui: new stream probe result rc=%d final=\"%s\" content=\"%s\" codec=%d redirects=%d\n",
		rc, info.final_url, info.content_type, (int)info.codec, info.redirect_count);)
	if (rc < 0) {
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	if (info.codec != RB_STREAM_CODEC_MP3 && info.codec != RB_STREAM_CODEC_AAC) {
		sprintf(msg, "Unsupported stream codec: %s (%.48s)", ProbeCodecName(info.codec), info.content_type);
		RadioSetStatus(app, msg);
		return;
	}
	if (!info.final_url[0]) {
		RadioSetStatus(app, "Stream probe did not return a playable URL.");
		return;
	}
#if defined(AMIGA_M68K)
	if (app->lastCompletedWasHttps && strncmp(info.final_url, "https://", 8) == 0 &&
		!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists()) {
		RADIO_DBG(printf("radio-done: Delay(4) between fully completed HTTPS sessions before starting next HTTPS URL\n");)
		RadioSetStatus(app, "Waiting briefly before next HTTPS stream...");
		Delay(4);
	}
#endif
	SafeCopy(app->inputName, sizeof(app->inputName), info.final_url);
	SafeCopy(app->currentRadioFavicon, sizeof(app->currentRadioFavicon), st->favicon);
	RADIO_DBG(printf("radio-art: station favicon=\"%s\"\n", app->currentRadioFavicon);)
	app->haveRadioHostAddr = info.have_host_addr;
	app->radioHostAddrBe = info.host_addr_be;
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	rb_station_display_name(st, msg, (int)sizeof(msg));
	SafeCopy(app->currentRadioStationName, sizeof(app->currentRadioStationName), msg);
	sprintf(msg, "Buffering - %.140s", app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio");
	RadioSetStatus(app, msg);
	RADIO_DBG(printf("radio-ui: new stream start url=\"%s\"\n", info.final_url);)
	StartPlayback(app);
}

static void CloseRadioWindow(MrApp *app)
{
	struct IntuiMessage *msg;
	if (!app->rbWin) return;
	if (app->rbGadList)
		GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL,
			GTLV_Labels, (ULONG)~0,
			GTLV_Selected, (ULONG)~0, TAG_DONE);
	ModifyIDCMP(app->rbWin, 0);
	while ((msg = GT_GetIMsg(app->rbWin->UserPort)) != NULL)
		GT_ReplyIMsg(msg);
	if (app->rbGadgets)
		RemoveGList(app->rbWin, app->rbGadgets, -1);
	CloseWindow(app->rbWin);
	app->rbWin = NULL;
	if (app->rbGadgets) {
		FreeGadgets(app->rbGadgets);
		app->rbGadgets = NULL;
		app->rbGadContext = NULL;
		app->rbGadList = NULL;
	}
	if (app->rbVisualInfo) {
		FreeVisualInfo(app->rbVisualInfo);
		app->rbVisualInfo = NULL;
	}
}

static void OpenRadioWindow(MrApp *app)
{
	struct NewWindow nw;
	struct NewGadget ng;
	struct Gadget *gad;
	static STRPTR codecs[] = { (STRPTR)"All", (STRPTR)"MP3", (STRPTR)"AAC", (STRPTR)"AAC+", NULL };
	if (app->rbWin || !app->win || !GadToolsBase) return;
	if (app->rbController.limit <= 0) {
		rb_controller_init(&app->rbController);
		app->rbShowHttps = FALSE;
		app->rbSchemeMode = 0; /* Default to HTTP; HTTPS remains optional/heavier on real hardware. */
		app->rbCountryMode = 0;
	}
	app->rbCountryMode = RadioCountryToIndex(app->rbController.countrycode);
	app->rbShowingFavourites = FALSE;
	app->rbSelectedFavourite = -1;
	app->rbVisualInfo = GetVisualInfoA(app->win->WScreen, NULL);
	if (!app->rbVisualInfo) return;
	app->rbGadContext = CreateContext(&app->rbGadgets);
	if (!app->rbGadContext) goto fail;
	NewList(&app->rbList);
	gad = app->rbGadContext;
	memset(&ng, 0, sizeof(ng));
	ng.ng_VisualInfo = app->rbVisualInfo; ng.ng_Flags = PLACETEXT_LEFT;
	/* Keep the radio dialog arranged as a clear vertical stack with generous
	 * outer/inner spacing: search+codec row, country row, results, buttons,
	 * and a bottom status line.  The larger top margin prevents the first row
	 * from clipping into the draggable title bar on ReAction/ClassAct systems.
	 */
	ng.ng_LeftEdge = 88; ng.ng_TopEdge = 24; ng.ng_Width = 220; ng.ng_Height = 18; ng.ng_GadgetText = (UBYTE *)"Search";
	ng.ng_GadgetID = RB_GID_SEARCH_TEXT; gad = CreateGadget(STRING_KIND, gad, &ng, GTST_String, (ULONG)app->rbController.name, GTST_MaxChars, RB_MAX_NAME, GA_RelVerify, TRUE, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 390; ng.ng_TopEdge = 24; ng.ng_Width = 90; ng.ng_GadgetText = (UBYTE *)"Codec"; ng.ng_GadgetID = RB_GID_CODEC;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)codecs, GTCY_Active, RadioCodecToIndex(app->rbController.codec), TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 88; ng.ng_TopEdge = 52; ng.ng_Width = 120; ng.ng_GadgetText = (UBYTE *)"Country";
	ng.ng_GadgetID = RB_GID_COUNTRY; gad = CreateGadget(STRING_KIND, gad, &ng, GTST_String, (ULONG)app->rbController.countrycode, GTST_MaxChars, RB_MAX_COUNTRY, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 270; ng.ng_TopEdge = 52; ng.ng_Width = 54; ng.ng_GadgetText = (UBYTE *)"Code"; ng.ng_GadgetID = RB_GID_COUNTRY_CODE; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioCountryLabels, GTCY_Active, app->rbCountryMode, GA_RelVerify, TRUE, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 372; ng.ng_TopEdge = 52; ng.ng_Width = 108; ng.ng_GadgetText = (UBYTE *)"URL"; ng.ng_GadgetID = RB_GID_SCHEME; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioSchemeLabels, GTCY_Active, app->rbSchemeMode, GA_RelVerify, TRUE, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 88; ng.ng_TopEdge = 80; ng.ng_Width = 90; ng.ng_GadgetText = (UBYTE *)"Limit"; ng.ng_GadgetID = RB_GID_LIMIT; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioSearchLimitLabels, GTCY_Active, RadioSearchLimitIndex(app->rbController.limit), TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 288; ng.ng_TopEdge = 80; ng.ng_Width = 90; ng.ng_GadgetText = (UBYTE *)"Max kbps"; ng.ng_GadgetID = RB_GID_BITRATE; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioBitrateLabels,
		GTCY_Active, app->rbController.max_bitrate <= 0 ? 0 : (app->rbController.max_bitrate <= 56 ? 1 : (app->rbController.max_bitrate <= 64 ? 2 : (app->rbController.max_bitrate <= 96 ? 3 : 4))), TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 8; ng.ng_TopEdge = 110; ng.ng_Width = 472; ng.ng_Height = 116; ng.ng_GadgetText = NULL; ng.ng_GadgetID = RB_GID_RADIO_RESULTS; ng.ng_Flags = 0;
	app->rbGadList = gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
		GTLV_Labels, (ULONG)&app->rbList,
		GTLV_Selected, (ULONG)~0,
		GA_RelVerify, TRUE, TAG_DONE); if (!gad) goto fail;
	ng.ng_TopEdge = 236; ng.ng_Height = 18; ng.ng_Flags = PLACETEXT_IN;
	ng.ng_LeftEdge = 8; ng.ng_Width = 58; ng.ng_GadgetText = (UBYTE *)"Search"; ng.ng_GadgetID = RB_GID_SEARCH; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 72; ng.ng_Width = 50; ng.ng_GadgetText = (UBYTE *)"Play"; ng.ng_GadgetID = RB_GID_PROBE; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 128; ng.ng_Width = 64; ng.ng_GadgetText = (UBYTE *)"Add Fav"; ng.ng_GadgetID = RB_GID_ADD_FAV; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 198; ng.ng_Width = 76; ng.ng_GadgetText = (UBYTE *)"Favourites"; ng.ng_GadgetID = RB_GID_FAVOURITES; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 280; ng.ng_Width = 44; ng.ng_GadgetText = (UBYTE *)"Up"; ng.ng_GadgetID = RB_GID_UP; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 330; ng.ng_Width = 56; ng.ng_GadgetText = (UBYTE *)"Down"; ng.ng_GadgetID = RB_GID_DOWN; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 392; ng.ng_Width = 70; ng.ng_GadgetText = (UBYTE *)"Close"; ng.ng_GadgetID = RB_GID_CLOSE; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 8; ng.ng_TopEdge = 264; ng.ng_Width = 472; ng.ng_GadgetText = NULL; ng.ng_GadgetID = RB_GID_STATUS; ng.ng_Flags = 0;
	gad = CreateGadget(STRING_KIND, gad, &ng, GTST_String, (ULONG)(app->lastRadioError[0] ? app->lastRadioError : "Ready."), GTST_MaxChars, 512, TAG_DONE); if (!gad) goto fail;
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = app->win->LeftEdge + 30; nw.TopEdge = app->win->TopEdge + 30;
	nw.Width = 496; nw.Height = 306;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH;
	nw.Title = (UBYTE *)"Internet Radio";
	nw.MinWidth = nw.MaxWidth = 496; nw.MinHeight = nw.MaxHeight = 306;
	nw.Type = WBENCHSCREEN;
	app->rbWin = OpenWindowTags(&nw, TAG_DONE);
	if (!app->rbWin) goto fail;
	AddGList(app->rbWin, app->rbGadgets, (UWORD)-1, -1, NULL);
	RefreshGList(app->rbGadgets, app->rbWin, NULL, -1);
	GT_RefreshWindow(app->rbWin, NULL);
	RadioRefreshResults(app);
	return;
fail:
	CloseRadioWindow(app);
}

static void HandleRadioWindow(MrApp *app)
{
	struct IntuiMessage *msg;
	if (!app->rbWin) return;
	while ((msg = GT_GetIMsg(app->rbWin->UserPort)) != NULL) {
		ULONG cls = msg->Class;
		UWORD code = msg->Code;
		struct Gadget *gad = (struct Gadget *)msg->IAddress;
		UWORD gid = gad ? gad->GadgetID : 0;
		GT_ReplyIMsg(msg);
		if (cls == IDCMP_CLOSEWINDOW) { CloseRadioWindow(app); return; }
		if (cls == IDCMP_REFRESHWINDOW) {
			GT_BeginRefresh(app->rbWin);
			GT_EndRefresh(app->rbWin, TRUE);
			continue;
		}
		if (cls == IDCMP_VANILLAKEY && code == 13) {
			RadioDoSearch(app);
			continue;
		}
		if (cls == IDCMP_GADGETUP) {
			if (gid == RB_GID_SEARCH_TEXT && code == 13)
				RadioDoSearch(app);
			else if (gid == RB_GID_RADIO_RESULTS)
				RadioSelectResult(app, (ULONG)code);
			else if (gid == RB_GID_SEARCH)
				RadioDoSearch(app);
			else if (gid == RB_GID_PROBE)
				RadioDoProbeAndPlay(app);
			else if (gid == RB_GID_ADD_FAV)
				RadioAddFavourite(app);
			else if (gid == RB_GID_FAVOURITES)
				RadioToggleFavourites(app);
			else if (gid == RB_GID_UP)
				RadioMoveSelection(app, -1);
			else if (gid == RB_GID_DOWN)
				RadioMoveSelection(app, 1);
			else if (gid == RB_GID_SCHEME) {
				ULONG active = 0;
				GT_GetGadgetAttrs(gad, app->rbWin, NULL, GTCY_Active, (ULONG)&active, TAG_DONE);
				app->rbSchemeMode = ClampInt((int)active, 0, 2);
				app->rbShowHttps = (app->rbSchemeMode != 0);
				RadioRefreshResults(app);
			}
			else if (gid == RB_GID_COUNTRY_CODE) {
				ULONG active = 0;
				struct Gadget *countryGad = FindRadioGadget(app, RB_GID_COUNTRY);
				GT_GetGadgetAttrs(gad, app->rbWin, NULL, GTCY_Active, (ULONG)&active, TAG_DONE);
				app->rbCountryMode = ClampInt((int)active, 0, 6);
				if (countryGad)
					GT_SetGadgetAttrs(countryGad, app->rbWin, NULL,
						GTST_String, (ULONG)RadioCountryFromIndex(app->rbCountryMode), TAG_DONE);
			}
			else if (gid == RB_GID_CLOSE) {
				CloseRadioWindow(app);
				return;
			}
		}
	}
}

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
		ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|aif|aiff)",
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
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY;
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
				es.es_TextFormat = (UBYTE *)"MiniAMP3\nMade by boingball\n(C)2026 - v1.0\nTo support this application visit:\nhttps://buymeacoffee.com/boingball\n-----\nMade with decoders from\nHelix MP3 / ACC\nby Real Networks\nlibfoxenflac\nby astoeckel\n\nESP8266Audio\nby earlephilhower\n-----\nAI Used\nClaude and Codex\nLate Nights\nMany";
				es.es_GadgetFormat = (UBYTE *)"OK";
				EasyRequest(app->win, &es, NULL, TAG_DONE);
			}
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_RADIO)
				OpenRadioWindow(app);
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
		if ((int)v >= 0 && (int)v < MR_RATE_COUNT && app->rateIndex != (int)v) {
			app->rateIndex = (int)v;
			UpdateSpeedGadgetChoices(app);
		}
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
		app->cd32Ultrafast = (app->rateIndex == MR_RATE_22050_INDEX && (int)v == 3);
		app->ultrafast = ((int)v == 2);
		app->superfastLowrate = ((int)v == 1 || app->cd32Ultrafast);
		if (app->cd32Ultrafast) {
			app->fastLowrate = 1;
			app->mono = 1;
			app->rateIndex = MR_RATE_22050_INDEX;
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
	app.magic = MR_APP_MAGIC;
	app.rateIndex = 1;		/* 11025 Hz */
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
	app.shownGaugeLevel = -1;
	app.shownChannelDisabled = -1;
	app.shownNextDisabled = -1;
	app.lastRadioStatusShown = -1;

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
		ULONG rbSig = (app.rbWin && app.rbWin->UserPort) ? (1UL << app.rbWin->UserPort->mp_SigBit) : 0;
		ULONG sigs = Wait(winSig | timerSig | doneSig | plSig | rbSig | SIGBREAKF_CTRL_C);

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
			UpdateAlbumHover(&app);
			ScrollAlbumHover(&app);
			ArmTimer(&app, MR_TICK_MICROS);
		}

		if (plSig && (sigs & plSig))
			HandlePlaylistWindow(&app);
		if (rbSig && (sigs & rbSig))
			HandleRadioWindow(&app);

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
					case GID_RADIO:
						OpenRadioWindow(&app);
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
			/* Mouse movement only updates the Album hover state.  The timer handles
			 * the slow text scrolling so normal pointer motion cannot repaint the
			 * metadata gadgets continuously. */
			UpdateAlbumHover(&app);
		}
	}

	/* Ordered, idempotent app-close teardown. */
	AppCloseShutdown(&app);

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
	/* Every playback child has been stopped and reaped above, so it is now safe
	 * to release the shared network libraries (AmiSSL master + bsdsocket.library)
	 * that the probe/search/streams opened.  Without this the app left
	 * bsdsocket.library open on exit and the next launch could not open a working
	 * socket ("Search failed" with the network otherwise up). */
	AppCloseDebug("close SocketBase fallback", &app);
	AppCloseDebug("AmiSSL shutdown fallback", &app);
	Radio_NetworkShutdown();
	AppCloseDebug("end", &app);
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
