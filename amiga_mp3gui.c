Warning: truncated output (original token count: 75873)
Total output lines: 9237

/*
 * MintAMP-GT - Mini Internet Amiga Media Player GadTools frontend for the Helix
 * fixed-point MP3 decoder.  The GUI wraps the existing amiga_mp3dec playback
 * frontend so the same Paula streaming path, fast-lowrate options, and buffer
 * handling are used from either Shell or Workbench.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniamp_memguard.h"
#include "amiga_display_text.h"
#include "radio_runtime_flags.h"

#if defined(AMIGA_M68K)
/* amiga_mp3dec.c InitSemaphore()s the shared cross-task stdout lock
 * radio_console_lock inside its main() -- but only when
 * RADIO_CONSOLE_LOCK_INIT_ELSEWHERE is NOT defined.  Here that main() is
 * renamed to HelixAmp3CliMain and becomes the per-playback-child entry point,
 * so letting it run the init would (a) leave the lock uninitialised until the
 * first playback child is spawned, even though the GUI task and the radio net
 * worker obtain it much earlier (the very first Internet Radio search prints
 * through it), and (b) re-run InitSemaphore() on every child spawn, racing any
 * task already holding it.  An ObtainSemaphore() on a still-zeroed
 * SignalSemaphore blocks forever, which is exactly the "search hangs right
 * after the filter status line appears" freeze.  Define the macro to skip that
 * block and instead InitSemaphore() once in this file's own main() below,
 * mirroring minimp3r.c. */
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
	volatile unsigned long fastInputBytes;
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
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <proto/timer.h>
#include <proto/icon.h>
#include <proto/wb.h>
/* #include <graphics/colormap.h> */
#include "picojpeg.h"
#include "lodepng.h"
#include "webpdec.h"
#include "svgdec.h"
#include "radio_stream.h"
#include "radio_browser_controller.h"
#ifndef OBP_FailIfBad
#define OBP_FailIfBad (TAG_USER + 0x01L)
#endif

/* ------------------------------------------------------------------------- */
/* Recoverable-free diagnostics (GadTools main/GUI-task FreeMem alert hunt).   */
/*                                                                            */
/* Hardware runs of the GadTools front end raise recoverable Exec alerts on    */
/* the second HTTPS stream:                                                    */
/*     01000009  AN_FreeTwice    -- the same block handed to FreeMem twice     */
/*     0100000F  AN_BadFreeAddr  -- FreeMem given an address Exec has no        */
/*                                  memory header for                          */
/* The free list stays intact (radio-memcheck reports OK), so this is a        */
/* stale/double free of a single block -- freeing a pointer whose owner field  */
/* was left non-NULL -- not general heap corruption.  minimp3r.c gained the    */
/* same BEGIN/END free-audit and "clear the owner the instant you free it"     */
/* discipline in commit a856b46 ("Finding the lost FreeMem closing alerts");   */
/* it was never ported here.  These records log FindTask(NULL) at every        */
/* GUI-task free site with the pointer, owner and generation, and bracket the  */
/* exact call whose FreeMem raises the alert on the next run: the log's last    */
/* un-paired BEGIN names the culprit.  RADIO_DEBUG-only, serialised through     */
/* radio_console_lock like every other RADIO_DBG. */
#ifdef RADIO_DEBUG
static unsigned long gGuiFreeAuditSeq;

static void GuiFreeAuditLog(const char *phase, const char *site,
	const char *owner, const void *ptr, unsigned long generation)
{
	RADIO_DBG(printf("free-audit[%lu] %s site=%s owner=%s task=%p ptr=%p gen=%lu cleared=%d\n",
		gGuiFreeAuditSeq, phase, site ? site : "?", owner ? owner : "?",
		(void *)FindTask(NULL), ptr, generation, ptr ? 0 : 1);)
}

static void GuiTaskIdentityLog(const char *phase)
{
	RADIO_DBG(printf("free-audit-task: phase=%s task=%p\n",
		phase ? phase : "?", (void *)FindTask(NULL));)
}

#define GUI_FREE_BEGIN(site, owner, ptr, gen) \
	do { ++gGuiFreeAuditSeq; GuiFreeAuditLog("BEGIN", (site), (owner), (const void *)(ptr), (unsigned long)(gen)); } while (0)
#define GUI_FREE_END(site, owner, ptr, gen) \
	GuiFreeAuditLog("END", (site), (owner), (const void *)(ptr), (unsigned long)(gen))
#define GUI_TASK_IDENTITY(phase) GuiTaskIdentityLog((phase))
#else
#define GUI_FREE_BEGIN(site, owner, ptr, gen) do { } while (0)
#define GUI_FREE_END(site, owner, ptr, gen) do { } while (0)
#define GUI_TASK_IDENTITY(phase) do { } while (0)
#endif

#define HELIXAMP3_MAX_PATH 256
#define HELIXAMP3_ARGC_MAX 28
#define MINTAMP_GT_VERSION "1.3.0"
#define HELIXAMP3_SETTINGS_VERSION 2
#define HELIXAMP3_RADIO_FAV_MAX 20
#define HELIXAMP3_QUALITY_MIN 0
#define HELIXAMP3_QUALITY_MAX 3
#define HELIXAMP3_SIGMASK(gui) (1UL << (gui)->win->UserPort->mp_SigBit)

/* AmigaOS Version command metadata.  Keep this independent of the settings
 * schema version above: release numbering does not imply a settings migration. */
static const char gMintAmpGtVersionTag[] __attribute__((used)) =
	"\0$VER: MintAMP-GT " MINTAMP_GT_VERSION " (05.09.2026)";
/* Bare name, no explicit "ENV:"/"ENVARC:" device prefix -- SaveEnvString()
 * below passes this through SetVar() with GVF_GLOBAL_ONLY (writes ENV:) and
 * separately with GVF_SAVE_VAR (which internally constructs the persistent
 * disk path as "ENVARC:" + name). An explicit "ENVARC:" baked into the name
 * itself made that second call target "ENVARC:ENVARC:MintAMP/..." -- a
 * malformed, double-prefixed path that silently failed to persist, while
 * the plain ENV: (RAM, cleared on reboot) write still succeeded -- exactly
 * the "settings don't survive a restart" symptom this fixes. */
#define GUI_ENV_PREFIX  "MintAMP"
#define GUI_STARTUP_STACK_SIZE 262144UL

#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       (ROW_FILEINFO + GUI_GADGET_HEIGHT + 10) /* two more pixels clear File info from the bottom border */

#define GUI_MARGIN           6
#define GUI_ROW_HEIGHT       16
#define GUI_ROW_GAP          1
#define GUI_SECTION_GAP      2
#define GUI_LABEL_HEIGHT     8
#define GUI_GADGET_HEIGHT    14
#define GUI_CONTROL_GAP      7
#define GUI_TOP_Y           18     /* leave breathing room below the title bar */
#define GUI_LABEL_WIDTH     78     /* wide enough for the longest left label ("File info:") in topaz 8 */
#define GUI_LABEL_GAP        6
#define GUI_FIELD_X         (GUI_MARGIN + GUI_LABEL_WIDTH + GUI_LABEL_GAP)
#define GUI_RIGHT_X         (GUI_WIN_W - GUI_MARGIN)
#define GUI_FIELD_W         (GUI_RIGHT_X - GUI_FIELD_X)
#define META_X              GUI_FIELD_X
#define FILEINFO_X          (GUI_FIELD_X + 16)   /* nudge the File info row right so its long label clears the left border */
#define FILEINFO_W          (GUI_RIGHT_X - FILEINFO_X)

#define ART_W               64
#define ART_H               64
#define ART_PAD              1
#define MAX_JPEG_DIM        1024
#define ART_FRAME_W         (ART_W + (ART_PAD * 2) + 4)
#define ART_FRAME_H         (ART_H + (ART_PAD * 2) + 4)
#define ART_FRAME_X         (GUI_RIGHT_X - ART_FRAME_W)
#define ART_FRAME_Y         GUI_TOP_Y
#define ART_X               (ART_FRAME_X + ART_PAD + 2)
#define ART_Y               (ART_FRAME_Y + ART_PAD + 2)

#define META_RIGHT          (ART_FRAME_X - GUI_SECTION_GAP)
#define META_W              (META_RIGHT - GUI_FIELD_X)
#define BROWSE_W            56
#define BROWSE_X            (META_RIGHT - BROWSE_W)
#define FILE_W              (BROWSE_X - GUI_FIELD_X - GUI_CONTROL_GAP)

#define CYCLE_W_LARGE       122
#define CHECK_W             20
#define CHECK_H             12
#define SPEED_X             GUI_FIELD_X
#define SPEED_W             190    /* holds the longest cycle label "22050 Mono Ultrafast" plus the arrow image */
#define FASTMEM_X          (SPEED_X + SPEED_W + GUI_CONTROL_GAP + 8)
#define STEREO_X           GUI_FIELD_X
#define CHANNEL_MODE_W     CYCLE_W_LARGE  /* fits "Fake stereo" plus the cycle arrow */
#define WIDTH_X            (GUI_FIELD_X + 212)  /* keep fake-stereo tuning close to the output selector */
#define WIDTH_W            96     /* holds "Very wide" without spilling into the Delay label */
#define DELAY_X            (WIDTH_X + WIDTH_W + GUI_CONTROL_GAP + 60)
#define DELAY_W            56

#define RATE_X              GUI_FIELD_X
#define RATE_W              72
#define QUALITY_X           (RATE_X + RATE_W + GUI_CONTROL_GAP + 76)
#define QUALITY_W           76
#define SUBBAND_X           (QUALITY_X + QUALITY_W + GUI_CONTROL_GAP + 84)
#define SUBBAND_W           84

/* Buffer and Volume share one row as two half-width sliders. GadTools draws
 * each slider's level string ("N sec" / "NNN%") just to the right of its box,
 * so BUFFER_VALUE_W reserves room for the buffer readout before the Volume
 * label, and VOLUME_VALUE_W keeps the volume readout inside the right border. */
#define BUFFER_X            GUI_FIELD_X
#define BUFFER_W            150
#define BUFFER_VALUE_W      48      /* "10 sec" (6 chars) in topaz 8 */
#define VOLUME_LABEL_W      56      /* "Volume:" in topaz 8 */
#define BUFVOL_GAP          24      /* visible gap between "10 sec" and "Volume:" */
#define VOLUME_X            (BUFFER_X + BUFFER_W + BUFFER_VALUE_W + BUFVOL_GAP + VOLUME_LABEL_W)
#define VOLUME_W            150
#define VOLUME_VALUE_W      32      /* "100%" (4 chars) */
#define TRANSPORT_W         48
#define TRANSPORT_H         20
#define TRANSPORT_GAP       GUI_CONTROL_GAP
#define TRANSPORT_COUNT     3
#define TRANSPORT_GROUP_W   ((TRANSPORT_COUNT * TRANSPORT_W) + ((TRANSPORT_COUNT - 1) * TRANSPORT_GAP))
#define PLAY_X              ((GUI_WIN_W - TRANSPORT_GROUP_W) / 2)
#define NEXT_X              (PLAY_X + TRANSPORT_W + TRANSPORT_GAP)
#define STOP_X              (NEXT_X + TRANSPORT_W + TRANSPORT_GAP)
/* Rewind / fast-forward flank the centred Play/Next/Stop group.  They are
 * narrower than the main transport buttons and sit in the otherwise-empty
 * space either side of it. */
#define SEEK_W              40
#define REWIND_X            (PLAY_X - TRANSPORT_GAP - SEEK_W)
#define FFWD_X              (STOP_X + TRANSPORT_W + TRANSPORT_GAP)
/* Seconds jumped per FF/RW click. */
#define SEEK_STEP_SECS      10
/* Internet Radio button: sits in the empty left flank of the transport row,
 * mirroring Filter/Playlist on the right. */
#define RADIO_BTN_W         64
#define RADIO_BTN_X         GUI_MARGIN
#define FILTER_W            62
#define PL_OPEN_W           70
#define PL_OPEN_X           (GUI_RIGHT_X - PL_OPEN_W)
#define FILTER_X            (PL_OPEN_X - GUI_CONTROL_GAP - FILTER_W)

#define ROW_FILE            GUI_TOP_Y
#define ROW_TITLE           (ROW_FILE + 14)
#define ROW_ARTIST          (ROW_TITLE + 14)
#define ROW_ALBUM           (ROW_ARTIST + 14)
#define ROW_RATING          (ROW_ALBUM + 14)
#define ROW_TRACK           (ROW_RATING + 14)
#define ROW_GENRE           (ROW_TRACK + 14)
#define ROW_SPEED           (ROW_GENRE + GUI_ROW_HEIGHT + GUI_SECTION_GAP)
#define ROW_PLAYBACK        (ROW_SPEED + GUI_ROW_HEIGHT + GUI_ROW_GAP)
#define ROW_CYCLES          (ROW_PLAYBACK + GUI_ROW_HEIGHT + GUI_ROW_GAP)
#define ROW_BUFVOL          (ROW_CYCLES + GUI_ROW_HEIGHT + GUI_SECTION_GAP)
#define ROW_PROGRESS        (ROW_BUFVOL + GUI_ROW_HEIGHT + GUI_SECTION_GAP)
#define ROW_BUTTONS         (ROW_PROGRESS + 18)
#define ROW_STATUS          (ROW_BUTTONS + TRANSPORT_H + GUI_SECTION_GAP)
#define ROW_FILEINFO        (ROW_STATUS + GUI_ROW_HEIGHT + GUI_ROW_GAP)

#define PROG_X              (GUI_MARGIN + 4)   /* keep the recessed frame (drawn at PROG_X-4) off the left border */
#define TIME_W              120                /* fits "-MM:SS / MM:SS" without spilling past the right border */
#define PROG_W              (GUI_WIN_W - PROG_X - TIME_W - GUI_CONTROL_GAP - GUI_MARGIN)
#define PROG_H              8
#define PROG_TOP_Y          (ROW_PROGRESS + 4)
#define TIME_X              (PROG_X + PROG_W + GUI_CONTROL_GAP)
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
 * ReAction frontend.  PNG, JPEG and ICO favicons are decoded (dispatched
 * on magic bytes, never URL extension or Content-Type).  PNG is the most
 * common real-world favicon format, so it is decoded here too via lodepng
 * (the same decoder minimp3r uses -- lodepng.c must be listed in the
 * GadTools frontend's GUI_SOURCES, see Makefile.amiga).  Disable the
 * whole artwork feature with -DENABLE_RADIO_ARTWORK=0, or just the PNG
 * path with -DENABLE_PNG_ARTWORK=0 (and drop lodepng.c from GUI_SOURCES
 * to also shrink the binary).  Never touches the MP3/ICY stream either
 * way. */
#ifndef ENABLE_RADIO_ARTWORK
#define ENABLE_RADIO_ARTWORK 1
#endif
/* PNG favicon decode is only meaningful when the artwork feature is on;
 * force it off when artwork is compiled out so the guard below never pulls
 * in lodepng calls for a disabled feature. */
#if ENABLE_RADIO_ARTWORK
#ifndef ENABLE_PNG_ARTWORK
#define ENABLE_PNG_ARTWORK 1
#endif
#else
#undef ENABLE_PNG_ARTWORK
#define ENABLE_PNG_ARTWORK 0
#endif
/* WebP favicon decode via webpdec.c (a small from-scratch VP8/VP8L decoder);
 * WebP is now the second most common favicon format after PNG.  Disable with
 * -DENABLE_WEBP_ARTWORK=0 (and drop webpdec.c from GUI_SOURCES to shrink the
 * binary).  Gated on ENABLE_RADIO_ARTWORK like the PNG path above. */
#if ENABLE_RADIO_ARTWORK
#ifndef ENABLE_WEBP_ARTWORK
#define ENABLE_WEBP_ARTWORK 1
#endif
#else
#undef ENABLE_WEBP_ARTWORK
#define ENABLE_WEBP_ARTWORK 0
#endif
/* SVG favicon decode via svgdec.c (fixed-point subset renderer, see svgdec.h).
 * Brings the GadTools frontend to format parity with minimp3r.  Disable with
 * -DENABLE_SVG_ARTWORK=0 (and drop svgdec.c from GUI_SOURCES). */
#if ENABLE_RADIO_ARTWORK
#ifndef ENABLE_SVG_ARTWORK
#define ENABLE_SVG_ARTWORK 1
#endif
#else
#undef ENABLE_SVG_ARTWORK
#define ENABLE_SVG_ARTWORK 0
#endif
#define HELIXAMP3_FAVICON_MAX_BYTES (256L * 1024L)

#define MENUNUM_PROJECT   0
#define MENUNUM_PLAYBACK  1
#define ITEMNUM_ABOUT     0
#define ITEMNUM_STREAM    1
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

enum {
	GID_FILE = 1,
	GID_BROWSE,
	GID_TITLE,
	GID_ARTIST,
	GID_ALBUM,
	GID_SPEED_MODE,
	GID_FAST_MEM,
	GID_CHANNEL_MODE,
	GID_FAKE_STEREO_WIDTH,
	GID_FAKE_STEREO_DELAY,
	GID_RATE,
	GID_BUFFER,
	GID_VOLUME,
	GID_QUALITY,
	GID_SUBBAND_CAP,
	GID_REWIND,
	GID_PLAY,
	GID_NEXT,
	GID_STOP,
	GID_FFWD,
	GID_HARDWARE_FILTER,
	GID_RADIO,
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

/* Compact GadTools Internet Radio layout.  Coordinates are relative to the
 * complete window (including its title bar), matching NewGadget semantics. */
#define RB_WIN_W            548
#define RB_WIN_H            262
#define RB_FILTER_ROW1_Y     20
#define RB_FILTER_ROW2_Y     42
#define RB_FILTER_ROW3_Y     64
#define RB_RESULTS_Y         88
#define RB_RESULTS_H        116
#define RB_BUTTONS_Y        210
#define RB_STATUS_Y         234

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
	struct Gadget  *gadFakeStereoWidth;
	struct Gadget  *gadFakeStereoDelay;
	struct Gadget  *gadRewind;
	struct Gadget  *gadPlay;
	struct Gadget  *gadNext;
	struct Gadget  *gadStop;
	struct Gadget  *gadFfwd;
	struct Gadget  *gadHardwareFilter;
	struct Gadget  *gadRadio;
	struct Gadget  *gadPlaylist;
	struct VisualInfo *visualInfo;
	struct Window  *plWin;
	struct Gadget  *plGadgets;
	struct Gadget  *plGadContext;
	struct Gadget  *plGadList;
	struct VisualInfo *plVisualInfo;
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
	int             rbSearchInProgress;
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
	/* Random tint for the drawn no-artwork radio fallback icon.  Rolled once
	 * per station/track (keyed on inputName) so it stays stable across the
	 * many redraws a single station triggers, and changes when you tune away. */
	unsigned long artFallbackKey;
	int           artFallbackHasColor;
	unsigned char artFallbackR;
	unsigned char artFallbackG;
	unsigned char artFallbackB;
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
	struct MsgPort *appPort;
	struct AppIcon *appIcon;
	struct DiskObject *appIconDiskObject;
	struct DiskObject appIconObject;
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
	int   subbandCapIndex;
	int   decodeThenPlay;
	int   bench;
	int   closeRequested;
	int   iconified;
	WORD  iconifyLeft;
	WORD  iconifyTop;
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
struct Library *IconBase;
struct Library *WorkbenchBase;
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
	"14700",
	"22050",
	"28600"
};

static const STRPTR kRateLabels[] = {
	(STRPTR)"8287",
	(STRPTR)"8820",
	(STRPTR)"11025",
	(STRPTR)"14700",
	(STRPTR)"22050",
	(STRPTR)"28600",
	NULL
};

static int RateIndexSupportsSuperfast(int rateIndex, int mono)
{
	return rateIndex >= (mono ? 0 : 1) && rateIndex <= 4;
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
	(STRPTR)"Fake stereo",
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
	if (gui->fakeStereo) return 2;
	return gui->mono ? 1 : 0;
}

static const STRPTR kQualityLabels[] = {
	(STRPTR)"Faster",
	(STRPTR)"Fast",
	(STRPTR)"Normal",
	(STRPTR)"Best",
	NULL
};

/* Manual override for --subband-cap N (see amiga_mp3dec.c's --subband-cap
 * help text). "Auto" (index 0) means don't pass --subband-cap at all and
 * let whatever fast-lowrate/ultrafast preset is active pick its own default
 * (or run uncapped at the full 32 subbands otherwise). The explicit values
 * below let a track that's still hiccuping at the fastest preset be capped
 * further by hand -- lower values drop more high-frequency detail but cost
 * less CPU in IMDCT/antialias/dequant. */
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
	{ NM_ITEM,  (STRPTR)"About MintAMP-GT...",0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_ABOUT) },
	{ NM_ITEM,  (STRPTR)"Internet Radio...",0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_STREAM) },
	{ NM_ITEM,  (STRPTR)"Iconify",          (STRPTR)"I", 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_ICONIFY) },
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
	SaveEnvInt("SubbandCapIndex", gui->subbandCapIndex);
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
		/* tags->artData is the only owner of this AllocMem() block; clear it
		 * (and artBytes) the instant it is freed so a second FreeTags()/reload
		 * cannot re-free the same pointer -- the stale-owner double free the
		 * closing alerts describe. */
		GUI_FREE_BEGIN("FreeTags", "tags-artData", tags->artData, tags->artBytes);
		FreeMem(tags->artData, tags->artBytes);
		tags->artData = NULL;
		tags->artBytes = 0;
		GUI_FREE_END("FreeTags", "tags-artData", tags->artData, 0);
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
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				if (i + 3 < len) {
					unsigned int loHi = bigEndian ? src[i + 2] : src[i + 3];
					unsigned int loLo = bigEndian ? src[i + 3] : src[i + 2];
					unsigned int loCp = (loHi << 8) | loLo;

					if (loCp >= 0xDC00 && loCp <= 0xDFFF)
						i += 2;
				}
				dst[out++] = '?';
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				dst[out++] = '?';
			} else if (cp < 0x20 || cp == 0x7F) {
				dst[out++] = '?';
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
				tags->artData = (unsigned char *)AllocMem(…45873 tokens truncated…_LeftEdge = bx;
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
	nw.Title = (UBYTE *)"MintAMP-GT Playlist";
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
	ClosePlaylistWindow(gui);
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
	UWORD gid;

	if (!gui->plWin)
		return;
	while ((msg = GT_GetIMsg(gui->plWin->UserPort)) != NULL) {
		classValue = msg->Class;
		code = msg->Code;
		gad = (struct Gadget *)msg->IAddress;
		gid = gad ? gad->GadgetID : 0;
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
		if (classValue != IDCMP_GADGETUP || !gid)
			continue;
		switch ((int)gid) {
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
		ASLFR_TitleText, (ULONG)"Select audio file for MintAMP-GT",
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
	nw.Title = (UBYTE *)"MintAMP-GT Internet Stream";
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
	ModifyIDCMP(win, 0);
	while ((msg = GT_GetIMsg(win->UserPort)) != NULL)
		GT_ReplyIMsg(msg);
	CloseWindow(win);
	FreeGadgets(gadgets);
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
	if (gui->subbandCapIndex > 0) {
		AddArg(args, "--subband-cap");
		sprintf(num, "%d", kSubbandCapValues[gui->subbandCapIndex]);
		AddArg(args, num);
	}
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
		SetStatus(gui, "Memory corruption detected; restart MintAMP before playing radio.");
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

	if (nilOut) {
		gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
			NP_Name, (ULONG)"MintAMP-GT playback",
			/* See AMIGA_PLAYBACK_TASK_PRIORITY's comment in amiga_mp3dec.c for
			 * the tradeoff -- CPU-bound decoding vs. keeping the GadTools event
			 * loop (and Stop) responsive. */
			NP_Priority, AMIGA_PLAYBACK_TASK_PRIORITY,
			NP_StackSize, 262144,
			NP_CurrentDir, dirLock,
			NP_Output, nilOut,
			NP_CloseOutput, TRUE,
			NP_CopyVars, FALSE,
			TAG_DONE);
	} else {
		gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
			NP_Name, (ULONG)"MintAMP-GT playback",
			/* See AMIGA_PLAYBACK_TASK_PRIORITY's comment in amiga_mp3dec.c for
			 * the tradeoff -- CPU-bound decoding vs. keeping the GadTools event
			 * loop (and Stop) responsive. */
			NP_Priority, AMIGA_PLAYBACK_TASK_PRIORITY,
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
			gui->fakeStereo = 0;
			gui->mono = 1;
			gui->rateIndex = 4;
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
		/* 22050 Mono Ultrafast forces mono, so grey the output cycle
		 * (and re-enable it when switching back to any other speed mode). */
		UpdateChannelGadgetState(gui);
		SetStatus(gui, code == 4 ?
			"22050 mono ultrafast enabled (reduced taps, 12 subband cap)." :
			code == 3 ?
			"Ultrafast enabled (26 subband cap)." :
			code == 2 ? "Superfast enabled for 8287/8820/11025/14700/22050 Hz." :
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
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, ChannelModeIndex(gui), TAG_DONE);
			SetStatus(gui, "Stop playback before changing channel mode.");
			break;
		}
		/* code: 0=Stereo, 1=Mono, 2=Fake stereo */
		if (code > 2)
			code = 0;
		gui->fakeStereo = (code == 2) ? 1 : 0;
		gui->mono = (code == 1) ? 1 : 0;
		if (gui->superfastLowrate && !RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui)))
			gui->rateIndex = DefaultSuperfastRateIndex(ChannelUsesMonoCost(gui));
		if (gui->gadRate)
			GT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
				GTCY_Labels, (ULONG)kRateLabels,
				GTCY_Active, gui->rateIndex,
				TAG_DONE);
		UpdateChannelGadgetState(gui);
		SetStatus(gui, gui->fakeStereo ? "Fake-stereo output enabled." :
			gui->mono ? "Mono output enabled." : "Stereo output enabled.");
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
		if (gui->rateIndex < 0 || gui->rateIndex > 5)
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
	case GID_SUBBAND_CAP:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, gui->subbandCapIndex, TAG_DONE);
			SetStatus(gui, "Stop playback before changing subbands.");
			break;
		}
		gui->subbandCapIndex = code;
		if (gui->subbandCapIndex < 0 || gui->subbandCapIndex >= (int)SUBBAND_CAP_COUNT)
			gui->subbandCapIndex = 0;
		SetStatus(gui, "Manual subband cap updated.");
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
		/* Internet streams must be probed (DNS/redirect/codec) before the
		 * decoder child is launched; launching StartPlayback() directly on a
		 * bare URL fails with "stream failed".  Route radio inputs through the
		 * same probe the radio browser uses. */
		if (IsRadioInputName(gui->inputName))
			RadioReplayCurrentUrl(gui);
		else
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
	case GID_REWIND:
		GuiSeekRelative(gui, -SEEK_STEP_SECS);
		break;
	case GID_FFWD:
		GuiSeekRelative(gui, SEEK_STEP_SECS);
		break;
	case GID_HARDWARE_FILTER:
		gui->hardwareFilter = !gui->hardwareFilter;
		ApplyHardwareAudioFilter(gui);
		DrawFilterButton(gui);
		SetStatus(gui, gui->hardwareFilter ?
			"Hardware filter enabled." : "Hardware filter disabled.");
		SaveGuiSettings(gui);
		break;
	case GID_RADIO:
		if (!gui->hasNetwork)
			SetStatus(gui, "No TCP/IP stack found - internet radio unavailable.");
		else
			OpenRadioWindow(gui);
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
					else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ICONIFY)
						GuiIconify(gui);
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
	/* GUI/main application task identity: every GUI_FREE_BEGIN/END below logs
	 * FindTask(NULL), and this is the pointer they must match for the
	 * recoverable AN_FreeTwice/AN_BadFreeAddr alerts to be pinned on the GUI
	 * task rather than the net worker or a playback child. */
	GUI_TASK_IDENTITY("application-startup-main-task");
	if (GuiOpen(&gui) != 0)
		return 1;
	GUI_TASK_IDENTITY("gui-event-loop");
	while (!gui.closeRequested) {
		ULONG winMask = (gui.win && gui.win->UserPort) ?
			(1UL << gui.win->UserPort->mp_SigBit) : 0;
		ULONG appMask = gui.appPort ? (1UL << gui.appPort->mp_SigBit) : 0;
		ULONG timerMask = gui.timerPort ? (1UL << gui.timerPort->mp_SigBit) : 0;
		ULONG doneMask = gui.donePort ? (1UL << gui.donePort->mp_SigBit) : 0;
		ULONG plMask = gui.plWin ? (1UL << gui.plWin->UserPort->mp_SigBit) : 0;
		ULONG rbMask = gui.rbWin ? (1UL << gui.rbWin->UserPort->mp_SigBit) : 0;
		ULONG sigs = Wait(winMask | appMask | timerMask |
			doneMask | plMask | rbMask | SIGBREAKF_CTRL_C);
		if (sigs & SIGBREAKF_CTRL_C)
			gui.closeRequested = 1;
		if (doneMask && (sigs & doneMask))
			HandleDoneSignal(&gui);
		if (timerMask && (sigs & timerMask))
			HandleTimerSignal(&gui);
		if (appMask && (sigs & appMask))
			GuiHandleAppIcon(&gui);
		HandlePlaylistPoll(&gui);
		HandleRadioWindow(&gui);
		GuiPoll(&gui);
	}
	if (gui.playbackActive)
		WaitForPlaybackShutdown(&gui);
	/* Walk the exec heap once on the way out (all builds, not just
	 * MINIAMP_DEBUG_ALLOC) so corruption that happened during a radio stream
	 * stop/switch this session is flagged now, before the disposal below frees
	 * anything through it.  Radio_CheckMiniMem() sets radioMemoryPoisoned when
	 * it finds a damaged chunk list. */
	Radio_CheckMiniMem("app-close before dispose");
	if (Radio_IsMemoryPoisoned()) {
		/* Corrupt exec heap (the AN_BadFreeAddr / MiniMem-detected corruption
		 * this codebase hits during flaky radio stream stop/switch): every step
		 * left in the normal close path -- SaveGuiSettings(), and GuiClose()'s
		 * FreeGadgets()/FreeMenus()/FreeVisualInfo()/CloseWindow()/CloseLibrary()
		 * and Radio_NetworkShutdown() -- frees memory or closes libraries through
		 * the same damaged allocator state.  FreeMem() walks the broken free list
		 * under Forbid(), so a second corrupting free hard-locks the machine
		 * (frozen mouse) instead of alerting.  Skip all further disposal and exit
		 * as directly as possible: the leak is recoverable with a reboot, another
		 * corrupting free is not.  This mirrors minimp3r.c's app-close guard. */
		RADIO_DBG(printf("app-close: memory corruption detected -- skipping SaveGuiSettings/GuiClose to avoid a corrupting free, exiting directly\n");)
		return 0;
	}
	SaveGuiSettings(&gui);
	GuiClose(&gui);
	return 0;
}

#if defined(AMIGA_M68K)
extern void LibnixFreeAllCompat_Install(void);
#endif

int main(int argc, char **argv)
{
	struct Task *task = FindTask(NULL);
	int rc;

	/* One-time init of the cross-task stdout lock shared with amiga_mp3dec.c,
	 * radio_stream.c and the Radio Browser modules.  Must run before the radio
	 * net worker task or any playback child touches it: the first Internet
	 * Radio search obtains it from the GUI and worker tasks, and an
	 * uninitialised SignalSemaphore makes that ObtainSemaphore() block forever.
	 * minimp3r.c does the same in its own main(). */
	InitSemaphore(&radio_console_lock);
#if defined(AMIGA_M68K)
	LibnixFreeAllCompat_Install();
#endif


	gGuiDetectedStackLower = (ULONG)task->tc_SPLower;
	gGuiDetectedStackUpper = (ULONG)task->tc_SPUpper;
	gGuiDetectedStackSize = gGuiDetectedStackUpper - gGuiDetectedStackLower;
	gGuiEffectiveStackSize = gGuiDetectedStackSize;

	if (gGuiDetectedStackSize >= GUI_STARTUP_STACK_SIZE) {
#if defined(DEBUG) || defined(RADIO_DEBUG)
		printf("MintAMP-GT: startup stack lower=%lu upper=%lu size=%lu, no swap needed\n",
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
	printf("MintAMP-GT: startup stack lower=%lu upper=%lu size=%lu, swapped to %lu bytes\n",
		gGuiDetectedStackLower, gGuiDetectedStackUpper, gGuiDetectedStackSize,
		gGuiEffectiveStackSize);
#endif
	rc = GuiMainReal(argc, argv);

	StackSwap(&gGuiOldStack);
	GUI_TASK_IDENTITY("shutdown-free-startup-stack");
	GUI_FREE_BEGIN("main", "startup-stack", gGuiAllocatedStack, GUI_STARTUP_STACK_SIZE);
	FreeMem(gGuiAllocatedStack, GUI_STARTUP_STACK_SIZE);
	gGuiAllocatedStack = NULL;
	GUI_FREE_END("main", "startup-stack", gGuiAllocatedStack, 0);
	return rc;
}

#else

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "MintAMP-GT GUI requires an AMIGA_M68K Intuition/ASL/GadTools build.\n");
	fprintf(stderr, "Use amiga_mp3dec --play --rate 11025 --buffer-seconds 10 file.mp3 on this host.\n");
	return 1;
}

#endif
