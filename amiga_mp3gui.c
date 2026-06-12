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
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/gadtools.h>

#define HELIXAMP3_MAX_PATH 256
#define HELIXAMP3_ARGC_MAX 16
#define HELIXAMP3_SIGMASK(gui) (1UL << (gui)->win->UserPort->mp_SigBit)

enum {
	GID_FILE = 1,
	GID_BROWSE,
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

typedef struct HelixAmp3Gui {
	struct Window  *win;
	struct Gadget  *gadgets;
	struct Gadget  *gadContext;
	struct Gadget  *gadFile;
	struct Gadget  *gadStatus;
	struct Gadget  *gadBuffer;
	struct Gadget  *gadPlay;
	struct Gadget  *gadStop;
	struct VisualInfo *vi;
	char  inputName[HELIXAMP3_MAX_PATH];
	char  fileText[HELIXAMP3_MAX_PATH];
	char  statusText[128];
	int   fastLowrate;
	int   fastMem;
	int   mono;
	int   rateIndex;
	int   bufferSeconds;
	int   qualityIndex;
	int   closeRequested;
} HelixAmp3Gui;

typedef struct HelixAmp3Player {
	volatile int running;
	volatile int stopRequested;
	int argc;
	char *argv[HELIXAMP3_ARGC_MAX];
	char argvStorage[HELIXAMP3_ARGC_MAX][HELIXAMP3_MAX_PATH];
	struct Process *process;
} HelixAmp3Player;

struct IntuitionBase *IntuitionBase;
struct Library *AslBase;
struct Library *GadToolsBase;
static HelixAmp3Player gGuiPlayer;

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

static void SafeCopy(char *dst, size_t dstSize, const char *src)
{
	if (!dst || dstSize == 0)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, dstSize - 1);
	dst[dstSize - 1] = '\0';
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
	if (kind == BUTTON_KIND)
		ng.ng_Flags = PLACETEXT_IN;
	else if (kind == CHECKBOX_KIND)
		ng.ng_Flags = PLACETEXT_RIGHT;
	else
		ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_VisualInfo = gui->vi;
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
		55, 16, 280, 14, "File:",
		GTTX_Text, (ULONG)gui->fileText,
		GTTX_Border, TRUE,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
		345, 14, 68, 16, "Browse...",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_LOWRATE,
		22, 46, 20, 12, "Fast-lowrate",
		GTCB_Checked, gui->fastLowrate,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_FAST_MEM,
		160, 46, 20, 12, "Fast-mem",
		GTCB_Checked, gui->fastMem,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_MONO,
		278, 46, 20, 12, "Mono",
		GTCB_Checked, gui->mono,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CYCLE_KIND, GID_RATE,
		55, 76, 98, 16, "Rate:",
		GTCY_Labels, (ULONG)kRateLabels,
		GTCY_Active, gui->rateIndex,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gad = MakeGadget(gui, gad, CYCLE_KIND, GID_QUALITY,
		238, 76, 112, 16, "Quality:",
		GTCY_Labels, (ULONG)kQualityLabels,
		GTCY_Active, gui->qualityIndex,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadBuffer = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_BUFFER,
		70, 108, 220, 16, "Buffer:",
		GTSL_Min, 1,
		GTSL_Max, 30,
		GTSL_Level, gui->bufferSeconds,
		GTSL_LevelFormat, (ULONG)"%ld sec");
	if (!gad)
		return -1;

	gui->gadPlay = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAY,
		45, 140, 72, 18, "Play",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
		310, 140, 72, 18, "Stop",
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0,
		TAG_IGNORE, 0);
	if (!gad)
		return -1;

	gui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
		58, 174, 350, 14, "Status:",
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

	screen = LockPubScreen(NULL);
	if (!screen) {
		fprintf(stderr, "cannot lock Workbench screen\n");
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	gui->vi = GetVisualInfo(screen, TAG_DONE);
	UnlockPubScreen(NULL, screen);
	if (!gui->vi) {
		fprintf(stderr, "cannot create GadTools visual info\n");
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
		FreeGadList(gui->gadgets);
		gui->gadgets = NULL;
		FreeVisualInfo(gui->vi);
		gui->vi = NULL;
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
	nw.Width = 420;
	nw.Height = 200;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_MOUSEMOVE | IDCMP_CLOSEWINDOW |
		IDCMP_REFRESHWINDOW | IDCMP_ACTIVEWINDOW;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
		WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE |
		WFLG_SMART_REFRESH;
	nw.FirstGadget = gui->gadgets;
	nw.Title = (UBYTE *)"MiniAMP3";
	nw.MinWidth = 420;
	nw.MinHeight = 200;
	nw.MaxWidth = 640;
	nw.MaxHeight = 256;
	nw.Type = WBENCHSCREEN;
	gui->win = OpenWindow(&nw);
	if (!gui->win) {
		fprintf(stderr, "cannot open MiniAMP3 window\n");
		FreeGadList(gui->gadgets);
		gui->gadgets = NULL;
		FreeVisualInfo(gui->vi);
		gui->vi = NULL;
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
		CloseLibrary(AslBase);
		AslBase = NULL;
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		return -1;
	}
	GT_RefreshWindow(gui->win, NULL);
	return 0;
}

static void GuiClose(HelixAmp3Gui *gui)
{
	if (gui->win) {
		CloseWindow(gui->win);
		gui->win = NULL;
	}
	if (gui->gadgets) {
		FreeGadList(gui->gadgets);
		gui->gadgets = NULL;
	}
	if (gui->vi) {
		FreeVisualInfo(gui->vi);
		gui->vi = NULL;
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
		SetStatus(gui, "File chosen. Press Play to start.");
	}
	FreeAslRequest(req);
}

static void AddArg(HelixAmp3Player *player, const char *text)
{
	if (player->argc >= HELIXAMP3_ARGC_MAX)
		return;
	SafeCopy(player->argvStorage[player->argc], HELIXAMP3_MAX_PATH, text);
	player->argv[player->argc] = player->argvStorage[player->argc];
	player->argc++;
}

static void BuildPlaybackArgs(HelixAmp3Gui *gui, HelixAmp3Player *player)
{
	char num[16];

	memset(player, 0, sizeof(*player));
	AddArg(player, "amiga_mp3dec");
	AddArg(player, "--play");
	if (gui->fastMem || gui->qualityIndex == 0 || gui->qualityIndex == 1)
		AddArg(player, "--fast-mem");
	if (gui->fastLowrate)
		AddArg(player, "--fast-lowrate");
	if (gui->mono)
		AddArg(player, "--mono");
	AddArg(player, "--rate");
	AddArg(player, kRates[gui->rateIndex]);
	AddArg(player, "--buffer-seconds");
	sprintf(num, "%d", gui->bufferSeconds);
	AddArg(player, num);
	if (gui->qualityIndex == 0)
		AddArg(player, "--play-fast-path");
	AddArg(player, gui->inputName);
	player->argv[player->argc] = NULL;
}

static void PlaybackEntry(void)
{
	gGuiPlayer.running = 1;
	gGuiPlayer.stopRequested = 0;
	gPlaybackInterrupted = 0;
	HelixAmp3CliMain(gGuiPlayer.argc, gGuiPlayer.argv);
	gGuiPlayer.running = 0;
	Forbid();
	gGuiPlayer.process = NULL;
	Permit();
}

static void StartPlayback(HelixAmp3Gui *gui)
{
	if (!gui->inputName[0]) {
		SetStatus(gui, "Browse to an MP3 first.");
		return;
	}
	if (gGuiPlayer.running) {
		SetStatus(gui, "Already playing; press Stop first.");
		return;
	}
	BuildPlaybackArgs(gui, &gGuiPlayer);
	gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
		NP_Name, (ULONG)"MiniAMP3 playback",
		NP_StackSize, 32768,
		TAG_DONE);
	if (!gGuiPlayer.process) {
		SetStatus(gui, "Cannot start playback process.");
		return;
	}
	gGuiPlayer.running = 1;
	SetStatus(gui, "Playing through amiga_mp3dec Paula path.");
}

static void StopPlayback(HelixAmp3Gui *gui)
{
	if (!gGuiPlayer.running) {
		SetStatus(gui, "Nothing is playing.");
		return;
	}
	gGuiPlayer.stopRequested = 1;
	gPlaybackInterrupted = 1;
	SetStatus(gui, "Stop requested; waiting for playback loop to exit.");
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
			GT_BeginRefresh(gui->win);
			GT_EndRefresh(gui->win, TRUE);
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
		Wait(HELIXAMP3_SIGMASK(&gui) | SIGBREAKF_CTRL_C);
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			gui.closeRequested = 1;
		GuiPoll(&gui);
	}
	if (gGuiPlayer.running)
		StopPlayback(&gui);
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
