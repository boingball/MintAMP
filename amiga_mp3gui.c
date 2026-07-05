/*
 * MiniAMP3 - compact AmigaOS GadTools mini-player frontend for the Helix
 * fixed-point MP3 decoder.  The GUI wraps the existing amiga_mp3dec playback
 * frontend so the same Paula streaming path, fast-lowrate options, and buffer
 * handling are used from either Shell or Workbench.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniamp_memguard.h"
#include "amiga_display_text.h"

#if defined(AMIGA_M68K)
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
#endif

#if !defined(AMIGA_M68K)
/* Keep in sync with definition in amiga_mp3dec.c */
typedef struct GuiPlaybackStatus {
	volatile int           phase;
	volatile long          spareMs;
	volatile unsigned long underruns;
	volatile unsigned long decodedFrames;
	volatile int           sampleRate;
	volatile unsigned long halfBufferMs;
	volatile unsigned long runId;
	volatile int           cleanupComplete;
	volatile int           cleanupStage;
	volatile int           startupStage;
	volatile int           requestedRate;
	volatile int           effectiveRate;
	volatile unsigned int  paulaPeriod;
	volatile unsigned long requestedBytes;
	volatile unsigned long tryBytes;
	volatile int           lastError;
	volatile int           openDeviceResult;
	volatile int           radioActive;
	volatile int           radioStatus;
	volatile int           radioBitrateKbps;
	volatile int           radioBufferedBytes;
	volatile int           radioMetaInt;
	volatile char          radioTitle[128];
	volatile char          radioStationName[128];
	volatile char          radioGenre[64];
	volatile char          radioStreamUrl[128];
	volatile char          radioContentType[64];
	volatile char          radioError[128];
} GuiPlaybackStatus;
#define GUIPLAY_PHASE_IDLE      0
#define GUIPLAY_PHASE_BUFFERING 1
#define GUIPLAY_PHASE_PLAYING   2
#define GUIPLAY_PHASE_UNDERRUN  3
#define GUIPLAY_PHASE_DONE      4
#define GUIPLAY_PHASE_STOPPING  5
#define GUIPLAY_PHASE_ERROR     6
#define GUIPLAY_PHASE_ERROR     6
#define GUIPLAY_CLEANUP_NONE          0
#define GUIPLAY_CLEANUP_ABORT_REAP    1
#define GUIPLAY_CLEANUP_DEVICE_CLOSED 2
#define GUIPLAY_CLEANUP_BUFFERS_FREED 3
#define GUIPLAY_CLEANUP_COMPLETE      4
#define GUISTART_NONE                  0
#define GUISTART_CHILD_ENTERED         10
#define GUISTART_ARGS_READY            20
#define GUISTART_INPUT_OPEN            30
#define GUISTART_INPUT_FOPEN_BEFORE    31
#define GUISTART_INPUT_FOPEN_AFTER     32
#define GUISTART_INPUT_PRELOAD_FASTMEM 35
#define GUISTART_INPUT_PREPARE         40
#define GUISTART_DECODER_ALLOC         50
#define GUISTART_DECODER_CONFIG        60
#define GUISTART_FASTLOWRATE_WARN_BEFORE 61
#define GUISTART_FASTLOWRATE_WARN_AFTER  62
#define GUISTART_PROBE_RATE            70
#define GUISTART_PROBE_RATE_DONE       80
#define GUISTART_STREAM_INIT           90
#define GUISTART_PREFILL               100
#define GUISTART_PREFILL_DONE          110
#define GUISTART_AUDIO_SETUP           120
#define GUISTART_CREATE_PORT           130
#define GUISTART_ALLOC_CHIP_BUFFERS    140
#define GUISTART_CREATE_IOREQUESTS     150
#define GUISTART_OPEN_DEVICE           160
#define GUISTART_OPEN_DEVICE_DONE      170
#define GUISTART_ALLOC_WORK_BUFFERS    180
#define GUISTART_AUDIO_SETUP_DONE      190
#define GUISTART_FILL_BUFFER_A         200
#define GUISTART_FILL_BUFFER_A_DONE    210
#define GUISTART_FILL_BUFFER_B         220
#define GUISTART_FILL_BUFFER_B_DONE    230
#define GUISTART_PREPARE_A             240
#define GUISTART_PREPARE_B             250
#define GUISTART_COMMIT_A              260
#define GUISTART_PLAYING               270
#define GUISTART_FAILED                900
#define GUISTART_CLEANUP               910
#ifdef MINIAMP3_DEBUG
static const char *GuiStartupStageName(int stage)
{
	switch (stage) {
	case GUISTART_INPUT_FOPEN_BEFORE: return "input fopen before";
	case GUISTART_INPUT_FOPEN_AFTER: return "input fopen after";
	case GUISTART_INPUT_PRELOAD_FASTMEM: return "copying input to Fast RAM";
	case GUISTART_INPUT_PREPARE: return "input prepare";
	case GUISTART_FASTLOWRATE_WARN_BEFORE: return "fast-lowrate warning gate before";
	case GUISTART_FASTLOWRATE_WARN_AFTER: return "fast-lowrate warning gate after";
	case GUISTART_PROBE_RATE: return "probing input rate";
	case GUISTART_PREFILL: return "prefill decode";
	case GUISTART_AUDIO_SETUP: return "audio setup";
	case GUISTART_CREATE_PORT: return "creating msg port";
	case GUISTART_ALLOC_CHIP_BUFFERS: return "allocating chip buffers";
	case GUISTART_CREATE_IOREQUESTS: return "creating IO requests";
	case GUISTART_OPEN_DEVICE: return "opening audio.device";
	case GUISTART_ALLOC_WORK_BUFFERS: return "allocating work buffers";
	case GUISTART_FILL_BUFFER_A: return "filling playback buffer A";
	case GUISTART_FILL_BUFFER_B: return "filling playback buffer B";
	case GUISTART_PREPARE_A: return "preparing buffer A";
	case GUISTART_PREPARE_B: return "preparing buffer B";
	case GUISTART_COMMIT_A: return "submitting first buffer";
	case GUISTART_PLAYING: return "playing";
	case GUISTART_FAILED: return "failed";
	case GUISTART_CLEANUP: return "cleanup";
	default: return "starting";
	}
}
#endif /* MINIAMP3_DEBUG */
#endif
/* Shared status written by the playback subprocess (amiga_mp3dec.c). */
extern GuiPlaybackStatus gGuiPlaybackStatus;
extern volatile int gMiniAmp3EmbeddedPlayback;

/* Decoder module discovery (set at startup, read by playback subprocess). */
extern char gDecoderModulesPath[512];
/* ASL pattern covering mp3/aac + all discovered decoder extensions, e.g. "#?.(mp3|aac|flac)" */
static char gSupportedExtPattern[512];

#ifdef MINIAMP3_DEBUG
#ifndef MINIAMP3_DEBUG_FMT_PTR
#if defined(AMIGA_M68K)
#define MINIAMP3_DEBUG_FMT_PTR(p) ((ULONG)(p))
#else
#define MINIAMP3_DEBUG_FMT_PTR(p) (p)
#endif
#endif
#endif

#ifdef AMIGA_M68K
#include <exec/types.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <diskfont/diskfont.h>
#include <devices/timer.h>
#include <hardware/cia.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <proto/timer.h>
/* #include <graphics/colormap.h> */
#include "picojpeg.h"
#include "radio_stream.h"
#include "radio_browser_controller.h"
#ifndef OBP_FailIfBad
#define OBP_FailIfBad (TAG_USER + 0x01L)
#endif

#define HELIXAMP3_MAX_PATH 256
#define HELIXAMP3_ARGC_MAX 28
#define HELIXAMP3_SETTINGS_VERSION 2
#define HELIXAMP3_RADIO_FAV_MAX 20
#define HELIXAMP3_QUALITY_MIN 0
#define HELIXAMP3_QUALITY_MAX 3
#define HELIXAMP3_SIGMASK(gui) (1UL << (gui)->win->UserPort->mp_SigBit)
#define GUI_ENV_PREFIX  "ENVARC:MiniAMP3"
#define GUI_STARTUP_STACK_SIZE 262144UL

#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       340    /* inner height */

#define GUI_MARGIN_L     8     /* left margin */
#define GUI_MARGIN_R     8     /* right margin */
#define GUI_TOP_Y       20     /* leave breathing room below the title bar */
#define GUI_ROW_H       18     /* row pitch - enough for Topaz 8 + padding */

#define ART_W           64
#define ART_H           64
#define MAX_JPEG_DIM    1024
#define ART_X           (GUI_WIN_W - ART_W - GUI_MARGIN_R)
#define ART_Y           GUI_TOP_Y

#define TEXT_COL_W      (ART_X - GUI_MARGIN_L - 8)
#define META_X          (GUI_MARGIN_L + 60)
#define META_RIGHT      (ART_X - 8)
#define META_W          (META_RIGHT - META_X)
#define BROWSE_W        56
#define BROWSE_X        (ART_X - BROWSE_W - 6)
#define FILE_W          (BROWSE_X - META_X - 4)
#define SLIDER_X        (GUI_MARGIN_L + 60)
#define BUFFER_SLIDER_W 300
#define VOLUME_SLIDER_W BUFFER_SLIDER_W
#define TRANSPORT_W     48
#define TRANSPORT_H     20
#define PLAY_X          (GUI_MARGIN_L + 154)
#define NEXT_X          (PLAY_X + TRANSPORT_W + 4)
#define STOP_X          (GUI_MARGIN_L + 306)
#define FILTER_X         (GUI_MARGIN_L + 390)
#define FILTER_W         54
#define PL_OPEN_X        (FILTER_X + FILTER_W + 8)
#define PL_OPEN_W        (GUI_WIN_W - PL_OPEN_X - GUI_MARGIN_R)
#define FILEINFO_X      (GUI_MARGIN_L + 84)
#define FILEINFO_W      (GUI_WIN_W - FILEINFO_X - GUI_MARGIN_R)

#define ROW_FILE        (GUI_TOP_Y)
#define ROW_TITLE       (GUI_TOP_Y + 1 * GUI_ROW_H)
#define ROW_ARTIST      (GUI_TOP_Y + 2 * GUI_ROW_H)
#define ROW_ALBUM       (GUI_TOP_Y + 3 * GUI_ROW_H)
#define ROW_RATING      (GUI_TOP_Y + 4 * GUI_ROW_H)
#define ROW_TRACK       (GUI_TOP_Y + 5 * GUI_ROW_H)
#define ROW_GENRE       (GUI_TOP_Y + 6 * GUI_ROW_H)
#define ROW_CHECKS      (GUI_TOP_Y + 7 * GUI_ROW_H + 4)
#define ROW_CHANNELS    (GUI_TOP_Y + 8 * GUI_ROW_H + 4)
#define ROW_CYCLES      (GUI_TOP_Y + 9 * GUI_ROW_H + 4)
#define ROW_BUFFER      (GUI_TOP_Y + 10 * GUI_ROW_H + 4)
#define ROW_VOLUME      (GUI_TOP_Y + 11 * GUI_ROW_H + 4)
#define ROW_PROGRESS    (GUI_TOP_Y + 12 * GUI_ROW_H + 8)
#define ROW_BUTTONS     (GUI_TOP_Y + 13 * GUI_ROW_H + 12)
#define ROW_STATUS      (ROW_BUTTONS + TRANSPORT_H + 4)
#define ROW_FILEINFO    (ROW_STATUS + GUI_ROW_H + 4)

#define PROG_X          (GUI_MARGIN_L + 8)
#define PROG_W          (GUI_WIN_W - PROG_X - 90 - GUI_MARGIN_R)
#define PROG_H          8
#define PROG_TOP_Y      (ROW_PROGRESS + 4)
#define TIME_X          (PROG_X + PROG_W + 6)
#define TIME_W          80
#define TIMER_TICK_MICROS 1000000UL
#define ART_TIMER_MICROS 20000UL
/* How long Stop is allowed to sit outstanding (child signalled but never
 * confirmed gone) before the GUI gives up waiting silently and says so.
 * Generous enough to cover a slow DNS lookup/TLS handshake plus buffer
 * drain, short enough that a genuinely wedged child (blocking bsdsocket/
 * AmiSSL call that never observes SIGBREAKF_CTRL_C) doesn't leave the user
 * staring at "Stopping..." with no feedback and no idea whether it's
 * frozen or about to recover. */
#define STOP_WATCHDOG_TIMEOUT_MICROS (20UL * 1000000UL)
/* Bound on WaitForPlaybackShutdown()'s app-close wait loop: each iteration
 * delays 1 tick (Delay(1), ~1/50s at the usual jiffy rate), so this is
 * roughly a minute total before giving up on a wedged playback child. See
 * the comment at the loop's timeout check for the tradeoff this accepts. */
#define APP_CLOSE_WEDGED_CHILD_MAX_TICKS 3000
#define ART_MCUS_PER_PUMP 16
#ifndef MINIAMP3_ART_REDUCED_JPEG
#define MINIAMP3_ART_REDUCED_JPEG 1
#endif
#ifndef MINIAMP3_ART_COMPARE_JPEG
#define MINIAMP3_ART_COMPARE_JPEG 0
#endif

/* Radio Browser station-favicon artwork, ported from minimp3r's
 * ReAction frontend.  JPEG and ICO favicons are decoded (dispatched on
 * magic bytes, never URL extension or Content-Type); PNG is intentionally
 * not supported here since GadTools does not link lodepng (see
 * Makefile.amiga) and this frontend targets lower-spec/lower-memory
 * systems than minimp3r.  Disable entirely with -DENABLE_RADIO_ARTWORK=0.
 * Never touches the MP3/ICY stream either way. */
#ifndef ENABLE_RADIO_ARTWORK
#define ENABLE_RADIO_ARTWORK 1
#endif
#define HELIXAMP3_FAVICON_MAX_BYTES (256L * 1024L)

#define MENUNUM_PROJECT   0
#define MENUNUM_PLAYBACK  1
#define ITEMNUM_ABOUT     0
#define ITEMNUM_STREAM    1
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

enum {
	GID_FILE = 1,
	GID_BROWSE,
	GID_TITLE,
	GID_ARTIST,
	GID_ALBUM,
	GID_SPEED_MODE,
	GID_FAST_MEM,
	GID_CHANNEL_MODE,
	GID_FAKE_STEREO,
	GID_FAKE_STEREO_WIDTH,
	GID_FAKE_STEREO_DELAY,
	GID_RATE,
	GID_BUFFER,
	GID_VOLUME,
	GID_QUALITY,
	GID_PLAY,
	GID_NEXT,
	GID_STOP,
	GID_HARDWARE_FILTER,
	GID_PLAYLIST,
	GID_STATUS,
	GID_RATING_LABEL,
	GID_RATING_VALUE,
	GID_TRACK,
	GID_GENRE,
	GID_FILEINFO,
	GID_STAR1,
	GID_STAR2,
	GID_STAR3,
	GID_STAR4,
	GID_STAR5,
	GID_COUNT,
	GID_STREAM_URL,
	GID_STREAM_OK,
	GID_STREAM_CANCEL
};

#define RB_GID_SEARCH_TEXT   300
#define RB_GID_CODEC         301
#define RB_GID_COUNTRY       302
#define RB_GID_COUNTRY_CODE 303
#define RB_GID_SCHEME       304
#define RB_GID_LIMIT        305
#define RB_GID_BITRATE      306
#define RB_GID_RADIO_RESULTS 307
#define RB_GID_SEARCH       308
#define RB_GID_PROBE        309
#define RB_GID_ADD_FAV      310
#define RB_GID_FAVOURITES   311
#define RB_GID_UP           312
#define RB_GID_DOWN         313
#define RB_GID_CLOSE        314
#define RB_GID_STATUS       315

/* Playlist window gadget IDs (separate range to avoid main window conflicts) */
#define PL_GID_LIST      200
#define PL_GID_ADD       201
#define PL_GID_REMOVE    202
#define PL_GID_CLEAR     203
#define PL_GID_PLAY      204
#define PL_GID_LOAD_M3U  205
#define PL_GID_SAVE_M3U  206

#define HELIXAMP3_PLAYLIST_MAX 128

typedef struct {
	char paths[HELIXAMP3_PLAYLIST_MAX][HELIXAMP3_MAX_PATH];
	char names[HELIXAMP3_PLAYLIST_MAX][80];
	struct Node nodes[HELIXAMP3_PLAYLIST_MAX];
	struct List list;
	int count;
	int selected;
	int current;
} Playlist;

typedef struct {
	const unsigned char *data;
	unsigned long pos;
	unsigned long size;
} PjpegSrc;

typedef struct ArtDecodeState {
	int active;
	int isPng;
	int mcuIndex;
	int totalMcus;
	pjpeg_image_info_t info;
	PjpegSrc src;
	unsigned char xMap[MAX_JPEG_DIM];
	unsigned char yMap[MAX_JPEG_DIM];
	unsigned long greyAccum[ART_W * ART_H];
	unsigned long rAccum[ART_W * ART_H];
	unsigned long gAccum[ART_W * ART_H];
	unsigned long bAccum[ART_W * ART_H];
	unsigned short greyCount[ART_W * ART_H];
	unsigned char greyOut[ART_W * ART_H];
	int reduce;
	int wantColor;
	unsigned long pumpCount;
	unsigned long decodeMicros;
	unsigned long processMicros;
	unsigned long startSecs;
	unsigned long startMicros;
} ArtDecodeState;

typedef struct Mp3Tags {
	char title[64];
	char artist[64];
	char album[64];
	char track[16];
	char genre[32];
	int  rating;
	int  bitrateKbps;
	int  sampleRate;
	int  channels;
	int  channelMode;
	int  modeExtension;
	unsigned long fileSize;
	int  durationSecs;
	unsigned char *artData;
	unsigned long artBytes;
	int artIsPng;
} Mp3Tags;

#define ART_COLOR_CACHE 64
typedef struct { unsigned long key; long pen; } ArtPenEntry;

typedef struct HelixAmp3Gui {
	struct Window  *win;
	struct Gadget  *gadgets;
	struct Gadget  *gadContext;
	struct Gadget  *gadFile;
	struct Gadget  *gadTitle;
	struct Gadget  *gadArtist;
	struct Gadget  *gadAlbum;
	struct Gadget  *gadTrack;
	struct Gadget  *gadGenre;
	struct Gadget  *gadFileInfo;
	struct Gadget  *gadRatingValue;
	struct Gadget  *gadStars[5];
	struct Gadget  *gadStatus;
	struct Gadget  *gadBuffer;
	struct Gadget  *gadVolume;
	struct Gadget  *gadSpeedMode;
	struct Gadget  *gadRate;
	struct Gadget  *gadFastMem;
	struct Gadget  *gadChannelMode;
	struct Gadget  *gadFakeStereo;
	struct Gadget  *gadFakeStereoWidth;
	struct Gadget  *gadFakeStereoDelay;
	struct Gadget  *gadPlay;
	struct Gadget  *gadNext;
	struct Gadget  *gadStop;
	struct Gadget  *gadHardwareFilter;
	struct Gadget  *gadPlaylist;
	struct VisualInfo *visualInfo;
	struct Window  *plWin;
	struct Gadget  *plGadgets;
	struct Gadget  *plGadContext;
	struct Gadget  *plGadList;
	Playlist playlist;
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
	int             hasNetwork;
	int             hasHttps;
	int             rbCountryMode;
	int             rbShowingFavourites;
	int             rbFavouriteCount;
	int             rbSelectedFavourite;
	char            rbFavouriteNames[HELIXAMP3_RADIO_FAV_MAX][RB_MAX_NAME];
	char            rbFavouriteUrls[HELIXAMP3_RADIO_FAV_MAX][RB_MAX_URL];
	char            rbStatusText[128];
	char            currentRadioStationName[RB_MAX_NAME];
	char            currentRadioFavicon[RB_MAX_FAVICON];
	struct VisualInfo *rbVisualInfo;
	RadioBrowserController rbController;
	struct Menu *menuStrip;
	int artEnabled;
	int artCacheEnabled;
	int artColorEnabled;
	int artCacheBypass;
	int artValid;
	int artLoading;
	int artRestartPending;
	int artCacheSavePending;
	unsigned char artGreyBuf[ART_W * ART_H];
	unsigned char artRGBBuf[ART_W * ART_H * 3];
	unsigned char artPenIdx[ART_W * ART_H];
	ArtPenEntry   artPenCache[ART_COLOR_CACHE];
	int           artPenCacheUsed;
	int           artPensBuilt;
	ArtDecodeState artDecode;
	struct MsgPort *timerPort;
	struct MsgPort *donePort;
	struct timerequest *timerReq;
	struct TextFont *smallFont;
	int timerOpen;
	int timerPending;
	int timerIsArt;
	Mp3Tags tags;
	char  inputName[HELIXAMP3_MAX_PATH];
	char  queuedInputName[HELIXAMP3_MAX_PATH];
	int   haveRadioHostAddr;
	unsigned long radioHostAddrBe;
	int   queuedHaveRadioHostAddr;
	unsigned long queuedRadioHostAddrBe;
	char  fileText[HELIXAMP3_MAX_PATH];
	char  lastDrawer[HELIXAMP3_MAX_PATH];
	char  statusText[128];
	char  fileInfoText[128];
	char  ratingText[16];
	int   fastLowrate;
	int   superfastLowrate;
	int   ultrafast;
	int   cd32Ultrafast;
	int   fastMem;
	int   mono;
	int   fakeStereo;
	int   fakeStereoWidthIndex;
	int   fakeStereoDelayIndex;
	int   hardwareFilter;
	int   rateIndex;
	int   bufferSeconds;
	int   volumePercent;
	int   qualityIndex;
	int   decodeThenPlay;
	int   bench;
	int   closeRequested;
	int   playbackActive;
	int   playbackDonePending;
	int   playbackStoppedByUser;
	int   playlistNextPending;
	int   queuedPlayPending;
	unsigned long playbackRunId;
	unsigned long playbackDoneRunId;
	unsigned long stopWatchdogMicros;
	int stopWatchdogFired;
	int lastCleanupStage;
	int lastStartupStage;
	int startupStageStableTicks;
	int startupStallShown;
	int   totalSecs;
	int   elapsedSecs;
	int   launchBufferSecs;
	unsigned long lastUnderrunCount;   /* last underrun count seen from IPC */
	long          lastDisplayedSpareMs; /* spare ms last shown in status bar */
	int           lastDisplayedPhase;   /* GUIPLAY_PHASE_* last shown in status bar */
	int           lastDrawnElapsedSecs; /* elapsed value last drawn in progress area */
	int           lastDrawnTotalSecs;   /* total value last drawn in progress area */
	int           progressEnabled;     /* 1 = redraw progress bar during playback */
} HelixAmp3Gui;

typedef struct HelixAmp3Args {
	int argc;
	char *argv[HELIXAMP3_ARGC_MAX];
	char argvStorage[HELIXAMP3_ARGC_MAX][HELIXAMP3_MAX_PATH];
} HelixAmp3Args;

typedef struct HelixAmp3Player {
	volatile int stopRequested;
	int argc;
	char **argv;
	struct Process *process;
} HelixAmp3Player;

static void UpdateTagDisplay(HelixAmp3Gui *gui);
static void SelectInternetStream(HelixAmp3Gui *gui, const char *url);
static void RadioSetStatus(HelixAmp3Gui *app, const char *text);
static void CloseRadioWindow(HelixAmp3Gui *gui);
static void OpenRadioWindow(HelixAmp3Gui *gui);
static void HandleRadioWindow(HelixAmp3Gui *gui);
static void RadioDoProbeAndPlay(HelixAmp3Gui *app);

struct IntuitionBase *IntuitionBase;
extern struct CIA ciaa;
struct Library *AslBase;
struct Library *GadToolsBase;
struct Library *DiskfontBase;
struct GfxBase *GfxBase;
static HelixAmp3Player gGuiPlayer;
static HelixAmp3Args gGuiArgs;
static struct Message gDoneMsg;
static struct MsgPort *gDonePort;
static volatile unsigned long gPlaybackRunCounter;
static volatile unsigned long gDoneRunId;
static volatile unsigned long gPlaybackEntryRunId;
static int gGuiFirstUiProgressLogged;

static struct TextAttr gTopaz8Attr = {
	(STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT
};

static struct TextAttr kFontPrefs[] = {
	{ (STRPTR)"xen.font",     9, FS_NORMAL, 0 },
	{ (STRPTR)"courier.font", 9, FS_NORMAL, 0 },
	{ (STRPTR)"topaz.font",   8, FS_NORMAL, FPF_ROMFONT }
};

static struct TextFont *OpenBestFont(void)
{
	int i;
	struct TextFont *f;

	if (DiskfontBase) {
		for (i = 0; i < 3; i++) {
			f = OpenDiskFont(&kFontPrefs[i]);
			if (f)
				return f;
		}
	}
	return OpenFont(&gTopaz8Attr);
}

static const char * const kRates[] = {
	"8287",
	"8820",
	"11025",
	"22050",
	"28600"
};

static const STRPTR kRateLabels[] = {
	(STRPTR)"8287",
	(STRPTR)"8820",
	(STRPTR)"11025",
	(STRPTR)"22050",
	(STRPTR)"28600",
	NULL
};

static int RateIndexSupportsSuperfast(int rateIndex, int mono)
{
	return rateIndex >= (mono ? 0 : 1) && rateIndex <= 3;
}

static int DefaultSuperfastRateIndex(int mono)
{
	return mono ? 0 : 1;
}

static int ChannelUsesMonoCost(const HelixAmp3Gui *gui)
{
	return gui->mono || gui->fakeStereo;
}

static const STRPTR kSpeedModeLabels[] = {
	(STRPTR)"Normal",
	(STRPTR)"Fast",
	(STRPTR)"Superfast",
	(STRPTR)"Ultrafast",
	(STRPTR)"22050 Mono Ultrafast",
	NULL
};

static const STRPTR kChannelModeLabels[] = {
	(STRPTR)"Stereo",
	(STRPTR)"Mono",
	NULL
};

static int SpeedModeIndex(const HelixAmp3Gui *gui)
{
	if (gui->cd32Ultrafast) return 4;
	if (gui->ultrafast) return 3;
	if (gui->superfastLowrate) return 2;
	if (gui->fastLowrate) return 1;
	return 0;
}

static int ChannelModeIndex(const HelixAmp3Gui *gui)
{
	return gui->mono ? 1 : 0;
}

static const STRPTR kQualityLabels[] = {
	(STRPTR)"Faster",
	(STRPTR)"Fast",
	(STRPTR)"Normal",
	(STRPTR)"Best",
	NULL
};

static const STRPTR kFakeStereoWidthLabels[] = {
	(STRPTR)"Very wide",
	(STRPTR)"Wide",
	(STRPTR)"Normal",
	(STRPTR)"Subtle",
	(STRPTR)"Narrow",
	NULL
};

static const int kFakeStereoShifts[] = { 1, 2, 3, 4, 5 };

static const STRPTR kFakeStereoDelayLabels[] = {
	(STRPTR)"48",
	(STRPTR)"64",
	(STRPTR)"96",
	(STRPTR)"128",
	(STRPTR)"192",
	NULL
};

static const int kFakeStereoDelays[] = { 48, 64, 96, 128, 192 };

static struct NewMenu myNewMenus[] = {
	{ NM_TITLE, (STRPTR)"Project",          0, 0, 0, 0 },
	{ NM_ITEM,  (STRPTR)"About MiniAMP3...",0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_ABOUT) },
	{ NM_ITEM,  (STRPTR)"Internet Radio...",0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_STREAM) },
	{ NM_ITEM,  (STRPTR)"Quit",             0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_QUIT) },
	{ NM_TITLE, (STRPTR)"Playback",         0, 0, 0, 0 },
	{ NM_ITEM,  (STRPTR)"Decode-then-play", 0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_DTP) },
	{ NM_ITEM,  (STRPTR)"Bench mode",       0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_BENCH) },
	{ NM_ITEM,  (STRPTR)"Artwork",          0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTWORK) },
	{ NM_ITEM,  (STRPTR)"Artwork Cache",    0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTCACHE) },
	{ NM_ITEM,  (STRPTR)"Colour Artwork",   0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTCOLOR) },
	{ NM_ITEM,  (STRPTR)"Refresh Artwork",   0, 0, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTREFRESH) },
	{ NM_ITEM,  (STRPTR)"Reload Art from File", 0, 0, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTRELOAD) },
	{ NM_ITEM,  (STRPTR)"Clean Artwork Cache", 0, 0, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTCLEAN) },
	{ NM_ITEM,  (STRPTR)"Progress Bar",     0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_PROGRESS) },
	{ NM_END,   NULL,                       0, 0, 0, 0 }
};

static void SafeCopy(char *dst, size_t dstSize, const char *src)
{
	if (!dst || dstSize == 0)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, dstSize - 1);
	dst[dstSize - 1] = '\0';
}


static int is_url_path(const char *path)
{
	return path && (!strncmp(path, "http://", 7) || !strncmp(path, "https://", 8));
}


static void GuiLogPathOp(const char *func, const char *path)
{
#ifdef MINIAMP3_DEBUG
	Printf("gui-path: %s path='%s' is_url=%ld\n",
		func ? func : "(null)", path ? path : "(null)",
		is_url_path(path) ? 1L : 0L);
#else
	(void)func;
	(void)path;
#endif
}

static BPTR SafeOpenPath(const char *func, const char *path, LONG mode)
{
	GuiLogPathOp(func, path);
	if (is_url_path(path))
		return (BPTR)0;
	return Open((STRPTR)path, mode);
}

static BPTR SafeLockPath(const char *func, const char *path, LONG mode)
{
	GuiLogPathOp(func, path);
	if (is_url_path(path))
		return (BPTR)0;
	return Lock((STRPTR)path, mode);
}

static BOOL SafeAddPartPath(const char *func, char *path, const char *part, ULONG size)
{
	GuiLogPathOp(func, path);
	GuiLogPathOp("AddPart(part)", part);
	if (is_url_path(path) || is_url_path(part))
		return FALSE;
	return AddPart((STRPTR)path, (STRPTR)part, size);
}

static void CopyDrawerFromPath(char *drawer, size_t drawerSize, const char *path)
{
	char *q;

	if (!drawer || drawerSize == 0)
		return;
	drawer[0] = '\0';
	if (!path || !path[0] || is_url_path(path))
		return;
	SafeCopy(drawer, drawerSize, path);
	q = drawer + strlen(drawer);
	while (q > drawer && *q != '/' && *q != ':')
		q--;
	if (*q == '/' || *q == ':')
		*(q + 1) = '\0';
	else
		drawer[0] = '\0';
}


static void EnvName(char *dst, size_t dstSize, const char *key)
{
	SafeCopy(dst, dstSize, GUI_ENV_PREFIX);
	strncat(dst, "/", dstSize - strlen(dst) - 1);
	strncat(dst, key, dstSize - strlen(dst) - 1);
}

static int LoadEnvIntMaybe(const char *key, int *outValue, int minValue, int maxValue)
{
	char name[64];
	char value[32];
	long n;
	int v;

	if (!outValue)
		return 0;
	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)value, sizeof(value) - 1, 0);
	if (n <= 0)
		return 0;
	value[n] = '\0';
	v = atoi(value);
	if (v < minValue)
		v = minValue;
	if (v > maxValue)
		v = maxValue;
	*outValue = v;
	return 1;
}

static int LoadEnvInt(const char *key, int fallback, int minValue, int maxValue)
{
	int v;

	if (LoadEnvIntMaybe(key, &v, minValue, maxValue))
		return v;
	return fallback;
}

static void LoadEnvString(const char *key, char *dst, size_t dstSize)
{
	char name[64];
	long n;

	if (!dst || dstSize == 0)
		return;
	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)dst, dstSize - 1, 0);
	if (n > 0)
		dst[n] = '\0';
	else
		dst[0] = '\0';
}

static void SaveEnvString(const char *key, const char *value)
{
	char name[64];

	EnvName(name, sizeof(name), key);
	if (!value)
		value = "";
	SetVar((STRPTR)name, (STRPTR)value, strlen(value), GVF_GLOBAL_ONLY);
	SetVar((STRPTR)name, (STRPTR)value, strlen(value), GVF_SAVE_VAR);
}

static int ClampInt(int value, int minValue, int maxValue)
{
	if (value < minValue) return minValue;
	if (value > maxValue) return maxValue;
	return value;
}

static void SaveEnvInt(const char *key, int value)
{
	char text[16];

	sprintf(text, "%d", value);
	SaveEnvString(key, text);
}

static void SaveGuiSettings(HelixAmp3Gui *gui)
{
	SaveEnvInt("FastLowrate", gui->fastLowrate);
	SaveEnvInt("SuperfastLowrate", gui->superfastLowrate);
	SaveEnvInt("Ultrafast", gui->ultrafast);
	SaveEnvInt("CD32Ultrafast", gui->cd32Ultrafast);
	SaveEnvInt("FastMem", gui->fastMem);
	SaveEnvInt("Mono", gui->mono);
	SaveEnvInt("FakeStereo", gui->fakeStereo);
	SaveEnvInt("FakeStereoWidthIndex", gui->fakeStereoWidthIndex);
	SaveEnvInt("FakeStereoDelayIndex", gui->fakeStereoDelayIndex);
	SaveEnvInt("HardwareFilter", gui->hardwareFilter);
	SaveEnvInt("RateIndex", gui->rateIndex);
	SaveEnvInt("BufferSeconds", gui->bufferSeconds);
	SaveEnvInt("Volume", gui->volumePercent);
	SaveEnvInt("QualityIndex", gui->qualityIndex);
	SaveEnvInt("SettingsVersion", HELIXAMP3_SETTINGS_VERSION);
	SaveEnvInt("DecodeThenPlay", gui->decodeThenPlay);
	SaveEnvInt("Bench", gui->bench);
	SaveEnvInt("Artwork", gui->artEnabled);
	SaveEnvInt("ArtworkCache", gui->artCacheEnabled);
	SaveEnvInt("ArtworkColour", gui->artColorEnabled);
	SaveEnvInt("ProgressBar", gui->progressEnabled);
	SaveEnvString("LastDrawer", gui->lastDrawer);
	{
		int i;
		char key[32];
		SaveEnvInt("RadioFavCount", ClampInt(gui->rbFavouriteCount, 0, HELIXAMP3_RADIO_FAV_MAX));
		for (i = 0; i < HELIXAMP3_RADIO_FAV_MAX; i++) {
			sprintf(key, "RadioFavName%d", i);
			SaveEnvString(key, gui->rbFavouriteNames[i]);
			sprintf(key, "RadioFavUrl%d", i);
			SaveEnvString(key, gui->rbFavouriteUrls[i]);
		}
	}
}

static void FreeTags(Mp3Tags *tags)
{
	if (!tags)
		return;
	if (tags->artData) {
		FreeMem(tags->artData, tags->artBytes);
		tags->artData = NULL;
		tags->artBytes = 0;
	}
	tags->artIsPng = 0;
}

static unsigned long ApicImageOffset(const unsigned char *payload,
	unsigned long payloadBytes)
{
	unsigned long pos = 1;

	if (!payload || payloadBytes < 4)
		return payloadBytes;
	while (pos < payloadBytes && payload[pos])
		pos++;
	pos++;
	if (pos >= payloadBytes)
		return payloadBytes;
	pos++;
	if (payload[0] == 1 || payload[0] == 2) {
		while (pos + 1 < payloadBytes &&
			!(payload[pos] == 0 && payload[pos + 1] == 0))
			pos += 2;
		pos += 2;
	} else {
		while (pos < payloadBytes && payload[pos])
			pos++;
		pos++;
	}
	return pos <= payloadBytes ? pos : payloadBytes;
}

static unsigned long PicImageOffset(const unsigned char *payload,
	unsigned long payloadBytes)
{
	unsigned long pos = 5;

	if (!payload || payloadBytes < 6)
		return payloadBytes;
	if (payload[0] == 1 || payload[0] == 2) {
		while (pos + 1 < payloadBytes &&
			!(payload[pos] == 0 && payload[pos + 1] == 0))
			pos += 2;
		pos += 2;
	} else {
		while (pos < payloadBytes && payload[pos])
			pos++;
		pos++;
	}
	return pos <= payloadBytes ? pos : payloadBytes;
}


static const char *Id3v1GenreName(unsigned int genre)
{
	static const char *const names[] = {
		"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk",
		"Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
		"Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
		"Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
		"Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
		"Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
		"Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
		"AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
		"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
		"Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
		"Eurodance", "Dream", "Southern Rock", "Comedy", "Cult",
		"Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
		"Native American", "Cabaret", "New Wave", "Psychedelic", "Rave",
		"Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk",
		"Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
		"Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing",
		"Fast Fusion", "Bebop", "Latin", "Revival", "Celtic",
		"Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock",
		"Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band",
		"Chorus", "Easy Listening", "Acoustic", "Humour", "Speech",
		"Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
		"Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam",
		"Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad",
		"Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo",
		"A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass",
		"Club-House", "Hardcore", "Terror", "Indie", "BritPop",
		"Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap",
		"Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian",
		"Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime",
		"JPop", "Synthpop", "Christmas", "Art Rock", "Baroque", "Bhangra",
		"Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM",
		"Eclectic", "Electro", "Electroclash", "Emo", "Experimental",
		"Garage", "Global", "IDM", "Illbient", "Industro-Goth", "Jam Band",
		"Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic",
		"Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze",
		"Space Rock", "Trop Rock", "World Music", "Neoclassical",
		"Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast",
		"Indie Rock", "G-Funk", "Dubstep", "Garage Rock", "Psybient"
	};

	return (genre < (sizeof(names) / sizeof(names[0]))) ? names[genre] : NULL;
}

static void NormalizeId3Genre(char *genre, size_t genreSize)
{
	char *p;
	char *end;
	unsigned long value;
	const char *name;

	if (!genre || genreSize == 0 || !genre[0])
		return;
	p = genre;
	if (*p == '(')
		p++;
	if (*p < '0' || *p > '9')
		return;
	value = strtoul(p, &end, 10);
	if (*end == ')')
		end++;
	if (*end != '\0' || value == 255)
		return;
	name = Id3v1GenreName((unsigned int)value);
	if (name)
		SafeCopy(genre, genreSize, name);
}

static void StripTrailing(char *s)
{
	int n;

	if (!s)
		return;
	n = (int)strlen(s);
	while (n > 0) {
		unsigned char c = (unsigned char)s[n - 1];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\0')
			break;
		s[--n] = '\0';
	}
}

static void CopyId3v1TextField(char *dst, size_t dstSize,
	const unsigned char *src, long len)
{
	long i;
	long out;

	if (!dst || dstSize == 0)
		return;
	dst[0] = '\0';
	if (!src || len <= 0)
		return;
	out = 0;
	for (i = 0; i < len && out + 1 < (long)dstSize; i++) {
		unsigned char c = src[i];
		if (c == 0)
			break;
		dst[out++] = (c >= 32 && c != 127) ? (char)c : '?';
	}
	dst[out] = '\0';
	StripTrailing(dst);
}

static void CopyId3v2TextField(char *dst, size_t dstSize,
	const unsigned char *src, long len)
{
	unsigned char enc;
	long i;
	long out;
	int bigEndian;

	if (!dst || dstSize == 0)
		return;
	dst[0] = '\0';
	if (!src || len <= 0)
		return;

	enc = src[0];
	src++;
	len--;

	if (enc == 0) {
		out = 0;
		for (i = 0; i < len && out + 1 < (long)dstSize; i++) {
			unsigned char c = src[i];
			if (c == 0)
				break;
			dst[out++] = (c >= 32 && c != 127) ? (char)c : '?';
		}
		dst[out] = '\0';
	} else if (enc == 1 || enc == 2) {
		bigEndian = (enc == 2) ? 1 : 0;
		if (len >= 2) {
			if (src[0] == 0xFE && src[1] == 0xFF) {
				bigEndian = 1;
				src += 2;
				len -= 2;
			} else if (src[0] == 0xFF && src[1] == 0xFE) {
				bigEndian = 0;
				src += 2;
				len -= 2;
			}
		}
		out = 0;
		for (i = 0; i + 1 < len && out + 1 < (long)dstSize; i += 2) {
			unsigned int hi = bigEndian ? src[i] : src[i + 1];
			unsigned int lo = bigEndian ? src[i + 1] : src[i];
			unsigned int cp = (hi << 8) | lo;

			if (cp == 0)
				break;
			if (cp < 0x20 || cp == 0x7F) {
				/* skip control chars */
			} else if (cp <= 0x00FF) {
				dst[out++] = (char)(cp & 0xFF);
			} else {
				dst[out++] = '?';
			}
		}
		dst[out] = '\0';
	} else if (enc == 3) {
		out = 0;
		for (i = 0; i < len && out + 1 < (long)dstSize; i++) {
			unsigned char c = src[i];
			if (c == 0)
				break;
			dst[out++] = (char)c;
		}
		dst[out] = '\0';
	} else {
		out = 0;
		src--;
		len++;
		for (i = 0; i < len && out + 1 < (long)dstSize; i++) {
			unsigned char c = src[i];
			if (c == 0)
				break;
			dst[out++] = (c >= 32 && c != 127) ? (char)c : '?';
		}
		dst[out] = '\0';
	}
	StripTrailing(dst);
}

static long Id3Synchsafe(const unsigned char *b)
{
	return ((long)(b[0] & 0x7f) << 21) | ((long)(b[1] & 0x7f) << 14) |
		((long)(b[2] & 0x7f) << 7) | (long)(b[3] & 0x7f);
}

static long Id3BigEndian32(const unsigned char *b)
{
	return ((long)b[0] << 24) | ((long)b[1] << 16) |
		((long)b[2] << 8) | (long)b[3];
}

static int IsMpegSyncHeader(const unsigned char *h)
{
	return h[0] == 0xff && (h[1] == 0xfb || h[1] == 0xfa ||
		h[1] == 0xf3 || h[1] == 0xf2 || h[1] == 0xe3 || h[1] == 0xe2);
}

static void ReadMpegInfo(FILE *f, Mp3Tags *tags, long *firstFrameOffset)
{
	static const int bitrateTab[16] = {
		0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
	};
	static const int samplerateTab[4] = { 44100, 48000, 32000, 0 };
	unsigned char h[4];
	int b;
	int idx;

	if (firstFrameOffset)
		*firstFrameOffset = -1L;
	if (!f || !tags)
		return;
	h[0] = h[1] = h[2] = h[3] = 0;
	while ((b = fgetc(f)) != EOF) {
		h[0] = h[1];
		h[1] = h[2];
		h[2] = h[3];
		h[3] = (unsigned char)b;
		if (IsMpegSyncHeader(h)) {
			long pos = ftell(f);
			if (firstFrameOffset && pos >= 4)
				*firstFrameOffset = pos - 4;
			idx = (h[2] >> 4) & 0x0f;
			tags->bitrateKbps = bitrateTab[idx];
			idx = (h[2] >> 2) & 0x03;
			tags->sampleRate = samplerateTab[idx];
			tags->channelMode = (h[3] >> 6) & 0x03;
			tags->modeExtension = (h[3] >> 4) & 0x03;
			tags->channels = (tags->channelMode == 3) ? 1 : 2;
			return;
		}
	}
}

static void ReadId3v1(FILE *f, Mp3Tags *tags)
{
	unsigned char buf[128];

	if (!f || !tags)
		return;
	if (fseek(f, -128L, SEEK_END) != 0)
		return;
	if (fread(buf, 1, sizeof(buf), f) != sizeof(buf))
		return;
	if (memcmp(buf, "TAG", 3) != 0)
		return;
	if (!tags->title[0])
		CopyId3v1TextField(tags->title, sizeof(tags->title), buf + 3, 30);
	if (!tags->artist[0])
		CopyId3v1TextField(tags->artist, sizeof(tags->artist), buf + 33, 30);
	if (!tags->album[0])
		CopyId3v1TextField(tags->album, sizeof(tags->album), buf + 63, 30);
	if (!tags->track[0] && buf[125] == 0 && buf[126] != 0)
		sprintf(tags->track, "%u", (unsigned int)buf[126]);
	if (!tags->genre[0] && buf[127] != 255) {
		const char *genreName = Id3v1GenreName((unsigned int)buf[127]);

		if (genreName)
			SafeCopy(tags->genre, sizeof(tags->genre), genreName);
		else
			sprintf(tags->genre, "ID3 genre %u", (unsigned int)buf[127]);
	}
}


static int ContainsTextNoCase(const char *s, const char *needle)
{
	int i;
	int j;

	if (!s || !needle || !needle[0])
		return 0;
	for (i = 0; s[i]; i++) {
		for (j = 0; needle[j]; j++) {
			char a = s[i + j];
			char b = needle[j];

			if (!a)
				return 0;
			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (!needle[j])
			return 1;
	}
	return 0;
}

static void DetectPictureMime(const unsigned char *payload,
	unsigned long payloadBytes, int version, int *isJpeg, int *isPng)
{
	char mime[40];
	unsigned long i;

	*isJpeg = 0;
	*isPng = 0;
	if (!payload || payloadBytes < 4)
		return;
	memset(mime, 0, sizeof(mime));
	if (version == 2) {
		for (i = 0; i < 3 && i + 1 < payloadBytes; i++)
			mime[i] = (char)payload[i + 1];
	} else {
		for (i = 1; i < payloadBytes && i < sizeof(mime); i++) {
			if (!payload[i])
				break;
			mime[i - 1] = (char)payload[i];
		}
	}
	if (ContainsTextNoCase(mime, "jpeg") || ContainsTextNoCase(mime, "jpg"))
		*isJpeg = 1;
	else if (ContainsTextNoCase(mime, "png"))
		*isPng = 1;
}

static const char kPopmOwner[] = "amiga-libhelix-mp3";

static int PopmPayloadMatchesOwner(const unsigned char *payload, long frameSize)
{
	long ownerBytes = (long)sizeof(kPopmOwner);

	return payload && frameSize >= ownerBytes + 1 &&
		memcmp(payload, kPopmOwner, (size_t)ownerBytes) == 0;
}

static int RatingFromPopm(const unsigned char *payload, long frameSize)
{
	long i;
	unsigned int rating;

	if (!payload || frameSize <= 0)
		return 0;
	for (i = 0; i < frameSize && payload[i] != 0; i++)
		;
	if (i + 1 >= frameSize)
		return 0;
	rating = payload[i + 1];
	if (rating == 0)
		return 0;
	return (int)((rating + 25) / 51);
}

static unsigned char PopmByteFromRating(int rating)
{
	if (rating <= 0)
		return 0;
	if (rating > 5)
		rating = 5;
	return (unsigned char)(rating * 51);
}

static void StoreId3FrameSize(unsigned char *dst, long size, int version)
{
	if (version == 4) {
		dst[0] = (unsigned char)((size >> 21) & 0x7f);
		dst[1] = (unsigned char)((size >> 14) & 0x7f);
		dst[2] = (unsigned char)((size >> 7) & 0x7f);
		dst[3] = (unsigned char)(size & 0x7f);
	} else {
		dst[0] = (unsigned char)((size >> 24) & 0xff);
		dst[1] = (unsigned char)((size >> 16) & 0xff);
		dst[2] = (unsigned char)((size >> 8) & 0xff);
		dst[3] = (unsigned char)(size & 0xff);
	}
}

static long MakePopmFrame(unsigned char *dst, int rating, int version)
{
	long payloadSize = (long)sizeof(kPopmOwner) + 5L;

	memcpy(dst, "POPM", 4);
	StoreId3FrameSize(dst + 4, payloadSize, version);
	dst[8] = 0;
	dst[9] = 0;
	memcpy(dst + 10, kPopmOwner, sizeof(kPopmOwner));
	dst[10 + sizeof(kPopmOwner)] = PopmByteFromRating(rating);
	memset(dst + 11 + sizeof(kPopmOwner), 0, 4);
	return 10L + payloadSize;
}

static int WriteRatingToId3Tag(const char *path, int rating)
{
	unsigned char hdr[10];
	unsigned char frame[64];
	FILE *f;
	long tagSize;
	long tagEnd;
	long frameBytes;
	long firstPopmRatingPos;
	int version;
	int wrote;

	if (!path || !path[0] || is_url_path(path))
		return 0;
	f = fopen(path, "r+b");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
		memcmp(hdr, "ID3", 3) != 0 || hdr[3] < 3 || hdr[3] > 4) {
		fclose(f);
		return 0;
	}
	version = hdr[3];
	tagSize = Id3Synchsafe(hdr + 6);
	tagEnd = ftell(f) + tagSize;
	frameBytes = MakePopmFrame(frame, rating, version);
	firstPopmRatingPos = -1;
	wrote = 0;
	while (ftell(f) + 10 <= tagEnd) {
		unsigned char fh[10];
		long frameSize;
		long payloadPos;

		if (fread(fh, 1, 10, f) != 10)
			break;
		if (fh[0] == 0) {
			long padPos = ftell(f) - 10;
			if (tagEnd - padPos >= frameBytes) {
				fseek(f, padPos, SEEK_SET);
				wrote = fwrite(frame, 1, (size_t)frameBytes, f) ==
					(size_t)frameBytes;
			} else if (firstPopmRatingPos >= 0 &&
				fseek(f, firstPopmRatingPos, SEEK_SET) == 0) {
				wrote = fputc(PopmByteFromRating(rating), f) != EOF;
			}
			break;
		}
		frameSize = version == 4 ? Id3Synchsafe(fh + 4) :
			Id3BigEndian32(fh + 4);
		payloadPos = ftell(f);
		if (frameSize <= 0 || payloadPos + frameSize > tagEnd)
			break;
		if (memcmp(fh, "POPM", 4) == 0) {
			unsigned char popm[64];
			long n = frameSize;
			long i;

			if (n > (long)sizeof(popm))
				n = (long)sizeof(popm);
			if (fread(popm, 1, (size_t)n, f) == (size_t)n) {
				for (i = 0; i < n && popm[i] != 0; i++)
					;
				if (i + 1 < frameSize && firstPopmRatingPos < 0)
					firstPopmRatingPos = payloadPos + i + 1;
				if (PopmPayloadMatchesOwner(popm, n)) {
					long ratingPos = payloadPos + (long)sizeof(kPopmOwner);

					if (fseek(f, ratingPos, SEEK_SET) == 0)
						wrote = fputc(PopmByteFromRating(rating), f) != EOF;
					break;
				}
			}
			if (fseek(f, payloadPos + frameSize, SEEK_SET) != 0)
				break;
			continue;
		}
		if (fseek(f, frameSize, SEEK_CUR) != 0)
			break;
	}
	if (!wrote && firstPopmRatingPos >= 0 &&
		fseek(f, firstPopmRatingPos, SEEK_SET) == 0)
		wrote = fputc(PopmByteFromRating(rating), f) != EOF;
	fclose(f);
	return wrote;
}

static void ReadId3v2Frames(FILE *f, Mp3Tags *tags, const unsigned char *hdr, int loadArt)
{
	unsigned char fh[10];
	long tagStart;
	long tagSize;
	long tagEnd;
	int version;

	version = hdr[3];
	tagStart = ftell(f);
	tagSize = Id3Synchsafe(hdr + 6);
	tagEnd = tagStart + tagSize;
	while (ftell(f) < tagEnd) {
		char id[5];
		long frameSize;
		long payloadPos;
		long remain;
		char *target;
		size_t targetSize;

		if (version == 2) {
			if (fread(fh, 1, 6, f) != 6)
				break;
			if (fh[0] == 0)
				break;
			id[0] = (char)fh[0]; id[1] = (char)fh[1]; id[2] = (char)fh[2]; id[3] = '\0';
			frameSize = ((long)fh[3] << 16) | ((long)fh[4] << 8) | (long)fh[5];
		} else {
			if (fread(fh, 1, 10, f) != 10)
				break;
			if (fh[0] == 0)
				break;
			id[0] = (char)fh[0]; id[1] = (char)fh[1]; id[2] = (char)fh[2]; id[3] = (char)fh[3]; id[4] = '\0';
			frameSize = version == 4 ? Id3Synchsafe(fh + 4) : Id3BigEndian32(fh + 4);
		}
		payloadPos = ftell(f);
		if (frameSize <= 0 || payloadPos + frameSize > tagEnd)
			break;
		if (loadArt && !tags->artData &&
			((version == 2 && strcmp(id, "PIC") == 0) ||
			strcmp(id, "APIC") == 0) &&
			frameSize > 4 && frameSize <= 512L * 1024L) {
			unsigned char *payload;

			payload = (unsigned char *)malloc((size_t)frameSize);
			if (payload && fread(payload, 1, (size_t)frameSize, f) ==
				(size_t)frameSize) {
				unsigned long imgOff;
				unsigned long imgBytes;
				int isJpeg;
				int isPng;

				DetectPictureMime(payload, (unsigned long)frameSize, version,
					&isJpeg, &isPng);
				imgOff = (version == 2) ? PicImageOffset(payload,
					(unsigned long)frameSize) : ApicImageOffset(payload,
					(unsigned long)frameSize);
				imgBytes = (unsigned long)frameSize - imgOff;
				if (imgOff < (unsigned long)frameSize && imgBytes > 4) {
					tags->artData = (unsigned char *)AllocMem(imgBytes,
						MEMF_ANY);
					if (tags->artData) {
						memcpy(tags->artData, payload + imgOff, imgBytes);
						tags->artBytes = imgBytes;
						tags->artIsPng = isPng || (!isJpeg && !isPng);
					}
				}
			}
			free(payload);
			remain = payloadPos + frameSize - ftell(f);
			if (remain > 0 && fseek(f, remain, SEEK_CUR) != 0)
				break;
			continue;
		}

		target = NULL;
		targetSize = 0;
		if ((version == 2 && strcmp(id, "TT2") == 0) || strcmp(id, "TIT2") == 0) {
			target = tags->title;
			targetSize = sizeof(tags->title);
		} else if ((version == 2 && strcmp(id, "TP1") == 0) || strcmp(id, "TPE1") == 0) {
			target = tags->artist;
			targetSize = sizeof(tags->artist);
		} else if ((version == 2 && strcmp(id, "TAL") == 0) || strcmp(id, "TALB") == 0) {
			target = tags->album;
			targetSize = sizeof(tags->album);
		} else if ((version == 2 && strcmp(id, "TRK") == 0) || strcmp(id, "TRCK") == 0) {
			target = tags->track;
			targetSize = sizeof(tags->track);
		} else if ((version == 2 && strcmp(id, "TCO") == 0) || strcmp(id, "TCON") == 0) {
			target = tags->genre;
			targetSize = sizeof(tags->genre);
		}
		if (strcmp(id, "POPM") == 0) {
			unsigned char popm[96];
			long n = frameSize;
			if (n > (long)sizeof(popm))
				n = (long)sizeof(popm);
			if (fread(popm, 1, (size_t)n, f) == (size_t)n) {
				int popmRating = RatingFromPopm(popm, n);

				if (PopmPayloadMatchesOwner(popm, n) || tags->rating == 0)
					tags->rating = popmRating;
			}
		} else if (target && !target[0]) {
			unsigned char text[96];
			long n = frameSize;
			if (n > (long)sizeof(text))
				n = (long)sizeof(text);
			if (fread(text, 1, (size_t)n, f) == (size_t)n) {
				CopyId3v2TextField(target, targetSize, text, n);
				if (target == tags->genre)
					NormalizeId3Genre(target, sizeof(tags->genre));
			}
		} else {
			if (fseek(f, frameSize, SEEK_CUR) != 0)
				break;
		}
		remain = payloadPos + frameSize - ftell(f);
		if (remain > 0 && fseek(f, remain, SEEK_CUR) != 0)
			break;
	}
	fseek(f, tagEnd, SEEK_SET);
}


static void TryFolderArt(const char *inputName, Mp3Tags *tags)
{
	static const char *kCoverNames[] = {
		"folder.jpg", "cover.jpg", "album.jpg", "front.jpg", NULL
	};
	char dirPath[HELIXAMP3_MAX_PATH];
	char artPath[HELIXAMP3_MAX_PATH];
	int i;

	if (!inputName || !tags || tags->artData || is_url_path(inputName))
		return;
	SafeCopy(dirPath, sizeof(dirPath), inputName);
	{
		char *q = dirPath + strlen(dirPath);
		while (q > dirPath && *q != '/' && *q != ':')
			q--;
		if (*q == '/' || *q == ':')
			*(q + 1) = '\0';
		else
			dirPath[0] = '\0';
	}
	for (i = 0; kCoverNames[i] && !tags->artData; i++) {
		FILE *af;

		SafeCopy(artPath, sizeof(artPath), dirPath);
		strncat(artPath, kCoverNames[i],
			sizeof(artPath) - strlen(artPath) - 1);
		GuiLogPathOp("TryFolderArt/fopen", artPath);
		af = fopen(artPath, "rb");
		if (af) {
			long sz;

			fseek(af, 0, SEEK_END);
			sz = ftell(af);
			fseek(af, 0, SEEK_SET);
			if (sz > 4 && sz <= 512L * 1024L) {
				tags->artData = (unsigned char *)AllocMem((unsigned long)sz,
					MEMF_ANY);
				if (tags->artData) {
					if (fread(tags->artData, 1, (size_t)sz, af) ==
						(size_t)sz) {
						tags->artBytes = (unsigned long)sz;
					} else {
						FreeMem(tags->artData, (unsigned long)sz);
						tags->artData = NULL;
						tags->artBytes = 0;
					}
				}
			}
			fclose(af);
		}
	}
}

static void ReadMp3Tags(const char *path, Mp3Tags *tags, int loadArt)
{
	FILE *f;
	unsigned char hdr[10];
	long firstFrameOffset;
	int hadId3v2;

	if (!tags)
		return;
	FreeTags(tags);
	memset(tags, 0, sizeof(*tags));
	if (is_url_path(path))
		return;
	GuiLogPathOp("ReadMp3Tags/fopen", path);
	f = fopen(path, "rb");
	if (!f)
		return;
	hadId3v2 = 0;
	firstFrameOffset = -1L;
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "ID3", 3) == 0) {
		hadId3v2 = 1;
		ReadId3v2Frames(f, tags, hdr, loadArt);
	} else {
		fseek(f, 0, SEEK_SET);
	}
	ReadMpegInfo(f, tags, &firstFrameOffset);
	if (tags->bitrateKbps > 0 && firstFrameOffset >= 0) {
		long fileSize;
		long audioBytes;

		if (fseek(f, 0, SEEK_END) == 0) {
			fileSize = ftell(f);
			tags->fileSize = fileSize > 0 ? (unsigned long)fileSize : 0;
			audioBytes = fileSize - firstFrameOffset;
			if (audioBytes > 0)
				tags->durationSecs = (int)(audioBytes * 8L /
					((long)tags->bitrateKbps * 1000L));
		}
	}
	if (tags->fileSize == 0 && fseek(f, 0, SEEK_END) == 0) {
		long fileSize = ftell(f);
		tags->fileSize = fileSize > 0 ? (unsigned long)fileSize : 0;
	}
	if (!hadId3v2)
		ReadId3v1(f, tags);
	fclose(f);
	if (loadArt)
		TryFolderArt(path, tags);
}

static void FormatReadyStatus(const Mp3Tags *tags, char *buf, size_t bufSize)
{
	if (tags && tags->bitrateKbps > 0 && tags->sampleRate > 0)
		sprintf(buf, "%d kbps / %d Hz - Ready.", tags->bitrateKbps,
			tags->sampleRate);
	else
		SafeCopy(buf, bufSize, "Ready.");
}

static void SetStatus(HelixAmp3Gui *gui, const char *text)
{
	if (!text)
		text = "";
	if (strcmp(gui->statusText, text) == 0)
		return;
	SafeCopy(gui->statusText, sizeof(gui->statusText), text);
	if (gui->win && gui->gadStatus) {
		GT_SetGadgetAttrs(gui->gadStatus, gui->win, NULL,
			GTTX_Text, (ULONG)gui->statusText,
			TAG_DONE);
	}
}

static void SetFileDisplay(HelixAmp3Gui *gui, const char *text)
{
	if (!text || !text[0])
		text = "<choose a file>";
	SafeCopy(gui->fileText, sizeof(gui->fileText), text);
	if (gui->win && gui->gadFile) {
		GT_SetGadgetAttrs(gui->gadFile, gui->win, NULL,
			GTTX_Text, (ULONG)gui->fileText,
			TAG_DONE);
	}
}

static int IsRadioInputName(const char *name)
{
	return is_url_path(name);
}

static void SetInternetStreamMetadata(HelixAmp3Gui *gui)
{
	if (!gui)
		return;
	FreeTags(&gui->tags);
	memset(&gui->tags, 0, sizeof(gui->tags));
	SafeCopy(gui->tags.title, sizeof(gui->tags.title), "Internet Radio");
	SafeCopy(gui->tags.artist, sizeof(gui->tags.artist), "-");
	SafeCopy(gui->tags.album, sizeof(gui->tags.album), "Internet Radio");
	SafeCopy(gui->tags.track, sizeof(gui->tags.track), "Live");
	SafeCopy(gui->fileInfoText, sizeof(gui->fileInfoText), "Internet Radio MP3/AAC/AAC+");
	if (gui->gadFileInfo)
		GT_SetGadgetAttrs(gui->gadFileInfo, gui->win, NULL,
			GTTX_Text, (ULONG)gui->fileInfoText, TAG_DONE);
	gui->totalSecs = 0;
}


static void CopyVolatileGuiString(char *dst, unsigned long dstSize, volatile const char *src)
{
	unsigned long i;
	char raw[256];
	if (!dst || dstSize == 0)
		return;
	if (!src) {
		dst[0] = 0;
		return;
	}
	for (i = 0; i + 1 < sizeof(raw) && src[i]; i++)
		raw[i] = (char)src[i];
	raw[i] = 0;
	AmigaUtf8ToDisplay(dst, dstSize, raw);
}

static void SplitRadioStreamTitle(const char *streamTitle, char *artist, unsigned long artistSize, char *title, unsigned long titleSize)
{
	const char *sep;
	char tmp[128];
	if (artist && artistSize) artist[0] = 0;
	if (title && titleSize) title[0] = 0;
	if (!streamTitle || !streamTitle[0])
		return;
	sep = strstr(streamTitle, " - ");
	if (!sep) {
		SafeCopy(title, titleSize, streamTitle);
		return;
	}
	SafeCopy(tmp, sizeof(tmp), streamTitle);
	sep = strstr(tmp, " - ");
	if (sep) {
		((char *)sep)[0] = 0;
		SafeCopy(artist, artistSize, tmp);
		SafeCopy(title, titleSize, sep + 3);
	}
}


static int RadioPlaybackHasStarted(void)
{
	return gGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
		gGuiPlaybackStatus.decodedFrames > 0;
}

static void FormatRadioStreamingStatus(HelixAmp3Gui *gui, const char *station, char *status, unsigned long statusSize)
{
	const char *name = station;
	char streamUrl[128];

	if (!status || statusSize == 0)
		return;
	if (!name || !name[0]) {
		CopyVolatileGuiString(streamUrl, sizeof(streamUrl), gGuiPlaybackStatus.radioStreamUrl);
		name = (gui && gui->currentRadioStationName[0]) ? gui->currentRadioStationName :
			(streamUrl[0] ? streamUrl : (gui && gui->inputName[0] ? gui->inputName : "Internet Radio"));
	}
	sprintf(status, "Streaming %.140s", name);
}

static void SetRadioFailureStatus(HelixAmp3Gui *gui, const char *fallback)
{
	char radioError[128], status[160];

	CopyVolatileGuiString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
	sprintf(status, "Stream failed: %s", radioError[0] ? radioError : fallback);
	SetStatus(gui, status);
	RadioSetStatus(gui, status);
}

static void UpdateRadioTagDisplay(HelixAmp3Gui *gui)
{
	char streamTitle[128], station[128], genre[64], contentType[64], artist[64], title[64], info[128], status[160];
	CopyVolatileGuiString(streamTitle, sizeof(streamTitle), gGuiPlaybackStatus.radioTitle);
	CopyVolatileGuiString(station, sizeof(station), gGuiPlaybackStatus.radioStationName);
	CopyVolatileGuiString(genre, sizeof(genre), gGuiPlaybackStatus.radioGenre);
	CopyVolatileGuiString(contentType, sizeof(contentType), gGuiPlaybackStatus.radioContentType);
	SetFileDisplay(gui, gui->inputName);
	SplitRadioStreamTitle(streamTitle, artist, sizeof(artist), title, sizeof(title));
	SafeCopy(gui->tags.title, sizeof(gui->tags.title), title[0] ? title : "-");
	SafeCopy(gui->tags.artist, sizeof(gui->tags.artist), artist[0] ? artist : "-");
	SafeCopy(gui->tags.album, sizeof(gui->tags.album), station[0] ? station : "Internet Radio");
	SafeCopy(gui->tags.track, sizeof(gui->tags.track), "Live");
	SafeCopy(gui->tags.genre, sizeof(gui->tags.genre), genre[0] ? genre : "-");
	gui->tags.bitrateKbps = gGuiPlaybackStatus.radioBitrateKbps;
	gui->tags.durationSecs = 0;
	gui->totalSecs = 0;
	if (gGuiPlaybackStatus.radioBitrateKbps > 0)
		sprintf(info, "Internet Radio MP3, %d kbps, %s", gGuiPlaybackStatus.radioBitrateKbps, contentType[0] ? contentType : "audio/mpeg");
	else
		sprintf(info, "Internet Radio MP3, %s", contentType[0] ? contentType : "audio/mpeg");
	UpdateTagDisplay(gui);
	SafeCopy(gui->fileInfoText, sizeof(gui->fileInfoText), info);
	if (gui->gadFileInfo)
		GT_SetGadgetAttrs(gui->gadFileInfo, gui->win, NULL,
			GTTX_Text, (ULONG)gui->fileInfoText, TAG_DONE);
	if (gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR) {
		SetRadioFailureStatus(gui, "radio error");
		return;
	}
	if (gGuiPlaybackStatus.radioStatus == RADIO_STATUS_BUFFERING &&
		!RadioPlaybackHasStarted()) {
		sprintf(status, "Buffering - %.140s", station[0] ? station :
			(gui->currentRadioStationName[0] ? gui->currentRadioStationName : "Internet Radio"));
		SetStatus(gui, status);
		RadioSetStatus(gui, status);
	} else if (gGuiPlaybackStatus.radioStatus == RADIO_STATUS_PLAYING ||
		(gGuiPlaybackStatus.radioStatus == RADIO_STATUS_BUFFERING && RadioPlaybackHasStarted())) {
		FormatRadioStreamingStatus(gui, station, status, sizeof(status));
		SetStatus(gui, status);
		RadioSetStatus(gui, status);
	}
}


static void FormatRatingText(HelixAmp3Gui *gui)
{
	int i;

	for (i = 0; i < 5; i++)
		gui->ratingText[i] = (i < gui->tags.rating) ? '*' : '-';
	sprintf(gui->ratingText + 5, " %d/5", gui->tags.rating);
}

static const char *MpegChannelModeName(const Mp3Tags *tags)
{
	if (!tags || tags->channels <= 0)
		return "?";
	if (tags->channelMode == 3 || tags->channels == 1)
		return "mono";
	if (tags->channelMode == 1) {
		/* In MPEG Layer III joint-stereo, mode-extension bit 1 denotes
		 * mid/side stereo.  Bit 0 denotes intensity stereo. */
		if (tags->modeExtension & 0x02)
			return "M/S";
		return "joint-stereo";
	}
	return "stereo";
}

static void FormatFileInfo(HelixAmp3Gui *gui)
{
	const char *ch = MpegChannelModeName(&gui->tags);
	unsigned long kb = (gui->tags.fileSize + 1023UL) / 1024UL;

	if (gui->tags.bitrateKbps > 0 || gui->tags.sampleRate > 0 ||
		gui->tags.fileSize > 0)
		sprintf(gui->fileInfoText, "%d kbps, %s, %d Hz, %lu KB",
			gui->tags.bitrateKbps, ch, gui->tags.sampleRate, kb);
	else
		SafeCopy(gui->fileInfoText, sizeof(gui->fileInfoText), "-");
}

static void SetRating(HelixAmp3Gui *gui, int rating)
{
	int i;

	if (rating < 0)
		rating = 0;
	if (rating > 5)
		rating = 5;
	gui->tags.rating = rating;
	FormatRatingText(gui);
	for (i = 0; i < 5; i++) {
		if (gui->win && gui->gadStars[i])
			GT_SetGadgetAttrs(gui->gadStars[i], gui->win, NULL,
				GA_Text, (ULONG)(i < rating ? "*" : "-"),
				TAG_DONE);
	}
	if (gui->win && gui->gadRatingValue)
		GT_SetGadgetAttrs(gui->gadRatingValue, gui->win, NULL,
			GTTX_Text, (ULONG)gui->ratingText,
			TAG_DONE);
}

static void UpdateTagDisplay(HelixAmp3Gui *gui)
{
	if (!gui->win)
		return;
	if (gui->gadTitle) {
		GT_SetGadgetAttrs(gui->gadTitle, gui->win, NULL,
			GTTX_Text, (ULONG)(gui->tags.title[0] ? gui->tags.title : "-"),
			TAG_DONE);
	}
	if (gui->gadArtist) {
		GT_SetGadgetAttrs(gui->gadArtist, gui->win, NULL,
			GTTX_Text, (ULONG)(gui->tags.artist[0] ? gui->tags.artist : "-"),
			TAG_DONE);
	}
	if (gui->gadAlbum) {
		GT_SetGadgetAttrs(gui->gadAlbum, gui->win, NULL,
			GTTX_Text, (ULONG)(gui->tags.album[0] ? gui->tags.album : "-"),
			TAG_DONE);
	}
	if (gui->gadTrack) {
		GT_SetGadgetAttrs(gui->gadTrack, gui->win, NULL,
			GTTX_Text, (ULONG)(gui->tags.track[0] ? gui->tags.track : "-"),
			TAG_DONE);
	}
	if (gui->gadGenre) {
		GT_SetGadgetAttrs(gui->gadGenre, gui->win, NULL,
			GTTX_Text, (ULONG)(gui->tags.genre[0] ? gui->tags.genre : "-"),
			TAG_DONE);
	}
	SetRating(gui, gui->tags.rating);
	FormatFileInfo(gui);
	if (gui->gadFileInfo) {
		GT_SetGadgetAttrs(gui->gadFileInfo, gui->win, NULL,
			GTTX_Text, (ULONG)gui->fileInfoText,
			TAG_DONE);
	}
}


static const unsigned char kBayer8x8[8][8] = {
	{  0, 32,  8, 40,  2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 }
};

static int PeekJpegDimensions(const unsigned char *data, unsigned long size,
	int *outW, int *outH)
{
	unsigned long pos = 2;
	if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
		return 0;
	while (pos + 4 <= size) {
		unsigned int segLen;
		unsigned char marker;
		if (data[pos] != 0xFF)
			return 0;
		marker = data[pos + 1];
		segLen = ((unsigned int)data[pos + 2] << 8) | data[pos + 3];
		if ((marker >= 0xC0 && marker <= 0xC3) ||
			(marker >= 0xC5 && marker <= 0xC7) ||
			(marker >= 0xC9 && marker <= 0xCB) ||
			(marker >= 0xCD && marker <= 0xCF)) {
			if (pos + 9 <= size) {
				*outH = ((int)data[pos + 5] << 8) | data[pos + 6];
				*outW = ((int)data[pos + 7] << 8) | data[pos + 8];
				return (*outW > 0 && *outH > 0) ? 1 : 0;
			}
		}
		if (segLen < 2)
			return 0;
		pos += 2 + segLen;
	}
	return 0;
}

static unsigned char pjpeg_cb(unsigned char *buf, unsigned char buf_size,
	unsigned char *bytes_actually_read, void *ud)
{
	PjpegSrc *src = (PjpegSrc *)ud;
	unsigned long left;
	unsigned char n;

	left = src->size - src->pos;
	n = (unsigned char)(left < (unsigned long)buf_size ? left :
		(unsigned long)buf_size);
	if (n) {
		memcpy(buf, src->data + src->pos, n);
		src->pos += n;
	}
	*bytes_actually_read = n;
	return 0;
}


static void ArtNow(unsigned long *secs, unsigned long *micros)
{
#if defined(AMIGA_M68K)
	ULONG s;
	ULONG u;
	CurrentTime(&s, &u);
	*secs = (unsigned long)s;
	*micros = (unsigned long)u;
#else
	*secs = 0;
	*micros = 0;
#endif
}

static unsigned long ArtElapsedMicros(unsigned long startSecs, unsigned long startMicros)
{
	unsigned long secs;
	unsigned long micros;
	ArtNow(&secs, &micros);
	if (secs < startSecs)
		return 0;
	if (micros < startMicros) {
		if (secs == startSecs)
			return 0;
		secs--;
		micros += 1000000UL;
	}
	return (secs - startSecs) * 1000000UL + (micros - startMicros);
}

static const char *JpegScanTypeName(int scanType)
{
	switch (scanType) {
	case PJPG_GRAYSCALE: return "grayscale";
	case PJPG_YH1V1: return "YH1V1";
	case PJPG_YH2V1: return "YH2V1";
	case PJPG_YH1V2: return "YH1V2";
	case PJPG_YH2V2: return "YH2V2";
	default: return "?";
	}
}

static void ArtAccumSample(unsigned long *accum, unsigned short *count,
	int dst, int grey, unsigned long weight)
{
	if (!weight)
		return;
	accum[dst] += (unsigned long)grey * weight;
	if ((unsigned long)count[dst] + weight > 0xffffUL)
		count[dst] = 0xffff;
	else
		count[dst] = (unsigned short)(count[dst] + weight);
}

static void ArtAccumReducedBlock(const pjpeg_image_info_t *info,
	unsigned long *accum, unsigned short *count, int outW, int outH,
	int srcX0, int srcY0, int blockW, int blockH, int grey)
{
	int srcX1 = srcX0 + blockW;
	int srcY1 = srcY0 + blockH;
	int dstX0;
	int dstX1;
	int dstY0;
	int dstY1;
	int dy;
	if (srcX0 >= info->m_width || srcY0 >= info->m_height)
		return;
	if (srcX1 > info->m_width)
		srcX1 = info->m_width;
	if (srcY1 > info->m_height)
		srcY1 = info->m_height;
	dstX0 = (srcX0 * outW) / info->m_width;
	dstX1 = ((srcX1 * outW) + info->m_width - 1) / info->m_width;
	dstY0 = (srcY0 * outH) / info->m_height;
	dstY1 = ((srcY1 * outH) + info->m_height - 1) / info->m_height;
	if (dstX1 > outW) dstX1 = outW;
	if (dstY1 > outH) dstY1 = outH;
	for (dy = dstY0; dy < dstY1; dy++) {
		int cellY0 = (dy * info->m_height + outH - 1) / outH;
		int cellY1 = ((dy + 1) * info->m_height + outH - 1) / outH;
		int oy0 = cellY0 > srcY0 ? cellY0 : srcY0;
		int oy1 = cellY1 < srcY1 ? cellY1 : srcY1;
		int dx;
		if (oy1 <= oy0)
			continue;
		for (dx = dstX0; dx < dstX1; dx++) {
			int cellX0 = (dx * info->m_width + outW - 1) / outW;
			int cellX1 = ((dx + 1) * info->m_width + outW - 1) / outW;
			int ox0 = cellX0 > srcX0 ? cellX0 : srcX0;
			int ox1 = cellX1 < srcX1 ? cellX1 : srcX1;
			if (ox1 > ox0)
				ArtAccumSample(accum, count, dy * outW + dx, grey,
					(unsigned long)(ox1 - ox0) * (unsigned long)(oy1 - oy0));
		}
	}
}

static void ArtAccumSampleColor(unsigned long *greyAcc,
	unsigned long *rAcc, unsigned long *gAcc, unsigned long *bAcc,
	unsigned short *count, int dst,
	int grey, unsigned char r, unsigned char g, unsigned char b,
	unsigned long weight)
{
	if (!weight)
		return;
	greyAcc[dst] += (unsigned long)grey * weight;
	rAcc[dst] += (unsigned long)r * weight;
	gAcc[dst] += (unsigned long)g * weight;
	bAcc[dst] += (unsigned long)b * weight;
	if ((unsigned long)count[dst] + weight > 0xffffUL)
		count[dst] = 0xffff;
	else
		count[dst] = (unsigned short)(count[dst] + weight);
}

static void ArtAccumReducedBlockColor(const pjpeg_image_info_t *info,
	unsigned long *greyAcc, unsigned long *rAcc, unsigned long *gAcc,
	unsigned long *bAcc, unsigned short *count, int outW, int outH,
	int srcX0, int srcY0, int blockW, int blockH,
	int grey, unsigned char r, unsigned char g, unsigned char b)
{
	int srcX1 = srcX0 + blockW;
	int srcY1 = srcY0 + blockH;
	int dstX0, dstX1, dstY0, dstY1;
	int dy;
	if (srcX0 >= info->m_width || srcY0 >= info->m_height)
		return;
	if (srcX1 > info->m_width)  srcX1 = info->m_width;
	if (srcY1 > info->m_height) srcY1 = info->m_height;
	dstX0 = (srcX0 * outW) / info->m_width;
	dstX1 = ((srcX1 * outW) + info->m_width - 1) / info->m_width;
	dstY0 = (srcY0 * outH) / info->m_height;
	dstY1 = ((srcY1 * outH) + info->m_height - 1) / info->m_height;
	if (dstX1 > outW) dstX1 = outW;
	if (dstY1 > outH) dstY1 = outH;
	for (dy = dstY0; dy < dstY1; dy++) {
		int cellY0 = (dy * info->m_height + outH - 1) / outH;
		int cellY1 = ((dy + 1) * info->m_height + outH - 1) / outH;
		int oy0 = cellY0 > srcY0 ? cellY0 : srcY0;
		int oy1 = cellY1 < srcY1 ? cellY1 : srcY1;
		int dx;
		if (oy1 <= oy0)
			continue;
		for (dx = dstX0; dx < dstX1; dx++) {
			int cellX0 = (dx * info->m_width + outW - 1) / outW;
			int cellX1 = ((dx + 1) * info->m_width + outW - 1) / outW;
			int ox0 = cellX0 > srcX0 ? cellX0 : srcX0;
			int ox1 = cellX1 < srcX1 ? cellX1 : srcX1;
			if (ox1 > ox0)
				ArtAccumSampleColor(greyAcc, rAcc, gAcc, bAcc, count,
					dy * outW + dx, grey, r, g, b,
					(unsigned long)(ox1 - ox0) *
					(unsigned long)(oy1 - oy0));
		}
	}
}

static int McuSampleOffset(const pjpeg_image_info_t *info, int x, int y)
{
	int blockX = x / 8;
	int blockY = y / 8;
	int blocksPerRow = info->m_MCUWidth / 8;
	int block = blockY * blocksPerRow + blockX;

	return block * 64 + (y & 7) * 8 + (x & 7);
}

static int JpegGreySample(const pjpeg_image_info_t *info, int off);
static int JpegSampleRGB(const pjpeg_image_info_t *info, int off,
	unsigned char *r, unsigned char *g, unsigned char *b);

static int DecodeJpegToGreyMode(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, int outW, int outH, int isPng, int reduce,
	unsigned long *elapsedMicros)
{
	pjpeg_image_info_t info;
	PjpegSrc src;
	unsigned char status;
	unsigned char xMap[MAX_JPEG_DIM];
	unsigned char yMap[MAX_JPEG_DIM];
	static unsigned long greyAccum[ART_W * ART_H];
	static unsigned short greyCount[ART_W * ART_H];
	unsigned long t0s;
	unsigned long t0u;
	int mcuIndex;
	int i;

	if (elapsedMicros)
		*elapsedMicros = 0;
	if (isPng || !jpegData || jpegBytes <= 4 || !greyOut ||
		outW <= 0 || outW > ART_W || outH <= 0 || outH > ART_H)
		return -1;
	ArtNow(&t0s, &t0u);
	src.data = jpegData;
	src.pos = 0;
	src.size = jpegBytes;
	memset(greyOut, 0x80, (size_t)(outW * outH));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(greyCount, 0, sizeof(greyCount));
	status = pjpeg_decode_init(&info, pjpeg_cb, &src, reduce ? 1 : 0);
	if (status != 0 || info.m_width <= 0 || info.m_height <= 0 ||
		info.m_width > MAX_JPEG_DIM || info.m_height > MAX_JPEG_DIM) {
		pjpeg_decode_free();
		return -1;
	}
	for (i = 0; i < info.m_width; i++)
		xMap[i] = (unsigned char)((i * outW) / info.m_width);
	for (i = 0; i < info.m_height; i++)
		yMap[i] = (unsigned char)((i * outH) / info.m_height);

	for (mcuIndex = 0; mcuIndex < info.m_MCUSPerRow * info.m_MCUSPerCol;
		mcuIndex++) {
		int mcuX;
		int mcuY;
		int y;

		status = pjpeg_decode_mcu();
		if (status == PJPG_NO_MORE_BLOCKS)
			break;
		if (status != 0) {
			pjpeg_decode_free();
			return -1;
		}
		mcuX = (mcuIndex % info.m_MCUSPerRow) * info.m_MCUWidth;
		mcuY = (mcuIndex / info.m_MCUSPerRow) * info.m_MCUHeight;
		if (reduce) {
			int by;
			int bx;
			for (by = 0; by < info.m_MCUHeight; by += 8) {
				for (bx = 0; bx < info.m_MCUWidth; bx += 8) {
					int off = McuSampleOffset(&info, bx, by);
					ArtAccumReducedBlock(&info, greyAccum, greyCount, outW, outH,
						mcuX + bx, mcuY + by, 8, 8, JpegGreySample(&info, off));
				}
			}
		} else for (y = 0; y < info.m_MCUHeight; y++) {
			int srcY = mcuY + y;
			int dstY;
			int x;

			if (srcY >= info.m_height)
				continue;
			dstY = yMap[srcY];
			for (x = 0; x < info.m_MCUWidth; x++) {
				int srcX = mcuX + x;
				if (srcX >= info.m_width)
					continue;
				ArtAccumSample(greyAccum, greyCount, dstY * outW + xMap[srcX],
					JpegGreySample(&info, McuSampleOffset(&info, x, y)), 1);
			}
		}
	}
	for (i = 0; i < outW * outH; i++) {
		if (greyCount[i])
			greyOut[i] = (unsigned char)((greyAccum[i] +
				(greyCount[i] / 2)) / greyCount[i]);
	}
	pjpeg_decode_free();
	if (elapsedMicros)
		*elapsedMicros = ArtElapsedMicros(t0s, t0u);
	return 0;
}

static int DecodeJpegToGrey(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, int outW, int outH, int isPng)
{
	return DecodeJpegToGreyMode(jpegData, jpegBytes, greyOut, outW, outH,
		isPng, MINIAMP3_ART_REDUCED_JPEG, NULL);
}




static void ApplyHardwareAudioFilter(HelixAmp3Gui *gui)
{
#if defined(AMIGA_M68K)
	/* The Amiga/CD32 analogue audio filter is controlled through CIA-A port A,
	 * bit 1, the same bit used for the power LED brightness.  It is global to
	 * the machine and independent of audio.device's Paula channel ownership.
	 * Low bit enables the filter/bright LED; high bit disables it/dims LED. */
	Forbid();
	if (gui && gui->hardwareFilter)
		ciaa.ciapra &= (UBYTE)~CIAF_LED;
	else
		ciaa.ciapra |= CIAF_LED;
	Permit();
#else
	(void)gui;
#endif
}

static void DrawFilterButton(HelixAmp3Gui *gui)
{
	struct RastPort *rp;
	int x, y;

	if (!gui || !gui->win || !gui->gadHardwareFilter)
		return;
	rp = gui->win->RPort;
	x = gui->gadHardwareFilter->LeftEdge + 8;
	y = gui->gadHardwareFilter->TopEdge + 5;
	SetAPen(rp, 1);
	Move(rp, x, y);
	Draw(rp, x, y + 9);
	Move(rp, x, y);
	Draw(rp, x + 9, y);
	Move(rp, x, y + 4);
	Draw(rp, x + 7, y + 4);
	Move(rp, x + 14, y);
	Draw(rp, x + 14, y + 9);
	Draw(rp, x + 23, y + 9);
	Move(rp, x + 28, y);
	Draw(rp, x + 39, y);
	Move(rp, x + 33, y);
	Draw(rp, x + 33, y + 9);
	if (gui->hardwareFilter) {
		RectFill(rp, gui->gadHardwareFilter->LeftEdge + 2,
			gui->gadHardwareFilter->TopEdge + 2,
			gui->gadHardwareFilter->LeftEdge + 5,
			gui->gadHardwareFilter->TopEdge + 5);
	}
}

static void DrawArtPanel(HelixAmp3Gui *gui);
static void DrawTransportIcons(HelixAmp3Gui *gui);
static void DrawFilterButton(HelixAmp3Gui *gui);
static void ApplyHardwareAudioFilter(HelixAmp3Gui *gui);
static void HandleDoneSignal(HelixAmp3Gui *gui);
static void SaveArtworkCache(HelixAmp3Gui *gui);
static void BuildArtColorPens(HelixAmp3Gui *gui);
static void ReleaseArtColorPens(HelixAmp3Gui *gui);

/* Radio network ownership boundary: bsdsocket.library/AmiSSL library bases are
 * task/process-owned here; same-image CreateNewProcTags children must not use
 * parent/global SocketBase/AmiSSL state.  Radio playback is therefore launched
 * as a separate decoder executable so DNS, socket/connect, SSL_connect/read and
 * SSL/CTX/AmiSSL cleanup happen in that executable's own data segment. */
static void GuiBuildRadioWorkerArgs(char *out, int outSize, char **argv, int argc)
{
	int i;
	int pos = 0;
	if (!out || outSize <= 0) return;
	out[0] = '\0';
	for (i = 1; i < argc; i++) {
		const char *a = argv[i] ? argv[i] : "";
		int needQuote = strchr(a, ' ') || strchr(a, '\t') || strchr(a, '"');
		if (pos + 4 >= outSize) break;
		if (i > 1) out[pos++] = ' ';
		if (needQuote) out[pos++] = '"';
		while (*a && pos + 3 < outSize) {
			if (*a == '"') out[pos++] = '\'';
			else out[pos++] = *a;
			a++;
		}
		if (needQuote && pos + 1 < outSize) out[pos++] = '"';
	}
	out[pos] = '\0';
}

static void StartPlayback(HelixAmp3Gui *gui);
static void ClosePlaylistWindow(HelixAmp3Gui *gui);
static void OpenPlaylistWindow(HelixAmp3Gui *gui);
static void RefreshPlaylistView(HelixAmp3Gui *gui);
static void HandlePlaylistPoll(HelixAmp3Gui *gui);
static void PlaylistLoadM3U(HelixAmp3Gui *gui);
static void PlaylistSaveM3U(HelixAmp3Gui *gui);

static int JpegGreySample(const pjpeg_image_info_t *info, int off)
{
	unsigned long r;
	unsigned long g;
	unsigned long b;

	if (info->m_comps == 1)
		return info->m_pMCUBufR[off];
	r = info->m_pMCUBufR[off];
	g = info->m_pMCUBufG[off];
	b = info->m_pMCUBufB[off];
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_JPEG_GREY)
	/* Approximate Rec.601 luma as (77R + 150G + 29B + 128) >> 8.
	 * This removes the previous DIVU-by-100 from the per-pixel artwork hot path. */
	__asm__ volatile (
		"mulu.w #77,%0\n\t"
		"mulu.w #150,%1\n\t"
		"mulu.w #29,%2\n\t"
		"add.l %1,%0\n\t"
		"add.l %2,%0\n\t"
		"add.l #128,%0\n\t"
		"lsr.l #8,%0"
		: "+d" (r), "+d" (g), "+d" (b));
	return (int)r;
#else
	return (int)((77UL * r + 150UL * g + 29UL * b + 128UL) >> 8);
#endif
}

static int JpegSampleRGB(const pjpeg_image_info_t *info, int off,
	unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (info->m_comps == 1) {
		unsigned char y = info->m_pMCUBufR[off];
		*r = *g = *b = y;
		return (int)y;
	}
	*r = info->m_pMCUBufR[off];
	*g = info->m_pMCUBufG[off];
	*b = info->m_pMCUBufB[off];
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_JPEG_GREY)
	{
		unsigned long rv = *r;
		unsigned long gv = *g;
		unsigned long bv = *b;
		__asm__ volatile (
			"mulu.w #77,%0\n\t"
			"mulu.w #150,%1\n\t"
			"mulu.w #29,%2\n\t"
			"add.l %1,%0\n\t"
			"add.l %2,%0\n\t"
			"add.l #128,%0\n\t"
			"lsr.l #8,%0"
			: "+d" (rv), "+d" (gv), "+d" (bv));
		return (int)rv;
	}
#else
	return (int)((77UL * *r + 150UL * *g + 29UL * *b + 128UL) >> 8);
#endif
}

static void FinishArtDecode(HelixAmp3Gui *gui, int ok)
{
	ArtDecodeState *st = &gui->artDecode;
	int i;

	pjpeg_decode_free();
	if (ok) {
#ifdef MINIAMP3_DEBUG
		unsigned long totalMicros = ArtElapsedMicros(st->startSecs, st->startMicros);
		Printf("artwork done: reduce=%s pumps=%lu decode_us=%lu process_us=%lu total_us=%lu cache=miss\n",
			MINIAMP3_DEBUG_FMT_PTR(st->reduce ? "yes" : "no"),
			st->pumpCount, st->decodeMicros, st->processMicros, totalMicros);
#endif
		for (i = 0; i < ART_W * ART_H; i++) {
			if (st->greyCount[i]) {
				unsigned short c = st->greyCount[i];
				unsigned long half = (unsigned long)c >> 1;
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_JPEG_GREY)
				/* DIVU.W (32/16) is ~2x faster than DIVU.L (32/32) on 68020.
				 * Safe here: max quotient is 255, fits in the 16-bit result. */
				{
					unsigned long d = st->greyAccum[i] + half;
					__asm__ volatile ("divu.w %1,%0" : "+d"(d) : "dm"(c));
					st->greyOut[i] = (unsigned char)(unsigned short)d;
				}
				if (st->wantColor) {
					unsigned long d;
					d = st->rAccum[i] + half;
					__asm__ volatile ("divu.w %1,%0" : "+d"(d) : "dm"(c));
					gui->artRGBBuf[i * 3    ] = (unsigned char)(unsigned short)d;
					d = st->gAccum[i] + half;
					__asm__ volatile ("divu.w %1,%0" : "+d"(d) : "dm"(c));
					gui->artRGBBuf[i * 3 + 1] = (unsigned char)(unsigned short)d;
					d = st->bAccum[i] + half;
					__asm__ volatile ("divu.w %1,%0" : "+d"(d) : "dm"(c));
					gui->artRGBBuf[i * 3 + 2] = (unsigned char)(unsigned short)d;
				} else {
					gui->artRGBBuf[i * 3    ] =
					gui->artRGBBuf[i * 3 + 1] =
					gui->artRGBBuf[i * 3 + 2] = st->greyOut[i];
				}
#else
				st->greyOut[i] = (unsigned char)((st->greyAccum[i] + half) / c);
				if (st->wantColor) {
					gui->artRGBBuf[i * 3    ] = (unsigned char)((st->rAccum[i] + half) / c);
					gui->artRGBBuf[i * 3 + 1] = (unsigned char)((st->gAccum[i] + half) / c);
					gui->artRGBBuf[i * 3 + 2] = (unsigned char)((st->bAccum[i] + half) / c);
				} else {
					gui->artRGBBuf[i * 3    ] =
					gui->artRGBBuf[i * 3 + 1] =
					gui->artRGBBuf[i * 3 + 2] = st->greyOut[i];
				}
#endif
			}
		}
		memcpy(gui->artGreyBuf, st->greyOut, ART_W * ART_H);
		gui->artValid = 1;
		if (gui->artColorEnabled)
			BuildArtColorPens(gui);
		/* The GUI and playback child share the same AmigaDOS/C runtime state.
		 * Avoid overlapping artwork-cache writes with playback startup. */
		if (gui->playbackActive)
			gui->artCacheSavePending = 1;
		else
			SaveArtworkCache(gui);
	}
	st->active = 0;
	gui->artLoading = 0;
	DrawArtPanel(gui);
	DrawTransportIcons(gui);
	DrawFilterButton(gui);
}

static void CancelArtDecode(HelixAmp3Gui *gui)
{
	ArtDecodeState *st = &gui->artDecode;

	if (!st->active && !gui->artLoading)
		return;
	st->active = 0;
	gui->artLoading = 0;
	pjpeg_decode_free();
	ReleaseArtColorPens(gui);
	DrawArtPanel(gui);
}

static void PumpArtDecode(HelixAmp3Gui *gui)
{
	ArtDecodeState *st = &gui->artDecode;
	int pumped;

	if (!st->active)
		return;
	st->pumpCount++;
	for (pumped = 0; pumped < ART_MCUS_PER_PUMP && st->active; pumped++) {
		unsigned char status;
		int mcuX;
		int mcuY;
		int y;
		unsigned long t0s;
		unsigned long t0u;

		if (st->mcuIndex >= st->totalMcus) {
			FinishArtDecode(gui, 1);
			break;
		}
		ArtNow(&t0s, &t0u);
		status = pjpeg_decode_mcu();
		st->decodeMicros += ArtElapsedMicros(t0s, t0u);
		if (status == PJPG_NO_MORE_BLOCKS) {
			FinishArtDecode(gui, 1);
			break;
		}
		if (status != 0) {
			FinishArtDecode(gui, 0);
			break;
		}
		mcuX = (st->mcuIndex % st->info.m_MCUSPerRow) * st->info.m_MCUWidth;
		mcuY = (st->mcuIndex / st->info.m_MCUSPerRow) * st->info.m_MCUHeight;
		st->mcuIndex++;
		ArtNow(&t0s, &t0u);
		if (st->reduce) {
			int by;
			int bx;
			for (by = 0; by < st->info.m_MCUHeight; by += 8) {
				for (bx = 0; bx < st->info.m_MCUWidth; bx += 8) {
					int off = McuSampleOffset(&st->info, bx, by);
					if (st->wantColor) {
						unsigned char r, g, b;
						int grey = JpegSampleRGB(&st->info, off, &r, &g, &b);
						ArtAccumReducedBlockColor(&st->info,
							st->greyAccum, st->rAccum, st->gAccum, st->bAccum,
							st->greyCount, ART_W, ART_H,
							mcuX + bx, mcuY + by, 8, 8, grey, r, g, b);
					} else {
						ArtAccumReducedBlock(&st->info,
							st->greyAccum, st->greyCount, ART_W, ART_H,
							mcuX + bx, mcuY + by, 8, 8,
							JpegGreySample(&st->info, off));
					}
				}
			}
		} else for (y = 0; y < st->info.m_MCUHeight; y++) {
			int srcY = mcuY + y;
			int dstY;
			int x;

			if (srcY >= st->info.m_height)
				continue;
			dstY = st->yMap[srcY];
			for (x = 0; x < st->info.m_MCUWidth; x++) {
				int srcX = mcuX + x;
				int dst;

				if (srcX >= st->info.m_width)
					continue;
				dst = dstY * ART_W + st->xMap[srcX];
				if (st->wantColor) {
					unsigned char r, g, b;
					int grey = JpegSampleRGB(&st->info,
						McuSampleOffset(&st->info, x, y), &r, &g, &b);
					ArtAccumSampleColor(st->greyAccum, st->rAccum,
						st->gAccum, st->bAccum, st->greyCount,
						dst, grey, r, g, b, 1);
				} else {
					ArtAccumSample(st->greyAccum, st->greyCount, dst,
						JpegGreySample(&st->info,
							McuSampleOffset(&st->info, x, y)), 1);
				}
			}
		}
		st->processMicros += ArtElapsedMicros(t0s, t0u);
	}
}

static void ArtworkCacheName(HelixAmp3Gui *gui, char *dst, size_t dstSize)
{
	const char *source, *base, *end;
	char safe[80];
	int i;
	int j;

	EnvName(dst, dstSize, "ArtCache");
	/* Radio input has no local path to key the cache off of; key it by the
	 * station favicon URL instead so the fetch is skipped on the next visit
	 * to the same station, matching minimp3r's ArtworkCacheName(). */
	source = (is_url_path(gui->inputName) && gui->currentRadioFavicon[0]) ?
		gui->currentRadioFavicon : gui->inputName;
	end = strchr(source, '?');
	if (!end)
		end = source + strlen(source);
	base = end;
	while (base > source && base[-1] != '/' && base[-1] != ':')
		base--;
	for (i = 0, j = 0; base + i < end && base[i] && j < (int)sizeof(safe) - 1; i++) {
		unsigned char c = (unsigned char)base[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9'))
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

static int LoadArtworkCache(HelixAmp3Gui *gui)
{
	char path[HELIXAMP3_MAX_PATH];
	FILE *f;
	unsigned char hdr[8];

	if (!gui->artCacheEnabled || gui->artCacheBypass || !gui->inputName[0])
		return 0;
	ArtworkCacheName(gui, path, sizeof(path));
	GuiLogPathOp("LoadArtworkCache/fopen", path);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
		memcmp(hdr, "M3AG64\0", 7) == 0 &&
		(hdr[7] == 1 || hdr[7] == 2) &&
		fread(gui->artGreyBuf, 1, ART_W * ART_H, f) == ART_W * ART_H) {
		int i;
		if (hdr[7] == 2 &&
			fread(gui->artRGBBuf, 1, ART_W * ART_H * 3, f) == ART_W * ART_H * 3) {
			/* v2: grey + RGB loaded */
		} else {
			/* v1 or partial: derive RGB from grey */
			for (i = 0; i < ART_W * ART_H; i++) {
				unsigned char g = gui->artGreyBuf[i];
				gui->artRGBBuf[i * 3    ] = g;
				gui->artRGBBuf[i * 3 + 1] = g;
				gui->artRGBBuf[i * 3 + 2] = g;
			}
		}
		fclose(f);
		gui->artValid = 1;
		gui->artLoading = 0;
		return 1;
	}
	fclose(f);
	return 0;
}

static void SaveArtworkCache(HelixAmp3Gui *gui)
{
	char dir[64];
	char path[HELIXAMP3_MAX_PATH];
	FILE *f;
	static const unsigned char hdr[8] = { 'M','3','A','G','6','4','\0', 2 };

	if (!gui->artCacheEnabled || !gui->inputName[0] || !gui->artValid)
		return;
	EnvName(dir, sizeof(dir), "ArtCache");
	CreateDir((STRPTR)dir);
	ArtworkCacheName(gui, path, sizeof(path));
	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite(gui->artGreyBuf, 1, ART_W * ART_H, f);
	fwrite(gui->artRGBBuf, 1, ART_W * ART_H * 3, f);
	fclose(f);
}

static void CleanArtworkCache(HelixAmp3Gui *gui)
{
	char dir[64];
	BPTR lock;
	struct FileInfoBlock *fib;
	int removed = 0;

	EnvName(dir, sizeof(dir), "ArtCache");
	lock = SafeLockPath("CleanArtworkCache/Lock", dir, ACCESS_READ);
	if (!lock) {
		SetStatus(gui, "Artwork cache is empty.");
		return;
	}
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(lock, fib)) {
		while (ExNext(lock, fib)) {
			char path[HELIXAMP3_MAX_PATH];
			int len = strlen(fib->fib_FileName);

			if (fib->fib_DirEntryType >= 0 || len < 7 ||
				strcmp(fib->fib_FileName + len - 7, ".grey64") != 0)
				continue;
			SafeCopy(path, sizeof(path), dir);
			strncat(path, "/", sizeof(path) - strlen(path) - 1);
			strncat(path, fib->fib_FileName, sizeof(path) - strlen(path) - 1);
			if (DeleteFile((STRPTR)path))
				removed++;
		}
	}
	if (fib)
		FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	if (removed) {
		char msg[64];
		sprintf(msg, "Removed %d cached artwork file(s).", removed);
		SetStatus(gui, msg);
	} else
		SetStatus(gui, "No cached artwork files to remove.");
}

#if ENABLE_RADIO_ARTWORK
static int GuiIsJpegMagic(const unsigned char *data, int bytes)
{
	return bytes >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

/* Detected only to skip a PNG-encoded ICO entry gracefully (GadTools does
 * not link a PNG decoder -- see the ENABLE_RADIO_ARTWORK comment above);
 * never decoded. */
static int GuiIsPngMagic(const unsigned char *data, int bytes)
{
	static const unsigned char sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	return bytes >= 8 && memcmp(data, sig, sizeof(sig)) == 0;
}

static int GuiIsIcoMagic(const unsigned char *data, int bytes)
{
	/* ICONDIR: 2 bytes reserved (0), 2 bytes type (1 = icon, 2 = cursor). */
	return bytes >= 6 && data[0] == 0 && data[1] == 0 &&
		data[2] == 1 && data[3] == 0;
}

static unsigned GuiIcoLE16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long GuiIcoLE32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
		((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

typedef struct GuiIcoEntry {
	unsigned width, height, bitCount;
	unsigned long size, offset;
} GuiIcoEntry;

/* Decodes a favicon JPEG straight into the grey+RGB thumbnail buffers in
 * one call (unlike StartArtDecode()'s chunked MCU-by-MCU pump for local
 * ID3/folder art): favicons are small and bounded by
 * HELIXAMP3_FAVICON_MAX_BYTES, and this only ever runs once per station
 * selection, so there is no need to spread the decode across GUI ticks. */
static int DecodeFaviconJpegToGrey(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	pjpeg_image_info_t info;
	PjpegSrc src;
	unsigned char status;
	unsigned char xMap[MAX_JPEG_DIM];
	unsigned char yMap[MAX_JPEG_DIM];
	static unsigned long greyAccum[ART_W * ART_H];
	static unsigned long rAccum[ART_W * ART_H];
	static unsigned long gAccum[ART_W * ART_H];
	static unsigned long bAccum[ART_W * ART_H];
	static unsigned short greyCount[ART_W * ART_H];
	int mcuIndex;
	int i;

	if (!jpegData || jpegBytes <= 4 || !greyOut ||
		outW <= 0 || outW > ART_W || outH <= 0 || outH > ART_H)
		return -1;
	src.data = jpegData;
	src.pos = 0;
	src.size = jpegBytes;
	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(rAccum, 0, sizeof(rAccum));
	memset(gAccum, 0, sizeof(gAccum));
	memset(bAccum, 0, sizeof(bAccum));
	memset(greyCount, 0, sizeof(greyCount));
	status = pjpeg_decode_init(&info, pjpeg_cb, &src, 0);
	if (status != 0 || info.m_width <= 0 || info.m_height <= 0 ||
		info.m_width > MAX_JPEG_DIM || info.m_height > MAX_JPEG_DIM) {
		pjpeg_decode_free();
		return -1;
	}
	for (i = 0; i < info.m_width; i++)
		xMap[i] = (unsigned char)((i * outW) / info.m_width);
	for (i = 0; i < info.m_height; i++)
		yMap[i] = (unsigned char)((i * outH) / info.m_height);
	for (mcuIndex = 0; mcuIndex < info.m_MCUSPerRow * info.m_MCUSPerCol; mcuIndex++) {
		int mcuX, mcuY, y;

		status = pjpeg_decode_mcu();
		if (status == PJPG_NO_MORE_BLOCKS)
			break;
		if (status != 0) {
			pjpeg_decode_free();
			return -1;
		}
		mcuX = (mcuIndex % info.m_MCUSPerRow) * info.m_MCUWidth;
		mcuY = (mcuIndex / info.m_MCUSPerRow) * info.m_MCUHeight;
		for (y = 0; y < info.m_MCUHeight; y++) {
			int srcY = mcuY + y;
			int dstY;
			int x;

			if (srcY >= info.m_height)
				continue;
			dstY = yMap[srcY];
			for (x = 0; x < info.m_MCUWidth; x++) {
				int srcX = mcuX + x;
				unsigned char r, g, b;
				int grey;

				if (srcX >= info.m_width)
					continue;
				grey = JpegSampleRGB(&info, McuSampleOffset(&info, x, y), &r, &g, &b);
				ArtAccumSampleColor(greyAccum, rAccum, gAccum, bAccum, greyCount,
					dstY * outW + xMap[srcX], grey, r, g, b, 1);
			}
		}
	}
	pjpeg_decode_free();
	for (i = 0; i < outW * outH; i++) {
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			unsigned long half = (unsigned long)c >> 1;

			greyOut[i] = (unsigned char)((greyAccum[i] + half) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + half) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + half) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + half) / c);
			}
		}
	}
	return 0;
}

/* Decodes the raw BITMAPINFOHEADER-style DIB embedded in a legacy
 * (non-PNG) ICO entry.  Only the depths real-world icon tools actually
 * emit are supported (32/24/8bpp, uncompressed); 4bpp/1bpp and
 * RLE-compressed DIBs are rejected.  The AND (transparency) mask that
 * follows the pixel data is ignored -- favicons are rarely meaningfully
 * transparent.  Ported from minimp3r's DecodeIcoDibToGrey(). */
static int DecodeIcoDibToGrey(const unsigned char *dib, unsigned long dibBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	unsigned long headerSize;
	long width, height;
	unsigned bitCount;
	unsigned long compression, paletteEntries, rowBytes;
	const unsigned char *palette;
	const unsigned char *pixels;
	/* Indexed by source pixel column/row (0..width-1 / 0..height-1), not by
	 * output size -- width/height are bounded to <=256 above, so size these
	 * for the source range, not ART_W/ART_H (the downsampled output is only
	 * written through xMap[]/yMap[]'s *values*, not their index range). */
	unsigned char xMap[256], yMap[256];
	static unsigned long greyAccum[ART_W * ART_H];
	static unsigned long rAccum[ART_W * ART_H];
	static unsigned long gAccum[ART_W * ART_H];
	static unsigned long bAccum[ART_W * ART_H];
	static unsigned short greyCount[ART_W * ART_H];
	long x, y;
	int i;

	if (!dib || dibBytes < 40 || !greyOut || outW <= 0 || outW > ART_W ||
		outH <= 0 || outH > ART_H)
		return -1;
	headerSize = GuiIcoLE32(dib);
	if (headerSize < 40 || headerSize > dibBytes)
		return -1;
	width = (long)GuiIcoLE32(dib + 4);
	/* Height counts the XOR colour rows plus the AND mask rows stacked
	 * together, so the actual image height is half the field's value. */
	height = (long)GuiIcoLE32(dib + 8) / 2;
	bitCount = GuiIcoLE16(dib + 14);
	compression = GuiIcoLE32(dib + 16);
	if (width <= 0 || height <= 0 || width > 256 || height > 256 ||
		compression != 0 /* BI_RGB, uncompressed */)
		return -1;
	if (bitCount != 32 && bitCount != 24 && bitCount != 8)
		return -1;

	paletteEntries = (bitCount == 8) ? 256UL : 0UL;
	palette = dib + headerSize;
	pixels = palette + paletteEntries * 4UL;
	rowBytes = (((unsigned long)width * bitCount + 31UL) / 32UL) * 4UL;
	if ((unsigned long)(pixels - dib) + rowBytes * (unsigned long)height > dibBytes)
		return -1;

	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(rAccum, 0, sizeof(rAccum));
	memset(gAccum, 0, sizeof(gAccum));
	memset(bAccum, 0, sizeof(bAccum));
	memset(greyCount, 0, sizeof(greyCount));
	for (x = 0; x < width; x++)
		xMap[x] = (unsigned char)((x * outW) / width);
	for (y = 0; y < height; y++)
		yMap[y] = (unsigned char)((y * outH) / height);

	/* DIB pixel rows are stored bottom-up. */
	for (y = 0; y < height; y++) {
		const unsigned char *row = pixels +
			(unsigned long)(height - 1 - y) * rowBytes;
		int dstY = yMap[y];

		for (x = 0; x < width; x++) {
			unsigned char r, g, b;
			int dst;

			if (bitCount == 8) {
				const unsigned char *pal = palette + 4 * row[x];
				b = pal[0]; g = pal[1]; r = pal[2];
			} else {
				const unsigned char *px = row + (bitCount == 32 ? 4 : 3) * x;
				b = px[0]; g = px[1]; r = px[2];
			}
			dst = dstY * outW + xMap[x];
			greyAccum[dst] += (77UL * r + 150UL * g + 29UL * b + 128UL) >> 8;
			rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
			if (greyCount[dst] != 0xffff) greyCount[dst]++;
		}
	}
	for (i = 0; i < outW * outH; i++)
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			unsigned long half = (unsigned long)c >> 1;

			greyOut[i] = (unsigned char)((greyAccum[i] + half) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + half) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + half) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + half) / c);
			}
		}
	return 0;
}

static int GuiIcoEntryCompare(const void *pa, const void *pb)
{
	const GuiIcoEntry *a = (const GuiIcoEntry *)pa;
	const GuiIcoEntry *b = (const GuiIcoEntry *)pb;
	unsigned long areaA = (unsigned long)a->width * (unsigned long)a->height;
	unsigned long areaB = (unsigned long)b->width * (unsigned long)b->height;

	if (areaA != areaB) return areaA > areaB ? -1 : 1;
	return (int)b->bitCount - (int)a->bitCount;
}

/* ICO files are a directory of one or more embedded images at different
 * sizes/depths.  Entries are tried largest-first for the best quality once
 * downsampled to the ART_W x ART_H thumbnail, falling through to a
 * smaller/other entry if the chosen one can't be decoded -- e.g. a
 * PNG-encoded entry (no PNG decoder in this frontend) or a legacy
 * 4bpp/1bpp DIB.  Ported from minimp3r's DecodeIcoToGrey(). */
static int DecodeIcoToGrey(const unsigned char *icoData, unsigned long icoBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	unsigned count, i, n;
	GuiIcoEntry entries[64];

	if (!icoData || icoBytes < 6 || !GuiIsIcoMagic(icoData, (int)icoBytes))
		return -1;
	count = GuiIcoLE16(icoData + 4);
	if (count == 0)
		return -1;
	if (count > 64)
		count = 64;
	n = 0;
	for (i = 0; i < count; i++) {
		const unsigned char *e = icoData + 6 + (unsigned long)i * 16UL;
		unsigned w, h;
		unsigned long size, offset;

		if (6UL + (unsigned long)(i + 1) * 16UL > icoBytes)
			break;
		w = e[0] ? e[0] : 256;
		h = e[1] ? e[1] : 256;
		size = GuiIcoLE32(e + 8);
		offset = GuiIcoLE32(e + 12);
		if (size < 8 || offset > icoBytes || size > icoBytes - offset)
			continue;
		entries[n].width = w;
		entries[n].height = h;
		entries[n].bitCount = GuiIcoLE16(e + 6);
		entries[n].size = size;
		entries[n].offset = offset;
		n++;
	}
	if (n == 0)
		return -1;
	qsort(entries, n, sizeof(entries[0]), GuiIcoEntryCompare);
	for (i = 0; i < n; i++) {
		const unsigned char *payload = icoData + entries[i].offset;
		unsigned long payloadBytes = entries[i].size;

		if (GuiIsPngMagic(payload, (int)payloadBytes))
			continue;
		if (DecodeIcoDibToGrey(payload, payloadBytes, greyOut, rgbOut,
			outW, outH) == 0)
			return 0;
	}
	return -1;
}

/* Favicon artwork is fetched from the Radio Browser station's "favicon"
 * field only, never from the MP3/ICY stream, and only once playback has
 * already picked a station (see RadioDoProbeAndPlay()/SelectInternetStream()).
 * The fetch goes through rb_probe_fetch_binary(), which shares its
 * HTTP/HTTPS/AmiSSL connection handling with the stream probe used to start
 * playback but is a separate code path the probe's stream-playback logic
 * never calls into.  Any failure here (bad URL, unsupported TLS, oversized
 * body, unsupported format, broken decode) just leaves artValid 0 and never
 * affects playback.  Ported from minimp3r's LoadRadioFaviconImage(). */
static int LoadRadioFaviconImage(HelixAmp3Gui *gui)
{
	char contentType[64];
	static unsigned char response[HELIXAMP3_FAVICON_MAX_BYTES];
	int bytes = 0;
	int rc;

	if (!gui || !gui->currentRadioFavicon[0]) {
		RADIO_DBG(printf("radio-art: no favicon URL for current station\n");)
		return 0;
	}
	RADIO_DBG(printf("radio-art: fetching favicon url=%s\n", gui->currentRadioFavicon);)
	rc = rb_probe_fetch_binary(gui->currentRadioFavicon, response, (int)sizeof(response),
		&bytes, contentType, (int)sizeof(contentType));
	if (rc != RB_STREAM_PROBE_OK) {
		RADIO_DBG(printf("radio-art: fetch failed rc=%d (%s)\n", rc, rb_probe_error_text(rc));)
		return 0;
	}
	RADIO_DBG(printf("radio-art: fetched %d bytes content-type=\"%s\"\n", bytes, contentType);)
	if (bytes <= 8)
		return 0;

	/* Dispatch purely on the actual bytes fetched, not the URL extension or
	 * declared Content-Type -- plenty of real sites serve a different format
	 * under a "favicon.ico" URL and/or a misleading Content-Type. */
	if (GuiIsJpegMagic(response, bytes)) {
		if (DecodeFaviconJpegToGrey(response, (unsigned long)bytes, gui->artGreyBuf,
			gui->artRGBBuf, ART_W, ART_H) != 0) {
			RADIO_DBG(printf("radio-art: jpeg decode failed\n");)
			return 0;
		}
		gui->artValid = 1;
		return 1;
	}
	if (GuiIsIcoMagic(response, bytes)) {
		if (DecodeIcoToGrey(response, (unsigned long)bytes, gui->artGreyBuf,
			gui->artRGBBuf, ART_W, ART_H) != 0) {
			RADIO_DBG(printf("radio-art: ico decode failed\n");)
			return 0;
		}
		gui->artValid = 1;
		return 1;
	}
	RADIO_DBG(printf("radio-art: rejected, unsupported favicon format (first bytes %02X %02X %02X %02X)\n",
		response[0], response[1], response[2], response[3]);)
	return 0;
}
#endif /* ENABLE_RADIO_ARTWORK */

static void StartArtDecode(HelixAmp3Gui *gui)
{
	ArtDecodeState *st = &gui->artDecode;
	unsigned char status;
	int i;

	ReleaseArtColorPens(gui);
	memset(st, 0, sizeof(*st));
	st->wantColor = gui->artColorEnabled;
	gui->artValid = 0;
	gui->artLoading = 0;
	if (LoadArtworkCache(gui)) {
#ifdef MINIAMP3_DEBUG
		Printf("artwork cache=hit bytes=%lu\n", gui->tags.artBytes);
#endif
		if (gui->artColorEnabled)
			BuildArtColorPens(gui);
		DrawArtPanel(gui);
		return;
	}
	if (!gui->tags.artData || gui->tags.artBytes <= 4 || gui->tags.artIsPng) {
#if ENABLE_RADIO_ARTWORK
		if (gui->artEnabled && is_url_path(gui->inputName) &&
			gui->currentRadioFavicon[0] && LoadRadioFaviconImage(gui)) {
			if (gui->artColorEnabled)
				BuildArtColorPens(gui);
			SaveArtworkCache(gui);
			DrawArtPanel(gui);
			return;
		}
#endif
		DrawArtPanel(gui);
		return;
	}
	memset(st->greyOut, 0x80, sizeof(st->greyOut));
	{
		int jpegW = 0, jpegH = 0;
		PeekJpegDimensions(gui->tags.artData, gui->tags.artBytes, &jpegW, &jpegH);
		if (jpegW <= 0 || jpegH <= 0)
			st->reduce = MINIAMP3_ART_REDUCED_JPEG ? 1 : 0;
		else
			st->reduce = (jpegW > ART_W * 4 || jpegH > ART_H * 4) ? 1 : 0;
	}
	ArtNow(&st->startSecs, &st->startMicros);
#if MINIAMP3_ART_COMPARE_JPEG
	{
		static unsigned char fullGrey[ART_W * ART_H];
		static unsigned char reducedGrey[ART_W * ART_H];
		unsigned long fullUs;
		unsigned long reducedUs;
		unsigned long sumDiff = 0;
		int maxDiff = 0;
		int diffPixels = 0;
		int n;
		if (DecodeJpegToGreyMode(gui->tags.artData, gui->tags.artBytes,
			fullGrey, ART_W, ART_H, gui->tags.artIsPng, 0, &fullUs) == 0 &&
			DecodeJpegToGreyMode(gui->tags.artData, gui->tags.artBytes,
			reducedGrey, ART_W, ART_H, gui->tags.artIsPng, 1, &reducedUs) == 0) {
			for (n = 0; n < ART_W * ART_H; n++) {
				int d = (int)fullGrey[n] - (int)reducedGrey[n];
				if (d < 0) d = -d;
				if (d) diffPixels++;
				if (d > maxDiff) maxDiff = d;
				sumDiff += (unsigned long)d;
			}
#ifdef MINIAMP3_DEBUG
			Printf("artwork compare: max_luma_diff=%d avg_luma_diff=%lu diff_pixels=%d full_us=%lu reduced_us=%lu\n",
				maxDiff, (sumDiff + (ART_W * ART_H / 2)) / (ART_W * ART_H),
				diffPixels, fullUs, reducedUs);
#endif
		}
	}
#endif
	st->src.data = gui->tags.artData;
	st->src.size = gui->tags.artBytes;
	status = pjpeg_decode_init(&st->info, pjpeg_cb, &st->src, st->reduce);
	if (status != 0 || st->info.m_width <= 0 || st->info.m_height <= 0 ||
		st->info.m_width > MAX_JPEG_DIM || st->info.m_height > MAX_JPEG_DIM) {
		pjpeg_decode_free();
		DrawArtPanel(gui);
		return;
	}
	for (i = 0; i < st->info.m_width; i++)
		st->xMap[i] = (unsigned char)((i * ART_W) / st->info.m_width);
	for (i = 0; i < st->info.m_height; i++)
		st->yMap[i] = (unsigned char)((i * ART_H) / st->info.m_height);
	st->totalMcus = st->info.m_MCUSPerRow * st->info.m_MCUSPerCol;
#ifdef MINIAMP3_DEBUG
	Printf("artwork JPEG: %dx%d bytes=%lu sampling=%s mcu=%dx%d total_mcus=%d reduce=%s cache=miss pump_limit=%d source_pixels=%lu reduced_blocks=%lu\n",
		st->info.m_width, st->info.m_height, gui->tags.artBytes,
		MINIAMP3_DEBUG_FMT_PTR(JpegScanTypeName(st->info.m_scanType)),
		st->info.m_MCUWidth,
		st->info.m_MCUHeight, st->totalMcus,
		MINIAMP3_DEBUG_FMT_PTR(st->reduce ? "yes" : "no"),
		ART_MCUS_PER_PUMP, (unsigned long)st->info.m_width *
		(unsigned long)st->info.m_height, (unsigned long)st->totalMcus *
		(unsigned long)(st->info.m_MCUWidth / 8) *
		(unsigned long)(st->info.m_MCUHeight / 8));
#endif
	st->active = 1;
	gui->artLoading = 1;
	SetStatus(gui, "Loading artwork...");
	DrawArtPanel(gui);
	PumpArtDecode(gui);
}

static int ArtGreyPen(HelixAmp3Gui *gui, int level)
{
	/* retained for potential future use */
	struct DrawInfo *dri;
	int pen;

	pen = level ? 1 : 0;
	if (!gui || !gui->win || !gui->win->WScreen)
		return pen;
	dri = GetScreenDrawInfo(gui->win->WScreen);
	if (dri) {
		if (level <= 0)
			pen = dri->dri_Pens[SHADOWPEN];
		else if (level == 1)
			pen = dri->dri_Pens[BACKGROUNDPEN];
		else
			pen = dri->dri_Pens[SHINEPEN];
		FreeScreenDrawInfo(gui->win->WScreen, dri);
	}
	return pen;
}

static void DrawTransportIcons(HelixAmp3Gui *gui)
{
	struct RastPort *rp;
	int playX;
	int playY;
	int stopX;
	int stopY;
	int nextX;
	int nextY;
	int i;

	if (!gui || !gui->win || !gui->gadPlay || !gui->gadStop)
		return;
	rp = gui->win->RPort;
	SetAPen(rp, 1);
	playX = gui->gadPlay->LeftEdge + (gui->gadPlay->Width / 2) - 5;
	playY = gui->gadPlay->TopEdge + (gui->gadPlay->Height / 2) - 5;
	for (i = 0; i < 10; i++) {
		int half = (9 - i) / 2;
		RectFill(rp, playX + i, playY + 5 - half, playX + i,
			playY + 5 + half);
	}
	stopX = gui->gadStop->LeftEdge + (gui->gadStop->Width / 2) - 5;
	stopY = gui->gadStop->TopEdge + (gui->gadStop->Height / 2) - 5;
	RectFill(rp, stopX, stopY, stopX + 9, stopY + 9);
	/* Next: two small right-pointing triangles (skip-forward) */
	if (gui->gadNext) {
		nextX = gui->gadNext->LeftEdge + (gui->gadNext->Width / 2) - 7;
		nextY = gui->gadNext->TopEdge + (gui->gadNext->Height / 2) - 4;
		for (i = 0; i < 7; i++) {
			int half = (6 - i) / 2;
			RectFill(rp, nextX + i, nextY + 3 - half,
				nextX + i, nextY + 3 + half);
		}
		nextX += 8;
		for (i = 0; i < 7; i++) {
			int half = (6 - i) / 2;
			RectFill(rp, nextX + i, nextY + 3 - half,
				nextX + i, nextY + 3 + half);
		}
	}
}

static void ReleaseArtColorPens(HelixAmp3Gui *gui)
{
	if (gui->artPensBuilt && gui->win) {
		struct ColorMap *cm = gui->win->WScreen->ViewPort.ColorMap;
		if (cm) {
			int i;
			for (i = 0; i < gui->artPenCacheUsed; i++)
				if (gui->artPenCache[i].pen >= 0)
					ReleasePen(cm, gui->artPenCache[i].pen);
		}
	}
	gui->artPensBuilt = 0;
	gui->artPenCacheUsed = 0;
}

static void BuildArtColorPens(HelixAmp3Gui *gui)
{
	struct ColorMap *cm;
	int i;

	ReleaseArtColorPens(gui);
	if (!gui->win || !gui->artValid)
		return;
	cm = gui->win->WScreen->ViewPort.ColorMap;
	if (!cm)
		return;

	/* Pass 1: build pen cache from unique undithered pixel colours. */
	for (i = 0; i < ART_W * ART_H; i++) {
		const unsigned char *p = &gui->artRGBBuf[i * 3];
		unsigned long key = ((unsigned long)p[0] << 16) |
		                    ((unsigned long)p[1] <<  8) | p[2];
		int j;
		for (j = 0; j < gui->artPenCacheUsed; j++)
			if (gui->artPenCache[j].key == key)
				break;
		if (j == gui->artPenCacheUsed && gui->artPenCacheUsed < ART_COLOR_CACHE) {
			ULONG r32 = (ULONG)p[0] | ((ULONG)p[0] << 8) | ((ULONG)p[0] << 16) | ((ULONG)p[0] << 24);
			ULONG g32 = (ULONG)p[1] | ((ULONG)p[1] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[1] << 24);
			ULONG b32 = (ULONG)p[2] | ((ULONG)p[2] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[2] << 24);
			gui->artPenCache[j].key = key;
			gui->artPenCache[j].pen = ObtainBestPen(cm, r32, g32, b32,
				OBP_FailIfBad, (Tag)FALSE, TAG_DONE);
			gui->artPenCacheUsed++;
		}
	}

	if (!gui->artPenCacheUsed)
		return;

	/* Pass 2: assign per-pixel pen index using Bayer-dithered colour.
	 * The dither offset pushes each pixel's colour toward lighter or darker,
	 * causing adjacent pixels to snap to different pens across palette
	 * boundaries — same technique as the greyscale path, extended to RGB. */
	for (i = 0; i < ART_W * ART_H; i++) {
		const unsigned char *p = &gui->artRGBBuf[i * 3];
		int dv = (int)kBayer8x8[(i / ART_W) & 7][i & 7] - 32;
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
		for (j = 0; j < gui->artPenCacheUsed; j++) {
			unsigned long k = gui->artPenCache[j].key;
			int dr = (int)((k >> 16) & 0xff) - rd;
			int dg = (int)((k >>  8) & 0xff) - gd;
			int db = (int)( k        & 0xff) - bd;
			unsigned long dist = (unsigned long)(dr*dr + dg*dg + db*db);
			if (dist < bestDist) { bestDist = dist; bestj = j; }
		}
		gui->artPenIdx[i] = (unsigned char)bestj;
	}
	gui->artPensBuilt = 1;
}

/* Integer Newton's-method sqrt, used to plot filled circles for the
 * fallback art icons below without pulling in <math.h>. */
static int ArtIconIntSqrt(int n)
{
	int x, y;
	if (n <= 0)
		return 0;
	x = n;
	y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2;
	}
	return x;
}

static void ArtIconFillCircle(struct RastPort *rp, int cx, int cy, int r)
{
	int dy;
	for (dy = -r; dy <= r; dy++) {
		int dx = ArtIconIntSqrt(r * r - dy * dy);
		RectFill(rp, cx - dx, cy + dy, cx + dx, cy + dy);
	}
}

/* Drawn when there's no station favicon (or it failed to load) for a
 * radio stream: a boombox silhouette (handle, antenna, body, speaker
 * ring, tuning dial), built entirely from RectFill/Move/Draw so it
 * needs no bitmap asset. */
static void DrawRadioIcon(struct RastPort *rp, int originX, int originY)
{
	int bx0 = originX + 10, by0 = originY + 34;
	int bx1 = originX + 54, by1 = originY + 58;

	SetAPen(rp, 1);

	Move(rp, bx0 + 10, by0);
	Draw(rp, bx0 + 10, by0 - 12);
	Draw(rp, bx1 - 10, by0 - 12);
	Draw(rp, bx1 - 10, by0);

	Move(rp, bx1 - 8, by0);
	Draw(rp, bx1 + 4, by0 - 16);
	RectFill(rp, bx1 + 2, by0 - 18, bx1 + 6, by0 - 14);

	Move(rp, bx0, by0);
	Draw(rp, bx1, by0);
	Draw(rp, bx1, by1);
	Draw(rp, bx0, by1);
	Draw(rp, bx0, by0);

	ArtIconFillCircle(rp, bx0 + 12, by0 + 12, 7);
	SetAPen(rp, 0);
	ArtIconFillCircle(rp, bx0 + 12, by0 + 12, 4);

	SetAPen(rp, 1);
	ArtIconFillCircle(rp, bx1 - 10, by0 + 12, 4);
}

/* Drawn when a local/offline file has no embedded artwork: a simple
 * eighth note (filled head, stem, flag), same no-asset approach. */
static void DrawMusicNoteIcon(struct RastPort *rp, int originX, int originY)
{
	int headCx = originX + 24;
	int headCy = originY + 46;
	int headR = 8;
	int stemX = headCx + headR - 1;
	int stemTopY = originY + 12;

	SetAPen(rp, 1);
	ArtIconFillCircle(rp, headCx, headCy, headR);

	Move(rp, stemX, headCy);
	Draw(rp, stemX, stemTopY);

	Draw(rp, stemX + 12, stemTopY + 8);
	Draw(rp, stemX, stemTopY + 16);
	Draw(rp, stemX, stemTopY);
}

/* Upper-cased file extension of a URL's path (ignoring any query string),
 * e.g. "http://x/icon.jpg?v=2" -> "JPG". The last dot in the final path
 * segment wins, so ".../Logo.svg.png" is reported as PNG. Mirrors minimp3r.c's
 * MrUrlExtensionUpper so both frontends report rejected favicon formats
 * the same way. */
static void ArtUrlExtensionUpper(const char *url, char *out, int outSize)
{
	const char *q, *hash, *end, *segment, *dot, *p;
	int len, i, j;
	if (!out || outSize <= 0)
		return;
	out[0] = '\0';
	if (!url || !url[0])
		return;
	q = strchr(url, '?');
	hash = strchr(url, '#');
	end = url + strlen(url);
	if (q && q < end)
		end = q;
	if (hash && hash < end)
		end = hash;
	len = (int)(end - url);
	segment = url + len;
	while (segment > url && segment[-1] != '/' && segment[-1] != ':')
		segment--;
	dot = (const char *)0;
	for (p = segment; p < end; p++) {
		if (*p == '.')
			dot = p;
	}
	if (!dot || dot + 1 >= end)
		return;
	dot++;
	for (i = 0, j = 0; dot + i < url + len && dot[i] && j < outSize - 1; i++) {
		unsigned char c = (unsigned char)dot[i];
		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 'a' + 'A');
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			out[j++] = (char)c;
		else
			break;
	}
	out[j] = '\0';
}

/* Radio stream whose favicon was fetched but rejected (unsupported format
 * or failed decode): show "No art" plus the rejected extension so a
 * station icon swap is visibly noticed even when nothing renders. Same
 * convention as minimp3r.c's DrawArtPanel. */
static void DrawNoArtFormatLabel(struct RastPort *rp, int originX, int originY,
	const char *favicon)
{
	char ext[16];
	char line1[16];
	char line2[16];
	int line1Len, line2Len, line1W, line2W;

	ArtUrlExtensionUpper(favicon, ext, sizeof(ext));
	SafeCopy(line1, sizeof(line1), "No art");
	line2[0] = '\0';
	if (ext[0])
		sprintf(line2, "(%s)", ext);

	line1Len = (int)strlen(line1);
	line2Len = (int)strlen(line2);
	line1W = TextLength(rp, line1, line1Len);
	line2W = line2Len > 0 ? TextLength(rp, line2, line2Len) : 0;

	SetAPen(rp, 1);
	if (line2Len > 0) {
		Move(rp, originX + (ART_W - line1W) / 2, originY + ART_H / 2 - 2);
		Text(rp, line1, line1Len);
		Move(rp, originX + (ART_W - line2W) / 2, originY + ART_H / 2 + 9);
		Text(rp, line2, line2Len);
	} else {
		Move(rp, originX + (ART_W - line1W) / 2, originY + ART_H / 2 + 2);
		Text(rp, line1, line1Len);
	}
}

static void DrawArtPanel(HelixAmp3Gui *gui)
{
	struct RastPort *rp;
	int x;
	int y;

	if (!gui->win)
		return;
	rp = gui->win->RPort;
	DrawBevelBox(rp, ART_X - 2, ART_Y - 2, ART_W + 4, ART_H + 4,
		GT_VisualInfo, (ULONG)gui->visualInfo,
		GTBB_Recessed, TRUE,
		TAG_DONE);
	if (gui->artValid) {
		if (gui->artColorEnabled && gui->artPensBuilt) {
			/* True colour path: pen indices pre-computed at load time —
			 * no ObtainBestPen calls during draw, just fast run-length RectFills. */
			for (y = 0; y < ART_H; y++) {
				const unsigned char *idxRow = &gui->artPenIdx[y * ART_W];
				int runStart = 0;
				unsigned char runIdx = idxRow[0];

				for (x = 1; x <= ART_W; x++) {
					unsigned char idx = (x < ART_W) ? idxRow[x] : 0xff;
					if (idx != runIdx) {
						long pen = gui->artPenCache[runIdx].pen;
						if (pen >= 0) {
							SetAPen(rp, (UWORD)pen);
							RectFill(rp, ART_X + runStart, ART_Y + y,
								ART_X + x - 1, ART_Y + y);
						}
						runStart = x;
						runIdx = idx;
					}
				}
			}
		} else {
			/* Greyscale path: ordered dithering with 8x8 Bayer matrix and
			 * three system pens (shadow / background / shine). */
			int pens[3];
			{
				struct DrawInfo *dri =
					GetScreenDrawInfo(gui->win->WScreen);
				if (dri) {
					pens[0] = dri->dri_Pens[SHADOWPEN];
					pens[1] = dri->dri_Pens[BACKGROUNDPEN];
					pens[2] = dri->dri_Pens[SHINEPEN];
					FreeScreenDrawInfo(gui->win->WScreen, dri);
				} else {
					pens[0] = 0; pens[1] = 1; pens[2] = 1;
				}
			}

			for (y = 0; y < ART_H; y++) {
				int runStart = 0;
				int runShade;
				{
					int g0 = gui->artGreyBuf[y * ART_W];
					int dv = (int)kBayer8x8[y & 7][0] - 32;
					int gd = g0 + dv * 3 / 4;
					runShade = gd >= 171 ? 2 : (gd >= 85 ? 1 : 0);
				}
				for (x = 1; x <= ART_W; x++) {
					int shade;
					if (x < ART_W) {
						int g = gui->artGreyBuf[y * ART_W + x];
						int dv = (int)kBayer8x8[y & 7][x & 7] - 32;
						int gd = g + dv * 3 / 4;
						shade = gd >= 171 ? 2 : (gd >= 85 ? 1 : 0);
					} else {
						shade = -1; /* sentinel to flush last run */
					}
					if (shade != runShade) {
						SetAPen(rp, pens[runShade]);
						RectFill(rp, ART_X + runStart, ART_Y + y,
							ART_X + x - 1, ART_Y + y);
						runStart = x;
						runShade = shade;
					}
				}
			}
		}
	} else {
		SetAPen(rp, 0);
		RectFill(rp, ART_X, ART_Y, ART_X + ART_W - 1, ART_Y + ART_H - 1);
		if (gui->artLoading) {
			SetAPen(rp, 1);
			Move(rp, ART_X + 10, ART_Y + ART_H / 2);
			Text(rp, "Loading", 7);
		} else if (IsRadioInputName(gui->inputName) && gui->currentRadioFavicon[0]) {
			DrawNoArtFormatLabel(rp, ART_X, ART_Y, gui->currentRadioFavicon);
		} else if (IsRadioInputName(gui->inputName)) {
			DrawRadioIcon(rp, ART_X, ART_Y);
		} else {
			DrawMusicNoteIcon(rp, ART_X, ART_Y);
		}
	}
}

static void UpdateArtDisplay(HelixAmp3Gui *gui)
{
	StartArtDecode(gui);
}

static void DrawProgressFrame(HelixAmp3Gui *gui)
{
	if (!gui->win)
		return;
	DrawBevelBox(gui->win->RPort,
		PROG_X - 4, PROG_TOP_Y - 4,
		PROG_W + 8, PROG_H + 8,
		GT_VisualInfo, (ULONG)gui->visualInfo,
		GTBB_Recessed, TRUE,
		TAG_DONE);
}

static void DrawProgress(HelixAmp3Gui *gui)
{
	struct RastPort *rp;
	int fill, empty;
	char timeBuf[32];
	int elapsed, total, remaining;
	int textWidth, textX;

	if (!gui->win)
		return;
	rp = gui->win->RPort;
	elapsed = gui->elapsedSecs - gui->launchBufferSecs;
	total = gui->totalSecs;
	if (elapsed < 0)
		elapsed = 0;
	if (total > 0 && elapsed > total)
		elapsed = total;
	fill = total > 0 ? (elapsed * PROG_W) / total : 0;
	if (fill < 0)
		fill = 0;
	if (fill > PROG_W)
		fill = PROG_W;
	empty = PROG_W - fill;

	if (gui->smallFont)
		SetFont(rp, gui->smallFont);
	if (fill > 0) {
		int fillPen = (gui->playbackActive &&
			gGuiPlaybackStatus.phase == GUIPLAY_PHASE_BUFFERING) ? 2 : 3;
		SetAPen(rp, fillPen);
		RectFill(rp, PROG_X, PROG_TOP_Y,
			PROG_X + fill - 1, PROG_TOP_Y + PROG_H - 1);
	}
	if (empty > 0) {
		SetAPen(rp, gui->win->DetailPen);
		RectFill(rp, PROG_X + fill, PROG_TOP_Y,
			PROG_X + PROG_W - 1, PROG_TOP_Y + PROG_H - 1);
	}

	if (IsRadioInputName(gui->inputName)) {
		if (elapsed > 0)
			sprintf(timeBuf, "%02d:%02d / Live", elapsed / 60, elapsed % 60);
		else
			sprintf(timeBuf, "Live / Live");
	} else if (total > 0) {
		remaining = total - elapsed;
		if (remaining < 0)
			remaining = 0;
		sprintf(timeBuf, "-%02d:%02d / %02d:%02d",
			remaining / 60, remaining % 60,
			total / 60, total % 60);
	} else {
		sprintf(timeBuf, " 00:00 / %02d:%02d", elapsed / 60, elapsed % 60);
	}

	SetAPen(rp, gui->win->DetailPen);
	RectFill(rp, TIME_X, PROG_TOP_Y - 1,
		TIME_X + TIME_W, PROG_TOP_Y + GUI_ROW_H);
	SetAPen(rp, 1);
	textWidth = TextLength(rp, timeBuf, strlen(timeBuf));
	textX = TIME_X + TIME_W - textWidth;
	if (textX < TIME_X)
		textX = TIME_X;
	Move(rp, textX, PROG_TOP_Y + rp->TxBaseline);
	Text(rp, timeBuf, strlen(timeBuf));
}


static void DrawProgressIfChanged(HelixAmp3Gui *gui)
{
	if (gui->elapsedSecs == gui->lastDrawnElapsedSecs &&
		gui->totalSecs == gui->lastDrawnTotalSecs)
		return;
	DrawProgress(gui);
	gui->lastDrawnElapsedSecs = gui->elapsedSecs;
	gui->lastDrawnTotalSecs = gui->totalSecs;
}

static void SendTimerRequest(HelixAmp3Gui *gui, ULONG micros)
{
	if (!gui->timerReq)
		return;
	if (gui->timerPending) {
		AbortIO((struct IORequest *)gui->timerReq);
		WaitIO((struct IORequest *)gui->timerReq);
		gui->timerPending = 0;
	}
	gui->timerReq->tr_node.io_Command = TR_ADDREQUEST;
	gui->timerReq->tr_time.tv_secs = micros / 1000000UL;
	gui->timerReq->tr_time.tv_micro = micros % 1000000UL;
	SendIO((struct IORequest *)gui->timerReq);
	gui->timerPending = 1;
	gui->timerIsArt = (micros == ART_TIMER_MICROS);
}

static void ResetCliParser(void);
static void ResetDecoderStatics(void);

static int PlaybackProcessStillExists(void)
{
	struct Task *task;

	/* The child posts its done message just before returning from PlaybackEntry.
	 * Do not launch another decoder until DOS has actually removed that task. */
	Forbid();
	task = FindTask((STRPTR)"MiniAMP3 playback");
	Permit();
	return task != NULL;
}

static int PlaybackCanFinalize(HelixAmp3Gui *gui)
{
	/* Deliberately does NOT require gGuiPlaybackStatus.cleanupComplete: that
	 * flag is set by the playback child itself right before it posts its
	 * done message and exits, so it is normally already 1 by the time DOS
	 * has actually removed the task.  But if the child ever dies through a
	 * path that skips that final bookkeeping (a wedged decoder/network read
	 * that never returns, an abnormal exit), cleanupComplete would never
	 * become 1 and this front end would sit in "Stopping..." forever with
	 * no way back short of quitting the app -- the "stuck stopping" symptom.
	 * The ReAction front end (minimp3r.c) finalizes on
	 * "playbackDonePending && !PlaybackProcessStillExists()" alone and is
	 * the more battle-tested of the two; match it here. Once the task is
	 * confirmed gone there is nothing left to wait for. */
	return gui->playbackDonePending &&
		gDoneRunId == gui->playbackRunId &&
		gGuiPlaybackStatus.runId == gui->playbackRunId &&
		!PlaybackProcessStillExists();
}


#if defined(AMIGA_M68K) && defined(MINIAMP3_DEBUG)
static int GuiAmigaDosInputOpenReadClose(const char *path)
{
	BPTR handle;
	unsigned char bytes[16];
	LONG nRead;

	if (!path || !path[0])
		return -1;
	handle = SafeOpenPath("GuiOpenRead/Open", path, MODE_OLDFILE);
	if (!handle)
		return -1;
	nRead = Read(handle, bytes, sizeof(bytes));
	Close(handle);
	return nRead == (LONG)sizeof(bytes) ? 0 : -1;
}

static void GuiRunAmigaDosInputRegression(HelixAmp3Gui *gui, int afterInterrupted)
{
	int child1;
	int child2;
	char msg[128];

	child1 = GuiAmigaDosInputOpenReadClose(gui->inputName);
	child2 = GuiAmigaDosInputOpenReadClose(gui->inputName);
	sprintf(msg, "DOS input self-test after %s: child1=%s child2=%s",
		afterInterrupted ? "stop" : "finish",
		child1 == 0 ? "ok" : "fail", child2 == 0 ? "ok" : "fail");
	SetStatus(gui, msg);
}
#endif


static const char *RadioStreamStateName(int phase)
{
	switch (phase) {
	case GUIPLAY_PHASE_IDLE: return "IDLE";
	case GUIPLAY_PHASE_BUFFERING: return "BUFFERING";
	case GUIPLAY_PHASE_PLAYING: return "PLAYING";
	case GUIPLAY_PHASE_UNDERRUN: return "UNDERRUN";
	case GUIPLAY_PHASE_STOPPING: return "STOPPING";
	case GUIPLAY_PHASE_DONE: return "DONE";
	case GUIPLAY_PHASE_ERROR: return "ERROR";
	default: return "UNKNOWN";
	}
}

static void radio_debug_state_summary(HelixAmp3Gui *gui, const char *reason)
{
	RADIO_DBG(printf("radio-state: reason=%s active=%d pending=%d stopping=%d stopRequested=%d donePending=%d uiState=%s streamState=%s codec=%s session=%lu\n",
		reason ? reason : "state",
		gui ? gui->playbackActive : 0,
		(gGuiPlayer.process != NULL),
		(gui && (gui->playbackDonePending || gGuiPlayer.stopRequested)) ? 1 : 0,
		gGuiPlayer.stopRequested,
		gui ? gui->playbackDonePending : 0,
		gui ? gui->statusText : "",
		RadioStreamStateName(gGuiPlaybackStatus.phase),
		gGuiPlaybackStatus.radioContentType,
		gui ? gui->playbackRunId : gGuiPlaybackStatus.runId);)
}

static void radio_reset_playback_state_after_stop(HelixAmp3Gui *gui, const char *reason)
{
	if (gui) {
		gui->playbackActive = 0;
		gui->playbackDonePending = 0;
		gui->playbackStoppedByUser = 0;
		gui->queuedPlayPending = 0;
		gui->queuedInputName[0] = '\0';
		gui->queuedHaveRadioHostAddr = 0;
		gui->queuedRadioHostAddrBe = 0;
		gui->playlistNextPending = 0;
		gui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
		gui->lastStartupStage = GUISTART_NONE;
		gui->startupStageStableTicks = 0;
		gui->startupStallShown = 0;
		gui->lastDisplayedPhase = GUIPLAY_PHASE_IDLE;
		gui->stopWatchdogMicros = 0;
		gui->stopWatchdogFired = 0;
	}
	gGuiPlayer.process = NULL;
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gDonePort = NULL;
	gDoneRunId = 0;
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_IDLE;
	gGuiPlaybackStatus.radioStatus = RADIO_STATUS_CLOSED;
	gGuiPlaybackStatus.radioActive = 0;
	gGuiPlaybackStatus.radioBufferedBytes = 0;
	gGuiPlaybackStatus.radioContentType[0] = '\0';
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
	gGuiPlaybackStatus.cleanupComplete = 1;
	radio_debug_state_summary(gui, reason);
}

static int radio_validate_ready_to_play(HelixAmp3Gui *gui)
{
	if (PlaybackProcessStillExists()) {
		RADIO_DBG(printf("Cannot start: previous stream still stopping\n");)
		return 0;
	}
	if (gGuiPlayer.process) {
		RADIO_DBG(printf("Cannot start: stale active session\n");)
		return 0;
	}
	if (gGuiPlayer.stopRequested) {
		RADIO_DBG(printf("Cannot start: previous stream still stopping\n");)
		return 0;
	}
	if (gui && gui->playbackDonePending) {
		RADIO_DBG(printf("Cannot start: donePending still set\n");)
		return 0;
	}
	if (gui && gui->playbackActive) {
		RADIO_DBG(printf("Cannot start: stale active session\n");)
		return 0;
	}
	if (gGuiPlaybackStatus.phase != GUIPLAY_PHASE_IDLE &&
		gGuiPlaybackStatus.phase != GUIPLAY_PHASE_DONE &&
		gGuiPlaybackStatus.phase != GUIPLAY_PHASE_ERROR) {
		RADIO_DBG(printf("Cannot start: previous stream still stopping\n");)
		return 0;
	}
	return 1;
}

static void FinalizePlayback(HelixAmp3Gui *gui)
{
	int stoppedByUser = gui->playbackStoppedByUser;
	int nextPending = gui->playlistNextPending;
	int queuedPlayPending = gui->queuedPlayPending;
	char queuedInputName[HELIXAMP3_MAX_PATH];
	int queuedHaveRadioHostAddr = gui->queuedHaveRadioHostAddr;
	unsigned long queuedRadioHostAddrBe = gui->queuedRadioHostAddrBe;
	int failedRadioStart;

	failedRadioStart = (!stoppedByUser && IsRadioInputName(gui->inputName) &&
		gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR &&
		gGuiPlaybackStatus.decodedFrames == 0);
	SafeCopy(queuedInputName, sizeof(queuedInputName), gui->queuedInputName);
	gui->playbackDonePending = 0;
	gui->playbackStoppedByUser = 0;
	gui->playbackActive = 0;
	gui->playlistNextPending = 0;
	gui->queuedPlayPending = 0;
	radio_reset_playback_state_after_stop(gui, stoppedByUser ? "stop-cleanup" : "playback-cleanup");
	if (gui->totalSecs > 0 && !stoppedByUser)
		gui->elapsedSecs = gui->totalSecs + gui->launchBufferSecs;
	DrawProgress(gui);
	ResetCliParser();
	/* Decoder statics are reset by the next playback child immediately before
	 * entering the decoder.  Do not reset them again from the GUI task after
	 * teardown; keeping all decoder-global mutation in the child avoids a
	 * second-play race on shared process address space. */
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	if (gui->artCacheSavePending) {
		gui->artCacheSavePending = 0;
		SaveArtworkCache(gui);
	}
	if (gui->artRestartPending) {
		gui->artRestartPending = 0;
		StartArtDecode(gui);
	}
	gui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
	gui->lastDisplayedPhase = GUIPLAY_PHASE_IDLE;
#if defined(AMIGA_M68K) && defined(MINIAMP3_DEBUG)
	GuiRunAmigaDosInputRegression(gui, stoppedByUser);
#else
	if (failedRadioStart) {
		SetRadioFailureStatus(gui, "radio stream failed");
	} else {
		SetStatus(gui, stoppedByUser ? "Stopped - ready." : "Playback finished - ready.");
	}
#endif
	if (gui->closeRequested) {
		gui->queuedInputName[0] = '\0';
		gui->haveRadioHostAddr = 0;
		gui->radioHostAddrBe = 0;
	} else if (!strcmp(queuedInputName, "radio-selection")) {
		RadioSetStatus(gui, "Starting queued stream...");
		RadioDoProbeAndPlay(gui);
	} else if (queuedInputName[0]) {
		CancelArtDecode(gui);
		SafeCopy(gui->inputName, sizeof(gui->inputName), queuedInputName);
		gui->haveRadioHostAddr = queuedHaveRadioHostAddr;
		gui->radioHostAddrBe = queuedRadioHostAddrBe;
		SetFileDisplay(gui, gui->inputName);
		ReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
		if (is_url_path(gui->inputName))
			SetInternetStreamMetadata(gui);
		else
			gui->totalSecs = gui->tags.durationSecs;
		gui->elapsedSecs = 0;
		gui->launchBufferSecs = 0;
		UpdateTagDisplay(gui);
		UpdateArtDisplay(gui);
		DrawProgress(gui);
		if (gui->artDecode.active)
			SendTimerRequest(gui, ART_TIMER_MICROS);
		if (queuedPlayPending) {
#if defined(AMIGA_M68K)
			RADIO_DBG(printf("radio-done: Delay before queued stream start after parent done received\n");)
			Delay(3);
#endif
			StartPlayback(gui);
		}
		else if (!gui->artDecode.active)
			SetStatus(gui, "Next file ready.");
	} else if ((!stoppedByUser || nextPending) &&
		gui->playlist.current >= 0 &&
		gui->playlist.current + 1 < gui->playlist.count) {
		/* Auto-advance to next playlist item (or forced via Next button) */
		gui->playlist.current++;
		gui->playlist.selected = gui->playlist.current;
		RefreshPlaylistView(gui);
		CancelArtDecode(gui);
		SafeCopy(gui->inputName, sizeof(gui->inputName),
			gui->playlist.paths[gui->playlist.current]);
		SetFileDisplay(gui, gui->inputName);
		ReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
		if (is_url_path(gui->inputName))
			SetInternetStreamMetadata(gui);
		else
			gui->totalSecs = gui->tags.durationSecs;
		gui->elapsedSecs = 0;
		gui->launchBufferSecs = 0;
		UpdateTagDisplay(gui);
		UpdateArtDisplay(gui);
		DrawProgress(gui);
		if (gui->artDecode.active)
			SendTimerRequest(gui, ART_TIMER_MICROS);
		StartPlayback(gui);
	} else {
		/* On a natural end-of-playlist, clear the position so subsequent
		 * Next presses don't claim there is an active track.  On a manual
		 * stop, keep the position so the Next button can advance from
		 * where the user left off. */
		if (!stoppedByUser)
			gui->playlist.current = -1;
	}
}

static void SignalPlaybackChildCtrlC(void)
{
	struct Task *child;
	Forbid();
	child = FindTask((STRPTR)"MiniAMP3 playback");
	if (child)
		Signal(child, SIGBREAKF_CTRL_C);
	Permit();
}

static void HandleTimerSignal(HelixAmp3Gui *gui)
{
	int expiredWasArt;

	if (!gui->timerReq)
		return;
	expiredWasArt = gui->timerIsArt;
	while (GetMsg(gui->timerPort))
		;
	gui->timerPending = 0;
	gui->timerIsArt = 0;

	if (gui->playbackActive && !gui->playbackDonePending && gGuiPlayer.stopRequested) {
		gPlaybackInterrupted = 1;
		SignalPlaybackChildCtrlC();
	}

	/* Poll the done port on every tick while playback is active so that a
	 * fast-exiting child whose signal wake was already consumed by a previous
	 * Wait() return does not leave the GUI permanently locked. */
	if (gui->playbackActive && gui->donePort) {
		struct Message *msg;
		int gotDone = 0;
		while ((msg = GetMsg(gui->donePort)) != NULL)
			gotDone = 1;
		if (gotDone && !gui->playbackDonePending) {
			gui->playbackDonePending = 1;
			gui->playbackStoppedByUser = gGuiPlayer.stopRequested ? 1 : 0;
			if (gui->playbackStoppedByUser)
				SetStatus(gui, "Stopping...");
			else if (IsRadioInputName(gui->inputName) &&
				gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR)
				SetRadioFailureStatus(gui, "radio error");
			else
				SetStatus(gui, "Playback finished - ready.");
		}
	}
	if (gui->playbackDonePending && PlaybackCanFinalize(gui))
		FinalizePlayback(gui);

	/* Recovery: if the playback process has exited but no done message was
	 * ever delivered to the GUI (e.g., the child read gDonePort as NULL in a
	 * race, or died before reaching its own cleanup-complete bookkeeping),
	 * force-finalize so the player does not stay stuck in the Stopping state
	 * indefinitely.  Deliberately does not require cleanupComplete -- see
	 * the comment on PlaybackCanFinalize(). */
	if (gui->playbackActive && !gui->playbackDonePending &&
		gGuiPlaybackStatus.runId == gui->playbackRunId &&
		!PlaybackProcessStillExists()) {
		gui->playbackDonePending = 1;
		gui->playbackStoppedByUser = gGuiPlayer.stopRequested ? 1 : 0;
		FinalizePlayback(gui);
	}

	/* Last-resort watchdog: Stop has been outstanding for a long time and the
	 * child still has not been confirmed gone -- most likely wedged inside a
	 * blocking bsdsocket/AmiSSL call that never observes SIGBREAKF_CTRL_C.
	 * There is no safe way to force-kill an AmigaOS task stuck inside a
	 * library call, and starting a new playback child while this one might
	 * still be alive would let two children race on the same shared decoder/
	 * IPC globals -- exactly the class of corruption this codebase already
	 * has scar tissue from. So this does not try to recover playback; it
	 * only replaces the indefinite silent "Stopping..." with an honest,
	 * one-shot status so the user knows a restart is needed instead of
	 * wondering whether the app is about to come back on its own. */
	if (gui->playbackActive && !gui->playbackDonePending && gGuiPlayer.stopRequested) {
		gui->stopWatchdogMicros += expiredWasArt ? ART_TIMER_MICROS : TIMER_TICK_MICROS;
		if (!gui->stopWatchdogFired && gui->stopWatchdogMicros >= STOP_WATCHDOG_TIMEOUT_MICROS) {
			gui->stopWatchdogFired = 1;
			RADIO_DBG(printf("radio-stop: watchdog timeout after %luus, stream not responding to Stop\n", gui->stopWatchdogMicros);)
			SetStatus(gui, "Stream isn't responding to Stop - restart the app to recover.");
		}
	} else {
		gui->stopWatchdogMicros = 0;
		gui->stopWatchdogFired = 0;
	}

	if (gui->playbackActive && !gui->playbackDonePending && !expiredWasArt) {
		int phase = gGuiPlaybackStatus.phase;
		unsigned long frames = gGuiPlaybackStatus.decodedFrames;
		int rate = gGuiPlaybackStatus.sampleRate;
		unsigned long underruns = gGuiPlaybackStatus.underruns;
		long spareMs = gGuiPlaybackStatus.spareMs;
		unsigned long halfBufferMs = gGuiPlaybackStatus.halfBufferMs;
		int phaseChanged = (phase != gui->lastDisplayedPhase);
		int isRadioInput = IsRadioInputName(gui->inputName);

		if (!gGuiFirstUiProgressLogged && frames > 0) {
			gGuiFirstUiProgressLogged = 1;
			RADIO_DBG(printf("radio-ui: first GadTools UI progress/status update phase=%d frames=%lu rate=%d status=\"%s\"\n",
				phase, frames, rate, gui->statusText);)
		}

		if (isRadioInput) {
			if (gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR) {
				SetRadioFailureStatus(gui, "radio error");
			} else if (gGuiPlaybackStatus.radioActive &&
				gGuiPlaybackStatus.radioStatus != RADIO_STATUS_STOPPING &&
				gGuiPlaybackStatus.radioStatus != RADIO_STATUS_CLOSED &&
				(phaseChanged || gGuiPlaybackStatus.radioStatus == RADIO_STATUS_PLAYING))
				UpdateRadioTagDisplay(gui);
		}

		if (phaseChanged)
			gui->lastDisplayedPhase = phase;

		/* Once the decoder reports a valid rate, fill in the Hz field that
		 * ReadMpegInfo leaves as 0 for non-MP3 formats (e.g. FLAC). */
		if (rate > 0 && gui->tags.sampleRate == 0) {
			gui->tags.sampleRate = rate;
			FormatFileInfo(gui);
			if (gui->gadFileInfo)
				GT_SetGadgetAttrs(gui->gadFileInfo, gui->win, NULL,
					GTTX_Text, (ULONG)gui->fileInfoText,
					TAG_DONE);
		}

		/* Derive audio position from decoded frames rather than wall-clock ticks.
		 * Each MP3 frame = 1152 samples.  Subtract the selected half-buffer
		 * duration for pipeline lag, falling back to the requested slider value
		 * until the playback subprocess publishes the actual duration. */
		if (frames > 0 && rate > 0) {
			long audioSecs = (long)((frames * 1152UL) / (unsigned long)rate);
			audioSecs -= halfBufferMs ?
				(long)((halfBufferMs + 999UL) / 1000UL) : gui->bufferSeconds;
			if (audioSecs < 0)
				audioSecs = 0;
			if (gui->totalSecs > 0 && audioSecs > gui->totalSecs)
				audioSecs = gui->totalSecs;
			gui->elapsedSecs = (int)audioSecs + gui->launchBufferSecs;
		} else {
			gui->elapsedSecs++;
		}

		switch (phase) {
		case GUIPLAY_PHASE_BUFFERING: {
			int stage = gGuiPlaybackStatus.startupStage;
			int stageChanged = (stage != gui->lastStartupStage);
			if (isRadioInput)
				break;
			if (stageChanged) {
				gui->lastStartupStage = stage;
				gui->startupStageStableTicks = 0;
				gui->startupStallShown = 0;
			} else if (stage != GUISTART_PLAYING) {
				gui->startupStageStableTicks++;
			}
#ifdef MINIAMP3_DEBUG
			{
				char buf[128];
				if (gui->startupStageStableTicks >= 5 && !gui->startupStallShown) {
					sprintf(buf, "Startup stalled at: %s r%d/%d run%lu st%d",
						GuiStartupStageName(stage), gGuiPlaybackStatus.requestedRate,
						gGuiPlaybackStatus.effectiveRate, gGuiPlaybackStatus.runId, stage);
					gui->startupStallShown = 1;
				} else if (stage > GUISTART_NONE) {
					sprintf(buf, "Starting: %s r%d/%d run%lu st%d",
						GuiStartupStageName(stage), gGuiPlaybackStatus.requestedRate,
						gGuiPlaybackStatus.effectiveRate, gGuiPlaybackStatus.runId, stage);
				} else if (halfBufferMs)
					sprintf(buf, "Buffering... (%lums half-buffer)", halfBufferMs);
				else
					sprintf(buf, "Buffering... (%ds requested)", gui->bufferSeconds);
				SetStatus(gui, buf);
			}
#else
			if (gui->startupStageStableTicks >= 5 && !gui->startupStallShown) {
				SetStatus(gui, "Playback startup is taking longer than expected.");
				gui->startupStallShown = 1;
			} else if (phaseChanged || stageChanged) {
				if (stage == GUISTART_INPUT_PRELOAD_FASTMEM)
					SetStatus(gui, "Copying file to Fast RAM...");
				else if (stage >= GUISTART_AUDIO_SETUP)
					SetStatus(gui, "Buffering...");
				else
					SetStatus(gui, "Starting playback...");
			}
#endif
			break;
		}
		case GUIPLAY_PHASE_UNDERRUN:
			if (isRadioInput)
				break;
			if (underruns != gui->lastUnderrunCount) {
				char buf[64];
				gui->lastUnderrunCount = underruns;
				sprintf(buf, "Playing - underruns: %lu", underruns);
				SetStatus(gui, buf);
			}
			break;
		case GUIPLAY_PHASE_ERROR:
			if (phaseChanged)
				SetStatus(gui, "Decoder error - playback stopped.");
			break;
		case GUIPLAY_PHASE_STOPPING:
			if (gGuiPlaybackStatus.cleanupStage != gui->lastCleanupStage) {
				gui->lastCleanupStage = gGuiPlaybackStatus.cleanupStage;
#ifdef MINIAMP3_DEBUG
				switch (gui->lastCleanupStage) {
				case GUIPLAY_CLEANUP_ABORT_REAP: SetStatus(gui, "Stopping: aborting/reaping audio IO..."); break;
				case GUIPLAY_CLEANUP_DEVICE_CLOSED: SetStatus(gui, "Stopping: audio.device closed..."); break;
				case GUIPLAY_CLEANUP_BUFFERS_FREED: SetStatus(gui, "Stopping: buffers freed..."); break;
				case GUIPLAY_CLEANUP_COMPLETE: SetStatus(gui, "Stopping: cleanup complete..."); break;
				default: SetStatus(gui, "Stopping: cleanup started..."); break;
				}
#else
				SetStatus(gui, "Stopping...");
#endif
			}
			break;
		case GUIPLAY_PHASE_PLAYING: {
			if (gui->artRestartPending) {
				gui->artRestartPending = 0;
				StartArtDecode(gui);
			}
#ifdef MINIAMP3_DEBUG
			long delta = spareMs - gui->lastDisplayedSpareMs;
			if (delta < 0)
				delta = -delta;
			if (delta > 50 || gui->lastUnderrunCount != underruns) {
				char buf[64];
				gui->lastDisplayedSpareMs = spareMs;
				if (gui->lastUnderrunCount > 0)
					sprintf(buf, "Playing (%lu underruns, %ldms spare)",
						underruns, spareMs);
				else
					sprintf(buf, "Playing (%ldms spare)", spareMs);
				SetStatus(gui, buf);
			}
#else
			if (phaseChanged) {
				gui->lastDisplayedSpareMs = spareMs;
				gui->lastUnderrunCount = underruns;
				if (IsRadioInputName(gui->inputName)) {
					char station[128], status[160];
					CopyVolatileGuiString(station, sizeof(station), gGuiPlaybackStatus.radioStationName);
					FormatRadioStreamingStatus(gui, station, status, sizeof(status));
					SetStatus(gui, status);
				} else
					SetStatus(gui, "Playing");
			}
#endif
			break;
		}
		default:
			break;
		}

		if (gui->progressEnabled)
			DrawProgressIfChanged(gui);
	}
	{
		int artCanPump = !gui->playbackActive ||
			gGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
			gGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN;

		if (artCanPump)
			PumpArtDecode(gui);
		SendTimerRequest(gui, gui->artDecode.active && artCanPump ?
			ART_TIMER_MICROS : TIMER_TICK_MICROS);
	}
}

static void HandleDoneSignal(HelixAmp3Gui *gui)
{
	struct Message *msg;
	int gotDone;

	if (!gui->donePort)
		return;

	gotDone = 0;
	while ((msg = GetMsg(gui->donePort)) != NULL)
		gotDone = 1;
	if (!gotDone) {
		/* No message on the port — but if playbackDonePending is already set
		 * (polled ahead by HandleTimerSignal), still check if we can finalize. */
		if (gui->playbackDonePending && PlaybackCanFinalize(gui))
			FinalizePlayback(gui);
		return;
	}

	/* HelixAmp3CliMain() has returned, but the child has not necessarily
	 * finished its DOS/runtime teardown yet.  Keep Play locked until the
	 * playback task itself has disappeared. */
	if (gDoneRunId != gui->playbackRunId) {
		SetStatus(gui, "Ignoring stale playback completion.");
		return;
	}

	if (!gui->playbackDonePending) {
		gui->playbackStoppedByUser = gGuiPlayer.stopRequested ? 1 : 0;
		gui->playbackDonePending = 1;
		if (gui->playbackStoppedByUser)
			SetStatus(gui, "Stopping...");
		else if (IsRadioInputName(gui->inputName) &&
			gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR)
			SetRadioFailureStatus(gui, "radio error");
		else
			SetStatus(gui, "Playback finished - ready.");
	}

	if (PlaybackCanFinalize(gui))
		FinalizePlayback(gui);
}

static void GuiRefresh(HelixAmp3Gui *gui)
{
	if (!gui->win)
		return;
	GT_BeginRefresh(gui->win);
	GT_EndRefresh(gui->win, TRUE);
	DrawProgressFrame(gui);
	DrawProgress(gui);
	DrawArtPanel(gui);
}

static void SetMenuItemChecked(HelixAmp3Gui *gui, int menuNum, int itemNum,
	int checked);

static void SetDecodeThenPlay(HelixAmp3Gui *gui, int enabled)
{
	gui->decodeThenPlay = enabled ? 1 : 0;
	if (gui->win && gui->gadBuffer) {
		GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
			GA_Disabled, gui->decodeThenPlay,
			TAG_DONE);
	}
	SetStatus(gui, gui->decodeThenPlay ?
		"Decode-then-play enabled; Buffer slider disabled." :
		"Streaming playback mode enabled.");
	SaveGuiSettings(gui);
}

static void SetArtworkEnabled(HelixAmp3Gui *gui, int enabled)
{
	gui->artEnabled = enabled ? 1 : 0;
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_ARTWORK,
		gui->artEnabled);
	CancelArtDecode(gui);
	if (gui->artEnabled && gui->inputName[0] && !gui->tags.artData) {
		if (is_url_path(gui->inputName))
			SetInternetStreamMetadata(gui);
		else {
			ReadMp3Tags(gui->inputName, &gui->tags, 1);
			gui->totalSecs = gui->tags.durationSecs;
		}
		UpdateTagDisplay(gui);
	}
	UpdateArtDisplay(gui);
	SetStatus(gui, gui->artEnabled ? "Artwork enabled." : "Artwork disabled.");
	SaveGuiSettings(gui);
}

static void ShowAbout(HelixAmp3Gui *gui)
{
	struct EasyStruct es;

	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (UBYTE *)"About MiniAMP3";
	es.es_TextFormat = (UBYTE *)"MiniAMP3\nMade by boingball\n(C)2026 - v1.0\nTo support this application visit:\nhttps://buymeacoffee.com/boingball\n-----\nMade with decoders from\nHelix MP3 / ACC\nby Real Networks\nlibfoxenflac\nby astoeckel\n\nESP8266Audio\nby earlephilhower\n-----\nAI Used\nClaude and Codex\nLate Nights\nMany";
	es.es_GadgetFormat = (UBYTE *)"OK";
	EasyRequest(gui->win, &es, NULL, TAG_DONE);
}

static struct Gadget *MakeGadgetWithTextAttr(HelixAmp3Gui *gui, struct Gadget *prev,
	ULONG kind, UWORD id, WORD left, WORD top, WORD width, WORD height,
	struct TextAttr *textAttr,
	const char *label, ULONG tag1, ULONG value1, ULONG tag2, ULONG value2,
	ULONG tag3, ULONG value3, ULONG tag4, ULONG value4)
{
	struct NewGadget ng;

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = top;
	ng.ng_Width = width;
	ng.ng_Height = height;
	ng.ng_GadgetText = (UBYTE *)label;
	ng.ng_GadgetID = id;
	ng.ng_TextAttr = textAttr ? textAttr :
		(kind == TEXT_KIND ? &gTopaz8Attr : NULL);
	if (kind == BUTTON_KIND)
		ng.ng_Flags = PLACETEXT_IN;
	else if (kind == CHECKBOX_KIND)
		ng.ng_Flags = PLACETEXT_RIGHT;
	else
		ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_VisualInfo = gui->visualInfo;
	if (kind == SLIDER_KIND)
		return CreateGadget(kind, prev, &ng,
			GA_Immediate, TRUE,
			GA_RelVerify, TRUE,
			tag1, value1,
			tag2, value2,
			tag3, value3,
			tag4, value4,
			TAG_DONE);

	return CreateGadget(kind, prev, &ng,
		tag1, value1,
		tag2, value2,
		tag3, value3,
		tag4, value4,
		TAG_DONE);
}

static struct Gadget *MakeGadget(HelixAmp3Gui *gui, struct Gadget *prev,
	ULONG kind, UWORD id, WORD left, WORD top, WORD width, WORD height,
	const char *label, ULONG tag1, ULONG value1, ULONG tag2, ULONG value2,
	ULONG tag3, ULONG value3, ULONG tag4, ULONG value4)
{
	return MakeGadgetWithTextAttr(gui, prev, kind, id, left, top, width, height,
		NULL, label, tag1, value1, tag2, value2, tag3, value3, tag4, value4);
}

static struct Gadget *MakeSliderGadget(HelixAmp3Gui *gui, struct Gadget *prev,
	UWORD id, WORD left, WORD top, WORD width, const char *label,
	LONG minValue, LONG maxValue, LONG level, const char *format,
	LONG maxLevelLen, LONG visible)
{
	struct NewGadget ng;

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = top;
	ng.ng_Width = width;
	ng.ng_Height = 16;
	ng.ng_GadgetText = (UBYTE *)label;
	ng.ng_GadgetID = id;
	ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_VisualInfo = gui->visualInfo;
	return CreateGadget(SLIDER_KIND, prev, &ng,
		GA_Immediate, TRUE,
		GA_RelVerify, TRUE,
		GTSL_Min, minValue,
		GTSL_Max, maxValue,
		GTSL_Level, level,
		GTSL_LevelFormat, (ULONG)format,
		GTSL_LevelPlace, PLACETEXT_IN,
		GTSL_MaxLevelLen, maxLevelLen,
		PGA_Visible, visible,
		TAG_DONE);
}

static void UpdateChannelGadgetState(HelixAmp3Gui *gui)
{
	if (!gui->win)
		return;
	if (gui->gadChannelMode)
		GT_SetGadgetAttrs(gui->gadChannelMode, gui->win, NULL,
			GA_Disabled, gui->fakeStereo, TAG_DONE);
	if (gui->gadFakeStereoWidth)
		GT_SetGadgetAttrs(gui->gadFakeStereoWidth, gui->win, NULL,
			GA_Disabled, !gui->fakeStereo, TAG_DONE);
	if (gui->gadFakeStereoDelay)
		GT_SetGadgetAttrs(gui->gadFakeStereoDelay, gui->win, NULL,
			GA_Disabled, !gui->fakeStereo, TAG_DONE);
}

static int GuiCreateGadgets(HelixAmp3Gui *gui)
{
	struct Gadget *gad;

	gui->gadContext = CreateContext(&gui->gadgets);
	if (!gui->gadContext)
		return -1;
	gad = gui->gadContext;

	gui->gadFile = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILE,
		META_X, ROW_FILE, FILE_W, 16, "File:",
		GTTX_Text, (ULONG)gui->fileText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
		BROWSE_X, ROW_FILE - 1, BROWSE_W, 16, "Browse",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadTitle = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TITLE,
		META_X, ROW_TITLE, META_W, 16, "Title:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadArtist = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ARTIST,
		META_X, ROW_ARTIST, META_W, 16, "Artist:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadAlbum = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ALBUM,
		META_X, ROW_ALBUM, META_W, 16, "Album:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, TEXT_KIND, GID_RATING_LABEL,
		GUI_MARGIN_L + 60, ROW_RATING, 1, 16, "Rating:",
		GTTX_Text, (ULONG)"",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStars[0] = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STAR1,
		GUI_MARGIN_L + 78, ROW_RATING - 1, 22, 16, "-",
		TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!gad) return -1;
	gui->gadStars[1] = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STAR2,
		GUI_MARGIN_L + 102, ROW_RATING - 1, 22, 16, "-",
		TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!gad) return -1;
	gui->gadStars[2] = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STAR3,
		GUI_MARGIN_L + 126, ROW_RATING - 1, 22, 16, "-",
		TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!gad) return -1;
	gui->gadStars[3] = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STAR4,
		GUI_MARGIN_L + 150, ROW_RATING - 1, 22, 16, "-",
		TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!gad) return -1;
	gui->gadStars[4] = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STAR5,
		GUI_MARGIN_L + 174, ROW_RATING - 1, 22, 16, "-",
		TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!gad) return -1;
	gui->gadRatingValue = gad = MakeGadget(gui, gad, TEXT_KIND, GID_RATING_VALUE,
		GUI_MARGIN_L + 206, ROW_RATING, 80, 16, "",
		GTTX_Text, (ULONG)gui->ratingText,
		TAG_IGNORE, 0, TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!gad) return -1;
	gui->gadTrack = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TRACK,
		META_X, ROW_TRACK, META_W, 16, "Track:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadGenre = gad = MakeGadget(gui, gad, TEXT_KIND, GID_GENRE,
		META_X, ROW_GENRE, META_W, 16, "Genre:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, TEXT_KIND, GID_COUNT,
		GUI_MARGIN_L + 14, ROW_CHECKS - 1, 60, 16, "",
		GTTX_Text, (ULONG)"Speed:",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadSpeedMode = gad = MakeGadget(gui, gad, CYCLE_KIND, GID_SPEED_MODE,
		GUI_MARGIN_L + 86, ROW_CHECKS - 2, 130, 16, "",
		GTCY_Labels, (ULONG)kSpeedModeLabels,
		GTCY_Active, SpeedModeIndex(gui),
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadFastMem = gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_MEM,
		GUI_MARGIN_L + 314, ROW_CHECKS, 20, 12, "Fast-mem",
		GTCB_Checked, gui->fastMem,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadChannelMode = gad = MakeGadget(gui, gad, CYCLE_KIND, GID_CHANNEL_MODE,
		GUI_MARGIN_L + 14, ROW_CHANNELS - 2, 74, 16, "",
		GTCY_Labels, (ULONG)kChannelModeLabels,
		GTCY_Active, ChannelModeIndex(gui),
		GA_Disabled, gui->fakeStereo,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadFakeStereo = gad = MakeGadget(gui, gad,
		CHECKBOX_KIND, GID_FAKE_STEREO,
		GUI_MARGIN_L + 92, ROW_CHANNELS, 20, 12, "Fake-st",
		GTCB_Checked, gui->fakeStereo,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadFakeStereoWidth = gad = MakeGadget(gui, gad,
		CYCLE_KIND, GID_FAKE_STEREO_WIDTH,
		GUI_MARGIN_L + 232, ROW_CHANNELS - 2, 92, 16, "Width:",
		GTCY_Labels, (ULONG)kFakeStereoWidthLabels,
		GTCY_Active, gui->fakeStereoWidthIndex,
		GA_Disabled, !gui->fakeStereo,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadFakeStereoDelay = gad = MakeGadget(gui, gad,
		CYCLE_KIND, GID_FAKE_STEREO_DELAY,
		GUI_MARGIN_L + 414, ROW_CHANNELS - 2, 70, 16, "Delay:",
		GTCY_Labels, (ULONG)kFakeStereoDelayLabels,
		GTCY_Active, gui->fakeStereoDelayIndex,
		GA_Disabled, !gui->fakeStereo,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadRate = gad = MakeGadget(gui, gad, CYCLE_KIND, GID_RATE,
		GUI_MARGIN_L + 48, ROW_CYCLES, 80, 16, "Rate:",
		GTCY_Labels, (ULONG)kRateLabels,
		GTCY_Active, gui->rateIndex,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CYCLE_KIND, GID_QUALITY,
		GUI_MARGIN_L + 230, ROW_CYCLES, 100, 16, "Quality:",
		GTCY_Labels, (ULONG)kQualityLabels,
		GTCY_Active, gui->qualityIndex,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadBuffer = gad = MakeSliderGadget(gui, gad, GID_BUFFER,
		SLIDER_X, ROW_BUFFER, BUFFER_SLIDER_W, "Buffer:",
		1, 10, gui->bufferSeconds, "%ld sec", 6, 2);
	if (!gad)
		return -1;

	gui->gadVolume = gad = MakeSliderGadget(gui, gad, GID_VOLUME,
		SLIDER_X, ROW_VOLUME, VOLUME_SLIDER_W, "Volume:",
		0, 100, gui->volumePercent, "%ld%%", 4, 30);
	if (!gad)
		return -1;

	gui->gadPlay = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAY,
		PLAY_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadNext = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_NEXT,
		NEXT_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
		STOP_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadHardwareFilter = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_HARDWARE_FILTER,
		FILTER_X, ROW_BUTTONS, FILTER_W, TRANSPORT_H, "",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadPlaylist = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAYLIST,
		PL_OPEN_X, ROW_BUTTONS, PL_OPEN_W, TRANSPORT_H, "Playlist",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
		META_X, ROW_STATUS, GUI_WIN_W - META_X - GUI_MARGIN_R, 16, "Status:",
		GTTX_Text, (ULONG)gui->statusText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadFileInfo = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILEINFO,
		FILEINFO_X, ROW_FILEINFO, FILEINFO_W, 16, "File info:",
		GTTX_Text, (ULONG)gui->fileInfoText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	return 0;
}


static void SetMenuItemChecked(HelixAmp3Gui *gui, int menuNum, int itemNum,
	int checked)
{
	struct MenuItem *item;

	if (!gui->menuStrip)
		return;
	item = ItemAddress(gui->menuStrip, FULLMENUNUM(menuNum, itemNum, NOSUB));
	if (!item)
		return;
	if (checked)
		item->Flags |= CHECKED;
	else
		item->Flags &= ~CHECKED;
}

static void SyncMenuChecks(HelixAmp3Gui *gui)
{
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_DTP,
		gui->decodeThenPlay);
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_BENCH, gui->bench);
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_ARTWORK,
		gui->artEnabled);
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_ARTCACHE,
		gui->artCacheEnabled);
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_ARTCOLOR,
		gui->artColorEnabled);
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_PROGRESS,
		gui->progressEnabled);
}

static void StopPlayback(HelixAmp3Gui *gui);
static void WaitForPlaybackShutdown(HelixAmp3Gui *gui);
static void GuiClose(HelixAmp3Gui *gui);

static void DrainWindowMessages(HelixAmp3Gui *gui)
{
	struct IntuiMessage *msg;

	if (!gui || !gui->win)
		return;
	while ((msg = GT_GetIMsg(gui->win->UserPort)) != NULL)
		GT_ReplyIMsg(msg);
}

/*
 * ScanDecoderModules — find all *.decoder files in PROGDIR:decoders/, load
 * each one briefly to read its extension list, then build:
 *   gDecoderModulesPath  — absolute path for the playback subprocess
 *   gSupportedExtPattern — AmigaDOS ASL pattern like "#?.(mp3|aac|flac)"
 */
static void ScanDecoderModules(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	BPTR     progDir;
	BPTR     lock;
	struct FileInfoBlock *fib;
	char     dirPath[512];
	char     extList[256];   /* collected "mp3|aac|flac|..." */
	int      extLen;

	extList[0] = '\0';
	extLen     = 0;

	/* Build path: PROGDIR:decoders/ */
	progDir = GetProgramDir();
	if (!progDir || !NameFromLock(progDir, (STRPTR)dirPath, (LONG)sizeof(dirPath))) {
		/* Fallback: use PROGDIR: assign directly */
		strncpy(dirPath, "PROGDIR:", sizeof(dirPath) - 12);
		dirPath[sizeof(dirPath) - 12] = '\0';
	} else {
		/* Append trailing / if not already present */
		{
			int l = (int)strlen(dirPath);
			if (l > 0 && dirPath[l - 1] != '/' && dirPath[l - 1] != ':') {
				dirPath[l]     = '/';
				dirPath[l + 1] = '\0';
			}
		}
	}
	strncat(dirPath, "decoders/", sizeof(dirPath) - strlen(dirPath) - 1);

	strncpy(gDecoderModulesPath, dirPath, sizeof(gDecoderModulesPath) - 1);
	gDecoderModulesPath[sizeof(gDecoderModulesPath) - 1] = '\0';

	/* Always include built-in MP3 and the standard AAC module extension. */
	strncpy(extList, "mp3|aac", sizeof(extList) - 1);
	extLen = 7;

	lock = SafeLockPath("DecoderModules/Lock", gDecoderModulesPath, ACCESS_READ);
	if (lock) {
		fib = (struct FileInfoBlock *)AllocMem(sizeof(*fib), MEMF_CLEAR);
		if (fib && Examine(lock, fib)) {
			while (ExNext(lock, fib)) {
				const char *fname = fib->fib_FileName;
				const char *dot = NULL;
				const char *p;
				BPTR seg;
				char modPath[600];
				int  dlen, flen;

				for (p = fname; *p; p++)
					if (*p == '.') dot = p;
				if (!dot || strcmp(dot, ".decoder") != 0)
					continue;

				dlen = (int)strlen(gDecoderModulesPath);
				flen = (int)strlen(fname);
				if (dlen + flen + 1 >= (int)sizeof(modPath))
					continue;
				memcpy(modPath, gDecoderModulesPath, (size_t)dlen);
				memcpy(modPath + dlen, fname, (size_t)(flen + 1));

				seg = LoadSeg((STRPTR)modPath);
				if (seg) {
					typedef struct DecoderOps *(*EntFn)(void);
					EntFn entry = (EntFn)((UBYTE *)BADDR(seg) + 4);
					const struct DecoderOps *ops = entry();

					if (ops && ops->info &&
						ops->info->magic == DECODER_MODULE_MAGIC) {
						const char *exts = ops->info->extensions;
						while (exts && *exts) {
							int elen = (int)strlen(exts);
							if (extLen + 1 + elen + 1 < (int)sizeof(extList)) {
								extList[extLen] = '|';
								memcpy(extList + extLen + 1, exts, (size_t)(elen + 1));
								extLen += 1 + elen;
							}
							exts += elen + 1;
						}
					}
					UnLoadSeg(seg);
				}
			}
			FreeMem(fib, sizeof(*fib));
		}
		UnLock(lock);
	}

	/* Build pattern: "#?.(mp3|aac|flac)" or "#?.mp3" if only one */
	if (strchr(extList, '|')) {
		int written = 0;
		written += snprintf(gSupportedExtPattern + written,
			sizeof(gSupportedExtPattern) - (size_t)written,
			"#?.(");
		written += snprintf(gSupportedExtPattern + written,
			sizeof(gSupportedExtPattern) - (size_t)written,
			"%s", extList);
		snprintf(gSupportedExtPattern + written,
			sizeof(gSupportedExtPattern) - (size_t)written, ")");
	} else {
		snprintf(gSupportedExtPattern, sizeof(gSupportedExtPattern),
			"#?.%s", extList);
	}
#else
	strncpy(gSupportedExtPattern, "#?.(mp3|aac)", sizeof(gSupportedExtPattern) - 1);
	gSupportedExtPattern[sizeof(gSupportedExtPattern) - 1] = '\0';
#endif
}

static int GuiOpen(HelixAmp3Gui *gui)
{
	struct NewWindow nw;

	/* Discover decoder modules and build the ASL file-browser pattern first,
	 * so gSupportedExtPattern and gDecoderModulesPath are ready for playback. */
	ScanDecoderModules();

	memset(gui, 0, sizeof(*gui));
	/* Probe bsdsocket.library/AmiSSL once up front so the menu/gadgets below
	 * can be greyed out for offline users instead of failing on first use --
	 * see Radio_HasNetwork()/Radio_HasHttps(). */
	Radio_NetworkInit();
	gui->hasNetwork = Radio_HasNetwork();
	gui->hasHttps = Radio_HasHttps();
	gui->fastLowrate = LoadEnvInt("FastLowrate", 1, 0, 1);
	gui->superfastLowrate = LoadEnvInt("SuperfastLowrate", 0, 0, 1);
	gui->ultrafast = LoadEnvInt("Ultrafast", 0, 0, 1);
	gui->cd32Ultrafast = LoadEnvInt("CD32Ultrafast", 0, 0, 1);
	gui->fastMem = LoadEnvInt("FastMem", 1, 0, 1);
	gui->mono = LoadEnvInt("Mono", 1, 0, 1);
	gui->fakeStereo = LoadEnvInt("FakeStereo", 0, 0, 1);
	gui->fakeStereoWidthIndex = LoadEnvInt("FakeStereoWidthIndex", 1, 0, 4);
	gui->fakeStereoDelayIndex = LoadEnvInt("FakeStereoDelayIndex", 2, 0, 4);
	gui->hardwareFilter = LoadEnvInt("HardwareFilter", 0, 0, 1);
	gui->rateIndex = LoadEnvInt("RateIndex", 2, 0, 4);
	if (gui->cd32Ultrafast) {
		gui->ultrafast = 0;
		gui->fastLowrate = 1;
		gui->superfastLowrate = 1;
		gui->mono = 1;
		gui->rateIndex = 3;
	} else if (gui->ultrafast) {
		gui->fastLowrate = 0;
		gui->superfastLowrate = 0;
	}
	if (gui->superfastLowrate) {
		gui->fastLowrate = 1;
		if (!RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui)))
			gui->rateIndex = DefaultSuperfastRateIndex(ChannelUsesMonoCost(gui));
	}
	gui->bufferSeconds = LoadEnvInt("BufferSeconds", 10, 1, 10);
	gui->volumePercent = LoadEnvInt("Volume", 100, 0, 100);
	gMiniAmp3RequestedVolume = (unsigned short)gui->volumePercent;
	gMiniAmp3VolumeSequence++;
	{
		int settingsVersion;
		int loadedQuality;
		int hasSettingsVersion = LoadEnvIntMaybe("SettingsVersion", &settingsVersion,
			1, HELIXAMP3_SETTINGS_VERSION);
		int hasQualityIndex = LoadEnvIntMaybe("QualityIndex", &loadedQuality,
			HELIXAMP3_QUALITY_MIN, HELIXAMP3_QUALITY_MAX);

		if (!hasSettingsVersion && hasQualityIndex) {
			/* Version 1 settings used 0=Fast, 1=Normal, 2=Best.
			 * Version 2 inserts Faster at index 0, so migrate once. */
			if (loadedQuality > 2)
				loadedQuality = 2;
			gui->qualityIndex = loadedQuality + 1;
			SaveEnvInt("QualityIndex", gui->qualityIndex);
			SaveEnvInt("SettingsVersion", HELIXAMP3_SETTINGS_VERSION);
		} else {
			gui->qualityIndex = hasQualityIndex ? loadedQuality : 1;
		}
	}
	gui->decodeThenPlay = LoadEnvInt("DecodeThenPlay", 0, 0, 1);
	gui->bench = LoadEnvInt("Bench", 0, 0, 1);
	gui->artEnabled = LoadEnvInt("Artwork", 1, 0, 1);
	gui->artCacheEnabled = LoadEnvInt("ArtworkCache", 1, 0, 1);
	gui->artColorEnabled = LoadEnvInt("ArtworkColour", 0, 0, 1);
	gui->progressEnabled = LoadEnvInt("ProgressBar", 0, 0, 1);
	LoadEnvString("LastDrawer", gui->lastDrawer, sizeof(gui->lastDrawer));
	{
		int i;
		char key[32];
		gui->rbFavouriteCount = LoadEnvInt("RadioFavCount", gui->rbFavouriteCount, 0, HELIXAMP3_RADIO_FAV_MAX);
		for (i = 0; i < HELIXAMP3_RADIO_FAV_MAX; i++) {
			sprintf(key, "RadioFavName%d", i);
			LoadEnvString(key, gui->rbFavouriteNames[i], sizeof(gui->rbFavouriteNames[i]));
			sprintf(key, "RadioFavUrl%d", i);
			LoadEnvString(key, gui->rbFavouriteUrls[i], sizeof(gui->rbFavouriteUrls[i]));
		}
	}
	SafeCopy(gui->statusText, sizeof(gui->statusText), "Ready.");
	gui->lastDisplayedPhase = GUIPLAY_PHASE_IDLE;
	gui->lastDrawnElapsedSecs = -1;
	gui->lastDrawnTotalSecs = -1;
	SafeCopy(gui->fileInfoText, sizeof(gui->fileInfoText), "-");
	FormatRatingText(gui);
	SetFileDisplay(gui, NULL);
	NewList(&gui->playlist.list);
	gui->playlist.count = 0;
	gui->playlist.selected = -1;
	gui->playlist.current = -1;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
	if (!IntuitionBase) {
		fprintf(stderr, "MiniAMP3 requires intuition.library V37 or newer.\n");
		return -1;
	}
	AslBase = OpenLibrary("asl.library", 37);
	if (!AslBase) {
		fprintf(stderr, "MiniAMP3 requires asl.library V37 or newer.\n");
		GuiClose(gui);
		return -1;
	}
	GadToolsBase = OpenLibrary("gadtools.library", 37);
	if (!GadToolsBase) {
		fprintf(stderr, "MiniAMP3 requires gadtools.library V37 or newer.\n");
		GuiClose(gui);
		return -1;
	}
	GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
	if (!GfxBase) {
		fprintf(stderr, "MiniAMP3 requires graphics.library V37 or newer.\n");
		GuiClose(gui);
		return -1;
	}
	DiskfontBase = OpenLibrary("diskfont.library", 36);
	gui->smallFont = OpenBestFont();

	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = 40;
	nw.TopEdge = 30;
	nw.Width = GUI_WIN_W;
	nw.Height = GUI_WIN_H;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW |
		IDCMP_REFRESHWINDOW | IDCMP_ACTIVEWINDOW | IDCMP_MENUPICK;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
		WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE |
		WFLG_SMART_REFRESH;
	nw.FirstGadget = NULL;
	nw.Title = (UBYTE *)"MiniAMP3";
	nw.MinWidth = GUI_WIN_W;
	nw.MinHeight = GUI_WIN_H;
	nw.MaxWidth = 680;
	nw.MaxHeight = 440;
	nw.Type = WBENCHSCREEN;
	gui->win = OpenWindowTags(&nw,
		WA_InnerWidth, GUI_WIN_W,
		WA_InnerHeight, GUI_WIN_H,
		TAG_DONE);
	if (!gui->win) {
		fprintf(stderr, "cannot open MiniAMP3 window\n");
		GuiClose(gui);
		return -1;
	}
	if (gui->smallFont)
		SetFont(gui->win->RPort, gui->smallFont);

	gui->visualInfo = GetVisualInfo(gui->win->WScreen,
		TAG_DONE);
	if (!gui->visualInfo) {
		fprintf(stderr, "cannot create GadTools visual info\n");
		GuiClose(gui);
		return -1;
	}
	if (gui->smallFont)
		SetFont(gui->win->RPort, gui->smallFont);
	if (GuiCreateGadgets(gui) != 0) {
		fprintf(stderr, "cannot create MiniAMP3 gadgets\n");
		GuiClose(gui);
		return -1;
	}
	AddGList(gui->win, gui->gadgets, (UWORD)-1, -1, NULL);
	RefreshGList(gui->gadgets, gui->win, NULL, -1);
	UpdateChannelGadgetState(gui);
	ApplyHardwareAudioFilter(gui);
	if (gui->decodeThenPlay && gui->gadBuffer) {
		GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
			GA_Disabled, TRUE,
			TAG_DONE);
	}

	gui->menuStrip = CreateMenus(myNewMenus, TAG_DONE);
	if (gui->menuStrip) {
		LayoutMenus(gui->menuStrip, gui->visualInfo, TAG_DONE);
		SyncMenuChecks(gui);
		SetMenuStrip(gui->win, gui->menuStrip);
		if (!gui->hasNetwork)
			OffMenu(gui->win, FULLMENUNUM(MENUNUM_PROJECT, ITEMNUM_STREAM, NOSUB));
	}
	gui->timerPort = CreateMsgPort();
	if (gui->timerPort)
		gui->timerReq = (struct timerequest *)CreateIORequest(gui->timerPort,
			sizeof(struct timerequest));
	if (gui->timerReq && OpenDevice(TIMERNAME, UNIT_VBLANK,
		(struct IORequest *)gui->timerReq, 0) == 0) {
		gui->timerOpen = 1;
	} else {
		if (gui->timerReq) {
			DeleteIORequest((struct IORequest *)gui->timerReq);
			gui->timerReq = NULL;
		}
		if (gui->timerPort) {
			DeleteMsgPort(gui->timerPort);
			gui->timerPort = NULL;
		}
	}
	gui->donePort = CreateMsgPort();
	if (gui->donePort) {
		memset(&gDoneMsg, 0, sizeof(gDoneMsg));
		gDoneMsg.mn_Length = sizeof(gDoneMsg);
		gDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
	}
	GT_RefreshWindow(gui->win, NULL);
	DrawProgressFrame(gui);
	DrawProgress(gui);
	DrawArtPanel(gui);
	DrawTransportIcons(gui);
	DrawFilterButton(gui);
	if (gui->timerOpen)
		SendTimerRequest(gui, TIMER_TICK_MICROS);
	return 0;
}

static void GuiClose(HelixAmp3Gui *gui)
{
	CancelArtDecode(gui);
	if (gui->playbackActive)
		WaitForPlaybackShutdown(gui);
	if (gui->rbWin)
		CloseRadioWindow(gui);
	if (gui->win) {
		/* Stop Intuition from queuing new IDCMP traffic, then reply anything
		 * already pending before the window and GadTools objects disappear.
		 * Leaving stale IntuiMessages on an app window port is a classic source
		 * of recoverable alerts on memory cleanup.
		 */
		ModifyIDCMP(gui->win, 0);
		DrainWindowMessages(gui);
	}
	if (gui->timerReq) {
		if (gui->timerPending) {
			AbortIO((struct IORequest *)gui->timerReq);
			WaitIO((struct IORequest *)gui->timerReq);
			gui->timerPending = 0;
			gui->timerIsArt = 0;
		}
		if (gui->timerOpen) {
			CloseDevice((struct IORequest *)gui->timerReq);
			gui->timerOpen = 0;
		}
		DeleteIORequest((struct IORequest *)gui->timerReq);
		gui->timerReq = NULL;
	}
	if (gui->timerPort) {
		DeleteMsgPort(gui->timerPort);
		gui->timerPort = NULL;
	}
	if (gui->donePort) {
		struct Message *msg;

		gDonePort = NULL;
		while ((msg = GetMsg(gui->donePort)) != NULL)
			;
		DeleteMsgPort(gui->donePort);
		gui->donePort = NULL;
	}
	ClosePlaylistWindow(gui);
	ReleaseArtColorPens(gui);
	FreeTags(&gui->tags);
	if (gui->win && gui->menuStrip)
		ClearMenuStrip(gui->win);
	if (gui->menuStrip) {
		FreeMenus(gui->menuStrip);
		gui->menuStrip = NULL;
	}
	if (gui->win && gui->gadgets)
		RemoveGList(gui->win, gui->gadgets, -1);
	if (gui->win) {
		DrainWindowMessages(gui);
		CloseWindow(gui->win);
		gui->win = NULL;
	}
	if (gui->gadgets) {
		FreeGadgets(gui->gadgets);
		gui->gadgets = NULL;
	}
	if (gui->visualInfo) {
		FreeVisualInfo(gui->visualInfo);
		gui->visualInfo = NULL;
	}
	if (gui->smallFont) {
		CloseFont(gui->smallFont);
		gui->smallFont = NULL;
	}
	if (DiskfontBase) {
		CloseLibrary(DiskfontBase);
		DiskfontBase = NULL;
	}
	if (GfxBase) {
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = NULL;
	}
	if (GadToolsBase) {
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
	}
	if (AslBase) {
		CloseLibrary(AslBase);
		AslBase = NULL;
	}
	if (IntuitionBase) {
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
	}
	Radio_NetworkShutdown();
}


static unsigned long GuiInputFileSize(const char *path)
{
#if defined(AMIGA_M68K)
	BPTR fh;
	LONG size;

	fh = SafeOpenPath("GuiOpenRead/Open", path, MODE_OLDFILE);
	if (!fh)
		return 0;
	if (Seek(fh, 0, OFFSET_END) < 0) {
		Close(fh);
		return 0;
	}
	size = Seek(fh, 0, OFFSET_CURRENT);
	Close(fh);
	return size > 0 ? (unsigned long)size : 0;
#else
	FILE *f;
	long size;

	GuiLogPathOp("GuiInputFileSize/fopen", path);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0;
	}
	size = ftell(f);
	fclose(f);
	return size > 0 ? (unsigned long)size : 0;
#endif
}

static int GuiFastMemoryCanHoldFile(const char *path, unsigned long *fileSizeOut,
	unsigned long *fastAvailOut)
{
	unsigned long fileSize;
	unsigned long fastAvail;

	if (is_url_path(path)) {
		if (fileSizeOut)
			*fileSizeOut = 0;
		if (fastAvailOut)
			*fastAvailOut = 0;
		return 1;
	}
	fileSize = GuiInputFileSize(path);
#if defined(AMIGA_M68K)
	fastAvail = (unsigned long)AvailMem(MEMF_FAST);
#else
	fastAvail = (unsigned long)-1;
#endif
	if (fileSizeOut)
		*fileSizeOut = fileSize;
	if (fastAvailOut)
		*fastAvailOut = fastAvail;
	return fileSize > 0 && fileSize < fastAvail;
}

static void GuiDisableFastMemIfTooSmall(HelixAmp3Gui *gui)
{
	unsigned long fileSize;
	unsigned long fastAvail;

	if (!gui->fastMem || !gui->inputName[0])
		return;
	if (GuiFastMemoryCanHoldFile(gui->inputName, &fileSize, &fastAvail))
		return;
	gui->fastMem = 0;
	if (gui->win && gui->gadFastMem)
		GT_SetGadgetAttrs(gui->gadFastMem, gui->win, NULL,
			GTCB_Checked, FALSE, TAG_DONE);
	SaveGuiSettings(gui);
	if (fileSize > 0 && fastAvail != (unsigned long)-1) {
		char buf[128];
		sprintf(buf, "Fast-mem disabled: file %lu bytes, Fast RAM %lu bytes.",
			fileSize, fastAvail);
		SetStatus(gui, buf);
	} else {
		SetStatus(gui, "Fast-mem disabled: not enough Fast RAM for this file.");
	}
}

static void GuiDisableFastMemForRadio(HelixAmp3Gui *gui)
{
	if (!gui || !gui->fastMem)
		return;
	gui->fastMem = 0;
	if (gui->win && gui->gadFastMem)
		GT_SetGadgetAttrs(gui->gadFastMem, gui->win, NULL,
			GTCB_Checked, FALSE, TAG_DONE);
	SetStatus(gui, "Fast-mem disabled for internet streams.");
	SaveGuiSettings(gui);
}

static const int kRadioSearchLimits[] = { 10, 25, 50, 100 };
static STRPTR kRadioSearchLimitLabels[] = { (STRPTR)"10", (STRPTR)"25", (STRPTR)"50", (STRPTR)"100", NULL };
#define GT_RADIO_SEARCH_LIMIT_COUNT ((int)(sizeof(kRadioSearchLimits) / sizeof(kRadioSearchLimits[0])))
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

static const char *ProbeCodecName(RbStreamCodec codec)
{
	if (codec == RB_STREAM_CODEC_MP3) return "MP3";
	if (codec == RB_STREAM_CODEC_AAC) return "AAC";
	if (codec == RB_STREAM_CODEC_OGG) return "OGG";
	return "unknown";
}

static void RadioSetStatus(HelixAmp3Gui *app, const char *text)
{
	struct Gadget *gad;
	if (!app) return;
	if (!text) text = "";
	if (strcmp(app->rbStatusText, text) == 0)
		return;
	SafeCopy(app->rbStatusText, sizeof(app->rbStatusText), text);
	if (!app->rbWin || !app->rbGadgets) return;
	gad = app->rbGadgets;
	while (gad && gad->GadgetID != RB_GID_STATUS) gad = gad->NextGadget;
	if (gad)
		GT_SetGadgetAttrs(gad, app->rbWin, NULL,
			GTST_String, (ULONG)app->rbStatusText, TAG_DONE);
}

static int RadioStationMatchesScheme(HelixAmp3Gui *app, const RadioBrowserStation *st)
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

static void RadioRefreshResults(HelixAmp3Gui *app)
{
	int i, row;
	int selectedRow = -1;
	int wantedController = -1;
	int wantedFavourite = -1;
	char display[RB_MAX_NAME];
	const RadioBrowserStation *st;
	const char *url;
	const char *reason;
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
			if (i == wantedFavourite) selectedRow = row;
			sprintf(app->rbNames[row], "%.48s | favourite", app->rbFavouriteNames[i]);
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
			printf("Radio Browser filter: name=\"%s\" scheme=%s codec=%s bitrate=%d max=%d reason=%s\n",
				st->name, url && strncmp(url, "https://", 8) == 0 ? "https" : (url && strncmp(url, "http://", 7) == 0 ? "http" : "other"),
				st->codec, st->bitrate, app->rbController.max_bitrate, reason);
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
	if (app->rbWin && app->rbGadList)
		GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL,
			GTLV_Labels, (ULONG)&app->rbList,
			GTLV_Selected, selectedRow >= 0 ? (ULONG)selectedRow : (ULONG)~0,
			TAG_DONE);
}

static struct Gadget *FindRadioGadget(HelixAmp3Gui *app, UWORD id)
{
	struct Gadget *gad = app->rbGadgets;
	while (gad) {
		if (gad->GadgetID == id) return gad;
		gad = gad->NextGadget;
	}
	return NULL;
}

static void RadioDoSearch(HelixAmp3Gui *app)
{
	struct Gadget *nameGad = FindRadioGadget(app, RB_GID_SEARCH_TEXT);
	struct Gadget *codecGad = FindRadioGadget(app, RB_GID_CODEC);
	struct Gadget *countryGad = FindRadioGadget(app, RB_GID_COUNTRY);
	struct Gadget *countryCodeGad = FindRadioGadget(app, RB_GID_COUNTRY_CODE);
	struct Gadget *schemeGad = FindRadioGadget(app, RB_GID_SCHEME);
	struct Gadget *limitGad = FindRadioGadget(app, RB_GID_LIMIT);
	struct Gadget *bitrateGad = FindRadioGadget(app, RB_GID_BITRATE);
	STRPTR text;
	ULONG v;
	int rc;
	char filterMsg[192];

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
	v = 0;
	if (schemeGad)
		GT_GetGadgetAttrs(schemeGad, app->rbWin, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
	app->rbSchemeMode = ClampInt((int)v, 0, 2);
	app->rbShowHttps = (app->rbSchemeMode != 0);
	v = 1;
	if (limitGad)
		GT_GetGadgetAttrs(limitGad, app->rbWin, NULL, GTCY_Active, (ULONG)&v, TAG_DONE);
	app->rbController.limit = kRadioSearchLimits[ClampInt((int)v, 0, GT_RADIO_SEARCH_LIMIT_COUNT - 1)];
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
	app->rbShowingFavourites = FALSE;
	RadioRefreshResults(app);
	if (rc < 0)
		RadioSetStatus(app, app->rbController.last_error);
	else {
		char msg[128];
		int hidden = app->rbController.raw_station_count - app->rbVisibleCount;
		if (app->rbVisibleCount == 0 && app->rbController.raw_station_count == 0)
			sprintf(msg, "No stations found");
		else if (app->rbVisibleCount == 0 && app->rbController.raw_station_count > 0)
			sprintf(msg, "No stations found after filters");
		else
			sprintf(msg, "Found %d stations, showing %d playable (%d hidden)",
				app->rbController.raw_station_count, app->rbVisibleCount, hidden < 0 ? 0 : hidden);
		RadioSetStatus(app, msg);
	}
}

static void RadioSelectResult(HelixAmp3Gui *app, ULONG eventSelected)
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
	printf("radio results selection event row/index: %ld\n", (long)selected);
#endif
	if (selected == (ULONG)~0 || selected >= (ULONG)app->rbVisibleCount) {
		app->rbSelectedFavourite = -1;
		rb_controller_set_selected(&app->rbController, -1);
#ifdef MINIAMP3_DEBUG
		printf("radio results controller selected_index: %d\n", app->rbController.selected_index);
#endif
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	row = selected;
	selected = (ULONG)app->rbVisibleToController[row];
	if (app->rbShowingFavourites) {
		app->rbSelectedFavourite = (int)selected;
		GT_SetGadgetAttrs(app->rbGadList, app->rbWin, NULL, GTLV_Selected, (ULONG)row, TAG_DONE);
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
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	rb_station_display_name(st, display, (int)sizeof(display));
#ifdef MINIAMP3_DEBUG
	printf("radio results controller selected_index: %d\n", app->rbController.selected_index);
	printf("radio results station display name: %s\n", display);
#endif
	sprintf(msg, "Selected: %.120s", display);
	RadioSetStatus(app, msg);
}

static int RadioCurrentSelectedRow(HelixAmp3Gui *app)
{
	int i, wanted;
	if (!app || app->rbVisibleCount <= 0) return -1;
	wanted = app->rbShowingFavourites ? app->rbSelectedFavourite : app->rbController.selected_index;
	for (i = 0; i < app->rbVisibleCount; i++)
		if (app->rbVisibleToController[i] == wanted)
			return i;
	return -1;
}

static void RadioMoveSelection(HelixAmp3Gui *app, int delta)
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

static void RadioAddFavourite(HelixAmp3Gui *app)
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
			SaveGuiSettings(app);
			RadioSetStatus(app, "Favourite updated.");
			return;
		}
	}
	if (app->rbFavouriteCount >= HELIXAMP3_RADIO_FAV_MAX) {
		RadioSetStatus(app, "Radio favourites are full.");
		return;
	}
	i = app->rbFavouriteCount++;
	SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), display);
	SafeCopy(app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]), url);
	SaveGuiSettings(app);
	sprintf(msg, "Added favourite: %.120s", display);
	RadioSetStatus(app, msg);
}

static void RadioToggleFavourites(HelixAmp3Gui *app)
{
	app->rbShowingFavourites = app->rbShowingFavourites ? FALSE : TRUE;
	RadioRefreshResults(app);
	RadioSetStatus(app, app->rbShowingFavourites ? "Showing radio favourites." : "Showing search results.");
}

static void RadioDoProbeAndPlay(HelixAmp3Gui *app)
{
	static unsigned char peek[512];
	RbStreamInfo info;
	int peekLen = 0;
	int rc;
	const RadioBrowserStation *st;
	char msg[512];
	if (Radio_IsMemoryPoisoned()) {
		RadioSetStatus(app, "Memory corruption detected; restart MiniAMP3 before playing radio.");
		RADIO_DBG(printf("radio-memory: refusing RadioDoProbeAndPlay after MiniMem/ring corruption\n");)
		return;
	}
	if (app->playbackActive || app->playbackDonePending || PlaybackProcessStillExists()) {
		SafeCopy(app->queuedInputName, sizeof(app->queuedInputName), "radio-selection");
		app->queuedHaveRadioHostAddr = 0;
		app->queuedRadioHostAddrBe = 0;
		app->queuedPlayPending = 1;
		RadioSetStatus(app, "Queued stream; stopping previous stream...");
		if (!gGuiPlayer.stopRequested)
			StopPlayback(app);
		return;
	}
	if (app->rbShowingFavourites) {
		if (app->rbSelectedFavourite < 0 || app->rbSelectedFavourite >= app->rbFavouriteCount) {
			RadioSetStatus(app, "Select a favourite first.");
			return;
		}
		if (rb_probe_url_looks_hls(app->rbFavouriteUrls[app->rbSelectedFavourite])) {
			RadioSetStatus(app, "HLS stream not supported");
			return;
		}
		if (app->playbackActive || app->playbackDonePending) {
			SafeCopy(app->queuedInputName, sizeof(app->queuedInputName), app->rbFavouriteUrls[app->rbSelectedFavourite]);
			app->queuedHaveRadioHostAddr = 0;
			app->queuedRadioHostAddrBe = 0;
			app->queuedPlayPending = 1;
			StopPlayback(app);
			RadioSetStatus(app, "Stopping current stream before playing favourite...");
			return;
		}
		/* Favourites only store a name and URL, no favicon. */
		app->currentRadioFavicon[0] = '\0';
		SelectInternetStream(app, app->rbFavouriteUrls[app->rbSelectedFavourite]);
		SafeCopy(app->currentRadioStationName, sizeof(app->currentRadioStationName), app->rbFavouriteNames[app->rbSelectedFavourite]);
		app->haveRadioHostAddr = 0;
		app->radioHostAddrBe = 0;
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
	if (!app->hasHttps && rb_station_play_url(st) && strncmp(rb_station_play_url(st), "https://", 8) == 0) {
		RadioSetStatus(app, "HTTPS/TLS streams are not supported yet");
		return;
	}
	memset(&info, 0, sizeof(info));
	RadioSetStatus(app, "Checking stream...");
	rc = rb_controller_probe_selected(&app->rbController, &info, peek, (int)sizeof(peek), &peekLen);
	if (rc < 0) {
		radio_reset_playback_state_after_stop(app, "probe-failed");
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	if (info.codec != RB_STREAM_CODEC_MP3 && info.codec != RB_STREAM_CODEC_AAC &&
		info.codec != RB_STREAM_CODEC_OGG) {
		sprintf(msg, "Unsupported stream codec: %s (%.48s)", ProbeCodecName(info.codec), info.content_type);
		radio_reset_playback_state_after_stop(app, "probe-unsupported-codec");
		RadioSetStatus(app, msg);
		return;
	}
	if (!info.final_url[0]) {
		radio_reset_playback_state_after_stop(app, "probe-no-url");
		RadioSetStatus(app, "Stream probe did not return a playable URL.");
		return;
	}
	if (app->playbackActive || app->playbackDonePending) {
		SafeCopy(app->queuedInputName, sizeof(app->queuedInputName), info.final_url);
		app->queuedHaveRadioHostAddr = info.have_host_addr;
		app->queuedRadioHostAddrBe = info.host_addr_be;
		app->queuedPlayPending = 1;
		StopPlayback(app);
		RadioSetStatus(app, "Stopping current stream before playing selection...");
		return;
	}
	SafeCopy(app->currentRadioFavicon, sizeof(app->currentRadioFavicon), st->favicon);
	RADIO_DBG(printf("radio-art: station favicon=\"%s\"\n", app->currentRadioFavicon);)
	SelectInternetStream(app, info.final_url);
	rb_station_display_name(st, msg, (int)sizeof(msg));
	SafeCopy(app->currentRadioStationName, sizeof(app->currentRadioStationName), msg);
	app->haveRadioHostAddr = info.have_host_addr;
	app->radioHostAddrBe = info.host_addr_be;
	sprintf(msg, "Buffering - %.140s", app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio");
	RadioSetStatus(app, msg);
	StartPlayback(app);
}

static void CloseRadioWindow(HelixAmp3Gui *app)
{
	struct IntuiMessage *msg;
	if (!app->rbWin) return;
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

static void OpenRadioWindow(HelixAmp3Gui *app)
{
	struct NewWindow nw;
	struct NewGadget ng;
	struct Gadget *gad;
	static STRPTR codecs[] = { (STRPTR)"All", (STRPTR)"MP3", (STRPTR)"AAC", (STRPTR)"AAC+", NULL };
	if (app->rbWin || !app->win || !GadToolsBase || !app->hasNetwork) return;
	rb_controller_init(&app->rbController);
	app->rbShowHttps = FALSE;
	app->rbSchemeMode = 0;
	app->rbCountryMode = RadioCountryToIndex(app->rbController.countrycode);
	app->rbShowingFavourites = FALSE;
	app->rbSelectedFavourite = -1;
	app->rbVisibleCount = 0;
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
	ng.ng_GadgetID = RB_GID_SEARCH_TEXT; gad = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, RB_MAX_NAME, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 390; ng.ng_TopEdge = 24; ng.ng_Width = 90; ng.ng_GadgetText = (UBYTE *)"Codec"; ng.ng_GadgetID = RB_GID_CODEC;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)codecs, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 88; ng.ng_TopEdge = 52; ng.ng_Width = 150; ng.ng_GadgetText = (UBYTE *)"Country";
	ng.ng_GadgetID = RB_GID_COUNTRY; gad = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, RB_MAX_COUNTRY, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 304; ng.ng_TopEdge = 52; ng.ng_Width = 70; ng.ng_GadgetText = (UBYTE *)"Code"; ng.ng_GadgetID = RB_GID_COUNTRY_CODE; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioCountryLabels, GTCY_Active, app->rbCountryMode, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 430; ng.ng_TopEdge = 52; ng.ng_Width = 55; ng.ng_GadgetText = (UBYTE *)"URL"; ng.ng_GadgetID = RB_GID_SCHEME; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioSchemeLabels, GTCY_Active, app->rbSchemeMode,
		GA_Disabled, !app->hasHttps, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 88; ng.ng_TopEdge = 80; ng.ng_Width = 90; ng.ng_GadgetText = (UBYTE *)"Limit"; ng.ng_GadgetID = RB_GID_LIMIT; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioSearchLimitLabels, GTCY_Active, 1, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 288; ng.ng_TopEdge = 80; ng.ng_Width = 90; ng.ng_GadgetText = (UBYTE *)"Max kbps"; ng.ng_GadgetID = RB_GID_BITRATE; ng.ng_Flags = PLACETEXT_LEFT;
	gad = CreateGadget(CYCLE_KIND, gad, &ng, GTCY_Labels, (ULONG)kRadioBitrateLabels, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 8; ng.ng_TopEdge = 110; ng.ng_Width = 524; ng.ng_Height = 116; ng.ng_GadgetText = NULL; ng.ng_GadgetID = RB_GID_RADIO_RESULTS; ng.ng_Flags = 0;
	app->rbGadList = gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
		GTLV_Labels, (ULONG)&app->rbList,
		GTLV_Selected, (ULONG)~0,
		GA_RelVerify, TRUE, TAG_DONE); if (!gad) goto fail;
	ng.ng_TopEdge = 236; ng.ng_Width = 86; ng.ng_Height = 18; ng.ng_Flags = PLACETEXT_IN;
	ng.ng_LeftEdge = 8; ng.ng_GadgetText = (UBYTE *)"Search"; ng.ng_GadgetID = RB_GID_SEARCH; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 100; ng.ng_GadgetText = (UBYTE *)"Play"; ng.ng_GadgetID = RB_GID_PROBE; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 192; ng.ng_GadgetText = (UBYTE *)"Add Fav"; ng.ng_GadgetID = RB_GID_ADD_FAV; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 284; ng.ng_GadgetText = (UBYTE *)"Favourites"; ng.ng_GadgetID = RB_GID_FAVOURITES; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 376; ng.ng_Width = 40; ng.ng_GadgetText = (UBYTE *)"Up"; ng.ng_GadgetID = RB_GID_UP; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 420; ng.ng_GadgetText = (UBYTE *)"Down"; ng.ng_GadgetID = RB_GID_DOWN; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 464; ng.ng_GadgetText = (UBYTE *)"Close"; ng.ng_GadgetID = RB_GID_CLOSE; gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE); if (!gad) goto fail;
	ng.ng_LeftEdge = 8; ng.ng_TopEdge = 264; ng.ng_Width = 524; ng.ng_GadgetText = NULL; ng.ng_GadgetID = RB_GID_STATUS; ng.ng_Flags = 0;
	gad = CreateGadget(STRING_KIND, gad, &ng, GTST_String, (ULONG)"Ready.", GTST_MaxChars, 512, TAG_DONE); if (!gad) goto fail;
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = app->win->LeftEdge + 30; nw.TopEdge = app->win->TopEdge + 30;
	nw.Width = 548; nw.Height = 306;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH;
	nw.Title = (UBYTE *)"Internet Radio";
	nw.MinWidth = nw.MaxWidth = 548; nw.MinHeight = nw.MaxHeight = 306;
	nw.Type = WBENCHSCREEN;
	app->rbWin = OpenWindowTags(&nw, TAG_DONE);
	if (!app->rbWin) goto fail;
	AddGList(app->rbWin, app->rbGadgets, (UWORD)-1, -1, NULL);
	RefreshGList(app->rbGadgets, app->rbWin, NULL, -1);
	GT_RefreshWindow(app->rbWin, NULL);
	return;
fail:
	CloseRadioWindow(app);
}

static void HandleRadioWindow(HelixAmp3Gui *app)
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
			if (gid == RB_GID_RADIO_RESULTS)
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
				app->rbSchemeMode = ClampInt((int)code, 0, 2);
				app->rbShowHttps = (app->rbSchemeMode != 0);
				RadioRefreshResults(app);
			}
			else if (gid == RB_GID_COUNTRY_CODE) {
				struct Gadget *countryGad = FindRadioGadget(app, RB_GID_COUNTRY);
				app->rbCountryMode = ClampInt((int)code, 0, 6);
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


/* --- Playlist implementation -------------------------------------------- */

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

static void PlaylistRebuildList(Playlist *pl)
{
	int i;
	NewList(&pl->list);
	for (i = 0; i < pl->count; i++) {
		pl->nodes[i].ln_Name = pl->names[i];
		pl->nodes[i].ln_Type = NT_USER;
		pl->nodes[i].ln_Pri = 0;
		AddTail(&pl->list, &pl->nodes[i]);
	}
}

static void RefreshPlaylistView(HelixAmp3Gui *gui)
{
	PlaylistRebuildList(&gui->playlist);
	if (gui->plWin && gui->plGadList) {
		ULONG sel = (gui->playlist.selected >= 0) ?
			(ULONG)gui->playlist.selected : (ULONG)~0;
		GT_SetGadgetAttrs(gui->plGadList, gui->plWin, NULL,
			GTLV_Labels, (ULONG)&gui->playlist.list,
			GTLV_Selected, sel,
			TAG_DONE);
	}
}

static void ClosePlaylistWindow(HelixAmp3Gui *gui)
{
	if (!gui->plWin)
		return;
	ModifyIDCMP(gui->plWin, 0);
	{
		struct IntuiMessage *msg;
		while ((msg = GT_GetIMsg(gui->plWin->UserPort)) != NULL)
			GT_ReplyIMsg(msg);
	}
	if (gui->plGadgets)
		RemoveGList(gui->plWin, gui->plGadgets, -1);
	CloseWindow(gui->plWin);
	gui->plWin = NULL;
	if (gui->plGadgets) {
		FreeGadgets(gui->plGadgets);
		gui->plGadgets = NULL;
		gui->plGadContext = NULL;
		gui->plGadList = NULL;
	}
}

#define PL_WIN_W  460
#define PL_LIST_H 192
#define PL_BTN_H  18
#define PL_BTN_Y  (PL_LIST_H + 28)
#define PL_BTN_Y2 (PL_BTN_Y + PL_BTN_H + 4)
#define PL_WIN_H  (PL_BTN_Y2 + PL_BTN_H + 10)

static void OpenPlaylistWindow(HelixAmp3Gui *gui)
{
	struct NewWindow nw;
	struct Gadget *gad;
	struct NewGadget ng;
	int bw;
	int bx;

	if (gui->plWin || !gui->win || !gui->visualInfo)
		return;

	gui->plGadContext = CreateContext(&gui->plGadgets);
	if (!gui->plGadContext)
		return;
	gad = gui->plGadContext;

	PlaylistRebuildList(&gui->playlist);

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = 8;
	ng.ng_TopEdge = 20;
	ng.ng_Width = PL_WIN_W - 16;
	ng.ng_Height = PL_LIST_H;
	ng.ng_GadgetText = NULL;
	ng.ng_GadgetID = PL_GID_LIST;
	ng.ng_Flags = 0;
	ng.ng_VisualInfo = gui->visualInfo;
	gui->plGadList = gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
		GTLV_Labels, (ULONG)&gui->playlist.list,
		GTLV_Selected, gui->playlist.selected >= 0 ? (ULONG)gui->playlist.selected : (ULONG)~0,
		GTLV_ShowSelected, (ULONG)NULL,
		GA_RelVerify, TRUE,
		TAG_DONE);
	if (!gad) goto fail;

	bw = (PL_WIN_W - 16 - 12) / 4;
	bx = 8;

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = bx;
	ng.ng_TopEdge = PL_BTN_Y;
	ng.ng_Width = bw;
	ng.ng_Height = PL_BTN_H;
	ng.ng_GadgetText = (UBYTE *)"Add";
	ng.ng_GadgetID = PL_GID_ADD;
	ng.ng_Flags = PLACETEXT_IN;
	ng.ng_VisualInfo = gui->visualInfo;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
	if (!gad) goto fail;
	bx += bw + 4;

	ng.ng_LeftEdge = bx;
	ng.ng_GadgetText = (UBYTE *)"Remove";
	ng.ng_GadgetID = PL_GID_REMOVE;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
	if (!gad) goto fail;
	bx += bw + 4;

	ng.ng_LeftEdge = bx;
	ng.ng_GadgetText = (UBYTE *)"Clear";
	ng.ng_GadgetID = PL_GID_CLEAR;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
	if (!gad) goto fail;
	bx += bw + 4;

	ng.ng_LeftEdge = bx;
	ng.ng_GadgetText = (UBYTE *)"Play";
	ng.ng_GadgetID = PL_GID_PLAY;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
	if (!gad) goto fail;

	/* Second button row: Load M3U | Save M3U */
	bx = 8;
	bw = (PL_WIN_W - 16 - 4) / 2;
	ng.ng_TopEdge = PL_BTN_Y2;
	ng.ng_Width = bw;

	ng.ng_LeftEdge = bx;
	ng.ng_GadgetText = (UBYTE *)"Load M3U";
	ng.ng_GadgetID = PL_GID_LOAD_M3U;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
	if (!gad) goto fail;
	bx += bw + 4;

	ng.ng_LeftEdge = bx;
	ng.ng_GadgetText = (UBYTE *)"Save M3U";
	ng.ng_GadgetID = PL_GID_SAVE_M3U;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
	if (!gad) goto fail;

	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = gui->win->LeftEdge + 20;
	nw.TopEdge  = gui->win->TopEdge + 20;
	nw.Width    = PL_WIN_W;
	nw.Height   = PL_WIN_H;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH;
	nw.Title = (UBYTE *)"MiniAMP3 Playlist";
	nw.MinWidth  = PL_WIN_W;
	nw.MinHeight = PL_WIN_H;
	nw.MaxWidth  = PL_WIN_W;
	nw.MaxHeight = PL_WIN_H;
	nw.Type = WBENCHSCREEN;
	gui->plWin = OpenWindowTags(&nw, TAG_DONE);
	if (!gui->plWin)
		goto fail;
	if (gui->smallFont)
		SetFont(gui->plWin->RPort, gui->smallFont);
	AddGList(gui->plWin, gui->plGadgets, (UWORD)-1, -1, NULL);
	RefreshGList(gui->plGadgets, gui->plWin, NULL, -1);
	GT_RefreshWindow(gui->plWin, NULL);
	return;

fail:
	if (gui->plGadgets) {
		FreeGadgets(gui->plGadgets);
		gui->plGadgets = NULL;
		gui->plGadContext = NULL;
		gui->plGadList = NULL;
	}
}

static void PlaylistLoadAndShow(HelixAmp3Gui *gui, int index)
{
	if (index < 0 || index >= gui->playlist.count)
		return;
	gui->playlist.current = index;
	gui->playlist.selected = index;
	RefreshPlaylistView(gui);
	CancelArtDecode(gui);
	SafeCopy(gui->inputName, sizeof(gui->inputName),
		gui->playlist.paths[index]);
	SetFileDisplay(gui, gui->inputName);
	if (IsRadioInputName(gui->inputName)) {
		GuiDisableFastMemForRadio(gui);
		FreeTags(&gui->tags);
		memset(&gui->tags, 0, sizeof(gui->tags));
		SetInternetStreamMetadata(gui);
	} else {
		ReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
		gui->totalSecs = gui->tags.durationSecs;
	}
	gui->elapsedSecs = 0;
	gui->launchBufferSecs = 0;
	UpdateTagDisplay(gui);
	UpdateArtDisplay(gui);
	DrawProgress(gui);
	if (gui->artDecode.active)
		SendTimerRequest(gui, ART_TIMER_MICROS);
}

static void PlaylistLoadM3U(HelixAmp3Gui *gui)
{
	struct FileRequester *req;
	BPTR fh;
	char m3uPath[HELIXAMP3_MAX_PATH];
	char drawer[HELIXAMP3_MAX_PATH];
	char lineBuf[HELIXAMP3_MAX_PATH + 4];
	char fullPath[HELIXAMP3_MAX_PATH];
	char statusMsg[64];
	int lineLen;
	int addCount;
	int isAbsolute;
	int j;
	int n;
	char ch;

	if (!AslBase) {
		SetStatus(gui, "ASL library not available.");
		return;
	}
	req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Load M3U Playlist",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.m3u",
		ASLFR_InitialDrawer,
			(ULONG)(gui->lastDrawer[0] ? gui->lastDrawer : NULL),
		TAG_DONE);
	if (!req)
		return;
	if (!AslRequestTags(req, ASLFR_Window, (ULONG)gui->plWin,
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		FreeAslRequest(req);
		return;
	}

	m3uPath[0] = '\0';
	drawer[0] = '\0';
	if (req->fr_Drawer && req->fr_Drawer[0]) {
		SafeCopy(gui->lastDrawer, sizeof(gui->lastDrawer), req->fr_Drawer);
		SafeCopy(drawer, sizeof(drawer), req->fr_Drawer);
		SafeCopy(m3uPath, sizeof(m3uPath), req->fr_Drawer);
		SafeAddPartPath("PlaylistM3U/AddPart", m3uPath, req->fr_File, sizeof(m3uPath));
	} else if (req->fr_File && req->fr_File[0]) {
		SafeCopy(m3uPath, sizeof(m3uPath), req->fr_File);
	}
	FreeAslRequest(req);
	if (!m3uPath[0])
		return;

	fh = SafeOpenPath("PlaylistLoadM3U/Open", m3uPath, MODE_OLDFILE);
	if (!fh) {
		SetStatus(gui, "Cannot open M3U file.");
		return;
	}

	addCount = 0;
	lineLen = 0;
	while (Read(fh, &ch, 1) == 1) {
		if (ch == '\n' || ch == '\r') {
			if (lineLen > 0) {
				lineBuf[lineLen] = '\0';
				while (lineLen > 0 &&
					(lineBuf[lineLen-1] == '\r' || lineBuf[lineLen-1] == ' '))
					lineBuf[--lineLen] = '\0';
				if (lineLen > 0 && lineBuf[0] != '#' &&
					gui->playlist.count < HELIXAMP3_PLAYLIST_MAX) {
					isAbsolute = 0;
					for (j = 0; lineBuf[j] && lineBuf[j] != '/'; j++) {
						if (lineBuf[j] == ':') { isAbsolute = 1; break; }
					}
					if (isAbsolute || lineBuf[0] == '/') {
						SafeCopy(fullPath, sizeof(fullPath), lineBuf);
					} else {
						SafeCopy(fullPath, sizeof(fullPath), drawer);
						SafeAddPartPath("PlaylistLoadM3U/AddPartItem", fullPath, lineBuf, sizeof(fullPath));
					}
					n = gui->playlist.count;
					SafeCopy(gui->playlist.paths[n], HELIXAMP3_MAX_PATH, fullPath);
					SafeCopy(gui->playlist.names[n], 80, PlaylistBaseName(fullPath));
					gui->playlist.count++;
					addCount++;
				}
				lineLen = 0;
			}
		} else if (lineLen < (int)(sizeof(lineBuf) - 1)) {
			lineBuf[lineLen++] = ch;
		}
	}
	/* Handle final line with no trailing newline */
	if (lineLen > 0) {
		lineBuf[lineLen] = '\0';
		while (lineLen > 0 &&
			(lineBuf[lineLen-1] == '\r' || lineBuf[lineLen-1] == ' '))
			lineBuf[--lineLen] = '\0';
		if (lineLen > 0 && lineBuf[0] != '#' &&
			gui->playlist.count < HELIXAMP3_PLAYLIST_MAX) {
			isAbsolute = 0;
			for (j = 0; lineBuf[j] && lineBuf[j] != '/'; j++) {
				if (lineBuf[j] == ':') { isAbsolute = 1; break; }
			}
			if (isAbsolute || lineBuf[0] == '/') {
				SafeCopy(fullPath, sizeof(fullPath), lineBuf);
			} else {
				SafeCopy(fullPath, sizeof(fullPath), drawer);
				SafeAddPartPath("PlaylistLoadM3U/AddPartItem", fullPath, lineBuf, sizeof(fullPath));
			}
			n = gui->playlist.count;
			SafeCopy(gui->playlist.paths[n], HELIXAMP3_MAX_PATH, fullPath);
			SafeCopy(gui->playlist.names[n], 80, PlaylistBaseName(fullPath));
			gui->playlist.count++;
			addCount++;
		}
	}
	Close(fh);
	RefreshPlaylistView(gui);
	sprintf(statusMsg, "Loaded %d tracks from M3U.", addCount);
	SetStatus(gui, statusMsg);
}

static void PlaylistSaveM3U(HelixAmp3Gui *gui)
{
	struct FileRequester *req;
	BPTR fh;
	char m3uPath[HELIXAMP3_MAX_PATH];
	char lineBuf[HELIXAMP3_MAX_PATH + 2];
	int i;
	int len;

	if (gui->playlist.count <= 0) {
		SetStatus(gui, "Playlist is empty — nothing to save.");
		return;
	}
	if (!AslBase) {
		SetStatus(gui, "ASL library not available.");
		return;
	}
	req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Save M3U Playlist",
		ASLFR_DoSaveMode, TRUE,
		ASLFR_InitialFile, (ULONG)"playlist.m3u",
		ASLFR_InitialDrawer,
			(ULONG)(gui->lastDrawer[0] ? gui->lastDrawer : NULL),
		TAG_DONE);
	if (!req)
		return;
	if (!AslRequestTags(req, ASLFR_Window, (ULONG)gui->plWin,
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		FreeAslRequest(req);
		return;
	}

	m3uPath[0] = '\0';
	if (req->fr_Drawer && req->fr_Drawer[0]) {
		SafeCopy(m3uPath, sizeof(m3uPath), req->fr_Drawer);
		SafeAddPartPath("PlaylistM3U/AddPart", m3uPath, req->fr_File, sizeof(m3uPath));
	} else if (req->fr_File && req->fr_File[0]) {
		SafeCopy(m3uPath, sizeof(m3uPath), req->fr_File);
	}
	FreeAslRequest(req);
	if (!m3uPath[0])
		return;

	fh = SafeOpenPath("PlaylistSaveM3U/Open", m3uPath, MODE_NEWFILE);
	if (!fh) {
		SetStatus(gui, "Cannot create M3U file.");
		return;
	}

	len = (int)strlen("#EXTM3U\n");
	if (Write(fh, (APTR)"#EXTM3U\n", len) != len)
		goto fail;

	for (i = 0; i < gui->playlist.count; i++) {
		SafeCopy(lineBuf, sizeof(lineBuf) - 1, gui->playlist.paths[i]);
		len = (int)strlen(lineBuf);
		lineBuf[len] = '\n';
		if (Write(fh, (APTR)lineBuf, len + 1) != len + 1)
			goto fail;
	}
	Close(fh);
	SetStatus(gui, "Playlist saved as M3U.");
	return;
fail:
	Close(fh);
	SetStatus(gui, "Error writing M3U file.");
}

static void HandlePlaylistPoll(HelixAmp3Gui *gui)
{
	struct IntuiMessage *msg;
	ULONG classValue;
	UWORD code;
	struct Gadget *gad;

	if (!gui->plWin)
		return;
	while ((msg = GT_GetIMsg(gui->plWin->UserPort)) != NULL) {
		classValue = msg->Class;
		code = msg->Code;
		gad = (struct Gadget *)msg->IAddress;
		GT_ReplyIMsg(msg);
		if (classValue == IDCMP_CLOSEWINDOW) {
			ClosePlaylistWindow(gui);
			return;
		}
		if (classValue == IDCMP_REFRESHWINDOW) {
			GT_BeginRefresh(gui->plWin);
			GT_EndRefresh(gui->plWin, TRUE);
			continue;
		}
		if (classValue != IDCMP_GADGETUP || !gad)
			continue;
		switch ((int)gad->GadgetID) {
		case PL_GID_LIST:
			gui->playlist.selected = (int)code;
			break;
		case PL_GID_ADD: {
			struct FileRequester *req;
			req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
				ASLFR_TitleText, (ULONG)"Add to playlist",
				ASLFR_DoMultiSelect, TRUE,
				ASLFR_DoPatterns, TRUE,
				ASLFR_InitialPattern, (ULONG)gSupportedExtPattern,
				ASLFR_InitialDrawer,
					(ULONG)(gui->lastDrawer[0] ? gui->lastDrawer : NULL),
				TAG_DONE);
			if (!req) break;
			if (AslRequestTags(req, ASLFR_Window, (ULONG)gui->plWin,
				ASLFR_SleepWindow, TRUE, TAG_DONE)) {
				char path[HELIXAMP3_MAX_PATH];
				if (req->fr_Drawer && req->fr_Drawer[0])
					SafeCopy(gui->lastDrawer, sizeof(gui->lastDrawer),
						req->fr_Drawer);
				if (req->fr_NumArgs > 0 && req->fr_ArgList) {
					/* Multi-select (asl v38+) */
					int i;
					for (i = 0; i < (int)req->fr_NumArgs && gui->playlist.count < HELIXAMP3_PLAYLIST_MAX; i++) {
						int n;
						path[0] = '\0';
						if (req->fr_Drawer && req->fr_Drawer[0]) {
							SafeCopy(path, sizeof(path), req->fr_Drawer);
							SafeAddPartPath("PlaylistAdd/AddPartMulti", path, req->fr_ArgList[i].wa_Name, sizeof(path));
						} else {
							SafeCopy(path, sizeof(path), req->fr_ArgList[i].wa_Name);
						}
						if (!path[0]) continue;
						n = gui->playlist.count;
						SafeCopy(gui->playlist.paths[n], sizeof(gui->playlist.paths[0]), path);
						SafeCopy(gui->playlist.names[n], sizeof(gui->playlist.names[0]),
							PlaylistBaseName(path));
						gui->playlist.count++;
					}
				} else if (req->fr_File && req->fr_File[0]) {
					/* Single-select fallback */
					int n;
					path[0] = '\0';
					if (req->fr_Drawer && req->fr_Drawer[0]) {
						SafeCopy(path, sizeof(path), req->fr_Drawer);
						SafeAddPartPath("ChooseMp3/AddPart", path, req->fr_File, sizeof(path));
					} else {
						SafeCopy(path, sizeof(path), req->fr_File);
					}
					if (path[0] && gui->playlist.count < HELIXAMP3_PLAYLIST_MAX) {
						n = gui->playlist.count;
						SafeCopy(gui->playlist.paths[n], sizeof(gui->playlist.paths[0]), path);
						SafeCopy(gui->playlist.names[n], sizeof(gui->playlist.names[0]),
							PlaylistBaseName(path));
						gui->playlist.count++;
					}
				}
				RefreshPlaylistView(gui);
			}
			FreeAslRequest(req);
			break;
		}
		case PL_GID_REMOVE:
			if (gui->playlist.selected >= 0 && gui->playlist.selected < gui->playlist.count) {
				int i;
				int sel = gui->playlist.selected;
				for (i = sel; i < gui->playlist.count - 1; i++) {
					SafeCopy(gui->playlist.paths[i], sizeof(gui->playlist.paths[0]),
						gui->playlist.paths[i + 1]);
					SafeCopy(gui->playlist.names[i], sizeof(gui->playlist.names[0]),
						gui->playlist.names[i + 1]);
				}
				gui->playlist.count--;
				if (gui->playlist.current > sel)
					gui->playlist.current--;
				else if (gui->playlist.current == sel)
					gui->playlist.current = -1;
				if (gui->playlist.selected >= gui->playlist.count)
					gui->playlist.selected = gui->playlist.count - 1;
				RefreshPlaylistView(gui);
			}
			break;
		case PL_GID_CLEAR:
			gui->playlist.count = 0;
			gui->playlist.selected = -1;
			gui->playlist.current = -1;
			RefreshPlaylistView(gui);
			break;
		case PL_GID_PLAY:
			if (gui->playlist.selected >= 0 && gui->playlist.selected < gui->playlist.count) {
				if (gui->playbackActive || gui->playbackDonePending) {
					SetStatus(gui, "Stop playback before starting playlist.");
					break;
				}
				PlaylistLoadAndShow(gui, gui->playlist.selected);
				StartPlayback(gui);
			}
			break;
		case PL_GID_LOAD_M3U:
			PlaylistLoadM3U(gui);
			break;
		case PL_GID_SAVE_M3U:
			PlaylistSaveM3U(gui);
			break;
		}
	}
}

/* --- End playlist implementation ---------------------------------------- */

static void ChooseMp3(HelixAmp3Gui *gui)
{
	struct FileRequester *req;
	char path[HELIXAMP3_MAX_PATH];

	if (!gui->lastDrawer[0] && gui->inputName[0])
		CopyDrawerFromPath(gui->lastDrawer, sizeof(gui->lastDrawer),
			gui->inputName);
	req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Select audio file for MiniAMP3",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)gSupportedExtPattern,
		ASLFR_InitialDrawer,
			(ULONG)(gui->lastDrawer[0] ? gui->lastDrawer : NULL),
		TAG_DONE);
	if (!req) {
		SetStatus(gui, "Cannot allocate ASL file requester.");
		return;
	}
	if (AslRequestTags(req, ASLFR_Window, (ULONG)gui->win,
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		path[0] = '\0';
		if (req->fr_Drawer && req->fr_Drawer[0]) {
			SafeCopy(gui->lastDrawer, sizeof(gui->lastDrawer),
				req->fr_Drawer);
			SafeCopy(path, sizeof(path), req->fr_Drawer);
			SafeAddPartPath("ChooseMp3/AddPart", path, req->fr_File, sizeof(path));
		} else {
			SafeCopy(path, sizeof(path), req->fr_File);
		}
		if (gui->playbackActive || gui->playbackDonePending) {
			SafeCopy(gui->queuedInputName, sizeof(gui->queuedInputName), path);
			SetStatus(gui, "Selected for next Play.");
		} else {
			CancelArtDecode(gui);
			SafeCopy(gui->inputName, sizeof(gui->inputName), path);
			SetFileDisplay(gui, gui->inputName);
			ReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
			if (is_url_path(gui->inputName))
				SetInternetStreamMetadata(gui);
			else
				gui->totalSecs = gui->tags.durationSecs;
			gui->elapsedSecs = 0;
			UpdateTagDisplay(gui);
			UpdateArtDisplay(gui);
			DrawProgress(gui);
			if (gui->artDecode.active)
				SendTimerRequest(gui, ART_TIMER_MICROS);
			if (!gui->artDecode.active) {
				FormatReadyStatus(&gui->tags, gui->statusText, sizeof(gui->statusText));
				SetStatus(gui, gui->statusText);
			}
			GuiDisableFastMemIfTooSmall(gui);
		}
	}
	FreeAslRequest(req);
}

static void SelectInternetStream(HelixAmp3Gui *gui, const char *url)
{
	if (!url || !url[0])
		return;
	if (!IsRadioInputName(url)) {
		SetStatus(gui, "Internet streams must start with http:// or https://");
		return;
	}
	if (gui->playbackActive || gui->playbackDonePending) {
		GuiDisableFastMemForRadio(gui);
		SafeCopy(gui->queuedInputName, sizeof(gui->queuedInputName), url);
		gui->queuedHaveRadioHostAddr = 0;
		gui->queuedRadioHostAddrBe = 0;
		SetStatus(gui, "Internet stream selected for next Play.");
		return;
	}
	CancelArtDecode(gui);
	GuiDisableFastMemForRadio(gui);
	SafeCopy(gui->inputName, sizeof(gui->inputName), url);
	gui->haveRadioHostAddr = 0;
	gui->radioHostAddrBe = 0;
	SetFileDisplay(gui, gui->inputName);
	SetInternetStreamMetadata(gui);
	gui->elapsedSecs = 0;
	gui->launchBufferSecs = 0;
	UpdateTagDisplay(gui);
	UpdateArtDisplay(gui);
	DrawProgress(gui);
	SetStatus(gui, "Internet stream ready.");
}

static void EnterInternetStream(HelixAmp3Gui *gui)
{
	struct NewWindow nw;
	struct Window *win;
	struct Gadget *gadgets;
	struct Gadget *gadContext;
	struct Gadget *gad;
	struct Gadget *gadString;
	struct NewGadget ng;
	struct IntuiMessage *msg;
	char *enteredUrl;
	char url[HELIXAMP3_MAX_PATH];
	int done;
	int accepted;

	if (!gui || !gui->win || !gui->visualInfo)
		return;
	SafeCopy(url, sizeof(url), IsRadioInputName(gui->inputName) ?
		gui->inputName : "http://");

	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = gui->win->LeftEdge + 20;
	nw.TopEdge = gui->win->TopEdge + 25;
	nw.Width = 560;
	nw.Height = 72;
	nw.DetailPen = gui->win->DetailPen;
	nw.BlockPen = gui->win->BlockPen;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
		IDCMP_VANILLAKEY;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
		WFLG_ACTIVATE | WFLG_RMBTRAP;
	nw.Title = (UBYTE *)"MiniAMP3 Internet Stream";
	nw.Type = CUSTOMSCREEN;
	nw.Screen = gui->win->WScreen;

	win = OpenWindowTags(&nw, TAG_DONE);
	if (!win) {
		SetStatus(gui, "Cannot open Internet Stream requester.");
		return;
	}

	gadgets = NULL;
	gadContext = CreateContext(&gadgets);
	if (!gadContext) {
		CloseWindow(win);
		SetStatus(gui, "Cannot create Internet Stream gadgets.");
		return;
	}
	gad = gadContext;

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = 80;
	ng.ng_TopEdge = 12;
	ng.ng_Width = 455;
	ng.ng_Height = 16;
	ng.ng_GadgetText = (UBYTE *)"URL:";
	ng.ng_GadgetID = GID_STREAM_URL;
	ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_VisualInfo = gui->visualInfo;
	gadString = gad = CreateGadget(STRING_KIND, gad, &ng,
		GTST_String, (ULONG)url,
		GTST_MaxChars, sizeof(url),
		GA_TabCycle, TRUE,
		GA_RelVerify, TRUE,
		TAG_DONE);

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = 175;
	ng.ng_TopEdge = 40;
	ng.ng_Width = 80;
	ng.ng_Height = 16;
	ng.ng_GadgetText = (UBYTE *)"OK";
	ng.ng_GadgetID = GID_STREAM_OK;
	ng.ng_Flags = PLACETEXT_IN;
	ng.ng_VisualInfo = gui->visualInfo;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = 305;
	ng.ng_TopEdge = 40;
	ng.ng_Width = 80;
	ng.ng_Height = 16;
	ng.ng_GadgetText = (UBYTE *)"Cancel";
	ng.ng_GadgetID = GID_STREAM_CANCEL;
	ng.ng_Flags = PLACETEXT_IN;
	ng.ng_VisualInfo = gui->visualInfo;
	gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);

	if (!gadString || !gad) {
		FreeGadgets(gadgets);
		CloseWindow(win);
		SetStatus(gui, "Cannot create Internet Stream gadgets.");
		return;
	}

	AddGList(win, gadgets, (UWORD)-1, -1, NULL);
	RefreshGList(gadgets, win, NULL, -1);
	GT_RefreshWindow(win, NULL);
	ActivateGadget(gadString, win, NULL);

	done = 0;
	accepted = 0;
	while (!done) {
		WaitPort(win->UserPort);
		while ((msg = GT_GetIMsg(win->UserPort)) != NULL) {
			ULONG classValue = msg->Class;
			struct Gadget *which = (struct Gadget *)msg->IAddress;
			GT_ReplyIMsg(msg);
			if (classValue == IDCMP_CLOSEWINDOW) {
				done = 1;
			} else if (classValue == IDCMP_VANILLAKEY &&
				(msg->Code == '\r' || msg->Code == '\n')) {
				accepted = 1;
				done = 1;
			} else if (classValue == IDCMP_REFRESHWINDOW) {
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
			} else if (classValue == IDCMP_GADGETUP && which) {
				if (which->GadgetID == GID_STREAM_OK ||
					which->GadgetID == GID_STREAM_URL) {
					accepted = 1;
					done = 1;
				} else if (which->GadgetID == GID_STREAM_CANCEL) {
					done = 1;
				}
			}
		}
	}

	enteredUrl = NULL;
	if (accepted) {
		GT_GetGadgetAttrs(gadString, win, NULL,
			GTST_String, (ULONG)&enteredUrl,
			TAG_DONE);
		if (enteredUrl)
			SafeCopy(url, sizeof(url), enteredUrl);
	}
	FreeGadgets(gadgets);
	CloseWindow(win);
	if (accepted) {
		/* Manually typed URLs never have a known station favicon; clear any
		 * favicon left over from a previously played station so the art
		 * panel doesn't show a stale image for this stream. */
		gui->currentRadioFavicon[0] = '\0';
		SelectInternetStream(gui, url);
	}
}

static void AddArg(HelixAmp3Args *args, const char *text)
{
	if (args->argc >= HELIXAMP3_ARGC_MAX)
		return;
	SafeCopy(args->argvStorage[args->argc], HELIXAMP3_MAX_PATH, text);
	args->argv[args->argc] = args->argvStorage[args->argc];
	args->argc++;
}

static void BuildPlaybackArgs(HelixAmp3Gui *gui, HelixAmp3Args *args)
{
	char num[16];
	int isRadio = is_url_path(gui->inputName);

	memset(args, 0, sizeof(*args));
	AddArg(args, "amiga_mp3dec");
	AddArg(args, "--play");
	if (is_url_path(gui->inputName)) {
		AddArg(args, "--radio-stream");
		if (gui->haveRadioHostAddr) {
			AddArg(args, "--radio-host-addr-be");
			sprintf(num, "%lu", gui->radioHostAddrBe);
			AddArg(args, num);
		}
		if (gui->rbController.selected_index >= 0) {
			const RadioBrowserStation *st = rb_controller_get_station(&gui->rbController, gui->rbController.selected_index);
			if (st && st->codec[0]) {
				AddArg(args, "--radio-codec-hint");
				AddArg(args, st->codec);
			}
		}
	}
	/* --fast-mem preloads the complete input and requires a finite, seekable
	 * local file.  Radio streams are live sockets/handles, so never pass the
	 * preload flag through for URL input even if an old setting is still on. */
	if (gui->fastMem && !isRadio)
		AddArg(args, "--fast-mem");
	if (gui->cd32Ultrafast) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
		AddArg(args, "--exp-reduced-taps");
		AddArg(args, "--subband-cap");
		AddArg(args, "12");
	} else if (gui->superfastLowrate ||
		(gui->ultrafast && strcmp(kRates[gui->rateIndex], "28600") != 0)) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
	} else if (gui->fastLowrate && strcmp(kRates[gui->rateIndex], "28600")) {
		AddArg(args, "--fast-lowrate");
	}
	if (gui->ultrafast && strcmp(kRates[gui->rateIndex], "28600") == 0)
		AddArg(args, "--ultrafast");
	if (gui->fakeStereo) {
		AddArg(args, "--fake-stereo");
		AddArg(args, "--fake-stereo-delay");
		sprintf(num, "%d", kFakeStereoDelays[gui->fakeStereoDelayIndex]);
		AddArg(args, num);
		AddArg(args, "--fake-stereo-shift");
		sprintf(num, "%d", kFakeStereoShifts[gui->fakeStereoWidthIndex]);
		AddArg(args, num);
	} else if (gui->mono) {
		AddArg(args, "--mono");
	} else {
		AddArg(args, "--stereo");
	}
	AddArg(args, "--rate");
	AddArg(args, kRates[gui->rateIndex]);
	AddArg(args, "--buffer-seconds");
	sprintf(num, "%d", gui->bufferSeconds);
	AddArg(args, num);
	AddArg(args, "--volume");
	sprintf(num, "%d", gui->volumePercent);
	AddArg(args, num);
	AddArg(args, "--quality");
	sprintf(num, "%d", gui->qualityIndex);
	AddArg(args, num);
	if (gui->decodeThenPlay)
		AddArg(args, "--decode-then-play");
	if (gui->bench)
		AddArg(args, "--bench");
	AddArg(args, gui->inputName);
	args->argv[args->argc] = NULL;
}

#ifdef MINIAMP3_DEBUG
static void DebugPrintPlaybackArgs(const char *label, const HelixAmp3Args *args)
{
	int i;
	printf("miniamp3-debug: %s argc=%d", label, args->argc);
	for (i = 0; i < args->argc; i++)
		printf(" %s", args->argv[i]);
	printf("\n");
}

static int DebugArgIndex(const HelixAmp3Args *args, const char *arg)
{
	int i;

	for (i = 0; i < args->argc; i++) {
		if (!strcmp(args->argv[i], arg))
			return i;
	}
	return -1;
}

static int DebugArgCount(const HelixAmp3Args *args, const char *arg)
{
	int i;
	int count = 0;

	for (i = 0; i < args->argc; i++) {
		if (!strcmp(args->argv[i], arg))
			count++;
	}
	return count;
}

static void DebugValidatePlaybackArgs(const char *label, const HelixAmp3Args *args,
	int expectedQuality, int expectedMono)
{
	char expected[16];
	int qualityIndex;

	sprintf(expected, "%d", expectedQuality);
	qualityIndex = DebugArgIndex(args, "--quality");
	if (qualityIndex < 0 || qualityIndex + 1 >= args->argc ||
		strcmp(args->argv[qualityIndex + 1], expected))
		printf("miniamp3-debug: ERROR %s missing expected --quality %s\n",
			label, expected);
	if (DebugArgCount(args, "--quality") != 1)
		printf("miniamp3-debug: ERROR %s emitted --quality %d times\n",
			label, DebugArgCount(args, "--quality"));
	if (DebugArgIndex(args, "--play-fast-path") >= 0)
		printf("miniamp3-debug: ERROR %s emitted --play-fast-path\n", label);
	if (expectedMono) {
		if (DebugArgCount(args, "--mono") != 1 || DebugArgCount(args, "--stereo") != 0)
			printf("miniamp3-debug: ERROR %s mono argument mismatch\n", label);
	} else if (DebugArgCount(args, "--stereo") != 1 || DebugArgCount(args, "--mono") != 0) {
		printf("miniamp3-debug: ERROR %s stereo argument mismatch\n", label);
	}
}

static void DebugSelftestPlaybackChannelArgs(HelixAmp3Gui *gui)
{
	HelixAmp3Gui copy;
	HelixAmp3Args testArgs;
	int quality;

	copy = *gui;
	copy.fakeStereo = 0;	/* this check validates plain --mono/--stereo emission */
	copy.mono = 1;
	BuildPlaybackArgs(&copy, &testArgs);
	DebugPrintPlaybackArgs("BuildPlaybackArgs mono checked", &testArgs);
	DebugValidatePlaybackArgs("BuildPlaybackArgs mono checked", &testArgs,
		copy.qualityIndex, 1);
	copy.mono = 0;
	BuildPlaybackArgs(&copy, &testArgs);
	DebugPrintPlaybackArgs("BuildPlaybackArgs mono unchecked", &testArgs);
	DebugValidatePlaybackArgs("BuildPlaybackArgs mono unchecked", &testArgs,
		copy.qualityIndex, 0);
	for (quality = HELIXAMP3_QUALITY_MIN; quality <= HELIXAMP3_QUALITY_MAX; quality++) {
		copy.qualityIndex = quality;
		BuildPlaybackArgs(&copy, &testArgs);
		DebugPrintPlaybackArgs(kQualityLabels[quality], &testArgs);
		DebugValidatePlaybackArgs(kQualityLabels[quality], &testArgs, quality, copy.mono);
	}
}
#endif

/* HelixAmp3CliMain() is a renamed command-line main() and is invoked more
 * than once by the GUI.  The C runtime getopt parser is process-global, so
 * after the first invocation optind normally points at argc.  Without
 * resetting it, the second invocation can skip all options and the filename,
 * leaving the GUI believing that a playback child is alive while no audio is
 * actually started. */
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

static void ResetDecoderStatics(void)
{
	extern int MP3ResetStatics(void);

	MP3ResetStatics();
}

static void PlaybackEntry(void)
{
	struct MsgPort *donePort;
	int stopBeforeStart;
	int earlyStop;
	ULONG pending;
	int ranDecoder;

	/* StartPlayback() already clears the stop flags before CreateNewProcTags().
	 * Do not clear them again here: Stop can be pressed after the GUI marks
	 * playback active but before this subprocess has entered the decoder.
	 * ResetDecoderStatics() clears decoder globals, so preserve an early Stop
	 * request and turn it back into an interrupt instead of letting the child
	 * run while the GUI is stuck in "Stopping...".
	 */
	stopBeforeStart = gGuiPlayer.stopRequested;
	pending = SetSignal(0, 0);
	ranDecoder = 0;
	gGuiPlaybackStatus.startupStage = GUISTART_CHILD_ENTERED;
	earlyStop = stopBeforeStart || gGuiPlayer.stopRequested ||
		gPlaybackInterrupted || (pending & SIGBREAKF_CTRL_C);
#ifdef MINIAMP3_DEBUG
	if (earlyStop)
		printf("miniamp3-debug: early Stop sampled before child entry\n");
	if (pending & SIGBREAKF_CTRL_C)
		printf("miniamp3-debug: Ctrl-C pending before reset\n");
#endif
	if (earlyStop)
		gPlaybackInterrupted = 1;
	ResetCliParser();
	gGuiPlaybackStatus.startupStage = GUISTART_ARGS_READY;
	if (gGuiPlayer.stopRequested || gPlaybackInterrupted)
		earlyStop = 1;
	if (!earlyStop)
		ResetDecoderStatics();
	gGuiPlaybackStatus.runId = gPlaybackEntryRunId;
	if (stopBeforeStart || gGuiPlayer.stopRequested || gPlaybackInterrupted ||
		(pending & SIGBREAKF_CTRL_C)) {
		earlyStop = 1;
		gPlaybackInterrupted = 1;
#ifdef MINIAMP3_DEBUG
		printf("miniamp3-debug: Stop observed after reset\n");
#endif
	}
	gGuiPlaybackStatus.startupStage = GUISTART_DECODER_CONFIG;

	/* MP3ResetStatics() may also touch command-line/playback globals in some
	 * decoder revisions, so establish the parser's initial state immediately
	 * before calling the renamed main() as well. */
	ResetCliParser();

	/* Stop may arrive while ResetDecoderStatics() is running.  Re-check the
	 * shared request afterwards so the reset cannot erase an early Stop. */
	if (stopBeforeStart || gGuiPlayer.stopRequested || gPlaybackInterrupted ||
		(pending & SIGBREAKF_CTRL_C)) {
		gPlaybackInterrupted = 1;
#ifdef MINIAMP3_DEBUG
		printf("miniamp3-debug: decoder main skipped\n");
#endif
	} else {
		ranDecoder = 1;
		gGuiPlaybackStatus.startupStage = GUISTART_STREAM_INIT;
		gMiniAmp3EmbeddedPlayback = 1;
		HelixAmp3CliMain(gGuiPlayer.argc, gGuiPlayer.argv);
		gGuiPlaybackStatus.startupStage = GUISTART_CLEANUP;
	}
	if (!ranDecoder) {
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
		gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
		gGuiPlaybackStatus.cleanupComplete = 1;
	} else {
		if (!gGuiPlaybackStatus.cleanupComplete) {
			gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
			gGuiPlaybackStatus.cleanupComplete = 1;
		}
		gMiniAmp3EmbeddedPlayback = 0;
	}

	/* Only the GUI task owns the public process/lifecycle fields.  Publish a
	 * completion message and let HandleDoneSignal() clear them after it has
	 * actually received that message.
	 * Re-assert the node type immediately before PutMsg: StartPlayback()
	 * reinitialises gDoneMsg before launching, but guard here as well in case
	 * any future code path reaches PutMsg without going through StartPlayback. */
	gDoneRunId = gGuiPlaybackStatus.runId;
	donePort = gDonePort;
	if (donePort) {
		gDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
		PutMsg(donePort, &gDoneMsg);
#ifdef MINIAMP3_DEBUG
		printf("miniamp3-debug: done message posted\n");
#endif
	}
}

static void StartPlayback(HelixAmp3Gui *gui)
{
	BPTR dirLock;
	BPTR nilOut;
	struct Process *thisProc;

	if (Radio_IsMemoryPoisoned()) {
		SetStatus(gui, "Memory corruption detected; restart MiniAMP3 before playing radio.");
		RADIO_DBG(printf("radio-memory: refusing StartPlayback after MiniMem/ring corruption url=\"%s\"\n", gui->inputName);)
		return;
	}
	if (!gui->inputName[0]) {
		SetStatus(gui, "Browse to an audio file first.");
		return;
	}
	if (!radio_validate_ready_to_play(gui)) {
		SetStatus(gui, "Cannot start: previous stream still stopping");
		radio_debug_state_summary(gui, "start-blocked");
		return;
	}
	/* A stopped playback task can still be unwinding audio.device buffers for a
	 * short time after the GUI state has been cleared.  Starting a new decoder
	 * while the old task is still closing the Paula channels is most visible
	 * after changing the requested output rate: the new child can block before
	 * publishing its first buffering/playing status, leaving the window stuck on
	 * "Streaming playback started.".  Treat the task name as the final arbiter
	 * and require the old child to disappear before launching another one. */
	if (PlaybackProcessStillExists()) {
		SetStatus(gui, "Previous playback process is still exiting.");
		return;
	}
	if (!gui->donePort) {
		SetStatus(gui, "Cannot start playback: no done port.");
		return;
	}
	/* Drain any stale done message from a previous cycle before launching.
	 * gDoneMsg is a single static Exec message node, so it must not remain
	 * queued when the next playback subprocess exits and posts it again.
	 * Re-initialise the node fields here: some AmigaOS exec implementations
	 * write NT_FREEMSG (0) into ln_Type when a message is removed from a port
	 * via GetMsg(), which would cause PutMsg() to silently mishandle the node
	 * on the second and subsequent play cycles, leaving the GUI permanently
	 * stuck on "Streaming playback started." */
	{
		struct Message *stale;

		while ((stale = GetMsg(gui->donePort)) != NULL)
			;
	}
	memset(&gDoneMsg, 0, sizeof(gDoneMsg));
	gDoneMsg.mn_Length = sizeof(gDoneMsg);
	gDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
	/* Artwork decoding belongs to the GUI task and may continue at the normal
	 * timer-pump rate while the playback child runs.  Keeping it active preserves
	 * the current cover and uses only the existing small per-tick work budget. */
	gui->elapsedSecs = 0;
	gui->lastUnderrunCount = 0;
	gui->lastDisplayedSpareMs = 0;
	gui->lastDisplayedPhase = GUIPLAY_PHASE_IDLE;
	gui->lastDrawnElapsedSecs = -1;
	gui->lastDrawnTotalSecs = -1;
	/* Zero the IPC block so stale data from a previous run is not visible
	 * before the new subprocess writes its first update. */
	memset((void *)&gGuiPlaybackStatus, 0, sizeof(gGuiPlaybackStatus));
	gui->playbackRunId = ++gPlaybackRunCounter;
	gui->playbackDoneRunId = 0;
	gui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
	gui->lastStartupStage = GUISTART_NONE;
	gui->startupStageStableTicks = 0;
	gui->startupStallShown = 0;
	gGuiPlaybackStatus.runId = gui->playbackRunId;
	gPlaybackEntryRunId = gui->playbackRunId;
	gui->launchBufferSecs = gui->decodeThenPlay ? 0 : gui->bufferSeconds;
	DrawProgress(gui);
	if (IsRadioInputName(gui->inputName))
		GuiDisableFastMemForRadio(gui);
	else
		GuiDisableFastMemIfTooSmall(gui);
	BuildPlaybackArgs(gui, &gGuiArgs);
#ifdef MINIAMP3_DEBUG
	DebugSelftestPlaybackChannelArgs(gui);
	DebugPrintPlaybackArgs("BuildPlaybackArgs selected", &gGuiArgs);
#endif
	gGuiPlayer.argc = gGuiArgs.argc;
	gGuiPlayer.argv = gGuiArgs.argv;
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gGuiFirstUiProgressLogged = 0;
	gDonePort = gui->donePort;
	gDoneRunId = 0;

	/* Give each playback process its own current-directory lock so relative
	 * paths remain resolvable across Stop/Play cycles.  DupLock(NULL) is safe
	 * and keeps the child behavior unchanged when no current directory exists.
	 */
	thisProc = (struct Process *)FindTask(NULL);
	dirLock = DupLock(thisProc ? thisProc->pr_CurrentDir : (BPTR)0);
#ifndef MINIAMP3_DEBUG
	nilOut = SafeOpenPath("StartPlayback/OpenNIL", "NIL:", MODE_NEWFILE);
#else
	nilOut = (BPTR)0;
#endif

	if (IsRadioInputName(gui->inputName)) {
		static char radioWorkerArgs[2048];
		static char radioWorkerCommand[2300];
		GuiBuildRadioWorkerArgs(radioWorkerArgs, sizeof(radioWorkerArgs), gGuiPlayer.argv, gGuiPlayer.argc);
		sprintf(radioWorkerCommand, "Run >NIL: amiga_mp3dec.fastexp %s", radioWorkerArgs);
		RADIO_DBG(printf("radio-worker: launching separate executable via Execute: %s\n", radioWorkerCommand);)
		if (Execute((STRPTR)radioWorkerCommand, (BPTR)0, nilOut)) {
			gGuiPlayer.process = (struct Process *)FindTask(NULL); /* non-NULL launch sentinel; worker owns network state */
			if (nilOut) { Close(nilOut); nilOut = (BPTR)0; }
			if (dirLock) { UnLock(dirLock); dirLock = (BPTR)0; }
		} else
			gGuiPlayer.process = NULL;
	} else if (nilOut) {
		gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
			NP_Name, (ULONG)"MiniAMP3 playback",
			NP_Priority, 0,
			NP_StackSize, 262144,
			NP_CurrentDir, dirLock,
			NP_Output, nilOut,
			NP_CloseOutput, TRUE,
			NP_CopyVars, FALSE,
			TAG_DONE);
	} else {
		gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
			NP_Name, (ULONG)"MiniAMP3 playback",
			NP_Priority, 0,
			NP_StackSize, 262144,
			NP_CurrentDir, dirLock,
			NP_CopyVars, FALSE,
			TAG_DONE);
	}
	if (!gGuiPlayer.process) {
		if (nilOut)
			Close(nilOut);
		if (dirLock)
			UnLock(dirLock);
		radio_reset_playback_state_after_stop(gui, "start-failed");
		SetStatus(gui, "Cannot start playback process.");
		return;
	}
	gui->playbackDonePending = 0;
	gui->playbackStoppedByUser = 0;
	gui->playbackActive = 1;
	if (IsRadioInputName(gui->inputName)) {
		char status[160];
		sprintf(status, "Buffering - %.140s", gui->currentRadioStationName[0] ? gui->currentRadioStationName : "Internet Radio");
		SetStatus(gui, status);
		RadioSetStatus(gui, status);
	} else
		SetStatus(gui, gui->decodeThenPlay ? "Buffering..." : "Starting playback...");
}

static void StopPlayback(HelixAmp3Gui *gui)
{
	if (!gui->playbackActive) {
		SetStatus(gui, "Nothing is playing.");
		return;
	}
	/* If the subprocess already exited but the done message has not been
	 * processed yet (race between subprocess exit and GUI event loop),
	 * handle it now to avoid signalling a stale/dead process. */
	if (!gGuiPlayer.process) {
		HandleDoneSignal(gui);
		return;
	}
	if (gGuiPlayer.stopRequested) {
		SetStatus(gui, "Stopping...");
		return;
	}
	/* Before signalling, poll the done port: the child may have already exited
	 * (fast-fail race) and its done message arrived before we got here.  If so,
	 * handle it now instead of signalling a stale process pointer. */
	if (gui->donePort) {
		struct Message *msg;
		int gotDone = 0;
		while ((msg = GetMsg(gui->donePort)) != NULL)
			gotDone = 1;
		if (gotDone) {
			gui->playbackDonePending = 1;
			gui->playbackStoppedByUser = 1;
			SetStatus(gui, "Stopping...");
			if (!PlaybackProcessStillExists())
				FinalizePlayback(gui);
			return;
		}
	}
	gGuiPlayer.stopRequested = 1;
	gPlaybackInterrupted = 1;
	gui->stopWatchdogMicros = 0;
	gui->stopWatchdogFired = 0;
	if (IsRadioInputName(gui->inputName)) {
		gGuiPlaybackStatus.radioStatus = (int)RADIO_STATUS_STOPPING;
		gGuiPlaybackStatus.radioActive = 0;
		gGuiPlaybackStatus.radioBufferedBytes = 0;
		RADIO_STOP_DEBUG_PRINTF(("radio-stop: GUI radio pointer cleared\n"));
	}
	/* Wake the playback subprocess immediately so it does not sit in WaitIO
	 * for the remainder of a multi-second audio buffer.  Use Forbid/FindTask/
	 * Signal/Permit so that if the child process is between exiting and
	 * RemTask during DOS cleanup, FindTask will fail and we skip the Signal
	 * safely instead of racing a task pointer that is about to go stale.
	 * (0x0100000F, the alert this project actually hits during flaky stream
	 * switches, is AN_BadFreeAddr -- "memory header not located", i.e. a bad
	 * address/size passed to FreeMem() -- not a signal-delivery alert; the
	 * Forbid/FindTask guard here is unrelated defensive hygiene, not a fix
	 * for that alert.) */
	SignalPlaybackChildCtrlC();
	SetStatus(gui, "Stopping...");
}


static void WaitForPlaybackShutdown(HelixAmp3Gui *gui)
{
	unsigned long wedgedTicks = 0;

	if (!gui->playbackActive)
		return;

	StopPlayback(gui);
	while (gui->playbackActive) {
		if (++wedgedTicks >= APP_CLOSE_WEDGED_CHILD_MAX_TICKS) {
			/* The child never observed Stop at all (not just a missed
			 * signal) -- most likely wedged inside a blocking bsdsocket/
			 * AmiSSL call. Re-signalling forever would hang the whole app
			 * on quit with no way out but a reboot, so give up and let
			 * GuiClose() proceed. This does not make the child disappear:
			 * it is a task still running in this program's own code/data
			 * segment, and abandoning it here is a real (if previously
			 * already-accepted-elsewhere-in-this-file) risk that segment
			 * gets torn down while it is still executing. It is still the
			 * better trade: the alternative is a guaranteed permanent
			 * hang, this is only a possible one. */
			RADIO_DBG(printf("app-close: giving up on wedged playback child after %lu ticks, leaking it\n", wedgedTicks);)
			break;
		}
		if (gui->donePort)
			HandleDoneSignal(gui);

		/* The done message can be consumed before the playback task has fully
		 * returned to DOS.  During application shutdown there may be no further
		 * timer or window wake-up, so poll the cleanup flags and task list here
		 * instead of letting GuiClose() delete ports/windows that the child may
		 * still reference. */
		if (gui->playbackDonePending && PlaybackCanFinalize(gui)) {
			FinalizePlayback(gui);
			break;
		}

		/* Deliberately does not require cleanupComplete -- see the comment
		 * on PlaybackCanFinalize(). */
		if (!gui->playbackDonePending &&
			gDoneRunId == gui->playbackRunId &&
			gGuiPlaybackStatus.runId == gui->playbackRunId &&
			!PlaybackProcessStillExists()) {
			gui->playbackDonePending = 1;
			gui->playbackStoppedByUser = 1;
			FinalizePlayback(gui);
			break;
		}

		gGuiPlayer.stopRequested = 1;
		gPlaybackInterrupted = 1;
		/* Skip signal if done message already received: the child may be
		 * mid-exit and its task pointer about to go stale.  Use Forbid/
		 * FindTask/Signal/Permit to close that race in all other cases.
		 * (0x0100000F is AN_BadFreeAddr -- a bad address/size passed to
		 * FreeMem() -- not a signal-delivery alert; see the note above
		 * StopPlayback()'s equivalent guard.) */
		if (!gui->playbackDonePending)
			SignalPlaybackChildCtrlC();
		Delay(1);
	}
}

static int GetSliderLevel(HelixAmp3Gui *gui, struct Gadget *gad, int fallback)
{
	ULONG level = (ULONG)fallback;

	if (gad && gui->win)
		GT_GetGadgetAttrs(gad, gui->win, NULL,
			GTSL_Level, (ULONG)&level,
			TAG_DONE);
	return (int)level;
}

static void SetGuiVolume(HelixAmp3Gui *gui, int percent, int persist,
	ULONG classValue, UWORD code)
{
	int oldPercent = gui->volumePercent;
	char text[64];

	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	gui->volumePercent = percent;
	GT_SetGadgetAttrs(gui->gadVolume, gui->win, NULL,
		GTSL_Level, gui->volumePercent, TAG_DONE);
	if (gui->volumePercent != oldPercent) {
		gMiniAmp3RequestedVolume = (unsigned short)gui->volumePercent;
		gMiniAmp3VolumeSequence++;
	}
	if (gui->volumePercent == 0)
		SetStatus(gui, "Volume muted.");
	else {
		sprintf(text, "Volume set to %d%%.", gui->volumePercent);
		SetStatus(gui, text);
	}
#ifdef MINIAMP3_DEBUG
	Printf("volume slider event class=%lu message code=%lu actual GTSL_Level=%ld shared volume=%lu sequence=%lu playback active=%s\n",
		(unsigned long)classValue, (unsigned long)code, (long)gui->volumePercent,
		(unsigned long)gMiniAmp3RequestedVolume,
		(unsigned long)gMiniAmp3VolumeSequence,
		MINIAMP3_DEBUG_FMT_PTR(gui->playbackActive ? "yes" : "no"));
#endif
	if (persist)
		SaveGuiSettings(gui);
}

static void SetGuiBuffer(HelixAmp3Gui *gui, int seconds, int persist)
{
	if (gui->playbackActive || gui->playbackDonePending) {
		GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
			GTSL_Level, gui->bufferSeconds, TAG_DONE);
		SetStatus(gui, "Stop playback before changing buffer depth.");
		return;
	}
	if (seconds < 1)
		seconds = 1;
	if (seconds > 10)
		seconds = 10;
	gui->bufferSeconds = seconds;
	GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
		GTSL_Level, gui->bufferSeconds,
		TAG_DONE);
	SetStatus(gui, "Buffer depth updated.");
	if (persist)
		SaveGuiSettings(gui);
}

static void HandleGuiAction(HelixAmp3Gui *gui, struct Gadget *gad, UWORD code,
	ULONG classValue, int persist)
{
	if (!gad)
		return;
	switch (gad->GadgetID) {
	case GID_BROWSE:
		ChooseMp3(gui);
		break;
	case GID_SPEED_MODE:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, SpeedModeIndex(gui), TAG_DONE);
			SetStatus(gui, "Stop playback before changing speed mode.");
			break;
		}
		/* code: 0=Normal, 1=Fast, 2=Superfast, 3=Ultrafast, 4=22050 Mono Ultrafast */
		gui->cd32Ultrafast = (code == 4) ? 1 : 0;
		gui->ultrafast = (code == 3) ? 1 : 0;
		gui->fastLowrate = ((code >= 1 && code <= 2) || code == 4) ? 1 : 0;
		gui->superfastLowrate = (code == 2 || code == 4) ? 1 : 0;
		if (gui->cd32Ultrafast) {
			gui->mono = 1;
			gui->rateIndex = 3;
		}
		if (gui->superfastLowrate &&
			!RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui)))
			gui->rateIndex = DefaultSuperfastRateIndex(ChannelUsesMonoCost(gui));
		if (gui->gadRate)
			GT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
				GTCY_Labels, (ULONG)kRateLabels,
				GTCY_Active, gui->rateIndex,
				TAG_DONE);
		if (gui->gadChannelMode)
			GT_SetGadgetAttrs(gui->gadChannelMode, gui->win, NULL,
				GTCY_Active, ChannelModeIndex(gui), TAG_DONE);
		SetStatus(gui, code == 4 ?
			"22050 mono ultrafast enabled (reduced taps, 12 subband cap)." :
			code == 3 ?
			"Ultrafast enabled (26 subband cap)." :
			code == 2 ? "Superfast enabled for 8287/8820/11025/22050 Hz." :
			code == 1 ? "Fast-lowrate enabled." : "Standard speed enabled.");
		SaveGuiSettings(gui);
		break;
	case GID_FAST_MEM:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCB_Checked, gui->fastMem, TAG_DONE);
			SetStatus(gui, "Stop playback before changing memory mode.");
			break;
		}
		gui->fastMem = !gui->fastMem;
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->fastMem, TAG_DONE);
		SetStatus(gui, gui->fastMem ? "Fast memory path enabled." : "Fast memory path disabled.");
		GuiDisableFastMemIfTooSmall(gui);
		SaveGuiSettings(gui);
		break;
	case GID_CHANNEL_MODE:
		if (gui->fakeStereo)
			break;
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, ChannelModeIndex(gui), TAG_DONE);
			SetStatus(gui, "Stop playback before changing channel mode.");
			break;
		}
		/* code: 0=Stereo, 1=Mono */
		gui->mono = (code == 1) ? 1 : 0;
		if (gui->superfastLowrate && !RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui)))
			gui->rateIndex = DefaultSuperfastRateIndex(ChannelUsesMonoCost(gui));
		if (gui->gadRate)
			GT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
				GTCY_Labels, (ULONG)kRateLabels,
				GTCY_Active, gui->rateIndex,
				TAG_DONE);
		SetStatus(gui, gui->mono ? "Mono output enabled." : "Stereo output enabled.");
		SaveGuiSettings(gui);
		break;
	case GID_FAKE_STEREO:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCB_Checked, gui->fakeStereo, TAG_DONE);
			SetStatus(gui, "Stop playback before changing channel mode.");
			break;
		}
		gui->fakeStereo = !gui->fakeStereo;
		if (gui->superfastLowrate &&
			!RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui)))
			gui->rateIndex = DefaultSuperfastRateIndex(ChannelUsesMonoCost(gui));
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->fakeStereo, TAG_DONE);
		UpdateChannelGadgetState(gui);
		if (gui->gadRate)
			GT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
				GTCY_Labels, (ULONG)kRateLabels,
				GTCY_Active, gui->rateIndex,
				TAG_DONE);
		SetStatus(gui, gui->fakeStereo ?
			"Fake-stereo enabled; Mono is temporarily ignored." :
			"Fake-stereo disabled; Mono selection restored.");
		SaveGuiSettings(gui);
		break;
	case GID_FAKE_STEREO_WIDTH:
		gui->fakeStereoWidthIndex = code;
		if (gui->fakeStereoWidthIndex < 0 || gui->fakeStereoWidthIndex > 4)
			gui->fakeStereoWidthIndex = 1;
		SetStatus(gui, "Fake-stereo width updated.");
		SaveGuiSettings(gui);
		break;
	case GID_FAKE_STEREO_DELAY:
		gui->fakeStereoDelayIndex = code;
		if (gui->fakeStereoDelayIndex < 0 || gui->fakeStereoDelayIndex > 4)
			gui->fakeStereoDelayIndex = 2;
		SetStatus(gui, "Fake-stereo delay updated.");
		SaveGuiSettings(gui);
		break;
	case GID_RATE:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, gui->rateIndex,
				TAG_DONE);
			SetStatus(gui, "Stop playback before changing output rate.");
			break;
		}
		gui->rateIndex = code;
		if (gui->rateIndex < 0 || gui->rateIndex > 4)
			gui->rateIndex = 2;
		if (gui->superfastLowrate &&
			!RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui))) {
			gui->superfastLowrate = 0;
			if (gui->gadSpeedMode)
				GT_SetGadgetAttrs(gui->gadSpeedMode, gui->win, NULL,
					GTCY_Active, SpeedModeIndex(gui), TAG_DONE);
			SetStatus(gui, "Selected rate uses standard playback; Superfast disabled.");
		} else {
			SetStatus(gui, "Output sample rate updated.");
		}
		SaveGuiSettings(gui);
		break;
	case GID_BUFFER:
		SetGuiBuffer(gui, GetSliderLevel(gui, gui->gadBuffer, code), persist);
		break;
	case GID_VOLUME:
		SetGuiVolume(gui, GetSliderLevel(gui, gui->gadVolume, code), persist,
			classValue, code);
		break;
	case GID_STAR1:
	case GID_STAR2:
	case GID_STAR3:
	case GID_STAR4:
	case GID_STAR5:
		SetRating(gui, (int)gad->GadgetID - GID_STAR1 + 1);
		if (WriteRatingToId3Tag(gui->inputName, gui->tags.rating))
			SetStatus(gui, "Rating written to the ID3 tag.");
		else
			SetStatus(gui, "Rating updated; no writable ID3v2 rating frame/padding.");
		break;
	case GID_QUALITY:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, gui->qualityIndex, TAG_DONE);
			SetStatus(gui, "Stop playback before changing quality.");
			break;
		}
		gui->qualityIndex = code;
		if (gui->qualityIndex < HELIXAMP3_QUALITY_MIN ||
			gui->qualityIndex > HELIXAMP3_QUALITY_MAX)
			gui->qualityIndex = 1;
		SetStatus(gui, "Quality profile updated.");
		SaveGuiSettings(gui);
		break;
	case GID_PLAY:
		if (gui->playbackActive || gui->playbackDonePending) {
			if (gui->queuedInputName[0]) {
				gui->queuedPlayPending = 1;
				StopPlayback(gui);
				SetStatus(gui, "Stopping current stream before playing selection...");
			} else {
				SetStatus(gui, "Playback is already starting or active.");
			}
			break;
		}
		/* If artwork is still decoding, pause it before the playback child is
		 * created.  Rapid Browse->Play can otherwise overlap GUI artwork work
		 * with the child task's first file reads on shared AmigaDOS/C runtime state. */
		if (gui->artDecode.active || gui->artLoading) {
			gui->artDecode.active = 0;
			gui->artRestartPending = 1;
			gui->artLoading = 1;
		}
		StartPlayback(gui);
		break;
	case GID_NEXT:
		if (gui->playlist.count == 0 || gui->playlist.current < 0) {
			SetStatus(gui, "No active playlist track to skip.");
			break;
		}
		if (gui->playlist.current + 1 >= gui->playlist.count) {
			SetStatus(gui, "Already at the last playlist track.");
			break;
		}
		if (gui->playbackActive || gui->playbackDonePending) {
			/* Stop playback; FinalizePlayback will advance to next */
			gui->playlistNextPending = 1;
			StopPlayback(gui);
		} else {
			/* Not playing — load next track immediately */
			gui->playlist.current++;
			gui->playlist.selected = gui->playlist.current;
			PlaylistLoadAndShow(gui, gui->playlist.current);
		}
		break;
	case GID_STOP:
		StopPlayback(gui);
		break;
	case GID_HARDWARE_FILTER:
		gui->hardwareFilter = !gui->hardwareFilter;
		ApplyHardwareAudioFilter(gui);
		DrawFilterButton(gui);
		SetStatus(gui, gui->hardwareFilter ?
			"Hardware filter enabled." : "Hardware filter disabled.");
		SaveGuiSettings(gui);
		break;
	case GID_PLAYLIST:
		if (gui->plWin)
			ClosePlaylistWindow(gui);
		else
			OpenPlaylistWindow(gui);
		break;
	}
}

static void GuiPoll(HelixAmp3Gui *gui)
{
	struct IntuiMessage *msg;
	ULONG classValue;
	UWORD code;
	struct Gadget *gad;

	while (gui->win && (msg = GT_GetIMsg(gui->win->UserPort)) != NULL) {
		classValue = msg->Class;
		code = msg->Code;
		gad = (struct Gadget *)msg->IAddress;
		GT_ReplyIMsg(msg);
		if (classValue == IDCMP_CLOSEWINDOW)
			gui->closeRequested = 1;
		else if (classValue == IDCMP_REFRESHWINDOW) {
			GuiRefresh(gui);
		} else if (classValue == IDCMP_MENUPICK && gui->menuStrip) {
			UWORD menuCode = code;
			while (menuCode != MENUNULL) {
				struct MenuItem *item = ItemAddress(gui->menuStrip, menuCode);
				if (item) {
					ULONG userData = (ULONG)GTMENUITEM_USERDATA(item);
					int mn = (int)(userData / 100);
					int it = (int)(userData % 100);
					if (mn == MENUNUM_PROJECT && it == ITEMNUM_QUIT)
						gui->closeRequested = 1;
					else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ABOUT)
						ShowAbout(gui);
					else if (mn == MENUNUM_PROJECT && it == ITEMNUM_STREAM)
						OpenRadioWindow(gui);
					else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_DTP)
						SetDecodeThenPlay(gui, !gui->decodeThenPlay);
					else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_BENCH) {
						gui->bench = !gui->bench;
						SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_BENCH,
							gui->bench);
						SetStatus(gui, gui->bench ?
							"Bench mode enabled." :
							"Bench mode disabled.");
						SaveGuiSettings(gui);
					} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTWORK)
						SetArtworkEnabled(gui, !gui->artEnabled);
					else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCACHE) {
						gui->artCacheEnabled = !gui->artCacheEnabled;
						SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_ARTCACHE,
							gui->artCacheEnabled);
						SetStatus(gui, gui->artCacheEnabled ?
							"Artwork cache enabled." : "Artwork cache disabled.");
						SaveGuiSettings(gui);
					} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCOLOR) {
						gui->artColorEnabled = !gui->artColorEnabled;
						SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_ARTCOLOR,
							gui->artColorEnabled);
						if (gui->artColorEnabled && gui->artValid)
							BuildArtColorPens(gui);
						else
							ReleaseArtColorPens(gui);
						DrawArtPanel(gui);
						SetStatus(gui, gui->artColorEnabled ?
							"Colour artwork pens enabled." :
							"Black and white artwork pens enabled.");
						SaveGuiSettings(gui);
					} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTREFRESH) {
						gui->artCacheBypass = 1;
						UpdateArtDisplay(gui);
						gui->artCacheBypass = 0;
						if (gui->artDecode.active)
							SendTimerRequest(gui, ART_TIMER_MICROS);
						SetStatus(gui, "Artwork refreshed.");
					} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTRELOAD) {
						if (gui->inputName[0]) {
							CancelArtDecode(gui);
							gui->artValid = 0;
							if (is_url_path(gui->inputName))
								SetInternetStreamMetadata(gui);
							else
								ReadMp3Tags(gui->inputName, &gui->tags,
									gui->artEnabled);
							gui->artCacheBypass = 1;
							UpdateArtDisplay(gui);
							gui->artCacheBypass = 0;
							if (gui->artDecode.active)
								SendTimerRequest(gui, ART_TIMER_MICROS);
						}
					} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCLEAN)
						CleanArtworkCache(gui);
					else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_PROGRESS) {
						gui->progressEnabled = !gui->progressEnabled;
						SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_PROGRESS,
							gui->progressEnabled);
						if (!gui->progressEnabled) {
							/* Blank the progress area immediately */
							if (gui->win) {
								struct RastPort *rp = gui->win->RPort;
								SetAPen(rp, gui->win->DetailPen);
								RectFill(rp, PROG_X, PROG_TOP_Y,
									PROG_X + PROG_W - 1, PROG_TOP_Y + PROG_H - 1);
							}
						} else {
							DrawProgress(gui);
						}
						SaveGuiSettings(gui);
					}
				}
				menuCode = item ? item->NextSelect : MENUNULL;
			}
		} else if (classValue == IDCMP_GADGETUP) {
			HandleGuiAction(gui, gad, code, classValue, TRUE);
			/* GadTools redraws the button face after a press, so repaint our
			 * hand-drawn transport icons once the gadget has popped back up. */
			DrawTransportIcons(gui);
			DrawFilterButton(gui);
		} else if (classValue == IDCMP_MOUSEMOVE) {
			if (gad &&
				(gad->GadgetID == GID_BUFFER ||
				gad->GadgetID == GID_VOLUME))
				HandleGuiAction(gui, gad, code, classValue, FALSE);
		}
	}
}

static struct StackSwapStruct gGuiNewStack;
static struct StackSwapStruct gGuiOldStack;
static APTR gGuiAllocatedStack;
static ULONG gGuiDetectedStackLower;
static ULONG gGuiDetectedStackUpper;
static ULONG gGuiDetectedStackSize;
static ULONG gGuiEffectiveStackSize;

static int GuiMainReal(int argc, char **argv)
{
	static HelixAmp3Gui gui;

	(void)argc;
	(void)argv;
	if (GuiOpen(&gui) != 0)
		return 1;
	while (!gui.closeRequested) {
		ULONG timerMask = gui.timerPort ? (1UL << gui.timerPort->mp_SigBit) : 0;
		ULONG doneMask = gui.donePort ? (1UL << gui.donePort->mp_SigBit) : 0;
		ULONG plMask = gui.plWin ? (1UL << gui.plWin->UserPort->mp_SigBit) : 0;
		ULONG rbMask = gui.rbWin ? (1UL << gui.rbWin->UserPort->mp_SigBit) : 0;
		ULONG sigs = Wait(HELIXAMP3_SIGMASK(&gui) | timerMask |
			doneMask | plMask | rbMask | SIGBREAKF_CTRL_C);
		if (sigs & SIGBREAKF_CTRL_C)
			gui.closeRequested = 1;
		if (doneMask && (sigs & doneMask))
			HandleDoneSignal(&gui);
		if (timerMask && (sigs & timerMask))
			HandleTimerSignal(&gui);
		HandlePlaylistPoll(&gui);
		HandleRadioWindow(&gui);
		GuiPoll(&gui);
	}
	if (gui.playbackActive)
		WaitForPlaybackShutdown(&gui);
	SaveGuiSettings(&gui);
	GuiClose(&gui);
	return 0;
}

int main(int argc, char **argv)
{
	struct Task *task = FindTask(NULL);
	int rc;

	gGuiDetectedStackLower = (ULONG)task->tc_SPLower;
	gGuiDetectedStackUpper = (ULONG)task->tc_SPUpper;
	gGuiDetectedStackSize = gGuiDetectedStackUpper - gGuiDetectedStackLower;
	gGuiEffectiveStackSize = gGuiDetectedStackSize;

	if (gGuiDetectedStackSize >= GUI_STARTUP_STACK_SIZE) {
#if defined(DEBUG) || defined(RADIO_DEBUG)
		printf("miniamp3: startup stack lower=%lu upper=%lu size=%lu, no swap needed\n",
			gGuiDetectedStackLower, gGuiDetectedStackUpper, gGuiDetectedStackSize);
#endif
		return GuiMainReal(argc, argv);
	}

	gGuiAllocatedStack = AllocMem(GUI_STARTUP_STACK_SIZE, MEMF_PUBLIC);
	if (!gGuiAllocatedStack)
		return 1;

	gGuiNewStack.stk_Lower = gGuiAllocatedStack;
	gGuiNewStack.stk_Upper = (ULONG)((UBYTE *)gGuiAllocatedStack + GUI_STARTUP_STACK_SIZE);
	gGuiNewStack.stk_Pointer = (APTR)gGuiNewStack.stk_Upper;
	gGuiEffectiveStackSize = GUI_STARTUP_STACK_SIZE;

	StackSwap(&gGuiNewStack);
	gGuiOldStack = gGuiNewStack;

#if defined(DEBUG) || defined(RADIO_DEBUG)
	printf("miniamp3: startup stack lower=%lu upper=%lu size=%lu, swapped to %lu bytes\n",
		gGuiDetectedStackLower, gGuiDetectedStackUpper, gGuiDetectedStackSize,
		gGuiEffectiveStackSize);
#endif
	rc = GuiMainReal(argc, argv);

	StackSwap(&gGuiOldStack);
	FreeMem(gGuiAllocatedStack, GUI_STARTUP_STACK_SIZE);
	gGuiAllocatedStack = NULL;
	return rc;
}

#else

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "MiniAMP3 GUI requires an AMIGA_M68K Intuition/ASL/GadTools build.\n");
	fprintf(stderr, "Use amiga_mp3dec --play --rate 11025 --buffer-seconds 10 file.mp3 on this host.\n");
	return 1;
}

#endif
