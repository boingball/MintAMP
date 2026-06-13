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

#ifdef AMIGA_M68K
#include <exec/types.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
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
#include <proto/timer.h>

#define HELIXAMP3_MAX_PATH 256
#define HELIXAMP3_ARGC_MAX 18
#define HELIXAMP3_SIGMASK(gui) (1UL << (gui)->win->UserPort->mp_SigBit)

#define GUI_LEFT_MARGIN   8
#define GUI_TOP_Y        16
#define GUI_ROW_H        14
#define ROW_FILE    (GUI_TOP_Y + 0 * GUI_ROW_H)
#define ROW_TITLE   (GUI_TOP_Y + 1 * GUI_ROW_H)
#define ROW_ARTIST  (GUI_TOP_Y + 2 * GUI_ROW_H)
#define ROW_ALBUM   (GUI_TOP_Y + 3 * GUI_ROW_H)
#define ROW_CHECKS  (GUI_TOP_Y + 4 * GUI_ROW_H + 4)
#define ROW_CYCLES  (GUI_TOP_Y + 5 * GUI_ROW_H + 4)
#define ROW_BUFFER  (GUI_TOP_Y + 6 * GUI_ROW_H + 4)
#define ROW_PROG    (GUI_TOP_Y + 7 * GUI_ROW_H + 4)
#define ROW_BUTTONS (GUI_TOP_Y + 8 * GUI_ROW_H + 8)
#define ROW_STATUS  (GUI_TOP_Y + 9 * GUI_ROW_H + 8)
#define WIN_W       460
#define WIN_H       (ROW_STATUS + GUI_ROW_H + 12)

#define PROG_X      60
#define PROG_W     300
#define PROG_H       8
#define PROG_TOP_Y ROW_PROG
#define TIME_X     (PROG_X + PROG_W + 8)
#define TIME_W      88
#define TIMER_TICK_MICROS 1000000UL

#define MENUNUM_PROJECT   0
#define MENUNUM_PLAYBACK  1
#define ITEMNUM_ABOUT     0
#define ITEMNUM_QUIT      1
#define ITEMNUM_DTP       0
#define ITEMNUM_BENCH     1

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
	GID_COUNT
};

typedef struct Mp3Tags {
	char title[64];
	char artist[64];
	char album[64];
	int  bitrateKbps;
	int  sampleRate;
	int  durationSecs;
} Mp3Tags;

typedef struct HelixAmp3Gui {
	struct Window  *win;
	struct Gadget  *gadgets;
	struct Gadget  *gadContext;
	struct Gadget  *gadFile;
	struct Gadget  *gadTitle;
	struct Gadget  *gadArtist;
	struct Gadget  *gadAlbum;
	struct Gadget  *gadStatus;
	struct Gadget  *gadBuffer;
	struct Gadget  *gadPlay;
	struct Gadget  *gadStop;
	struct VisualInfo *visualInfo;
	struct Menu *menuStrip;
	struct MsgPort *timerPort;
	struct MsgPort *donePort;
	struct timerequest *timerReq;
	struct TextFont *smallFont;
	struct TextAttr smallTextAttr;
	int timerOpen;
	int timerPending;
	Mp3Tags tags;
	char  inputName[HELIXAMP3_MAX_PATH];
	char  fileText[HELIXAMP3_MAX_PATH];
	char  statusText[128];
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
	int   totalSecs;
	int   elapsedSecs;
	int   launchBufferSecs;
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
struct GfxBase *GfxBase;
static HelixAmp3Player gGuiPlayer;
static HelixAmp3Args gGuiArgs;
static struct Message gDoneMsg;
static struct MsgPort *gDonePort;

static const char * const kRates[] = {
	"8287",
	"8820",
	"11025",
	"22050"
};

static const STRPTR kRateLabels[] = {
	(STRPTR)"8287",
	(STRPTR)"8820",
	(STRPTR)"11025",
	(STRPTR)"22050",
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

static void CopyTagField(char *dst, size_t dstSize, const unsigned char *src,
	long len)
{
	long i;
	long start;
	long out;

	if (!dst || dstSize == 0)
		return;
	dst[0] = '\0';
	if (!src || len <= 0)
		return;
	start = 0;
	if (src[0] == 0 || src[0] == 3)
		start = 1;
	out = 0;
	for (i = start; i < len && out + 1 < (long)dstSize; i++) {
		unsigned char c = src[i];
		if (c == '\0')
			break;
		dst[out++] = (char)c;
	}
	dst[out] = '\0';
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
		CopyTagField(tags->title, sizeof(tags->title), buf + 3, 30);
	if (!tags->artist[0])
		CopyTagField(tags->artist, sizeof(tags->artist), buf + 33, 30);
	if (!tags->album[0])
		CopyTagField(tags->album, sizeof(tags->album), buf + 63, 30);
}

static void ReadId3v2Frames(FILE *f, Mp3Tags *tags, const unsigned char *hdr)
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
		target = NULL;
		if ((version == 2 && strcmp(id, "TT2") == 0) || strcmp(id, "TIT2") == 0)
			target = tags->title;
		else if ((version == 2 && strcmp(id, "TP1") == 0) || strcmp(id, "TPE1") == 0)
			target = tags->artist;
		else if ((version == 2 && strcmp(id, "TAL") == 0) || strcmp(id, "TALB") == 0)
			target = tags->album;
		if (target && !target[0]) {
			unsigned char text[96];
			long n = frameSize;
			if (n > (long)sizeof(text))
				n = (long)sizeof(text);
			if (fread(text, 1, (size_t)n, f) == (size_t)n)
				CopyTagField(target, 64, text, n);
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

static void ReadMp3Tags(const char *path, Mp3Tags *tags)
{
	FILE *f;
	unsigned char hdr[10];
	long firstFrameOffset;
	int hadId3v2;

	if (!tags)
		return;
	memset(tags, 0, sizeof(*tags));
	f = fopen(path, "rb");
	if (!f)
		return;
	hadId3v2 = 0;
	firstFrameOffset = -1L;
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "ID3", 3) == 0) {
		hadId3v2 = 1;
		ReadId3v2Frames(f, tags, hdr);
	} else {
		fseek(f, 0, SEEK_SET);
	}
	ReadMpegInfo(f, tags, &firstFrameOffset);
	if (tags->bitrateKbps > 0 && firstFrameOffset >= 0) {
		long fileSize;
		long audioBytes;

		if (fseek(f, 0, SEEK_END) == 0) {
			fileSize = ftell(f);
			audioBytes = fileSize - firstFrameOffset;
			if (audioBytes > 0)
				tags->durationSecs = (int)(audioBytes * 8L /
					((long)tags->bitrateKbps * 1000L));
		}
	}
	if (!hadId3v2)
		ReadId3v1(f, tags);
	fclose(f);
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
		SetAPen(rp, 3);
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
	if (!gui->timerReq || gui->timerPending)
		return;
	gui->timerReq->tr_node.io_Command = TR_ADDREQUEST;
	gui->timerReq->tr_time.tv_secs = micros / 1000000UL;
	gui->timerReq->tr_time.tv_micro = micros % 1000000UL;
	SendIO((struct IORequest *)gui->timerReq);
	gui->timerPending = 1;
}

static void HandleTimerSignal(HelixAmp3Gui *gui)
{
	if (!gui->timerReq)
		return;
	while (GetMsg(gui->timerPort))
		;
	gui->timerPending = 0;

	if (gui->playbackActive) {
		gui->elapsedSecs++;
		DrawProgress(gui);
	}
	SendTimerRequest(gui, TIMER_TICK_MICROS);
}

static void HandleDoneSignal(HelixAmp3Gui *gui)
{
	struct Message *msg;
	int stoppedByUser;

	if (!gui->donePort)
		return;
	while ((msg = GetMsg(gui->donePort)) != NULL)
		;

	stoppedByUser = gGuiPlayer.stopRequested;
	gui->playbackActive = 0;
	gGuiPlayer.process = NULL;
	gDonePort = NULL;
	if (gui->totalSecs > 0)
		gui->elapsedSecs = gui->totalSecs + gui->launchBufferSecs;
	DrawProgress(gui);
	SetStatus(gui, stoppedByUser ? "Stopped." : "Playback finished.");
	if (stoppedByUser)
		gGuiPlayer.stopRequested = 0;
}

static void GuiRefresh(HelixAmp3Gui *gui)
{
	if (!gui->win)
		return;
	GT_BeginRefresh(gui->win);
	GT_EndRefresh(gui->win, TRUE);
	DrawProgressFrame(gui);
	DrawProgress(gui);
}

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
	ng.ng_TextAttr = &gui->smallTextAttr;
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
		55, ROW_FILE, 280, 14, "File:",
		GTTX_Text, (ULONG)gui->fileText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
		345, ROW_FILE - 2, 68, 16, "Browse",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadTitle = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TITLE,
		55, ROW_TITLE, 300, 14, "Title:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadArtist = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ARTIST,
		55, ROW_ARTIST, 300, 14, "Artist:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadAlbum = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ALBUM,
		55, ROW_ALBUM, 300, 14, "Album:",
		GTTX_Text, (ULONG)"-",
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_LOWRATE,
		22, ROW_CHECKS, 20, 12, "Fast-lr",
		GTCB_Checked, gui->fastLowrate,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_MEM,
		160, ROW_CHECKS, 20, 12, "Fast-mem",
		GTCB_Checked, gui->fastMem,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_MONO,
		278, ROW_CHECKS, 20, 12, "Mono",
		GTCB_Checked, gui->mono,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CYCLE_KIND, GID_RATE,
		55, ROW_CYCLES, 98, 16, "Rate:",
		GTCY_Labels, (ULONG)kRateLabels,
		GTCY_Active, gui->rateIndex,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CYCLE_KIND, GID_QUALITY,
		238, ROW_CYCLES, 112, 16, "Quality:",
		GTCY_Labels, (ULONG)kQualityLabels,
		GTCY_Active, gui->qualityIndex,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadBuffer = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_BUFFER,
		70, ROW_BUFFER, 220, 16, "Buffer:",
		GTSL_Min, 1,
		GTSL_Max, 30,
		GTSL_Level, gui->bufferSeconds,
		GTSL_LevelFormat, (ULONG)"%ld sec");
	if (!gad)
		return -1;

	gui->gadPlay = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAY,
		45, ROW_BUTTONS, 72, 18, "Play",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
		310, ROW_BUTTONS, 72, 18, "Stop",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
		58, ROW_STATUS, 350, 14, "Status:",
		GTTX_Text, (ULONG)gui->statusText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	return 0;
}

static int GuiOpen(HelixAmp3Gui *gui)
{
	struct NewWindow nw;
	struct Screen *screen;

	memset(gui, 0, sizeof(*gui));
	gui->fastLowrate = 1;
	gui->fastMem = 1;
	gui->mono = 1;
	gui->rateIndex = 2;
	gui->bufferSeconds = 10;
	gui->qualityIndex = 0;
	SafeCopy(gui->statusText, sizeof(gui->statusText), "Ready.");
	SetFileDisplay(gui, NULL);

	gui->smallTextAttr.ta_Name = (STRPTR)"topaz.font";
	gui->smallTextAttr.ta_YSize = 8;
	gui->smallTextAttr.ta_Style = FS_NORMAL;
	gui->smallTextAttr.ta_Flags = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
	if (!IntuitionBase) {
		fprintf(stderr, "MiniAMP3 requires intuition.library V37 or newer.\n");
		return -1;
	}
	AslBase = OpenLibrary("asl.library", 37);
	if (!AslBase) {
		fprintf(stderr, "MiniAMP3 requires asl.library V37 or newer.\n");
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	GadToolsBase = OpenLibrary("gadtools.library", 37);
	if (!GadToolsBase) {
		fprintf(stderr, "MiniAMP3 requires gadtools.library V37 or newer.\n");
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
	if (!GfxBase) {
		fprintf(stderr, "MiniAMP3 requires graphics.library V37 or newer.\n");
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	gui->smallFont = OpenFont(&gui->smallTextAttr);

	screen = LockPubScreen(NULL);
	if (!screen) {
		fprintf(stderr, "cannot lock Workbench screen\n");
		if (gui->smallFont) {
			CloseFont(gui->smallFont);
			gui->smallFont = NULL;
		}
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = NULL;
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	gui->visualInfo = GetVisualInfo(screen, TAG_DONE);
	UnlockPubScreen(NULL, screen);
	if (!gui->visualInfo) {
		fprintf(stderr, "cannot create GadTools visual info\n");
		if (gui->smallFont) {
			CloseFont(gui->smallFont);
			gui->smallFont = NULL;
		}
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = NULL;
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}

	if (GuiCreateGadgets(gui) != 0) {
		fprintf(stderr, "cannot create MiniAMP3 gadgets\n");
		FreeGadgets(gui->gadgets);
		gui->gadgets = NULL;
		FreeVisualInfo(gui->visualInfo);
		gui->visualInfo = NULL;
		if (gui->smallFont) {
			CloseFont(gui->smallFont);
			gui->smallFont = NULL;
		}
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = NULL;
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}

	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = 40;
	nw.TopEdge = 30;
	nw.Width = WIN_W;
	nw.Height = WIN_H;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_MOUSEMOVE | IDCMP_CLOSEWINDOW |
		IDCMP_REFRESHWINDOW | IDCMP_ACTIVEWINDOW | IDCMP_MENUPICK;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
		WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE |
		WFLG_SMART_REFRESH;
	nw.FirstGadget = gui->gadgets;
	nw.Title = (UBYTE *)"MiniAMP3";
	nw.MinWidth = WIN_W;
	nw.MinHeight = WIN_H;
	nw.MaxWidth = 680;
	nw.MaxHeight = 220;
	nw.Type = WBENCHSCREEN;
	gui->win = OpenWindow(&nw);
	if (!gui->win) {
		fprintf(stderr, "cannot open MiniAMP3 window\n");
		FreeGadgets(gui->gadgets);
		gui->gadgets = NULL;
		FreeVisualInfo(gui->visualInfo);
		gui->visualInfo = NULL;
		if (gui->smallFont) {
			CloseFont(gui->smallFont);
			gui->smallFont = NULL;
		}
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = NULL;
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	gui->menuStrip = CreateMenus(myNewMenus, TAG_DONE);
	if (gui->menuStrip) {
		LayoutMenus(gui->menuStrip, gui->visualInfo, TAG_DONE);
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
	if (gui->timerOpen)
		SendTimerRequest(gui, TIMER_TICK_MICROS);
	return 0;
}

static void GuiClose(HelixAmp3Gui *gui)
{
	if (gui->timerReq) {
		if (gui->timerPending) {
			AbortIO((struct IORequest *)gui->timerReq);
			WaitIO((struct IORequest *)gui->timerReq);
			gui->timerPending = 0;
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
	if (gui->win && gui->menuStrip)
		ClearMenuStrip(gui->win);
	if (gui->menuStrip) {
		FreeMenus(gui->menuStrip);
		gui->menuStrip = NULL;
	}
	if (gui->win) {
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

	req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Select MP3 for MiniAMP3",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.mp3",
		TAG_DONE);
	if (!req) {
		SetStatus(gui, "Cannot allocate ASL file requester.");
		return;
	}
	if (AslRequest(req, NULL)) {
		path[0] = '\0';
		if (req->fr_Drawer && req->fr_Drawer[0]) {
			SafeCopy(path, sizeof(path), req->fr_Drawer);
			AddPart(path, req->fr_File, sizeof(path));
		} else {
			SafeCopy(path, sizeof(path), req->fr_File);
		}
		SafeCopy(gui->inputName, sizeof(gui->inputName), path);
		SetFileDisplay(gui, gui->inputName);
		ReadMp3Tags(gui->inputName, &gui->tags);
		gui->totalSecs = gui->tags.durationSecs;
		gui->elapsedSecs = 0;
		UpdateTagDisplay(gui);
		DrawProgress(gui);
		FormatReadyStatus(&gui->tags, gui->statusText, sizeof(gui->statusText));
		SetStatus(gui, gui->statusText);
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
	if (gui->fastLowrate)
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

static void ResetDecoderStatics(void)
{
	extern int MP3ResetStatics(void);

	MP3ResetStatics();
}

static void PlaybackEntry(void)
{
	struct MsgPort *donePort;

	ResetDecoderStatics();
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	HelixAmp3CliMain(gGuiPlayer.argc, gGuiPlayer.argv);
	donePort = gDonePort;
	gDonePort = NULL;
	gGuiPlayer.process = NULL;
	if (donePort)
		PutMsg(donePort, &gDoneMsg);
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
	if (gui->playbackActive) {
		SetStatus(gui, "Already playing; press Stop first.");
		return;
	}
	if (!gui->donePort) {
		SetStatus(gui, "Cannot start playback: no done port.");
		return;
	}
	gui->elapsedSecs = 0;
	gui->launchBufferSecs = gui->decodeThenPlay ? 0 : gui->bufferSeconds;
	DrawProgress(gui);
	BuildPlaybackArgs(gui, &gGuiArgs);
	gGuiPlayer.argc = gGuiArgs.argc;
	gGuiPlayer.argv = gGuiArgs.argv;
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	gDonePort = gui->donePort;

	/* Give each playback process its own current-directory lock so relative
	 * paths remain resolvable across Stop/Play cycles.  DupLock(NULL) is safe
	 * and keeps the child behavior unchanged when no current directory exists.
	 */
	thisProc = (struct Process *)FindTask(NULL);
	dirLock = DupLock(thisProc ? thisProc->pr_CurrentDir : (BPTR)0);
	nilOut = Open((STRPTR)"NIL:", MODE_NEWFILE);

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
	if (gGuiPlayer.stopRequested) {
		SetStatus(gui, "Stopping...");
		return;
	}
	gGuiPlayer.stopRequested = 1;
	gPlaybackInterrupted = 1;
	/* Wake the playback subprocess immediately so it does not sit in WaitIO
	 * for the remainder of a multi-second audio buffer. */
	if (gGuiPlayer.process)
		Signal((struct Task *)gGuiPlayer.process, SIGBREAKF_CTRL_C);
	SetStatus(gui, "Stopping...");
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
		gui->fastLowrate = !gui->fastLowrate;
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->fastLowrate, TAG_DONE);
		SetStatus(gui, gui->fastLowrate ? "Fast-lowrate enabled." : "Fast-lowrate disabled.");
		break;
	case GID_FAST_MEM:
		gui->fastMem = !gui->fastMem;
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->fastMem, TAG_DONE);
		SetStatus(gui, gui->fastMem ? "Fast memory path enabled." : "Fast memory path disabled.");
		break;
	case GID_MONO:
		gui->mono = !gui->mono;
		GT_SetGadgetAttrs(gad, gui->win, NULL, GTCB_Checked, gui->mono, TAG_DONE);
		SetStatus(gui, gui->mono ? "Mono output enabled." : "Stereo output enabled.");
		break;
	case GID_RATE:
		gui->rateIndex = code;
		if (gui->rateIndex < 0 || gui->rateIndex > 3)
			gui->rateIndex = 2;
		SetStatus(gui, "Output sample rate updated.");
		break;
	case GID_BUFFER:
		gui->bufferSeconds = code;
		if (gui->bufferSeconds < 1)
			gui->bufferSeconds = 1;
		if (gui->bufferSeconds > 30)
			gui->bufferSeconds = 30;
		GT_SetGadgetAttrs(gui->gadBuffer, gui->win, NULL,
			GTSL_Level, gui->bufferSeconds,
			TAG_DONE);
		SetStatus(gui, "Buffer depth updated.");
		break;
	case GID_QUALITY:
		gui->qualityIndex = code;
		if (gui->qualityIndex < 0 || gui->qualityIndex > 2)
			gui->qualityIndex = 0;
		SetStatus(gui, "Quality profile updated.");
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
						SetStatus(gui, gui->bench ?
							"Bench mode enabled." :
							"Bench mode disabled.");
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
	HelixAmp3Gui gui;

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
			ULONG sigs = Wait(doneMask | SIGBREAKF_CTRL_C);
			if (sigs & doneMask)
				HandleDoneSignal(&gui);
			if (sigs & SIGBREAKF_CTRL_C)
				break;
		}
	}
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
