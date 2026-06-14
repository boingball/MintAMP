/*
 * MiniAMP3 - compact AmigaOS GadTools mini-player frontend for the Helix
 * fixed-point MP3 decoder.  The GUI wraps the existing amiga_mp3dec playback
 * frontend so the same Paula streaming path, fast-lowrate options, and buffer
 * handling are used from either Shell or Workbench.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AMIGA_M68K)
#define main HelixAmp3CliMain
#include "amiga_mp3dec.c"
#undef main
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
} GuiPlaybackStatus;
#define GUIPLAY_PHASE_IDLE      0
#define GUIPLAY_PHASE_BUFFERING 1
#define GUIPLAY_PHASE_PLAYING   2
#define GUIPLAY_PHASE_UNDERRUN  3
#define GUIPLAY_PHASE_DONE      4
#define GUIPLAY_PHASE_STOPPING  5
#define GUIPLAY_CLEANUP_NONE          0
#define GUIPLAY_CLEANUP_ABORT_REAP    1
#define GUIPLAY_CLEANUP_DEVICE_CLOSED 2
#define GUIPLAY_CLEANUP_BUFFERS_FREED 3
#define GUIPLAY_CLEANUP_COMPLETE      4
#define GUISTART_NONE                  0
#define GUISTART_CHILD_ENTERED         10
#define GUISTART_ARGS_READY            20
#define GUISTART_INPUT_OPEN            30
#define GUISTART_INPUT_PREPARE         40
#define GUISTART_DECODER_ALLOC         50
#define GUISTART_DECODER_CONFIG        60
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
static const char *GuiStartupStageName(int stage)
{
	switch (stage) {
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
#endif
/* Shared status written by the playback subprocess (amiga_mp3dec.c). */
extern GuiPlaybackStatus gGuiPlaybackStatus;

#ifdef AMIGA_M68K
#include <exec/types.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <diskfont/diskfont.h>
#include <devices/timer.h>
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
#include "picojpeg.h"

#define HELIXAMP3_MAX_PATH 256
#define HELIXAMP3_ARGC_MAX 18
#define HELIXAMP3_SIGMASK(gui) (1UL << (gui)->win->UserPort->mp_SigBit)
#define GUI_ENV_PREFIX  "ENVARC:MiniAMP3"

#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       320    /* inner height */

#define GUI_MARGIN_L     8     /* left margin */
#define GUI_MARGIN_R     8     /* right margin */
#define GUI_TOP_Y       36     /* y of first gadget row */
#define GUI_ROW_H       18     /* row pitch - enough for Topaz 8 + padding */

#define ART_W           64
#define ART_H           64
#define MAX_JPEG_DIM    1024
#define ART_X           (GUI_WIN_W - ART_W - GUI_MARGIN_R)
#define ART_Y           GUI_TOP_Y

#define TEXT_COL_W      (ART_X - GUI_MARGIN_L - 8)

#define ROW_FILE        (GUI_TOP_Y)
#define ROW_TITLE       (GUI_TOP_Y + 1 * GUI_ROW_H)
#define ROW_ARTIST      (GUI_TOP_Y + 2 * GUI_ROW_H)
#define ROW_ALBUM       (GUI_TOP_Y + 3 * GUI_ROW_H)
#define ROW_RATING      (GUI_TOP_Y + 4 * GUI_ROW_H)
#define ROW_TRACK       (GUI_TOP_Y + 5 * GUI_ROW_H)
#define ROW_GENRE       (GUI_TOP_Y + 6 * GUI_ROW_H)
#define ROW_CHECKS      (GUI_TOP_Y + 7 * GUI_ROW_H + 4)
#define ROW_CYCLES      (GUI_TOP_Y + 8 * GUI_ROW_H + 4)
#define ROW_BUFFER      (GUI_TOP_Y + 9 * GUI_ROW_H + 4)
#define ROW_PROGRESS    (GUI_TOP_Y + 10 * GUI_ROW_H + 8)
#define ROW_BUTTONS     (GUI_TOP_Y + 11 * GUI_ROW_H + 12)
#define ROW_STATUS      (GUI_TOP_Y + 12 * GUI_ROW_H + 12)
#define ROW_FILEINFO    (GUI_TOP_Y + 13 * GUI_ROW_H + 12)

#define PROG_X          (GUI_MARGIN_L + 8)
#define PROG_W          (GUI_WIN_W - PROG_X - 90 - GUI_MARGIN_R)
#define PROG_H          8
#define PROG_TOP_Y      (ROW_PROGRESS + 4)
#define TIME_X          (PROG_X + PROG_W + 6)
#define TIME_W          80
#define TIMER_TICK_MICROS 1000000UL
#define ART_TIMER_MICROS 20000UL
#define ART_MCUS_PER_PUMP 4

#define MENUNUM_PROJECT   0
#define MENUNUM_PLAYBACK  1
#define ITEMNUM_ABOUT     0
#define ITEMNUM_QUIT      1
#define ITEMNUM_DTP       0
#define ITEMNUM_BENCH     1
#define ITEMNUM_ARTWORK   2
#define ITEMNUM_PROGRESS  3

enum {
	GID_FILE = 1,
	GID_BROWSE,
	GID_TITLE,
	GID_ARTIST,
	GID_ALBUM,
	GID_FAST_LOWRATE,
	GID_FAST_MEM,
	GID_MONO,
	GID_RATE,
	GID_BUFFER,
	GID_QUALITY,
	GID_PLAY,
	GID_STOP,
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
	GID_COUNT
};

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
	unsigned short greyCount[ART_W * ART_H];
	unsigned char greyOut[ART_W * ART_H];
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
	unsigned long fileSize;
	int  durationSecs;
	unsigned char *artData;
	unsigned long artBytes;
	int artIsPng;
} Mp3Tags;

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
	struct Gadget  *gadPlay;
	struct Gadget  *gadStop;
	struct VisualInfo *visualInfo;
	struct Menu *menuStrip;
	int artEnabled;
	int artValid;
	int artLoading;
	unsigned char artGreyBuf[ART_W * ART_H];
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
	char  fileText[HELIXAMP3_MAX_PATH];
	char  lastDrawer[HELIXAMP3_MAX_PATH];
	char  statusText[128];
	char  fileInfoText[128];
	char  ratingText[16];
	int   fastLowrate;
	int   fastMem;
	int   mono;
	int   rateIndex;
	int   bufferSeconds;
	int   qualityIndex;
	int   decodeThenPlay;
	int   bench;
	int   closeRequested;
	int   playbackActive;
	int   playbackDonePending;
	int   playbackStoppedByUser;
	unsigned long playbackRunId;
	unsigned long playbackDoneRunId;
	int lastCleanupStage;
	int lastStartupStage;
	int startupStageStableTicks;
	int startupStallShown;
	int   totalSecs;
	int   elapsedSecs;
	int   launchBufferSecs;
	unsigned long lastUnderrunCount;   /* last underrun count seen from IPC */
	long          lastDisplayedSpareMs; /* spare ms last shown in status bar */
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

struct IntuitionBase *IntuitionBase;
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

static const STRPTR kQualityLabels[] = {
	(STRPTR)"Fast",
	(STRPTR)"Normal",
	(STRPTR)"Best",
	NULL
};

static struct NewMenu myNewMenus[] = {
	{ NM_TITLE, (STRPTR)"Project",          0, 0, 0, 0 },
	{ NM_ITEM,  (STRPTR)"About MiniAMP3...",0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_ABOUT) },
	{ NM_ITEM,  (STRPTR)"Quit",             0, 0, 0,
		(APTR)(MENUNUM_PROJECT * 100 + ITEMNUM_QUIT) },
	{ NM_TITLE, (STRPTR)"Playback",         0, 0, 0, 0 },
	{ NM_ITEM,  (STRPTR)"Decode-then-play", 0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_DTP) },
	{ NM_ITEM,  (STRPTR)"Bench mode",       0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_BENCH) },
	{ NM_ITEM,  (STRPTR)"Artwork",          0, CHECKIT | MENUTOGGLE, 0,
		(APTR)(MENUNUM_PLAYBACK * 100 + ITEMNUM_ARTWORK) },
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


static void CopyDrawerFromPath(char *drawer, size_t drawerSize, const char *path)
{
	char *q;

	if (!drawer || drawerSize == 0)
		return;
	drawer[0] = '\0';
	if (!path || !path[0])
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

static int LoadEnvInt(const char *key, int fallback, int minValue, int maxValue)
{
	char name[64];
	char value[32];
	long n;
	int v;

	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)value, sizeof(value) - 1, 0);
	if (n <= 0)
		return fallback;
	value[n] = '\0';
	v = atoi(value);
	if (v < minValue)
		v = minValue;
	if (v > maxValue)
		v = maxValue;
	return v;
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

static void SaveEnvInt(const char *key, int value)
{
	char text[16];

	sprintf(text, "%d", value);
	SaveEnvString(key, text);
}

static void SaveGuiSettings(HelixAmp3Gui *gui)
{
	SaveEnvInt("FastLowrate", gui->fastLowrate);
	SaveEnvInt("FastMem", gui->fastMem);
	SaveEnvInt("Mono", gui->mono);
	SaveEnvInt("RateIndex", gui->rateIndex);
	SaveEnvInt("BufferSeconds", gui->bufferSeconds);
	SaveEnvInt("QualityIndex", gui->qualityIndex);
	SaveEnvInt("DecodeThenPlay", gui->decodeThenPlay);
	SaveEnvInt("Bench", gui->bench);
	SaveEnvInt("Artwork", gui->artEnabled);
	SaveEnvInt("ProgressBar", gui->progressEnabled);
	SaveEnvString("LastDrawer", gui->lastDrawer);
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
			tags->channels = (((h[3] >> 6) & 0x03) == 3) ? 1 : 2;
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
	if (!tags->genre[0] && buf[127] != 255)
		sprintf(tags->genre, "ID3 genre %u", (unsigned int)buf[127]);
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

	if (!path || !path[0])
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
		if ((version == 2 && strcmp(id, "TT2") == 0) || strcmp(id, "TIT2") == 0)
			target = tags->title;
		else if ((version == 2 && strcmp(id, "TP1") == 0) || strcmp(id, "TPE1") == 0)
			target = tags->artist;
		else if ((version == 2 && strcmp(id, "TAL") == 0) || strcmp(id, "TALB") == 0)
			target = tags->album;
		else if ((version == 2 && strcmp(id, "TRK") == 0) || strcmp(id, "TRCK") == 0)
			target = tags->track;
		else if ((version == 2 && strcmp(id, "TCO") == 0) || strcmp(id, "TCON") == 0)
			target = tags->genre;
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
			if (fread(text, 1, (size_t)n, f) == (size_t)n)
				CopyId3v2TextField(target, 64, text, n);
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

	if (!inputName || !tags || tags->artData)
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


static void FormatRatingText(HelixAmp3Gui *gui)
{
	int i;

	for (i = 0; i < 5; i++)
		gui->ratingText[i] = (i < gui->tags.rating) ? '*' : '-';
	sprintf(gui->ratingText + 5, " %d/5", gui->tags.rating);
}

static void FormatFileInfo(HelixAmp3Gui *gui)
{
	const char *ch = gui->tags.channels == 1 ? "mono" :
		(gui->tags.channels == 2 ? "stereo" : "?");
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


static const unsigned char kBayer4x4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 }
};

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

static int McuSampleOffset(const pjpeg_image_info_t *info, int x, int y)
{
	int blockX = x / 8;
	int blockY = y / 8;
	int blocksPerRow = info->m_MCUWidth / 8;
	int block = blockY * blocksPerRow + blockX;

	return block * 64 + (y & 7) * 8 + (x & 7);
}

static int DecodeJpegToGrey(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, int outW, int outH, int isPng)
{
	pjpeg_image_info_t info;
	PjpegSrc src;
	unsigned char status;
	unsigned char xMap[MAX_JPEG_DIM];
	unsigned char yMap[MAX_JPEG_DIM];
	static unsigned long greyAccum[ART_W * ART_H];
	static unsigned short greyCount[ART_W * ART_H];
	int mcuIndex;
	int i;

	if (isPng || !jpegData || jpegBytes <= 4 || !greyOut ||
		outW <= 0 || outW > ART_W || outH <= 0 || outH > ART_H)
		return -1;
	src.data = jpegData;
	src.pos = 0;
	src.size = jpegBytes;
	memset(greyOut, 0x80, (size_t)(outW * outH));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(greyCount, 0, sizeof(greyCount));
	/* Full MCU decoding gives the tiny artwork thumbnail enough source
	 * samples to preserve cover edges and faces instead of block averages. */
	status = pjpeg_decode_init(&info, pjpeg_cb, &src, 0);
	if (status != 0 || info.m_width <= 0 || info.m_height <= 0 ||
		info.m_width > MAX_JPEG_DIM || info.m_height > MAX_JPEG_DIM)
		return -1;
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
		if (status != 0)
			return -1;
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
				int dstX;
				int off;
				int g;

				if (srcX >= info.m_width)
					continue;
				dstX = xMap[srcX];
				off = McuSampleOffset(&info, x, y);
				if (info.m_comps == 1)
					g = info.m_pMCUBufR[off];
				else
					g = ((int)info.m_pMCUBufR[off] * 30 +
						(int)info.m_pMCUBufG[off] * 59 +
						(int)info.m_pMCUBufB[off] * 11) / 100;
				{
					int dst = dstY * outW + dstX;

					greyAccum[dst] += (unsigned long)g;
					if (greyCount[dst] != 0xffff)
						greyCount[dst]++;
				}
			}
		}
	}
	for (i = 0; i < outW * outH; i++) {
		if (greyCount[i])
			greyOut[i] = (unsigned char)((greyAccum[i] +
				(greyCount[i] / 2)) / greyCount[i]);
	}
	return 0;
}


static void DrawArtPanel(HelixAmp3Gui *gui);
static void HandleDoneSignal(HelixAmp3Gui *gui);

static int JpegGreySample(const pjpeg_image_info_t *info, int off)
{
	if (info->m_comps == 1)
		return info->m_pMCUBufR[off];
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_JPEG_GREY)
	{
		unsigned long r = info->m_pMCUBufR[off];
		unsigned long g = info->m_pMCUBufG[off];
		unsigned long b = info->m_pMCUBufB[off];
		__asm__ volatile (
			"mulu.w #30,%0\n\t"
			"mulu.w #59,%1\n\t"
			"mulu.w #11,%2\n\t"
			"add.l %1,%0\n\t"
			"add.l %2,%0\n\t"
			"divu.w #100,%0\n\t"
			"andi.l #65535,%0"
			: "+d" (r), "+d" (g), "+d" (b));
		return (int)r;
	}
#else
	return ((int)info->m_pMCUBufR[off] * 30 +
		(int)info->m_pMCUBufG[off] * 59 +
		(int)info->m_pMCUBufB[off] * 11) / 100;
#endif
}

static void FinishArtDecode(HelixAmp3Gui *gui, int ok)
{
	ArtDecodeState *st = &gui->artDecode;
	int i;

	if (ok) {
		for (i = 0; i < ART_W * ART_H; i++) {
			if (st->greyCount[i])
				st->greyOut[i] = (unsigned char)((st->greyAccum[i] +
					(st->greyCount[i] / 2)) / st->greyCount[i]);
		}
		memcpy(gui->artGreyBuf, st->greyOut, ART_W * ART_H);
		gui->artValid = 1;
	}
	st->active = 0;
	gui->artLoading = 0;
	DrawArtPanel(gui);
}

static void CancelArtDecode(HelixAmp3Gui *gui)
{
	ArtDecodeState *st = &gui->artDecode;

	if (!st->active && !gui->artLoading)
		return;
	st->active = 0;
	gui->artLoading = 0;
	DrawArtPanel(gui);
}

static void PumpArtDecode(HelixAmp3Gui *gui)
{
	ArtDecodeState *st = &gui->artDecode;
	int pumped;

	if (!st->active)
		return;
	for (pumped = 0; pumped < ART_MCUS_PER_PUMP && st->active; pumped++) {
		unsigned char status;
		int mcuX;
		int mcuY;
		int y;

		if (st->mcuIndex >= st->totalMcus) {
			FinishArtDecode(gui, 1);
			break;
		}
		status = pjpeg_decode_mcu();
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
		for (y = 0; y < st->info.m_MCUHeight; y++) {
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
				st->greyAccum[dst] += (unsigned long)JpegGreySample(&st->info,
					McuSampleOffset(&st->info, x, y));
				if (st->greyCount[dst] != 0xffff)
					st->greyCount[dst]++;
			}
		}
	}
}

static void StartArtDecode(HelixAmp3Gui *gui)
{
	ArtDecodeState *st = &gui->artDecode;
	unsigned char status;
	int i;

	memset(st, 0, sizeof(*st));
	gui->artValid = 0;
	gui->artLoading = 0;
	if (!gui->tags.artData || gui->tags.artBytes <= 4 || gui->tags.artIsPng) {
		DrawArtPanel(gui);
		return;
	}
	memset(st->greyOut, 0x80, sizeof(st->greyOut));
	st->src.data = gui->tags.artData;
	st->src.size = gui->tags.artBytes;
	status = pjpeg_decode_init(&st->info, pjpeg_cb, &st->src, 0);
	if (status != 0 || st->info.m_width <= 0 || st->info.m_height <= 0 ||
		st->info.m_width > MAX_JPEG_DIM || st->info.m_height > MAX_JPEG_DIM) {
		DrawArtPanel(gui);
		return;
	}
	for (i = 0; i < st->info.m_width; i++)
		st->xMap[i] = (unsigned char)((i * ART_W) / st->info.m_width);
	for (i = 0; i < st->info.m_height; i++)
		st->yMap[i] = (unsigned char)((i * ART_H) / st->info.m_height);
	st->totalMcus = st->info.m_MCUSPerRow * st->info.m_MCUSPerCol;
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
		/* Resolve all three pens with a single GetScreenDrawInfo/Free pair. */
		int pens[3];
		{
			struct DrawInfo *dri = gui->win ?
				GetScreenDrawInfo(gui->win->WScreen) : NULL;
			if (dri) {
				pens[0] = dri->dri_Pens[SHADOWPEN];
				pens[1] = dri->dri_Pens[BACKGROUNDPEN];
				pens[2] = dri->dri_Pens[SHINEPEN];
				FreeScreenDrawInfo(gui->win->WScreen, dri);
			} else {
				pens[0] = 0;
				pens[1] = 1;
				pens[2] = 1;
			}
		}

		/* Render using horizontal RectFill runs instead of per-pixel WritePixel. */
		for (y = 0; y < ART_H; y++) {
			int runStart = 0;
			int runShade;

			/* Compute first pixel's shade to seed the run. */
			{
				int g0 = gui->artGreyBuf[y * ART_W];
				int dv = kBayer4x4[y & 3][0] - 8;
				int gd = g0 + dv * 2;

				runShade = gd >= 176 ? 2 : (gd >= 80 ? 1 : 0);
			}
			for (x = 1; x <= ART_W; x++) {
				int shade;

				if (x < ART_W) {
					int g = gui->artGreyBuf[y * ART_W + x];
					int dv = kBayer4x4[y & 3][x & 3] - 8;
					int gd = g + dv * 2;

					shade = gd >= 176 ? 2 : (gd >= 80 ? 1 : 0);
				} else {
					shade = -1; /* sentinel to flush last run */
				}
				if (shade != runShade) {
					/* Flush the completed run. */
					SetAPen(rp, pens[runShade]);
					RectFill(rp,
						ART_X + runStart, ART_Y + y,
						ART_X + x - 1, ART_Y + y);
					runStart = x;
					runShade = shade;
				}
			}
		}
	} else {
		const char *label = gui->artLoading ? "Loading" : "No art";
		SetAPen(rp, 0);
		RectFill(rp, ART_X, ART_Y, ART_X + ART_W - 1, ART_Y + ART_H - 1);
		SetAPen(rp, 1);
		Move(rp, ART_X + (gui->artLoading ? 10 : 16), ART_Y + ART_H / 2);
		Text(rp, label, gui->artLoading ? 7 : 6);
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

	if (total > 0) {
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
	return gui->playbackDonePending &&
		gDoneRunId == gui->playbackRunId &&
		gGuiPlaybackStatus.runId == gui->playbackRunId &&
		gGuiPlaybackStatus.cleanupComplete &&
		!PlaybackProcessStillExists();
}

static void FinalizePlayback(HelixAmp3Gui *gui)
{
	int stoppedByUser = gui->playbackStoppedByUser;

	gui->playbackDonePending = 0;
	gui->playbackStoppedByUser = 0;
	gui->playbackActive = 0;
	gGuiPlayer.process = NULL;
	gDonePort = NULL;
	if (gui->totalSecs > 0 && !stoppedByUser)
		gui->elapsedSecs = gui->totalSecs + gui->launchBufferSecs;
	DrawProgress(gui);
	ResetCliParser();
	ResetDecoderStatics();
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gui->lastCleanupStage = GUIPLAY_CLEANUP_NONE;
	SetStatus(gui, stoppedByUser ? "Stopped - ready." : "Playback finished - ready.");
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
			SetStatus(gui, gui->playbackStoppedByUser ?
				"Stopping playback process..." : "Finishing playback process...");
		}
	}
	if (gui->playbackDonePending && PlaybackCanFinalize(gui))
		FinalizePlayback(gui);

	if (gui->playbackActive && !gui->playbackDonePending && !expiredWasArt) {
		int phase = gGuiPlaybackStatus.phase;
		unsigned long frames = gGuiPlaybackStatus.decodedFrames;
		int rate = gGuiPlaybackStatus.sampleRate;
		unsigned long underruns = gGuiPlaybackStatus.underruns;
		long spareMs = gGuiPlaybackStatus.spareMs;
		unsigned long halfBufferMs = gGuiPlaybackStatus.halfBufferMs;

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
			char buf[128];
			int stage = gGuiPlaybackStatus.startupStage;
			if (stage != gui->lastStartupStage) {
				gui->lastStartupStage = stage;
				gui->startupStageStableTicks = 0;
				gui->startupStallShown = 0;
			} else if (stage != GUISTART_PLAYING) {
				gui->startupStageStableTicks++;
			}
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
			break;
		}
		case GUIPLAY_PHASE_UNDERRUN:
			if (underruns != gui->lastUnderrunCount) {
				char buf[64];
				gui->lastUnderrunCount = underruns;
				sprintf(buf, "Underrun! (total: %lu)", underruns);
				SetStatus(gui, buf);
			}
			break;
		case GUIPLAY_PHASE_STOPPING:
			if (gGuiPlaybackStatus.cleanupStage != gui->lastCleanupStage) {
				gui->lastCleanupStage = gGuiPlaybackStatus.cleanupStage;
				switch (gui->lastCleanupStage) {
				case GUIPLAY_CLEANUP_ABORT_REAP: SetStatus(gui, "Stopping: aborting/reaping audio IO..."); break;
				case GUIPLAY_CLEANUP_DEVICE_CLOSED: SetStatus(gui, "Stopping: audio.device closed..."); break;
				case GUIPLAY_CLEANUP_BUFFERS_FREED: SetStatus(gui, "Stopping: buffers freed..."); break;
				case GUIPLAY_CLEANUP_COMPLETE: SetStatus(gui, "Stopping: cleanup complete..."); break;
				default: SetStatus(gui, "Stopping: cleanup started..."); break;
				}
			}
			break;
		case GUIPLAY_PHASE_PLAYING: {
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
			break;
		}
		default:
			break;
		}

		if (gui->progressEnabled)
			DrawProgress(gui);
	}
	PumpArtDecode(gui);
	SendTimerRequest(gui, gui->artDecode.active ? ART_TIMER_MICROS :
		TIMER_TICK_MICROS);
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
		SetStatus(gui, gui->playbackStoppedByUser ?
			"Stopping playback process..." : "Finishing playback process...");
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
		ReadMp3Tags(gui->inputName, &gui->tags, 1);
		gui->totalSecs = gui->tags.durationSecs;
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
	es.es_TextFormat = (UBYTE *)"MiniAMP3\nHelix fixed-point MP3 decoder\nAmigaOS GadTools frontend";
	es.es_GadgetFormat = (UBYTE *)"OK";
	EasyRequest(gui->win, &es, NULL, TAG_DONE);
}

static struct Gadget *MakeGadget(HelixAmp3Gui *gui, struct Gadget *prev,
	ULONG kind, UWORD id, WORD left, WORD top, WORD width, WORD height,
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
	if (kind == TEXT_KIND)
	ng.ng_TextAttr = &gTopaz8Attr;
else
	ng.ng_TextAttr = NULL;
	if (kind == BUTTON_KIND)
		ng.ng_Flags = PLACETEXT_IN;
	else if (kind == CHECKBOX_KIND)
		ng.ng_Flags = PLACETEXT_RIGHT;
	else
		ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_VisualInfo = gui->visualInfo;
	return CreateGadget(kind, prev, &ng,
		tag1, value1,
		tag2, value2,
		tag3, value3,
		tag4, value4,
		TAG_DONE);
}

static int GuiCreateGadgets(HelixAmp3Gui *gui)
{
	struct Gadget *gad;

	gui->gadContext = CreateContext(&gui->gadgets);
	if (!gui->gadContext)
		return -1;
	gad = gui->gadContext;

	gui->gadFile = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILE,
		GUI_MARGIN_L + 48, ROW_FILE, TEXT_COL_W - 100, 16, "File:",
		GTTX_Text, (ULONG)gui->fileText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
		ART_X - 56, ROW_FILE - 1, 56, 16, "Browse",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadTitle = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TITLE,
		GUI_MARGIN_L + 54, ROW_TITLE, TEXT_COL_W - 54, 16, "Title:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadArtist = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ARTIST,
		GUI_MARGIN_L + 60, ROW_ARTIST, TEXT_COL_W - 54, 16, "Artist:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadAlbum = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ALBUM,
		GUI_MARGIN_L + 54, ROW_ALBUM, TEXT_COL_W - 54, 16, "Album:",
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
		GUI_MARGIN_L + 54, ROW_TRACK, TEXT_COL_W - 54, 16, "Track:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadGenre = gad = MakeGadget(gui, gad, TEXT_KIND, GID_GENRE,
		GUI_MARGIN_L + 54, ROW_GENRE, TEXT_COL_W - 54, 16, "Genre:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_LOWRATE,
		GUI_MARGIN_L + 14, ROW_CHECKS, 20, 12, "Fast-lr",
		GTCB_Checked, gui->fastLowrate,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_MEM,
		GUI_MARGIN_L + 150, ROW_CHECKS, 20, 12, "Fast-mem",
		GTCB_Checked, gui->fastMem,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_MONO,
		GUI_MARGIN_L + 280, ROW_CHECKS, 20, 12, "Mono",
		GTCB_Checked, gui->mono,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CYCLE_KIND, GID_RATE,
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

	gui->gadBuffer = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_BUFFER,
		GUI_MARGIN_L + 62, ROW_BUFFER,
		GUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 80, 16, "Buffer:",
		GTSL_Min, 1,
		GTSL_Max, 30,
		GTSL_Level, gui->bufferSeconds,
		GTSL_LevelFormat, (ULONG)"%ld sec");
	if (!gad)
		return -1;

	gui->gadPlay = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAY,
		GUI_MARGIN_L + 120, ROW_BUTTONS, 80, 18, "Play",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
		GUI_MARGIN_L + 300, ROW_BUTTONS, 80, 18, "Stop",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
		GUI_MARGIN_L + 60, ROW_STATUS, GUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 80, 16, "Status:",
		GTTX_Text, (ULONG)gui->statusText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadFileInfo = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILEINFO,
		GUI_MARGIN_L + 68, ROW_FILEINFO, GUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 88, 16, "File info:",
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
	SetMenuItemChecked(gui, MENUNUM_PLAYBACK, ITEMNUM_PROGRESS,
		gui->progressEnabled);
}

static void StopPlayback(HelixAmp3Gui *gui);
static void GuiClose(HelixAmp3Gui *gui);

static void DrainWindowMessages(HelixAmp3Gui *gui)
{
	struct IntuiMessage *msg;

	if (!gui || !gui->win)
		return;
	while ((msg = GT_GetIMsg(gui->win->UserPort)) != NULL)
		GT_ReplyIMsg(msg);
}

static int GuiOpen(HelixAmp3Gui *gui)
{
	struct NewWindow nw;

	memset(gui, 0, sizeof(*gui));
	gui->fastLowrate = LoadEnvInt("FastLowrate", 1, 0, 1);
	gui->fastMem = LoadEnvInt("FastMem", 1, 0, 1);
	gui->mono = LoadEnvInt("Mono", 1, 0, 1);
	gui->rateIndex = LoadEnvInt("RateIndex", 2, 0, 4);
	gui->bufferSeconds = LoadEnvInt("BufferSeconds", 10, 1, 30);
	gui->qualityIndex = LoadEnvInt("QualityIndex", 0, 0, 2);
	gui->decodeThenPlay = LoadEnvInt("DecodeThenPlay", 0, 0, 1);
	gui->bench = LoadEnvInt("Bench", 0, 0, 1);
	gui->artEnabled = LoadEnvInt("Artwork", 1, 0, 1);
	gui->progressEnabled = LoadEnvInt("ProgressBar", 0, 0, 1);
	LoadEnvString("LastDrawer", gui->lastDrawer, sizeof(gui->lastDrawer));
	SafeCopy(gui->statusText, sizeof(gui->statusText), "Ready.");
	SafeCopy(gui->fileInfoText, sizeof(gui->fileInfoText), "-");
	FormatRatingText(gui);
	SetFileDisplay(gui, NULL);

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
	nw.MaxHeight = 420;
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
	if (gui->timerOpen)
		SendTimerRequest(gui, TIMER_TICK_MICROS);
	return 0;
}

static void GuiClose(HelixAmp3Gui *gui)
{
	CancelArtDecode(gui);
	if (gui->playbackActive)
		StopPlayback(gui);
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
}

static void ChooseMp3(HelixAmp3Gui *gui)
{
	struct FileRequester *req;
	char path[HELIXAMP3_MAX_PATH];

	if (!gui->lastDrawer[0] && gui->inputName[0])
		CopyDrawerFromPath(gui->lastDrawer, sizeof(gui->lastDrawer),
			gui->inputName);
	req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Select MP3 for MiniAMP3",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.mp3",
		ASLFR_InitialDrawer,
			(ULONG)(gui->lastDrawer[0] ? gui->lastDrawer : NULL),
		TAG_DONE);
	if (!req) {
		SetStatus(gui, "Cannot allocate ASL file requester.");
		return;
	}
	if (AslRequest(req, NULL)) {
		path[0] = '\0';
		if (req->fr_Drawer && req->fr_Drawer[0]) {
			SafeCopy(gui->lastDrawer, sizeof(gui->lastDrawer),
				req->fr_Drawer);
			SafeCopy(path, sizeof(path), req->fr_Drawer);
			AddPart(path, req->fr_File, sizeof(path));
		} else {
			SafeCopy(path, sizeof(path), req->fr_File);
		}
		SafeCopy(gui->inputName, sizeof(gui->inputName), path);
		SetFileDisplay(gui, gui->inputName);
		ReadMp3Tags(gui->inputName, &gui->tags, gui->artEnabled);
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
	}
	FreeAslRequest(req);
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

	memset(args, 0, sizeof(*args));
	AddArg(args, "amiga_mp3dec");
	AddArg(args, "--play");
	if (gui->fastMem || gui->qualityIndex == 0 || gui->qualityIndex == 1)
		AddArg(args, "--fast-mem");
	if (gui->fastLowrate && strcmp(kRates[gui->rateIndex], "28600"))
		AddArg(args, "--fast-lowrate");
	if (gui->mono)
		AddArg(args, "--mono");
	AddArg(args, "--rate");
	AddArg(args, kRates[gui->rateIndex]);
	AddArg(args, "--buffer-seconds");
	sprintf(num, "%d", gui->bufferSeconds);
	AddArg(args, num);
	if (gui->qualityIndex == 0)
		AddArg(args, "--play-fast-path");
	if (gui->decodeThenPlay)
		AddArg(args, "--decode-then-play");
	if (gui->bench)
		AddArg(args, "--bench");
	AddArg(args, gui->inputName);
	args->argv[args->argc] = NULL;
}

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
	int ranDecoder;

	/* StartPlayback() already clears the stop flags before CreateNewProcTags().
	 * Do not clear them again here: Stop can be pressed after the GUI marks
	 * playback active but before this subprocess has entered the decoder.
	 * ResetDecoderStatics() clears decoder globals, so preserve an early Stop
	 * request and turn it back into an interrupt instead of letting the child
	 * run while the GUI is stuck in "Stopping...".
	 */
	stopBeforeStart = gGuiPlayer.stopRequested;
	ranDecoder = 0;
	gGuiPlaybackStatus.startupStage = GUISTART_CHILD_ENTERED;

	/* A freshly-created Process can inherit a pending Ctrl-C condition from
	 * DOS startup/runtime handling.  Clear only our playback interrupt bit
	 * before entering the decoder; StopPlayback() can set it again afterwards. */
	SetSignal(0, SIGBREAKF_CTRL_C);
	ResetCliParser();
	gGuiPlaybackStatus.startupStage = GUISTART_ARGS_READY;
	ResetDecoderStatics();
	gGuiPlaybackStatus.runId = gPlaybackEntryRunId;
	gGuiPlaybackStatus.startupStage = GUISTART_DECODER_CONFIG;

	/* MP3ResetStatics() may also touch command-line/playback globals in some
	 * decoder revisions, so establish the parser's initial state immediately
	 * before calling the renamed main() as well. */
	ResetCliParser();

	/* Stop may arrive while ResetDecoderStatics() is running.  Re-check the
	 * shared request afterwards so the reset cannot erase an early Stop. */
	if (stopBeforeStart || gGuiPlayer.stopRequested)
		gPlaybackInterrupted = 1;
	else {
		ranDecoder = 1;
		gGuiPlaybackStatus.startupStage = GUISTART_STREAM_INIT;
		HelixAmp3CliMain(gGuiPlayer.argc, gGuiPlayer.argv);
		gGuiPlaybackStatus.startupStage = GUISTART_CLEANUP;
	}
	if (!ranDecoder) {
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
		gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
		gGuiPlaybackStatus.cleanupComplete = 1;
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
	}
}

static void StartPlayback(HelixAmp3Gui *gui)
{
	BPTR dirLock;
	BPTR nilOut;
	struct Process *thisProc;

	if (!gui->inputName[0]) {
		SetStatus(gui, "Browse to an MP3 first.");
		return;
	}
	if (gui->playbackActive || gui->playbackDonePending) {
		SetStatus(gui, gui->playbackDonePending ?
			"Previous playback process is still exiting." :
			"Already playing; press Stop first.");
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
	CancelArtDecode(gui);
	DrawArtPanel(gui);
	gui->elapsedSecs = 0;
	gui->lastUnderrunCount = 0;
	gui->lastDisplayedSpareMs = 0;
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
	BuildPlaybackArgs(gui, &gGuiArgs);
	gGuiPlayer.argc = gGuiArgs.argc;
	gGuiPlayer.argv = gGuiArgs.argv;
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gDonePort = gui->donePort;
	gDoneRunId = 0;

	/* Give each playback process its own current-directory lock so relative
	 * paths remain resolvable across Stop/Play cycles.  DupLock(NULL) is safe
	 * and keeps the child behavior unchanged when no current directory exists.
	 */
	thisProc = (struct Process *)FindTask(NULL);
	dirLock = DupLock(thisProc ? thisProc->pr_CurrentDir : (BPTR)0);
#ifdef NDEBUG
	nilOut = Open((STRPTR)"NIL:", MODE_NEWFILE);
#else
	nilOut = (BPTR)0;
#endif

	if (nilOut) {
		gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
			NP_Name, (ULONG)"MiniAMP3 playback",
			NP_StackSize, 262144,
			NP_CurrentDir, dirLock,
			NP_Output, nilOut,
			NP_CloseOutput, TRUE,
			NP_CopyVars, FALSE,
			TAG_DONE);
	} else {
		gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
			NP_Name, (ULONG)"MiniAMP3 playback",
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
		gDonePort = NULL;
		SetStatus(gui, "Cannot start playback process.");
		return;
	}
	gui->playbackDonePending = 0;
	gui->playbackStoppedByUser = 0;
	gui->playbackActive = 1;
	SetStatus(gui, gui->decodeThenPlay ?
		"Decoding to RAM, then playing..." :
		"Streaming playback started.");
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
		SetStatus(gui, "Stop requested; waiting for audio cleanup...");
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
			SetStatus(gui, "Stopping playback process...");
			if (!PlaybackProcessStillExists())
				FinalizePlayback(gui);
			return;
		}
	}
	gGuiPlayer.stopRequested = 1;
	gPlaybackInterrupted = 1;
	/* Wake the playback subprocess immediately so it does not sit in WaitIO
	 * for the remainder of a multi-second audio buffer. */
	if (gGuiPlayer.process)
		Signal((struct Task *)gGuiPlayer.process, SIGBREAKF_CTRL_C);
	SetStatus(gui, "Stop requested; waiting for audio cleanup...");
}

static void HandleGuiAction(HelixAmp3Gui *gui, struct Gadget *gad, UWORD code)
{
	if (!gad)
		return;
	switch (gad->GadgetID) {
	case GID_BROWSE:
		ChooseMp3(gui);
		break;
	case GID_FAST_LOWRATE:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCB_Checked, gui->fastLowrate, TAG_DONE);
			SetStatus(gui, "Stop playback before changing rate mode.");
			break;
		}
		gui->fastLowrate = !gui->fastLowrate;
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->fastLowrate, TAG_DONE);
		SetStatus(gui, gui->fastLowrate ? "Fast-lowrate enabled." : "Fast-lowrate disabled.");
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
		SaveGuiSettings(gui);
		break;
	case GID_MONO:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCB_Checked, gui->mono, TAG_DONE);
			SetStatus(gui, "Stop playback before changing channel mode.");
			break;
		}
		gui->mono = !gui->mono;
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->mono, TAG_DONE);
		SetStatus(gui, gui->mono ? "Mono output enabled." : "Stereo output enabled.");
		SaveGuiSettings(gui);
		break;
	case GID_RATE:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gad, gui->win, NULL,
				GTCY_Active, gui->rateIndex, TAG_DONE);
			SetStatus(gui, "Stop playback before changing output rate.");
			break;
		}
		gui->rateIndex = code;
		if (gui->rateIndex < 0 || gui->rateIndex > 4)
			gui->rateIndex = 2;
		SetStatus(gui, "Output sample rate updated.");
		SaveGuiSettings(gui);
		break;
	case GID_BUFFER:
		if (gui->playbackActive || gui->playbackDonePending) {
			GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
				GTSL_Level, gui->bufferSeconds, TAG_DONE);
			SetStatus(gui, "Stop playback before changing buffer depth.");
			break;
		}
		gui->bufferSeconds = code;
		if (gui->bufferSeconds < 1)
			gui->bufferSeconds = 1;
		if (gui->bufferSeconds > 30)
			gui->bufferSeconds = 30;
		GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
			GTSL_Level, gui->bufferSeconds,
			TAG_DONE);
		SetStatus(gui, "Buffer depth updated.");
		SaveGuiSettings(gui);
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
		if (gui->qualityIndex < 0 || gui->qualityIndex > 2)
			gui->qualityIndex = 0;
		SetStatus(gui, "Quality profile updated.");
		SaveGuiSettings(gui);
		break;
	case GID_PLAY:
		StartPlayback(gui);
		break;
	case GID_STOP:
		StopPlayback(gui);
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
			HandleGuiAction(gui, gad, code);
		} else if (classValue == IDCMP_MOUSEMOVE) {
			if (gad && gad->GadgetID == GID_BUFFER)
				HandleGuiAction(gui, gad, code);
		}
	}
}

int main(int argc, char **argv)
{
	static HelixAmp3Gui gui;

	(void)argc;
	(void)argv;
	if (GuiOpen(&gui) != 0)
		return 1;
	while (!gui.closeRequested) {
		ULONG timerMask = gui.timerPort ? (1UL << gui.timerPort->mp_SigBit) : 0;
		ULONG doneMask = gui.donePort ? (1UL << gui.donePort->mp_SigBit) : 0;
		ULONG sigs = Wait(HELIXAMP3_SIGMASK(&gui) | timerMask |
			doneMask | SIGBREAKF_CTRL_C);
		if (sigs & SIGBREAKF_CTRL_C)
			gui.closeRequested = 1;
		if (doneMask && (sigs & doneMask))
			HandleDoneSignal(&gui);
		if (timerMask && (sigs & timerMask))
			HandleTimerSignal(&gui);
		GuiPoll(&gui);
	}
	if (gui.playbackActive) {
		StopPlayback(&gui);
		while (gui.playbackActive && gui.donePort) {
			ULONG doneMask = 1UL << gui.donePort->mp_SigBit;
			ULONG timerMask = gui.timerPort ?
				(1UL << gui.timerPort->mp_SigBit) : 0;
			ULONG sigs = Wait(doneMask | timerMask | SIGBREAKF_CTRL_C);

			if (sigs & SIGBREAKF_CTRL_C) {
				/* Do not tear down the done port while the playback process can
				 * still post gDoneMsg to it.  Keep the shutdown path deterministic
				 * and keep nudging the child until it acknowledges Stop.
				 */
				gGuiPlayer.stopRequested = 1;
				gPlaybackInterrupted = 1;
				if (gGuiPlayer.process)
					Signal((struct Task *)gGuiPlayer.process, SIGBREAKF_CTRL_C);
			}
			if (sigs & doneMask)
				HandleDoneSignal(&gui);
			if (timerMask && (sigs & timerMask))
				HandleTimerSignal(&gui);
		}
	}
	SaveGuiSettings(&gui);
	GuiClose(&gui);
	return 0;
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
