Warning: truncated output (original token count: 69740)
Total output lines: 7145

/*
 * MintAMP - Mini Internet Amiga Media Player ReAction/ClassAct frontend for the Helix fixed-point
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
#include "radio_runtime_flags.h"
#include "amiga_display_text.h"
#include "miniamp_memguard.h"

#if defined(AMIGA_M68K)

/* Pull the whole decoder/playback engine into this object, with its command
 * line entry point renamed.  Mirrors the trick used by amiga_mp3gui.c so we
 * share gGuiPlaybackStatus, gMiniAmp3EmbeddedPlayback and gPlaybackInterrupted
 * with the playback child without any extra glue. */
/* Tell amiga_mp3dec.c's main() (renamed to HelixAmp3CliMain just below, and
 * used as the per-playback-child entry point in this build) not to
 * InitSemaphore() radio_console_lock itself -- this file's own real main()
 * does that exactly once, before any child/worker task exists to race it. */
#define RADIO_CONSOLE_LOCK_INIT_ELSEWHERE 1
#define MINTAMP_EMBEDDED_FRONTEND 1
#define main HelixAmp3CliMain
#include "amiga_mp3dec.c"
#undef main
#undef MINTAMP_EMBEDDED_FRONTEND
#undef printf
#undef fprintf
#undef fputs
#undef puts
#undef putchar
#undef fflush
#undef fwrite

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <workbench/workbench.h>
#include <hardware/cia.h>
#include "picojpeg.h"
#include "lodepng.h"
#include "webpdec.h"
#include "svgdec.h"
#include "radio_stream.h"
#include "radio_browser_controller.h"
#include "radio_browser_http.h"

/* See the matching comment in amiga_mp3gui.c's GUI_ENV_PREFIX: bare name,
 * no explicit device prefix -- GVF_SAVE_VAR already constructs the
 * persistent "ENVARC:" + name path itself, and an explicit "ENVARC:" baked
 * in here doubled up into a malformed path that silently failed to persist
 * across reboots while the plain ENV: (RAM) write kept working. */
#define MR_ENV_PREFIX "MintAMP"
#define MINTAMP_VERSION "1.3.0"
#define MR_SETTINGS_VERSION 1
#define MR_RADIO_FAV_MAX 20

/* AmigaOS Version command metadata; unrelated to MR_SETTINGS_VERSION. */
static const char gMintAmpVersionTag[] __attribute__((used)) =
	"\0$VER: MintAMP " MINTAMP_VERSION " (05.09.2026)";
#if !defined(__AROS__) && !defined(MR_DISABLE_CIA_FILTER)
#define MR_ENABLE_CIA_FILTER 1
#endif


#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/getfile.h>
#include <gadgets/chooser.h>
#include <gadgets/listbrowser.h>
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
#include <proto/listbrowser.h>
#include <proto/slider.h>
#include <proto/checkbox.h>
#include <proto/fuelgauge.h>
#include <proto/string.h>
#include <proto/label.h>
#include <proto/graphics.h>
#include <proto/icon.h>

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
#define MR_STARTUP_STACK_SIZE 262144UL

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
/* SVG favicon support via svgdec.c (a small from-scratch, fixed-point
 * subset decoder -- see svgdec.h).  Disable by building with
 * -DENABLE_SVG_ARTWORK=0 and dropping svgdec.c from the Makefile's
 * source list to shrink the binary. */
#ifndef ENABLE_SVG_ARTWORK
#define ENABLE_SVG_ARTWORK 1
#endif
/* WebP favicon support via webpdec.c (a small from-scratch VP8/VP8L decoder).
 * Disable by building with -DENABLE_WEBP_ARTWORK=0 and dropping webpdec.c from
 * the Makefile's source list to shrink the binary. */
#ifndef ENABLE_WEBP_ARTWORK
#define ENABLE_WEBP_ARTWORK 1
#endif
#define MR_FAVICON_MAX_BYTES (256L * 1024L)

/* ------------------------------------------------------------------------- */
/* Recoverable-free diagnostics (main/GUI-task FreeMem alert hunt).           */
/*                                                                            */
/* The last hardware run finished without heap corruption or a crash, but     */
/* AmigaOS still raised two recoverable Exec alerts, both from ONE task:       */
/*     01000009  AN_FreeTwice    -- the same block handed to FreeMem twice     */
/*     0100000F  AN_BadFreeAddr  -- FreeMem given an address Exec's allocator  */
/*                                  has no memory header for                   */
/* The alerting task was NOT the radio net worker (that task's pointer is      */
/* logged separately by radio_stream.c), so the GUI/main application task is   */
/* the prime suspect.  A stale-but-non-NULL owner pointer freed a second time  */
/* looks identical to these two alerts, which is why guarding a free with      */
/* "if (ptr) FreeMem(ptr)" is not enough: the pointer has to be proven live.   */
/*                                                                            */
/* These numbered BEGIN/END records log FindTask(NULL) at every main-task      */
/* free site together with the pointer, its owner/type, the session or         */
/* generation it belongs to, and whether the owner field was already cleared   */
/* (cleared=1 on a BEGIN is exactly the stale-owner double-free the alert       */
/* describes).  Pairing every FreeMem/free with a BEGIN before and an END      */
/* after also brackets the exact call whose FreeMem raises the alert on the    */
/* next run, so the log's last un-paired BEGIN names the culprit.  RADIO_DEBUG- */
/* only and serialized through radio_console_lock like every other RADIO_DBG.  */
#ifdef RADIO_DEBUG
static unsigned long gMrFreeAuditSeq;

static void MrFreeAuditLog(const char *phase, const char *site,
	const char *owner, const void *ptr, unsigned long generation)
{
	RADIO_DBG_PRINTF(("free-audit[%lu] %s site=%s owner=%s task=%p ptr=%p gen=%lu cleared=%d\n",
		gMrFreeAuditSeq, phase, site ? site : "?", owner ? owner : "?",
		(void *)FindTask(NULL), ptr, generation, ptr ? 0 : 1));
}

/* Log the identity of the running task at a life-cycle checkpoint, so the log
 * ties the FindTask(NULL) pointer in the free records above to a named phase
 * (startup, event loop, artwork, stream-done, audio cleanup, shutdown). */
static void MrTaskIdentityLog(const char *phase)
{
	RADIO_DBG_PRINTF(("free-audit-task: phase=%s task=%p\n",
		phase ? phase : "?", (void *)FindTask(NULL)));
}

#define MR_FREE_BEGIN(site, owner, ptr, gen) \
	do { ++gMrFreeAuditSeq; MrFreeAuditLog("BEGIN", (site), (owner), (const void *)(ptr), (unsigned long)(gen)); } while (0)
#define MR_FREE_END(site, owner, ptr, gen) \
	MrFreeAuditLog("END", (site), (owner), (const void *)(ptr), (unsigned long)(gen))
#define MR_TASK_IDENTITY(phase) MrTaskIdentityLog((phase))
#else
#define MR_FREE_BEGIN(site, owner, ptr, gen) do { } while (0)
#define MR_FREE_END(site, owner, ptr, gen) do { } while (0)
#define MR_TASK_IDENTITY(phase) do { } while (0)
#endif

/* How often we poll the shared playback status block while a track plays.
 * Keep the heartbeat responsive, but throttle expensive text redraws below. */
#define MR_TICK_MICROS   250000UL
#define MR_METADATA_TICKS 4
#define MR_TIME_TICKS     2
/* How long Stop is allowed to sit outstanding (child signalled but never
 * confirmed gone) before the GUI gives up waiting silently and says so.
 * See the matching define/comment in amiga_mp3gui.c. */
#define STOP_WATCHDOG_TIMEOUT_MICROS (20UL * 1000000UL)
/* Bound on AppCloseShutdown()'s re-signal loop: each iteration delays 5
 * ticks (Delay(5), ~1/10s at the usual 50Hz jiffy rate), so this is roughly
 * a minute total before giving up on a wedged playback child and letting
 * the app close anyway rather than hang the whole task forever. */
#define APP_CLOSE_WEDGED_CHILD_MAX_TICKS 600

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
	GID_SUBBAND_CAP,
	GID_CHANNEL,
	GID_VOLUME,
	GID_BUFFER,
	GID_FASTMEM,
	GID_SPEED,
	GID_WIDTH,
	GID_DELAY,
	GID_PLAY,
	GID_NEXT,
	GID_STOP,
	GID_REW,
	GID_FFWD,
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
 * role.  14700 Hz (stride 3) sits between 11025 and 22050 as a middle-ground
 * rate -- see the matching entry in amiga_mp3gui.c's kRates.
 * kRates[MR_RATE_22050_INDEX] must stay "22050" -- several speed/
 * ultrafast/CD32 code paths below key off that specific rate. */
static const char * const kRates[] = {
	"8820", "11025", "14700", "22050", "28600"
};
#define MR_RATE_COUNT  ((int)(sizeof(kRates) / sizeof(kRates[0])))
#define MR_RATE_22050_INDEX 3

static const STRPTR kRateLabels[] = {
	(STRPTR)"8820 Hz",
	(STRPTR)"11025 Hz",
	(STRPTR)"14700 Hz",
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

/* Manual override for --subband-cap N -- see the matching comment in
 * amiga_mp3gui.c's kSubbandCapLabels. "Auto" (index 0) leaves whatever the
 * active fast-lowrate/ultrafast preset already picked untouched. */
static const STRPTR kSubbandCapLabels[] = {
	(STRPTR)"Auto",
	(STRPTR)"26",
	(STRPTR)"20",
	(STRPTR)"16",
	(STRPTR)"12",
	(STRPTR)"10",
	(STRPTR)"8",
	NULL
};
static const int kSubbandCapValues[] = { 0, 26, 20, 16, 12, 10, 8 };
#define SUBBAND_CAP_COUNT (sizeof(kSubbandCapValues) / sizeof(kSubbandCapValues[0]))

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
	(STRPTR)"Normal M/S",
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
#define ITEMNUM_ICONIFY   2
#define ITEMNUM_QUIT      3
#define ITEMNUM_DTP       0
#define ITEMNUM_BENCH     1
#define ITEMNUM_ARTWORK   2
#define ITEMNUM_ARTCACHE  3
#define ITEMNUM_ARTCOLOR  4
#define ITEMNUM_ARTREFRESH 5
#define ITEMNUM_ARTRELOAD  6
#define ITEMNUM_ARTCLEAN   7
#define ITEMNUM_PROGRESS   8
/* Mutually-exclusive "HTTPS stream wait" choices occupy the next block of
 * Playback items. The wait is the settle gap inserted before the next HTTPS
 * radio stream's decoder child is launched, giving the previous stream's
 * AmiSSL/socket teardown time to finish; slow links need a longer gap. */
#define ITEMNUM_HTTPSWAIT_BASE 9
#define HTTPS_WAIT_COUNT       5
#define PLAYBACK_ITEM_COUNT    (ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT)

/* Amiga Delay() ticks (50 ticks = 1 second). Index 0 keeps the historical
 * 4-tick (~80 ms) default so behaviour is unchanged unless the user opts in. */
static const int kHttpsWaitTicks[HTTPS_WAIT_COUNT] = { 4, 25, 50, 100, 200 };
static const char * const kHttpsWaitDesc[HTTPS_WAIT_COUNT] = {
	"default (~0.1s)", "0.5s", "1s", "2s", "4s"
};

static struct Menu kMenus[2];
static struct MenuItem kProjectItems[4];
static struct MenuItem kPlaybackItems[PLAYBACK_ITEM_COUNT];
static struct IntuiText kProjectText[4];
static struct IntuiText kPlaybackText[PLAYBACK_ITEM_COUNT];
static const char * const kProjectLabels[4] = {
	"About MintAMP...", "Internet Radio", "Iconify", "Quit"
};
static const char * const kPlaybackLabels[PLAYBACK_ITEM_COUNT] = {
	"Decode-then-play", "Bench mode", "Artwork", "Artwork Cache",
	"Colour Artwork", "Refresh Artwork", "Reload Art from File",
	"Clear Artwork Cache", "Progress Bar",
	"HTTPS wait: Default", "HTTPS wait: 0.5 sec", "HTTPS wait: 1 sec",
	"HTTPS wait: 2 sec", "HTTPS wait: 4 sec"
};

static void MrInitMenuStrip(void)
{
	int i;
	memset(kMenus, 0, sizeof(kMenus));
	memset(kProjectItems, 0, sizeof(kProjectItems));
	memset(kPlaybackItems, 0, sizeof(kPlaybackItems));
	memset(kProjectText, 0, sizeof(kProjectText));
	memset(kPlaybackText, 0, sizeof(kPlaybackText));
	kMenus[0].NextMenu = &kMenus[1];
	kMenus[0].LeftEdge = 0;
	kMenus[0].TopEdge = 0;
	kMenus[0].Width = 72;
	kMenus[0].Height = 10;
	kMenus[0].Flags = MENUENABLED;
	kMenus[0].MenuName = (STRPTR)"Project";
	kMenus[0].FirstItem = &kProjectItems[0];
	kMenus[1].LeftEdge = 72;
	kMenus[1].TopEdge = 0;
	kMenus[1].Width = 80;
	kMenus[1].Height = 10;
	kMenus[1].Flags = MENUENABLED;
	kMenus[1].MenuName = (STRPTR)"Playback";
	kMenus[1].FirstItem = &kPlaybackItems[0];
	for (i = 0; i < 4; i++) {
		/* Pen 0 is the screen's light/background pen, pen 1 is black on
		 * the standard 4-colour Workbench palette -- these were swapped,
		 * so JAM1 (which only uses FrontPen) drew the item text in the
		 * pale pen instead of black. */
		kProjectText[i].FrontPen = 1;
		kProjectText[i].BackPen = 0;
		kProjectText[i].DrawMode = JAM1;
		kProjectText[i].LeftEdge = 2;
		kProjectText[i].TopEdge = 1;
		kProjectText[i].IText = (STRPTR)kProjectLabels[i];
		kProjectItems[i].NextItem = (i < 3) ? &kProjectItems[i + 1] : NULL;
		kProjectItems[i].LeftEdge = 0;
		kProjectItems[i].TopEdge = i * 10;
		kProjectItems[i].Width = 152;
		kProjectItems[i].Height = 10;
		kProjectItems[i].Flags = ITEMTEXT | ITEMENABLED | HIGHCOMP;
		kProjectItems[i].ItemFill = (APTR)&kProjectText[i];
		kProjectItems[i].NextSelect = MENUNULL;
	}
	for (i = 0; i < PLAYBACK_ITEM_COUNT; i++) {
		/* Same FrontPen/BackPen swap as kProjectText above. */
		kPlaybackText[i].FrontPen = 1;
		kPlaybackText[i].BackPen = 0;
		kPlaybackText[i].DrawMode = JAM1;
		kPlaybackText[i].LeftEdge = 14;
		kPlaybackText[i].TopEdge = 1;
		kPlaybackText[i].IText = (STRPTR)kPlaybackLabels[i];
		kPlaybackItems[i].NextItem = (i < PLAYBACK_ITEM_COUNT - 1) ? &kPlaybackItems[i + 1] : NULL;
		kPlaybackItems[i].LeftEdge = 0;
		kPlaybackItems[i].TopEdge = i * 10;
		kPlaybackItems[i].Width = 200;
		kPlaybackItems[i].Height = 10;
		kPlaybackItems[i].Flags = ITEMTEXT | ITEMENABLED | HIGHCOMP;
		if (i == ITEMNUM_DTP || i == ITEMNUM_BENCH || i == ITEMNUM_ARTWORK ||
			i == ITEMNUM_ARTCACHE || i == ITEMNUM_ARTCOLOR || i == ITEMNUM_PROGRESS)
			kPlaybackItems[i].Flags |= CHECKIT | MENUTOGGLE;
		if (i >= ITEMNUM_HTTPSWAIT_BASE &&
			i < ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT) {
			/* Radio-style group: CHECKIT (no MENUTOGGLE) plus a MutualExclude
			 * mask of the other four HTTPS-wait items, so picking one clears
			 * the rest. SyncMenuChecks() also enforces the persisted choice. */
			int j;
			LONG mx = 0;
			for (j = ITEMNUM_HTTPSWAIT_BASE;
				j < ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT; j++)
				if (j != i)
					mx |= (1L << j);
			kPlaybackItems[i].Flags |= CHECKIT;
			kPlaybackItems[i].MutualExclude = mx;
		}
		kPlaybackItems[i].ItemFill = (APTR)&kPlaybackText[i];
		kPlaybackItems[i].NextSelect = MENUNULL;
	}
}


/* ------------------------------------------------------------------------- */
/* Library / class bases                                                     */
/* ------------------------------------------------------------------------- */

struct IntuitionBase *IntuitionBase;
struct Library *UtilityBase;
struct Library *AslBase;
struct Library *IconBase;
struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *GetFileBase;
struct Library *ChooserBase;
struct Library *ListBrowserBase;
struct Library *SliderBase;
struct Library *CheckBoxBase;
struct Library *FuelGaugeBase;
struct Library *StringBase;
struct Library *LabelBase;

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
#define MR_WINDOW_TITLE "MintAMP"

/* ------------------------------------------------------------------------- */
/* Application state                                                         */
/* ------------------------------------------------------------------------- */

typedef struct MrApp {
	unsigned long magic;
	Object         *winObj;
	struct Window  *win;
	struct Menu    *menuStrip;

	Object         *fileGad;
	Object         *rateGad;
	Object         *qualityGad;
	Object         *subbandCapGad;
	Object         *channelGad;
	Object         *volumeGad;
	Object         *bufferGad;
	Object         *fastMemGad;
	Object         *speedGad;
	Object         *widthGad;
	Object         *delayGad;
	Object         *playGad;
	Object         *nextGad;
	Object         *stopGad;
	Object         *rewGad;
	Object         *ffwdGad;
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

	Object         *plWinObj;
	struct Window  *plWin;
	Object         *plListGad;
	Object         *plAddGad;
	Object         *plRemoveGad;
	Object         *plClearGad;
	Object         *plPlayGad;
	Object         *plLoadGad;
	Object         *plSaveGad;
	Object         *plCloseGad;
	struct List     plList;
	struct Node    *plNodes[MR_PLAYLIST_MAX];
	char            plNames[MR_PLAYLIST_MAX][80];

	Object         *rbWinObj;
	struct Window  *rbWin;
	Object         *rbSearchGad;
	Object         *rbCodecGad;
	Object         *rbCountryGad;
	Object         *rbCountryCodeGad;
	Object         *rbSchemeGad;
	Object         *rbLimitGad;
	Object         *rbBitrateGad;
	Object         *rbListGad;
	Object         *rbStatusGad;
	Object         *rbDoSearchGad;
	Object         *rbPlayGad;
	Object         *rbAddFavGad;
	Object         *rbFavouritesGad;
	Object         *rbUpGad;
	Object         *rbDownGad;
	Object         *rbCloseGad;
	struct List     rbList;
	struct Node    *rbNodes[RB_CONTROLLER_MAX_STATIONS];
	char            rbNames[RB_CONTROLLER_MAX_STATIONS][96];
	int             rbVisibleToController[RB_CONTROLLER_MAX_STATIONS];
	int             rbVisibleCount;
	int             rbShowHttps;
	int             rbSchemeMode;
	int             hasNetwork;
	int             hasHttps;
	unsigned long   stopWatchdogMicros;
	int             stopWatchdogFired;
	int             rbCountryMode;
	int             rbShowingFavourites;
	int             rbFavouriteCount;
	int             rbSelectedFavourite;
	int             rbSearchInProgress;
	char            rbFavouriteNames[MR_RADIO_FAV_MAX][RB_MAX_NAME];
	char            rbFavouriteUrls[MR_RADIO_FAV_MAX][RB_MAX_URL];
	char            currentRadioStationName[RB_MAX_NAME];
	char            currentRadioFavicon[RB_MAX_FAVICON];
	/* Final URL the current station name/favicon belong to, so a Play/replay
	 * (which passes no station name) can tell "same stream, keep the artwork"
	 * from "a different URL, reset it". */
	char            currentRadioArtUrl[MR_MAX_PATH];
	RadioBrowserController rbController;

	struct MsgPort   *timerPort;
	struct timerequest *timerReq;
	int               timerRunning;
	struct MsgPort   *donePort;
	struct MsgPort   *appPort;

	char  inputName[MR_MAX_PATH];
	char  lastDrawer[MR_MAX_PATH];
	char  playlist[MR_PLAYLIST_MAX][MR_MAX_PATH];
	int   rateIndex;
	int   qualityIndex;
	int   subbandCapIndex;
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
	int   httpsWaitIndex;   /* index into kHttpsWaitTicks[] */
	int   artEnabled;
	int   artCacheEnabled;
	int   artColorEnabled;
	/* Random tint for the drawn no-artwork radio fallback icon.  Rolled once
	 * per station/track (keyed on inputName) so it stays stable across the
	 * many redraws a single station triggers, and changes when you tune away. */
	unsigned long artFallbackKey;
	int           artFallbackHasColor;
	unsigned char artFallbackR;
	unsigned char artFallbackG;
	unsigned char artFallbackB;
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
	int   shownWidthDisabled;
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
static void FinalizePlayback(MrApp *app);
static void RadioSetStatus(MrApp *app, const char *text);
static int SetStatusIfChanged(MrApp *app, const char *text);
static void SetStatus(MrApp *app, const char *text);
static void RadioDoProbeAndPlay(MrApp *app);
static void RadioProbeUrlAndStart(MrApp *app, const char *url, const char *stationName);
static void RadioSelectResult(MrApp *app, ULONG eventSelected);

static void SyncMenuChecks(MrApp *app);
static void SetDecodeThenPlay(MrApp *app, int enabled);

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
	printf("APP_CLOSE: %s streamState=%s playbackActive=%d playbackDonePending=%d activeChildCount=%lu gPlayer.process=%p gPlayer.task=%p gPlayer.stopRequested=%d active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld browser_probe_socket_counts=unavailable net_base=%p tls_base=%p tls_master=%p\n",
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

/* Returns 1 if the playback child is confirmed gone (safe to dispose the
 * shared browser/network/GUI objects it can touch), 0 if it is still active
 * or wedged -- the caller must then leak those objects and exit without
 * touching them rather than free/close something the child still holds a
 * reference to (Radio_NetworkShutdown() closing bsdsocket.library/AmiSSL out
 * from under a child that still owns an open socket/SSL context was seen
 * interleaving with in-flight ring/ICY writes right before an exec-heap
 * corruption). */
static int AppCloseShutdown(MrApp *app)
{
	if (!app)
		return 1;
	app->shuttingDown = 1;
	MR_TASK_IDENTITY("final-application-shutdown");
	AppCloseDebug("begin", app);
	AppCloseDebug("playback state", app);
	if (AppHasActivePlaybackChild(app)) {
		RADIO_DBG(printf("APP_CLOSE: stopping playback\n");)
		AppCloseDebug("stop active playback if needed", app);
		if (!StopPlaybackAndWait(app, 500, "Failed to stop previous stream")) {
			/* The child missed the first stop request (e.g. it was stalled
			 * inside a radio pump loop).  Keep re-asserting the shared stop
			 * flags and re-signalling CTRL_C while waiting -- a single Signal
			 * sent before the child sampled its signal mask can be consumed
			 * without effect, and the old bare Delay() wait here left the app
			 * frozen forever when that happened.  Matches the GadTools
			 * front-end's WaitForPlaybackShutdown() loop.
			 *
			 * But if the child is genuinely wedged inside a blocking call
			 * that never observes SIGBREAKF_CTRL_C at all (not just a missed
			 * signal), re-signalling forever never helps either -- give up
			 * after a bound so the app can still close instead of hanging
			 * the whole task, at the cost of leaking the wedged child (no
			 * safe way to force-kill a task stuck inside a library call). */
			int wedgedTicks;
			RADIO_DBG(printf("APP_CLOSE: waiting child\n");)
			for (wedgedTicks = 0; wedgedTicks < APP_CLOSE_WEDGED_CHILD_MAX_TICKS &&
				PlaybackProcessStillExists(); wedgedTicks++) {
				struct Task *child;
				gPlayer.stopRequested = 1;
				gPlaybackInterrupted = 1;
				Forbid();
				child = FindTask((STRPTR)"MintAMP playback");
				if (child)
					Signal(child, SIGBREAKF_CTRL_C);
				Permit();
				HandleDoneSignal(app);
				Delay(5);
			}
			if (PlaybackProcessStillExists()) {
				RADIO_DBG(printf("app-close: giving up on wedged playback child after %d ticks, leaking it\n", wedgedTicks);)
				RADIO_DBG(printf("APP_CLOSE: child still active after timeout -- leaking shared browser/network/GUI objects, skipping disposal\n");)
				return 0;
			}
			HandleDoneSignal(app);
		}
	} else {
		AppCloseDebug("playback already idle, skip stop", app);
		AppCloseDebug("playback idle, no stop needed", app);
	}
	RADIO_DBG(printf("APP_CLOSE: child done\n");)
	/* HandleDoneSignal() -> FinalizePlayback() should already have run by
	 * now (that's what clears playbackActive/playbackDonePending in the
	 * loops above); this is a belt-and-suspenders catch for the case where
	 * the child task exited (PlaybackProcessStillExists() false) without the
	 * done message ever being drained here, so a wedged/lost message cannot
	 * leave playbackActive stuck true and this function still returns "safe
	 * to dispose" regardless. */
	if (app->playbackActive || app->playbackDonePending) {
		HandleDoneSignal(app);
		if (app->playbackActive || app->playbackDonePending)
			FinalizePlayback(app);
	}
	RADIO_DBG(printf("APP_CLOSE: finalize done\n");)
	/* Let audio.device/DOS settle after the child task's exit before this
	 * task starts touching the shared objects it used. */
	Delay(10);
	RADIO_DBG(printf("APP_CLOSE: now disposing GUI/network\n");)
	AppCloseDebug("dispose radio browser controller", app);
	AppCloseDebug("free favourites/search results", app);
	AppCloseDebug("dispose GUI objects", app);
	return 1;
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
		/* Mono-only: drop any stale saved fake-stereo so the greyed Mode/width
		 * chooser comes up parked on "Normal M/S" rather than a fake-stereo
		 * width the output can never actually use. */
		app->mono = 1;
		app->fakeStereo = 0;
		app->rateIndex = MR_RATE_22050_INDEX;
	}
	app->bufferSeconds = LoadEnvInt("BufferSeconds", app->bufferSeconds, 1, 10);
	app->volumePercent = LoadEnvInt("Volume", app->volumePercent, 0, 100);
	app->qualityIndex = LoadEnvInt("QualityIndex", app->qualityIndex, 0, 3);
	app->subbandCapIndex = LoadEnvInt("SubbandCapIndex", app->subbandCapIndex, 0, SUBBAND_CAP_COUNT - 1);
	app->decodeThenPlay = LoadEnvInt("DecodeThenPlay", app->decodeThenPlay, 0, 1);
	app->httpsWaitIndex = LoadEnvInt("HttpsWaitIndex", app->httpsWaitIndex, 0, HTTPS_WAIT_COUNT - 1);
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
	SaveEnvInt("SubbandCapIndex", app->subbandCapIndex);
	SaveEnvInt("SettingsVersion", MR_SETTINGS_VERSION);
	SaveEnvInt("DecodeThenPlay", app->decodeThenPlay);
	SaveEnvInt("HttpsWaitIndex", app->httpsWaitIndex);
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
	char raw[256];
	if (!dst || dstSize == 0) return;
	if (!src) { dst[0] = 0; return; }
	for (i = 0; i + 1 < sizeof(raw) && src[i]; i++) raw[i] = (char)src[i];
	raw[i] = 0;
	AmigaUtf8ToDisplay(dst, dstSize, raw);
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
		/* "22050 Mono Ultrafast" only exists at 22050 Hz; leaving that rate
		 * must fall back to plain "Ultrafast" (index 2, same as picking it
		 * by hand -- see the v==2 case in SyncFromGadgets()), not "Normal". */
		app->cd32Ultrafast = 0;
		app->ultrafast = 1;
		app->superfastLowrate = 0;
		app->fastLowrate = 0;
	}
	if (app->win && app->speedGad)
		SetGadgetAttrs((struct Gadget *)app->speedGad, app->win, NULL,
			CHOOSER_LabelArray, (ULONG)(app->rateIndex == MR_RATE_22050_INDEX ? kSpeedLabels : kSpeedLabelsNo22050),
			CHOOSER_Selected, (ULONG)SpeedChoiceFromApp(app),
			TAG_DONE);
}

static void UpdateChannelGadgetState(MrApp *app)
{
	/* The output channel count is forced -- so the Mono/Stereo chooser must be
	 * greyed -- whenever fake stereo is engaged or 22050 Mono Ultrafast
	 * (cd32Ultrafast) is selected.  The latter is mono-only, so the fake-stereo
	 * Mode/width and Delay choosers are meaningless there and get greyed too. */
	int channelDisabled = (app->fakeStereo || app->cd32Ultrafast) ? TRUE : FALSE;
	int widthDisabled = app->cd32Ultrafast ? TRUE : FALSE;
	if (channelDisabled != app->shownChannelDisabled) {
		app->shownChannelDisabled = channelDisabled;
		if (app->win && app->channelGad)
			SetGadgetAttrs((struct Gadget *)app->channelGad, app->win, NULL,
				GA_Disabled, (ULONG)channelDisabled, TAG_DONE);
	}
	if (widthDisabled != app->shownWidthDisabled) {
		app->shownWidthDisabled = widthDisabled;
		if (app->win && app->widthGad)
			SetGadgetAttrs((struct Gadget *)app->widthGad, app->win, NULL,
				GA_Disabled, (ULONG)widthDisabled, TAG_DONE);
		if (app->win && app->delayGad)
			SetGadgetAttrs((struct Gadget *)app->delayGad, app->win, NULL,
				GA_Disabled, (ULONG)widthDisabled, TAG_DONE);
	}
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
	AddArg(args, "MintAMP");
	AddArg(args, "--play");
	if (isRadio) {
		AddArg(args, "--radio-stream");
		if (app->haveRadioHostAddr) {
			AddArg(args, "--radio-host-addr-be");
			sprintf(num, "%lu", app->radioHostAddrBe);
			AddArg(args, num);
		}
		if (!app->rbShowingFavourites && app->rbController.selected_index >= 0) {
			const RadioBrowserStation *st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
			if (st && st->codec[0]) {
				AddArg(args, "--radio-codec-hint");
				AddArg(args, st->codec);
			}
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
	/* The ASM polyphase (--exp-poly) and reduced-tap dewindowing
	 * (--exp-reduced-taps) are no longer toggled from the GUI: the Quality
	 * level (--quality below) selects them via ApplyQualityOptions() in
	 * amiga_mp3dec.c ("Faster" enables both, plus ASM Huffman and quarter-rate
	 * FDCT32).  CD32 Ultrafast still adds its own --exp-reduced-taps above as
	 * part of its fixed preset. */
	/* Manual subband cap always comes last so it overrides whatever default
	 * a fast-lowrate/ultrafast preset above already picked (e.g. CD32
	 * Ultrafast's hardcoded --subband-cap 12) -- --subband-cap N just does
	 * a plain last-flag-wins atoi() assignment in amiga_mp3dec.c. */
	if (app->subbandCapIndex > 0) {
		AddArg(args, "--subband-cap");
		sprintf(num, "%d", kSubbandCapValues[app->subbandCapIndex]);
		AddArg(args, num);
	}
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
	stru…39740 tokens truncated…);
	app->rbController.max_bitrate = kRadioBitrateMax[ClampInt((int)v, 0, 4)];
	sprintf(filterMsg, "Search filters: name=\"%.40s\" codec=%s country=%s max bitrate=%s limit=%d", app->rbController.name[0] ? app->rbController.name : "Any", app->rbController.codec[0] ? app->rbController.codec : "Any", app->rbController.countrycode[0] ? app->rbController.countrycode : "Any", RadioBitrateFilterLabel(app->rbController.max_bitrate), app->rbController.limit);
	RadioSetStatus(app, filterMsg);
	rc = rb_controller_search(&app->rbController);
	Radio_CheckMiniMem("after radio browser JSON parse");
	app->rbSearchInProgress = 0; app->rbShowingFavourites = FALSE; RadioRefreshResults(app);
	if (rc < 0) RadioSetStatus(app, app->rbController.last_error);
	else { char msg[128]; int hidden = app->rbController.raw_station_count - app->rbVisibleCount; if (app->rbVisibleCount == 0 && app->rbController.raw_station_count > 0) sprintf(msg, "No stations found after filters"); else if (app->rbVisibleCount == 0 && app->rbController.raw_station_count == 0) sprintf(msg, "No stations found"); else sprintf(msg, "Found %d stations, showing %d playable (%d hidden)", app->rbController.raw_station_count, app->rbVisibleCount, hidden < 0 ? 0 : hidden); RadioSetStatus(app, msg); }
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
	if (!app || !app->rbWin || !app->rbListGad) return;
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

	if (!app->rbWin || !app->rbListGad) return;
	if (selected == (ULONG)~0)
		GetAttr(LISTBROWSER_Selected, app->rbListGad, &selected);
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
		SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL,
			LISTBROWSER_Selected, (ULONG)row,
			LISTBROWSER_ShowSelected, TRUE,
			LISTBROWSER_MakeVisible, (ULONG)row, TAG_DONE);
		sprintf(msg, "Selected favourite: %.120s", app->rbFavouriteNames[app->rbSelectedFavourite]);
		RadioSetStatus(app, msg);
		return;
	}
	app->rbSelectedFavourite = -1;
	if (rb_controller_set_selected(&app->rbController, (int)selected) < 0) {
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL,
		LISTBROWSER_Selected, (ULONG)row,
		LISTBROWSER_ShowSelected, TRUE,
		LISTBROWSER_MakeVisible, (ULONG)row, TAG_DONE);
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
	if (app->playbackActive || app->playbackDonePending || PlaybackProcessStillExists()) {
		/* Something is already playing -- a radio stream OR a local file.  Queue
		 * this station and stop the current playback; FinalizePlayback() drains
		 * the "radio-selection" marker and re-enters here once the child is gone,
		 * so pressing Play on a station swaps out whatever was playing instead of
		 * rejecting with "Already playing - press Stop first."  Previously this
		 * branch was gated on MrIsRadioInput(app->inputName), so a local file
		 * playing fell through to StartPlayback()'s active-playback guard. */
		SafeCopy(app->queuedStreamUrl, sizeof(app->queuedStreamUrl), "radio-selection");
		RadioSetStatus(app, "Queued stream; stopping previous stream...");
		RADIO_DBG(printf("radio-ui: queued stream while stopping old stream\n");)
		if (!gPlayer.stopRequested)
			StopPlayback(app);
		return;
	}
	if (Radio_PlaybackOwnsNetwork()) {
		RADIO_DBG(printf("radio-probe: play/probe skipped while radio playback child owns networking\n");)
		RadioSetStatus(app, "Radio playback owns networking; stop before probing another stream.");
		return;
	}
	if (app->rbShowingFavourites) {
		if (app->rbSelectedFavourite < 0 || app->rbSelectedFavourite >= app->rbFavouriteCount) {
			RadioSetStatus(app, "Select a favourite first.");
			return;
		}
		RADIO_DBG(printf("radio-ui: favourite probe requested url=\"%s\" name=\"%s\"\n",
			app->rbFavouriteUrls[app->rbSelectedFavourite], app->rbFavouriteNames[app->rbSelectedFavourite]);)
		RadioProbeUrlAndStart(app, app->rbFavouriteUrls[app->rbSelectedFavourite],
			app->rbFavouriteNames[app->rbSelectedFavourite]);
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
		RadioSetStatus(app, "HTTPS not supported in this build");
		return;
	}
	memset(&info, 0, sizeof(info));
	RadioSetStatus(app, "Connecting...");
	Radio_LogTestModeSummary();
	{
		int probeDisabled = rb_probe_stream_probe_disabled();
		RADIO_DBG(printf("radio-probe: flag check MP3_NO_STREAM_PROBE enabled=%d testEnable=%d before selected probe\n", probeDisabled, rb_probe_stream_probe_test_enabled());)
		if (!probeDisabled) {
			RADIO_DBG(printf("radio-ui: new stream probe start url=\"%s\"\n", rb_station_play_url(st));)
		}
	}
	rc = rb_controller_probe_selected(&app->rbController, &info, peek, (int)sizeof(peek), &peekLen);
	RADIO_DBG(printf("radio-ui: new stream probe result rc=%d final=\"%s\" content=\"%s\" codec=%d redirects=%d\n",
		rc, info.final_url, info.content_type, (int)info.codec, info.redirect_count);)
	if (rc < 0) {
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	if (info.codec != RB_STREAM_CODEC_MP3 && info.codec != RB_STREAM_CODEC_AAC &&
		info.codec != RB_STREAM_CODEC_OGG) {
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
		/* Settle gap before the next HTTPS stream so the previous session's
		 * AmiSSL/socket teardown can finish; user-tunable via the Playback
		 * "HTTPS wait" menu (index 0 keeps the historical ~80 ms default). */
		int waitTicks = kHttpsWaitTicks[(app->httpsWaitIndex >= 0 &&
			app->httpsWaitIndex < HTTPS_WAIT_COUNT) ? app->httpsWaitIndex : 0];
		RADIO_DBG(printf("radio-done: Delay(%d) between fully completed HTTPS sessions before starting next HTTPS URL\n", waitTicks);)
		RadioSetStatus(app, "Waiting briefly before next HTTPS stream...");
		Delay(waitTicks);
	}
#endif
	SafeCopy(app->inputName, sizeof(app->inputName), info.final_url);
	/* This station's name/favicon (set just below) belong to this URL, so a
	 * later Play/replay can recognise it and keep the artwork. */
	SafeCopy(app->currentRadioArtUrl, sizeof(app->currentRadioArtUrl), info.final_url);
	{
		int artworkDisabled = rb_probe_artwork_disabled();
		RADIO_DBG(printf("radio-art: flag check MP3_NO_ARTWORK enabled=%d testEnable=%d before favicon/artwork fetch\n", artworkDisabled, rb_probe_artwork_test_enabled());)
		if (artworkDisabled) {
			app->currentRadioFavicon[0] = '\0';
			if (radio_runtime_flag_enabled("MP3_NO_ARTWORK"))
				RADIO_DBG(printf("radio-art: skipped by MP3_NO_ARTWORK\n");)
			else
				RADIO_DBG(printf("radio-art: disabled for run after fatal TLS/artwork transport fault\n");)
		} else {
			SafeCopy(app->currentRadioFavicon, sizeof(app->currentRadioFavicon), st->favicon);
			RADIO_DBG(printf("radio-art: station favicon=\"%s\"\n", app->currentRadioFavicon);)
		}
	}
	app->haveRadioHostAddr = info.have_host_addr;
	app->radioHostAddrBe = info.host_addr_be;
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	rb_station_display_name(st, msg, (int)sizeof(msg));
	SafeCopy(app->currentRadioStationName, sizeof(app->currentRadioStationName), msg);
	sprintf(msg, "Buffering - %.140s", app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio");
	RadioSetStatus(app, msg);
	RADIO_DBG(printf("radio-ui: new stream start url=\"%s\"\n", info.final_url);)
	if (Radio_IsMemoryPoisoned()) {
		RadioSetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
		RADIO_DBG(printf("radio-memory: refusing station switch after MiniMem/ring corruption url=\"%s\"\n", info.final_url);)
		return;
	}
	StartPlayback(app);
	Radio_CheckMiniMem("after station switch");
}

static Object *RadioButton(ULONG id, const char *text)
{
	return (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, GA_Text, (ULONG)text, TAG_DONE);
}

static void CloseRadioWindow(MrApp *app)
{
	if (app->rbWinObj) {
		if (app->rbWin && app->rbListGad) {
			RADIO_DBG(printf("app-dispose: before SetGadgetAttrs detach listbrowser listObj=%p list=%p\n",
				app->rbListGad, &app->rbList);)
			SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL, LISTBROWSER_Labels, (ULONG)~0, TAG_DONE);
			RADIO_DBG(printf("app-dispose: after detach listbrowser\n");)
			Radio_CheckMiniMem("after detach radio listbrowser");
		}
		RADIO_DBG(printf("app-dispose: before RA_CloseWindow rbWinObj=%p rbWin=%p\n", app->rbWinObj, app->rbWin);)
		RA_CloseWindow(app->rbWinObj);
		app->rbWin = NULL;
		RADIO_DBG(printf("app-dispose: after RA_CloseWindow rbWinObj\n");)
		RADIO_DBG(printf("app-dispose: before DisposeObject rbWinObj=%p\n", app->rbWinObj);)
		DisposeObject(app->rbWinObj);
		app->rbWinObj = NULL;
		RADIO_DBG(printf("app-dispose: after DisposeObject rbWinObj\n");)
		Radio_CheckMiniMem("after DisposeObject rbWinObj");
	}
	if (app->rbList.lh_Head) {
		RADIO_DBG(printf("app-dispose: before FreeListBrowserList list=%p count=%ld\n",
			&app->rbList, CountListNodes(&app->rbList));)
		FreeListBrowserList(&app->rbList);
		RADIO_DBG(printf("app-dispose: after FreeListBrowserList\n");)
		Radio_CheckMiniMem("after FreeListBrowserList radio");
	}
	NewList(&app->rbList);
	app->rbSearchGad = app->rbCodecGad = app->rbCountryGad = app->rbCountryCodeGad = NULL;
	app->rbSchemeGad = app->rbLimitGad = app->rbBitrateGad = app->rbListGad = NULL;
	app->rbStatusGad = app->rbDoSearchGad = app->rbPlayGad = app->rbAddFavGad = NULL;
	app->rbFavouritesGad = app->rbUpGad = app->rbDownGad = app->rbCloseGad = NULL;
}

static void OpenRadioWindow(MrApp *app)
{
	Object *root = NULL;
	static STRPTR codecs[] = { (STRPTR)"All", (STRPTR)"MP3", (STRPTR)"AAC", (STRPTR)"AAC+", NULL };
	/* Window geometry, fitted to the actual screen.  The radio window's natural
	 * size is 540x340; on a standard PAL/NTSC Workbench screen (256/200 px tall)
	 * a 340-tall window centred with WPOS_CENTERSCREEN puts its title/drag bar
	 * ABOVE the top of the screen, so it can't be dragged.  Cap the size to the
	 * screen and anchor the top below the screen title bar so the drag bar is
	 * always reachable. */
	struct Screen *rbScreen = app->win ? app->win->WScreen : NULL;
	LONG rbScrW = rbScreen ? (LONG)rbScreen->Width : 640;
	LONG rbScrH = rbScreen ? (LONG)rbScreen->Height : 256;
	LONG rbBarH = rbScreen ? (LONG)rbScreen->BarHeight + 1 : 11;
	LONG rbWinW = 540;
	LONG rbWinH = 340;
	LONG rbWinLeft;
	LONG rbWinTop;
	if (app->rbWinObj) {
		struct Window *rbWin = NULL;

		GetAttr(WINDOW_Window, app->rbWinObj, (ULONG *)&rbWin);
		if (rbWin) {
			WindowToFront(rbWin);
			ActivateWindow(rbWin);
		}
		return;
	}
	if (!app->win || !app->hasNetwork)
		return;
	if (app->rbController.limit <= 0) { rb_controller_init(&app->rbController); app->rbShowHttps = FALSE; app->rbSchemeMode = 0; app->rbCountryMode = 0; }
	app->rbCountryMode = RadioCountryToIndex(app->rbController.countrycode); app->rbShowingFavourites = FALSE; app->rbSelectedFavourite = -1; NewList(&app->rbList);
	app->rbSearchGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_SEARCH_TEXT, GA_RelVerify, TRUE, STRINGA_TextVal, (ULONG)app->rbController.name, STRINGA_MaxChars, RB_MAX_NAME, TAG_DONE);
	app->rbCodecGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_CODEC, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)codecs, CHOOSER_Selected, RadioCodecToIndex(app->rbController.codec), TAG_DONE);
	app->rbCountryGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_COUNTRY, GA_RelVerify, TRUE, STRINGA_TextVal, (ULONG)app->rbController.countrycode, STRINGA_MaxChars, RB_MAX_COUNTRY, TAG_DONE);
	app->rbCountryCodeGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_COUNTRY_CODE, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioCountryLabels, CHOOSER_Selected, app->rbCountryMode, TAG_DONE);
	app->rbSchemeGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_SCHEME, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioSchemeLabels, CHOOSER_Selected, app->rbSchemeMode, GA_Disabled, !app->hasHttps, TAG_DONE);
	app->rbLimitGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_LIMIT, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioSearchLimitLabels, CHOOSER_Selected, RadioSearchLimitIndex(app->rbController.limit), TAG_DONE);
	app->rbBitrateGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_BITRATE, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioBitrateLabels, CHOOSER_Selected, app->rbController.max_bitrate <= 0 ? 0 : (app->rbController.max_bitrate <= 56 ? 1 : (app->rbController.max_bitrate <= 64 ? 2 : (app->rbController.max_bitrate <= 96 ? 3 : 4))), TAG_DONE);
	app->rbListGad = (Object *)NewObject(LISTBROWSER_GetClass(), NULL, GA_ID, RB_GID_RADIO_RESULTS, GA_RelVerify, TRUE, LISTBROWSER_Labels, (ULONG)&app->rbList, LISTBROWSER_Selected, (ULONG)~0, LISTBROWSER_ShowSelected, TRUE, LISTBROWSER_AutoFit, TRUE, LISTBROWSER_Separators, TRUE, TAG_DONE);
	app->rbStatusGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_STATUS, GA_ReadOnly, TRUE, STRINGA_TextVal, (ULONG)(app->lastRadioError[0] ? app->lastRadioError : "Ready."), STRINGA_MaxChars, 512, TAG_DONE);
	app->rbDoSearchGad = RadioButton(RB_GID_SEARCH, "Search"); app->rbPlayGad = RadioButton(RB_GID_PROBE, "Play"); app->rbAddFavGad = RadioButton(RB_GID_ADD_FAV, "Add Fav"); app->rbFavouritesGad = RadioButton(RB_GID_FAVOURITES, "Favourites"); app->rbUpGad = RadioButton(RB_GID_UP, "Up"); app->rbDownGad = RadioButton(RB_GID_DOWN, "Down"); app->rbCloseGad = RadioButton(RB_GID_CLOSE, "Close");
	if (!app->rbSearchGad || !app->rbCodecGad || !app->rbCountryGad || !app->rbCountryCodeGad || !app->rbSchemeGad || !app->rbLimitGad || !app->rbBitrateGad || !app->rbListGad || !app->rbStatusGad || !app->rbDoSearchGad || !app->rbPlayGad || !app->rbAddFavGad || !app->rbFavouritesGad || !app->rbUpGad || !app->rbDownGad || !app->rbCloseGad) goto fail;
	root = (Object *)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE, LAYOUT_SpaceInner, TRUE, LAYOUT_DeferLayout, TRUE,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, ADD_LABELLED(app->rbSearchGad, "Search"), ADD_LABELLED(app->rbCodecGad, "Codec"), TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, ADD_LABELLED(app->rbCountryGad, "Country"), ADD_LABELLED(app->rbCountryCodeGad, "Code"), ADD_LABELLED(app->rbSchemeGad, "URL"), TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, ADD_LABELLED(app->rbLimitGad, "Limit"), ADD_LABELLED(app->rbBitrateGad, "Max kbps"), TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)app->rbListGad, CHILD_MinHeight, 120,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_EvenSize, TRUE, LAYOUT_AddChild, (ULONG)app->rbDoSearchGad, LAYOUT_AddChild, (ULONG)app->rbPlayGad, LAYOUT_AddChild, (ULONG)app->rbAddFavGad, LAYOUT_AddChild, (ULONG)app->rbFavouritesGad, LAYOUT_AddChild, (ULONG)app->rbUpGad, LAYOUT_AddChild, (ULONG)app->rbDownGad, LAYOUT_AddChild, (ULONG)app->rbCloseGad, TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)app->rbStatusGad, CHILD_WeightedHeight, 0, TAG_DONE);
	if (!root) goto fail;
	if (rbWinW > rbScrW) rbWinW = rbScrW;
	if (rbWinH > rbScrH - rbBarH) rbWinH = rbScrH - rbBarH;
	/* Open offset from the main window, NOT centred.  The main window is itself
	 * centred, so a centred radio window lands with its title bar directly over
	 * the main window's -- the overlapping drag bars fight and the drag grabs
	 * the window behind, until the user moves the main window out from under it.
	 * Offsetting keeps the two title bars apart (the GadTools frontend already
	 * opens its radio window at main + 30 for this reason).  Clamp so the whole
	 * window still lands on-screen. */
	rbWinLeft = (app->win ? (LONG)app->win->LeftEdge : 0) + 30;
	rbWinTop  = (app->win ? (LONG)app->win->TopEdge : rbBarH) + 30;
	if (rbWinLeft + rbWinW > rbScrW) rbWinLeft = rbScrW - rbWinW;
	if (rbWinLeft < 0) rbWinLeft = 0;
	if (rbWinTop + rbWinH > rbScrH) rbWinTop = rbScrH - rbWinH;
	if (rbWinTop < rbBarH) rbWinTop = rbBarH;
	/* WA_IDCMP must include IDCMP_NEWSIZE for a size-gadget window: without it
	 * window.class never sees the resize that commits its border/drag regions,
	 * so the title bar stays inert until the user manually resizes the window
	 * (the "only draggable after I resize it down" symptom).  The main window,
	 * which lists IDCMP_NEWSIZE, drags fine.  IDCMP_VANILLAKEY lets Enter in the
	 * search string submit, matching the main window. */
	app->rbWinObj = (Object *)NewObject(WINDOW_GetClass(), NULL, WA_Title, (ULONG)"Internet Radio", WA_Activate, TRUE, WA_DepthGadget, TRUE, WA_DragBar, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE, WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_IDCMPUPDATE | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE | IDCMP_VANILLAKEY, WA_Left, rbWinLeft, WA_Top, rbWinTop, WA_Width, rbWinW, WA_Height, rbWinH, WINDOW_ParentGroup, (ULONG)root, TAG_DONE);
	if (!app->rbWinObj) goto fail; app->rbWin = (struct Window *)RA_OpenWindow(app->rbWinObj); if (!app->rbWin) goto fail; WindowToFront(app->rbWin); ActivateWindow(app->rbWin); RadioRefreshResults(app); return;
fail:
	if (!app->rbWinObj && root) DisposeObject(root); CloseRadioWindow(app);
}

static void HandleRadioWindow(MrApp *app)
{
	ULONG result; UWORD code = 0; if (!app->rbWinObj) return;
	while ((result = RA_HandleInput(app->rbWinObj, &code)) != WMHI_LASTMSG) {
		switch (result & WMHI_CLASSMASK) {
		case WMHI_CLOSEWINDOW: CloseRadioWindow(app); return;
		case WMHI_GADGETUP:
			/* Memory corruption detected: refuse every radio-browser action
			 * (search/play/add-fav/favourites/up/down/scheme/country) that
			 * could probe, fetch, or start playback against a heap already
			 * known to be damaged -- only Close is still allowed, since the
			 * user must still be able to get out of this window. No queued
			 * stream may survive this either. */
			if (Radio_IsMemoryPoisoned() && (result & WMHI_GADGETMASK) != RB_GID_CLOSE) {
				RadioSetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
				app->queuedStreamUrl[0] = '\0';
				app->playlistNextPending = 0;
				break;
			}
			switch (result & WMHI_GADGETMASK) {
			case RB_GID_SEARCH_TEXT: case RB_GID_SEARCH: RadioDoSearch(app); break;
			case RB_GID_RADIO_RESULTS: RadioSelectResult(app, (ULONG)code); break;
			case RB_GID_PROBE: RadioDoProbeAndPlay(app); break;
			case RB_GID_ADD_FAV: RadioAddFavourite(app); break;
			case RB_GID_FAVOURITES: RadioToggleFavourites(app); break;
			case RB_GID_UP: RadioMoveSelection(app, -1); break;
			case RB_GID_DOWN: RadioMoveSelection(app, 1); break;
			case RB_GID_SCHEME: { ULONG active = 0; GetAttr(CHOOSER_Selected, app->rbSchemeGad, &active); app->rbSchemeMode = ClampInt((int)active, 0, 2); app->rbShowHttps = (app->rbSchemeMode != 0); RadioRefreshResults(app); break; }
			case RB_GID_COUNTRY_CODE: { ULONG active = 0; GetAttr(CHOOSER_Selected, app->rbCountryCodeGad, &active); app->rbCountryMode = ClampInt((int)active, 0, 6); if (app->rbCountryGad) SetGadgetAttrs((struct Gadget *)app->rbCountryGad, app->rbWin, NULL, STRINGA_TextVal, (ULONG)RadioCountryFromIndex(app->rbCountryMode), TAG_DONE); break; }
			case RB_GID_CLOSE: CloseRadioWindow(app); return;
			}
			break;
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
	int sel = app->playlistSelected >= 0 ? app->playlistSelected : app->playlistCurrent;
	if (app->plWin && app->plListGad) {
		SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
			LISTBROWSER_Labels, (ULONG)~0,
			TAG_DONE);
	}
	if (app->plList.lh_Head)
		FreeListBrowserList(&app->plList);
	NewList(&app->plList);
	memset(app->plNodes, 0, sizeof(app->plNodes));
	for (i = 0; i < app->playlistCount; i++) {
		SafeCopy(app->plNames[i], sizeof(app->plNames[i]), PlaylistBaseName(app->playlist[i]));
		app->plNodes[i] = AllocListBrowserNode(1,
			LBNA_Column, 0,
				LBNCA_Text, (ULONG)app->plNames[i],
			TAG_DONE);
		if (app->plNodes[i])
			AddTail(&app->plList, app->plNodes[i]);
	}
	if (app->plWin && app->plListGad) {
		SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
			LISTBROWSER_Labels, (ULONG)&app->plList,
			LISTBROWSER_Selected, app->playlistCount > 0 && sel >= 0 ? (ULONG)sel : (ULONG)~0,
			LISTBROWSER_ShowSelected, TRUE,
			LISTBROWSER_MakeVisible, app->playlistCount > 0 && sel >= 0 ? (ULONG)sel : 0,
			GA_Disabled, app->playlistCount == 0 ? TRUE : FALSE,
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
		ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|wma|8svx|iff|svx|aif|aiff)",
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
	if (app->plWinObj) {
		if (app->plWin && app->plListGad) {
			RADIO_DBG(printf("app-dispose: before SetGadgetAttrs detach playlist listObj=%p list=%p\n",
				app->plListGad, &app->plList);)
			SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
				LISTBROWSER_Labels, (ULONG)~0,
				TAG_DONE);
			RADIO_DBG(printf("app-dispose: after detach playlist listbrowser\n");)
			Radio_CheckMiniMem("after detach playlist listbrowser");
		}
		RADIO_DBG(printf("app-dispose: before RA_CloseWindow plWinObj=%p plWin=%p\n", app->plWinObj, app->plWin);)
		RA_CloseWindow(app->plWinObj);
		app->plWin = NULL;
		RADIO_DBG(printf("app-dispose: after RA_CloseWindow plWinObj\n");)
		RADIO_DBG(printf("app-dispose: before DisposeObject playerWinObj=%p\n", app->plWinObj);)
		DisposeObject(app->plWinObj);
		app->plWinObj = NULL;
		RADIO_DBG(printf("app-dispose: after DisposeObject playerWinObj\n");)
		Radio_CheckMiniMem("after DisposeObject playerWinObj");
	}
	if (app->plList.lh_Head) {
		RADIO_DBG(printf("app-dispose: before FreeListBrowserList list=%p count=%ld\n",
			&app->plList, CountListNodes(&app->plList));)
		FreeListBrowserList(&app->plList);
		RADIO_DBG(printf("app-dispose: after FreeListBrowserList\n");)
		Radio_CheckMiniMem("after FreeListBrowserList playlist");
	}
	NewList(&app->plList);
	app->plListGad = NULL;
	app->plAddGad = NULL;
	app->plRemoveGad = NULL;
	app->plClearGad = NULL;
	app->plPlayGad = NULL;
	app->plLoadGad = NULL;
	app->plSaveGad = NULL;
	app->plCloseGad = NULL;
}

static Object *PlaylistButton(ULONG id, const char *text)
{
	return (Object *)NewObject(BUTTON_GetClass(), NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		GA_Text, (ULONG)text,
		TAG_DONE);
}

static void OpenPlaylistWindow(MrApp *app)
{
	Object *root = NULL;
	int sel;

	if (app->plWinObj || !app->win)
		return;

	RefreshPlaylistView(app);
	sel = app->playlistSelected >= 0 ? app->playlistSelected : app->playlistCurrent;
	if (sel < 0 || sel >= app->playlistCount)
		sel = 0;

	app->plListGad = (Object *)NewObject(LISTBROWSER_GetClass(), NULL,
		GA_ID, PL_GID_LIST,
		GA_RelVerify, TRUE,
		GA_Disabled, app->playlistCount == 0 ? TRUE : FALSE,
		LISTBROWSER_Labels, (ULONG)&app->plList,
		LISTBROWSER_Selected, app->playlistCount > 0 ? (ULONG)sel : (ULONG)~0,
		LISTBROWSER_MakeVisible, (ULONG)sel,
		LISTBROWSER_ShowSelected, TRUE,
		LISTBROWSER_AutoFit, TRUE,
		LISTBROWSER_Separators, TRUE,
		TAG_DONE);
	app->plAddGad = PlaylistButton(PL_GID_ADD, "Add");
	app->plRemoveGad = PlaylistButton(PL_GID_REMOVE, "Remove");
	app->plClearGad = PlaylistButton(PL_GID_CLEAR, "Clear");
	app->plPlayGad = PlaylistButton(PL_GID_PLAY, "Play");
	app->plLoadGad = PlaylistButton(PL_GID_LOAD_M3U, "Load M3U");
	app->plSaveGad = PlaylistButton(PL_GID_SAVE_M3U, "Save M3U");
	app->plCloseGad = PlaylistButton(PL_GID_CLOSE, "Close");

	if (!app->plListGad || !app->plAddGad || !app->plRemoveGad ||
		!app->plClearGad || !app->plPlayGad || !app->plLoadGad ||
		!app->plSaveGad || !app->plCloseGad)
		goto fail;

	root = (Object *)NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_SpaceInner, TRUE,
		LAYOUT_DeferLayout, TRUE,
		LAYOUT_AddChild, (ULONG)app->plListGad,
		CHILD_MinHeight, 120,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			LAYOUT_AddChild, (ULONG)app->plAddGad,
			LAYOUT_AddChild, (ULONG)app->plRemoveGad,
			LAYOUT_AddChild, (ULONG)app->plClearGad,
			LAYOUT_AddChild, (ULONG)app->plPlayGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			LAYOUT_AddChild, (ULONG)app->plLoadGad,
			LAYOUT_AddChild, (ULONG)app->plSaveGad,
			LAYOUT_AddChild, (ULONG)app->plCloseGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		TAG_DONE);
	if (!root)
		goto fail;

	app->plWinObj = (Object *)NewObject(WINDOW_GetClass(), NULL,
		WA_Title, (ULONG)"MintAMP Playlist",
		WA_Activate, TRUE,
		WA_DepthGadget, TRUE,
		WA_DragBar, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_IDCMPUPDATE |
			IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
		WA_Width, 420,
		WA_Height, 120,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_ParentGroup, (ULONG)root,
		TAG_DONE);
	if (!app->plWinObj)
		goto fail;

	app->plWin = (struct Window *)RA_OpenWindow(app->plWinObj);
	if (!app->plWin)
		goto fail;
	WindowToFront(app->plWin);
	ActivateWindow(app->plWin);
	RefreshPlaylistView(app);
	return;

fail:
	if (!app->plWinObj && root)
		DisposeObject(root);
	ClosePlaylistWindow(app);
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
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)app->win,
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
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
	ULONG result;
	UWORD code = 0;
	if (!app->plWinObj)
		return;
	while ((result = RA_HandleInput(app->plWinObj, &code)) != WMHI_LASTMSG) {
		switch (result & WMHI_CLASSMASK) {
		case WMHI_CLOSEWINDOW:
			ClosePlaylistWindow(app);
			return;
		case WMHI_GADGETUP:
			switch (result & WMHI_GADGETMASK) {
			case PL_GID_LIST:
				if (app->playlistCount > 0 && app->plListGad) {
					ULONG selected = (ULONG)code;
					if (selected == (ULONG)~0)
						GetAttr(LISTBROWSER_Selected, app->plListGad, &selected);
					if ((int)selected < app->playlistCount) {
						app->playlistSelected = (int)selected;
						SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
							LISTBROWSER_Selected, selected,
							LISTBROWSER_ShowSelected, TRUE,
							LISTBROWSER_MakeVisible, selected, TAG_DONE);
					}
				}
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
			break;
		}
	}
}


static void SetMenuItemChecked(MrApp *app, int menuNum, int itemNum, int checked)
{
	struct MenuItem *item;
	if (!app->menuStrip)
		return;
	item = ItemAddress(app->menuStrip, FULLMENUNUM(menuNum, itemNum, NOSUB));
	if (!item)
		return;
	if (checked)
		item->Flags |= CHECKED;
	else
		item->Flags &= ~CHECKED;
}

static void SyncMenuChecks(MrApp *app)
{
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_DTP,
		app->decodeThenPlay);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_BENCH, app->bench);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTWORK,
		app->artEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTCACHE,
		app->artCacheEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTCOLOR,
		app->artColorEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_PROGRESS,
		app->progressEnabled);
	{
		int i;
		for (i = 0; i < HTTPS_WAIT_COUNT; i++)
			SetMenuItemChecked(app, MENUNUM_PLAYBACK,
				ITEMNUM_HTTPSWAIT_BASE + i, app->httpsWaitIndex == i);
	}
}

static void MrIconify(MrApp *app)
{
	if (!app || !app->winObj || !app->win)
		return;
	/* Do not leave utility windows floating after the player becomes an
	 * AppIcon.  Playback/timer/done ports remain alive and audio continues. */
	CloseRadioWindow(app);
	ClosePlaylistWindow(app);
	if (RA_Iconify(app->winObj))
		app->win = NULL;
}

static void MrUniconify(MrApp *app)
{
	if (!app || !app->winObj || app->win)
		return;
	app->win = (struct Window *)RA_OpenWindow(app->winObj);
	if (!app->win)
		return;
	DrawArtPanel(app);
	WindowToFront(app->win);
	ActivateWindow(app->win);
}

static void SetDecodeThenPlay(MrApp *app, int enabled)
{
	app->decodeThenPlay = enabled ? 1 : 0;
	if (app->win && app->bufferGad) {
		SetGadgetAttrs((struct Gadget *)app->bufferGad, app->win, NULL,
			GA_Disabled, app->decodeThenPlay,
			TAG_DONE);
	}
	SyncMenuChecks(app);
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
			int mn = (int)MENUNUM(code), it = (int)ITEMNUM(code);
			if (mn == MENUNUM_PROJECT && it == ITEMNUM_QUIT) *done = 1;
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ABOUT) {
				struct EasyStruct es;
				es.es_StructSize = sizeof(es);
				es.es_Flags = 0;
				es.es_Title = (UBYTE *)"About MintAMP";
				es.es_TextFormat = (UBYTE *)"MintAMP\nMini Internet Amiga Media Player\nReAction Edition\nMade by boingball\n(C)2026 - v" MINTAMP_VERSION "\nTo support this application visit:\nhttps://buymeacoffee.com/boingball\n-----\nMade with decoders from\nHelix MP3 / AAC\nby Real Networks\nlibfoxenflac\nby astoeckel\n\nESP8266Audio\nby earlephilhower\n-----\nAI Used\nClaude and Codex\nLate Nights\nMany";
				es.es_GadgetFormat = (UBYTE *)"OK";
				EasyRequest(app->win, &es, NULL, TAG_DONE);
			}
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_RADIO)
				OpenRadioWindow(app);
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ICONIFY)
				MrIconify(app);
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
			} else if (mn == MENUNUM_PLAYBACK &&
				it >= ITEMNUM_HTTPSWAIT_BASE &&
				it < ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT) {
				char msg[64];
				app->httpsWaitIndex = it - ITEMNUM_HTTPSWAIT_BASE;
				sprintf(msg, "HTTPS stream wait set to %s.",
					kHttpsWaitDesc[app->httpsWaitIndex]);
				SetStatus(app, msg);
				SaveSettings(app);
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
	if (app->subbandCapGad && GetAttr(CHOOSER_Selected, app->subbandCapGad, &v)) {
		if ((int)v >= 0 && (int)v < (int)SUBBAND_CAP_COUNT)
			app->subbandCapIndex = (int)v;
	}
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
	if (app->speedGad && GetAttr(CHOOSER_Selected, app->speedGad, &v)) {
		app->cd32Ultrafast = (app->rateIndex == MR_RATE_22050_INDEX && (int)v == 3);
		app->ultrafast = ((int)v == 2);
		app->superfastLowrate = ((int)v == 1 || app->cd32Ultrafast);
		if (app->cd32Ultrafast) {
			app->fastLowrate = 1;
			app->mono = 1;
			app->rateIndex = MR_RATE_22050_INDEX;
			/* Reflect the forced mono output in the (about-to-be-greyed)
			 * Mono/Stereo chooser so it doesn't keep showing "Stereo". */
			if (app->win && app->channelGad)
				SetGadgetAttrs((struct Gadget *)app->channelGad, app->win, NULL,
					CHOOSER_Selected, (ULONG)1, TAG_DONE);
		} else if (app->ultrafast)
			app->fastLowrate = 0;
		else
			/* The Speed chooser is now the sole owner of fast-lowrate (the
			 * "Fast low-rate decode" checkbox was removed): Normal=off,
			 * Superfast low-rate=on. */
			app->fastLowrate = app->superfastLowrate;
	}
	if (app->widthGad && GetAttr(CHOOSER_Selected, app->widthGad, &v)) {
		if (app->cd32Ultrafast) {
			/* 22050 Mono Ultrafast is mono-only, so fake stereo can't apply:
			 * force it off and keep the greyed Mode/width chooser parked on the
			 * "Normal M/S" entry rather than leaving a stale width selected. */
			app->fakeStereo = 0;
			if (app->win && app->widthGad)
				SetGadgetAttrs((struct Gadget *)app->widthGad, app->win, NULL,
					CHOOSER_Selected, (ULONG)0, TAG_DONE);
		} else {
			app->fakeStereo = ((int)v > 0);
			app->fakeStereoWidthIndex = app->fakeStereo ? (int)v - 1 : 0;
		}
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

static struct StackSwapStruct gMrNewStack;
static struct StackSwapStruct gMrOldStack;
static APTR gMrAllocatedStack;
static ULONG gMrDetectedStackLower;
static ULONG gMrDetectedStackUpper;
static ULONG gMrDetectedStackSize;
static ULONG gMrEffectiveStackSize;

static int MrMainReal(int argc, char **argv)
{
	static MrApp app;
	ULONG winSig = 0;
	ULONG appSig = 0;
	ULONG timerSig = 0;
	ULONG doneSig = 0;
	int done = 0;

	(void)argc;
	(void)argv;

	/* Record the GUI/main application task identity up front: every free
	 * bracketed by MR_FREE_BEGIN/END below logs FindTask(NULL), and this is
	 * the pointer they must match for the recoverable AN_FreeTwice/
	 * AN_BadFreeAddr alerts to be pinned on the GUI task rather than the net
	 * worker or a playback child. */
	MR_TASK_IDENTITY("application-startup-main-task");

	/* First-run defaults (used until the user saves settings): the fastest
	 * out-of-the-box preset so RC1 plays smoothly on a stock 020/030 --
	 * 11025 Hz, mono, Ultrafast speed, "Faster" quality, decode from Fast RAM,
	 * with artwork and colour artwork on. */
	app.magic = MR_APP_MAGIC;
	app.rateIndex = 1;		/* 11025 Hz */
	app.qualityIndex = 0;		/* Faster (fastest quality preset) */
	app.mono = 1;
	app.fastMem = 1;
	app.fastLowrate = 0;
	app.ultrafast = 1;		/* Ultrafast speed preset */
	app.cd32Ultrafast = 0;
	app.volumePercent = 100;
	app.bufferSeconds = 10;
	app.fakeStereoDelayIndex = 0;
	app.artEnabled = 1;
	app.artCacheEnabled = 1;
	app.artColorEnabled = 1;
	app.progressEnabled = 0;
	app.playlistCurrent = -1;
	app.playlistSelected = -1;
	app.lastPhaseShown = -1;
	app.shownGaugeLevel = -1;
	app.shownChannelDisabled = -1;
	app.shownWidthDisabled = -1;
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
	/* Open bsdsocket.library/AmiSSL once for the whole app run; every probe,
	 * favicon fetch, browser search and stream still opens/closes its own
	 * socket and, for HTTPS, its own SSL/SSL_CTX -- only the shared libraries
	 * stay open until Radio_NetworkShutdown() at final app exit. */
	Radio_NetworkInit();
	/* Probe the result up front so the menu/gadgets below can be greyed out
	 * for offline users instead of failing on first use. */
	app.hasNetwork = Radio_HasNetwork();
	app.hasHttps = Radio_HasHttps();

	LoadSettings(&app);

	app.donePort = CreateMsgPort();
	if (!app.donePort) {
		fprintf(stderr, "MintAMP: could not create the reply port.\n");
		CloseLibs();
		return 1;
	}

	if (!OpenTimer(&app)) {
		fprintf(stderr, "MintAMP: could not open timer.device.\n");
		CloseTimer(&app);
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

	app.appPort = CreateMsgPort();
	if (!app.appPort) {
		fprintf(stderr, "MintAMP: could not create the Workbench AppPort.\n");
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
		DrainReplyMsgPortForClose(app.appPort, "open failure");
		DeleteMsgPort(app.appPort);
		app.appPort = NULL;
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

	/* The Speed chooser was built with the full label list so the layout
	 * reserved room for the longest option; now that the window is open and
	 * sized, narrow it to the rate-appropriate set (the box keeps its width).
	 * Also apply the initial greyed state for the Mono/Stereo and Mode/width
	 * choosers in case a saved 22050-Mono-Ultrafast / fake-stereo setting is
	 * in effect. */
	UpdateSpeedGadgetChoices(&app);
	UpdateChannelGadgetState(&app);

	GetAttr(WINDOW_SigMask, app.winObj, &winSig);
	appSig   = 1UL << app.appPort->mp_SigBit;
	timerSig = 1UL << app.timerPort->mp_SigBit;
	doneSig  = 1UL << app.donePort->mp_SigBit;

	/* Paint the (empty) artwork panel once now that the layout has sized the
	 * placeholder, so the recessed box is shown before the first file loads. */
	DrawArtPanel(&app);

	MR_TASK_IDENTITY("gui-event-loop");
	while (!done) {
		ULONG plSig = 0;
		ULONG rbSig = 0;
		ULONG sigs;
		if (app.plWinObj)
			GetAttr(WINDOW_SigMask, app.plWinObj, &plSig);
		if (app.rbWinObj)
			GetAttr(WINDOW_SigMask, app.rbWinObj, &rbSig);
		sigs = Wait(winSig | appSig | timerSig | doneSig | plSig | rbSig |
			SIGBREAKF_CTRL_C);

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

		if (sigs & (winSig | appSig)) {
			ULONG result;
			UWORD code = 0;
			while ((result = RA_HandleInput(app.winObj, &code)) != WMHI_LASTMSG) {
				switch (result & WMHI_CLASSMASK) {
				case WMHI_CLOSEWINDOW:
					done = 1;
					break;
				case WMHI_ICONIFY:
					MrIconify(&app);
					break;
				case WMHI_UNICONIFY:
					MrUniconify(&app);
					GetAttr(WINDOW_SigMask, app.winObj, &winSig);
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
						if (MrIsRadioInput(app.inputName))
							RadioProbeUrlAndStart(&app, app.inputName, NULL);
						else
							StartPlayback(&app);
						break;
					case GID_NEXT:
						SyncFromGadgets(&app);
						PlaylistNext(&app);
						break;
					case GID_STOP:
						StopPlayback(&app);
						break;
					case GID_REW:
						MrSeekRelative(&app, -MR_SEEK_STEP_SECS);
						break;
					case GID_FFWD:
						MrSeekRelative(&app, MR_SEEK_STEP_SECS);
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

	/* Ordered, idempotent app-close teardown. Only proceed to dispose the
	 * GUI/browser/network objects the playback child can touch if it is
	 * confirmed gone -- a wedged child means those objects are leaked
	 * (and CloseLibs()/Radio_NetworkShutdown() skipped) rather than freed
	 * or closed out from under a task that might still reference them. */
	if (!AppCloseShutdown(&app)) {
		RADIO_DBG(printf("app-close: wedged playback child, exiting without disposing GUI/network objects\n");)
		return 0;
	}
	if (Radio_IsMemoryPoisoned()) {
		/* Corrupt exec heap: SaveSettings()/MrCloseWindow()'s DisposeObject()/
		 * Radio_NetworkShutdown()/CloseLibs() all allocate or free memory
		 * (GadTools teardown, bsdsocket.library/AmiSSL closure) that could
		 * write through the same already-damaged allocator state that
		 * poisoned the session. Skip every further disposal step and exit
		 * as directly as possible -- a leak here is recoverable with a
		 * reboot; another corrupting free is not. */
		RADIO_DBG(printf("APP_CLOSE: memory corruption detected -- skipping SaveSettings/GUI dispose/Radio_NetworkShutdown/CloseLibs, exiting directly\n");)
		return 0;
	}

	SyncFromGadgets(&app);
	SaveSettings(&app);
	RADIO_DBG(printf("app-close: SaveSettings done\n");)
	DrainGuiPorts(&app, "before dispose");
	DisposeGuiObjectsBeforeCloseLibs(&app);
	DrainGuiPorts(&app, "after DisposeGuiObjectsBeforeCloseLibs");
	Delay(2);
	DrainGuiPorts(&app, "after dispose delay");
	RADIO_DBG(printf("app-close: GUI dispose/drain done\n");)
	/* Every playback child has been stopped and reaped above, so it is now safe
	 * to release the shared network libraries (AmiSSL master + bsdsocket.library)
	 * that the probe/search/streams opened.  Without this the app left
	 * bsdsocket.library open on exit and the next launch could not open a working
	 * socket ("Search failed" with the network otherwise up). */
	AppCloseDebug("close network base fallback", &app);
	AppCloseDebug("AmiSSL shutdown fallback", &app);
	Radio_NetworkShutdown();
	RADIO_DBG(printf("app-close: Radio_NetworkShutdown done\n");)
	AppCloseDebug("end", &app);
	PreCloseLibsAudit(&app);
	CloseLibs();
	PostCloseLibsAudit(&app, 1);
	RADIO_DBG(printf("app-close: CloseLibs done, returning from main\n");)
	Radio_CheckMiniMem("before app exit");
	MiniMem_ReportLeaks();
	return 0;
}


#if defined(AMIGA_M68K)
extern void LibnixFreeAllCompat_Install(void);
#endif

int main(int argc, char **argv)
{
	struct Task *task = FindTask(NULL);
	int rc;

	/*
	 * Workbench launches have no normal Shell console. Route incidental
	 * stdout/stderr output to NIL: before startup code can print or flush.
	 */
	if (argc == 0) {
		(void)freopen("NIL:", "w", stdout);
		(void)freopen("NIL:", "w", stderr);
	}

	/* True program entry point: init the cross-task stdout lock exactly
	 * once here, before any playback child or the net worker task can be
	 * spawned. See amiga_mp3dec.c's radio_console_lock definition and
	 * RADIO_CONSOLE_LOCK_INIT_ELSEWHERE above. */
	InitSemaphore(&radio_console_lock);
#if defined(AMIGA_M68K)
	LibnixFreeAllCompat_Install();
#endif


	gMrDetectedStackLower = (ULONG)task->tc_SPLower;
	gMrDetectedStackUpper = (ULONG)task->tc_SPUpper;
	gMrDetectedStackSize = gMrDetectedStackUpper - gMrDetectedStackLower;
	gMrEffectiveStackSize = gMrDetectedStackSize;

	if (gMrDetectedStackSize >= MR_STARTUP_STACK_SIZE) {
#if defined(DEBUG) || defined(RADIO_DEBUG)
		printf("MintAMP: startup stack lower=%lu upper=%lu size=%lu, no swap needed\n",
			gMrDetectedStackLower, gMrDetectedStackUpper, gMrDetectedStackSize);
#endif
		return MrMainReal(argc, argv);
	}

	gMrAllocatedStack = AllocMem(MR_STARTUP_STACK_SIZE, MEMF_PUBLIC);
	if (!gMrAllocatedStack)
		return 1;

	gMrNewStack.stk_Lower = gMrAllocatedStack;
	gMrNewStack.stk_Upper = (ULONG)((UBYTE *)gMrAllocatedStack + MR_STARTUP_STACK_SIZE);
	gMrNewStack.stk_Pointer = (APTR)gMrNewStack.stk_Upper;
	gMrEffectiveStackSize = MR_STARTUP_STACK_SIZE;

	StackSwap(&gMrNewStack);
	gMrOldStack = gMrNewStack;

#if defined(DEBUG) || defined(RADIO_DEBUG)
	printf("MintAMP: startup stack lower=%lu upper=%lu size=%lu, swapped to %lu bytes\n",
		gMrDetectedStackLower, gMrDetectedStackUpper, gMrDetectedStackSize,
		gMrEffectiveStackSize);
#endif
	rc = MrMainReal(argc, argv);

	StackSwap(&gMrOldStack);
	MR_TASK_IDENTITY("shutdown-free-startup-stack");
	MR_FREE_BEGIN("MrMain", "startup-stack", gMrAllocatedStack, MR_STARTUP_STACK_SIZE);
	FreeMem(gMrAllocatedStack, MR_STARTUP_STACK_SIZE);
	gMrAllocatedStack = NULL;
	MR_FREE_END("MrMain", "startup-stack", gMrAllocatedStack, 0);
	return rc;
}

#else	/* !AMIGA_M68K */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr,
		"MintAMP is an AmigaOS ReAction/ClassAct frontend and needs an "
		"AMIGA_M68K build.\n");
	return 1;
}

#endif
