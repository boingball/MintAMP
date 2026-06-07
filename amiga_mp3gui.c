/*
 * HelixAMP3 - compact AmigaOS mini-player frontend for the Helix fixed-point
 * MP3 decoder.  The GUI wraps the existing amiga_mp3dec playback frontend so
 * the same Paula streaming path, fast-lowrate options, and buffer handling are
 * used from either Shell or Workbench.
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
#include <libraries/asl.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/asl.h>
#include <proto/dos.h>

#define HELIXAMP3_MAX_PATH 256
#define HELIXAMP3_ARGC_MAX 12
#define HELIXAMP3_SIGMASK(gui) (1UL << (gui)->win->UserPort->mp_SigBit)

#define GID_BROWSE 1
#define GID_PROFILE 2
#define GID_BUFFER_DOWN 3
#define GID_BUFFER_UP 4
#define GID_RATE 5
#define GID_PLAY 6
#define GID_STOP 7

#define HIT_NONE 0

typedef struct RectDef {
	int id;
	WORD left;
	WORD top;
	WORD right;
	WORD bottom;
} RectDef;

typedef struct HelixAmp3Gui {
	struct Window *win;
	char inputName[HELIXAMP3_MAX_PATH];
	char status[96];
	int profile;
	int bufferSeconds;
	int rateIndex;
	int closeRequested;
	int redrawRequested;
} HelixAmp3Gui;

typedef struct HelixAmp3Player {
	volatile int running;
	volatile int stopRequested;
	int argc;
	char *argv[HELIXAMP3_ARGC_MAX];
	char argvStorage[HELIXAMP3_ARGC_MAX][HELIXAMP3_MAX_PATH];
	struct Process *process;
} HelixAmp3Player;

static HelixAmp3Player gGuiPlayer;

static const char * const kProfiles[] = {
	"Fast",
	"Medium",
	"Slow"
};

static const char * const kRates[] = {
	"8287",
	"8820",
	"11025"
};

static const RectDef kRects[] = {
	{ GID_BROWSE,      388,  24, 476,  41 },
	{ GID_PROFILE,    104,  54, 206,  71 },
	{ GID_BUFFER_DOWN,104,  82, 126,  99 },
	{ GID_BUFFER_UP,  184,  82, 206,  99 },
	{ GID_RATE,       104, 110, 206, 127 },
	{ GID_PLAY,       254, 110, 342, 127 },
	{ GID_STOP,       366, 110, 454, 127 },
	{ HIT_NONE, 0, 0, 0, 0 }
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

static void DrawBox(struct RastPort *rp, const RectDef *r, const char *text)
{
	RectFill(rp, r->left, r->top, r->right, r->bottom);
	SetAPen(rp, 0);
	Move(rp, r->left + 6, r->top + 13);
	Text(rp, text, (LONG)strlen(text));
	SetAPen(rp, 1);
}

static const RectDef *FindRect(int id)
{
	int i;
	for (i = 0; kRects[i].id != HIT_NONE; i++) {
		if (kRects[i].id == id)
			return &kRects[i];
	}
	return NULL;
}

static int HitTest(WORD x, WORD y)
{
	int i;
	for (i = 0; kRects[i].id != HIT_NONE; i++) {
		if (x >= kRects[i].left && x <= kRects[i].right &&
			y >= kRects[i].top && y <= kRects[i].bottom)
			return kRects[i].id;
	}
	return HIT_NONE;
}

static void GuiRedraw(HelixAmp3Gui *gui)
{
	struct RastPort *rp;
	char bufferText[32];
	char fileText[80];
	const char *base;

	if (!gui->win)
		return;
	rp = gui->win->RPort;
	SetAPen(rp, 0);
	RectFill(rp, 8, 12, gui->win->Width - 9, gui->win->Height - 9);
	SetAPen(rp, 1);
	Move(rp, 16, 20);
	Text(rp, "HelixAMP3 mini MP3 player", 26);

	Move(rp, 16, 37);
	Text(rp, "MP3:", 4);
	base = gui->inputName[0] ? gui->inputName : "<choose a file>";
	SafeCopy(fileText, sizeof(fileText), base);
	Move(rp, 60, 37);
	Text(rp, fileText, (LONG)strlen(fileText));
	DrawBox(rp, FindRect(GID_BROWSE), "Browse");

	Move(rp, 16, 67);
	Text(rp, "Profile:", 8);
	DrawBox(rp, FindRect(GID_PROFILE), kProfiles[gui->profile]);

	Move(rp, 16, 95);
	Text(rp, "Buffer:", 7);
	DrawBox(rp, FindRect(GID_BUFFER_DOWN), "-");
	sprintf(bufferText, "%d sec", gui->bufferSeconds);
	Move(rp, 132, 95);
	Text(rp, bufferText, (LONG)strlen(bufferText));
	DrawBox(rp, FindRect(GID_BUFFER_UP), "+");

	Move(rp, 16, 123);
	Text(rp, "Rate:", 5);
	DrawBox(rp, FindRect(GID_RATE), kRates[gui->rateIndex]);
	DrawBox(rp, FindRect(GID_PLAY), gGuiPlayer.running ? "Playing" : "Play");
	DrawBox(rp, FindRect(GID_STOP), "Stop");

	Move(rp, 16, 151);
	Text(rp, gui->status, (LONG)strlen(gui->status));
}

static int GuiOpen(HelixAmp3Gui *gui)
{
	struct NewWindow nw;

	memset(gui, 0, sizeof(*gui));
	gui->profile = 1;
	gui->bufferSeconds = 4;
	gui->rateIndex = 0;
	SafeCopy(gui->status, sizeof(gui->status), "Choose an MP3, then Play. Stop asks playback to end.");
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = 40;
	nw.TopEdge = 30;
	nw.Width = 492;
	nw.Height = 170;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
		WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH;
	nw.Title = (UBYTE *)"HelixAMP3";
	nw.MinWidth = 320;
	nw.MinHeight = 150;
	nw.MaxWidth = 640;
	nw.MaxHeight = 220;
	nw.Type = WBENCHSCREEN;
	gui->win = OpenWindow(&nw);
	if (!gui->win) {
		fprintf(stderr, "cannot open HelixAMP3 window\n");
		return -1;
	}
	GuiRedraw(gui);
	return 0;
}

static void GuiClose(HelixAmp3Gui *gui)
{
	if (gui->win) {
		CloseWindow(gui->win);
		gui->win = NULL;
	}
}

static void GuiPoll(HelixAmp3Gui *gui)
{
	struct IntuiMessage *msg;
	ULONG classValue;
	UWORD code;
	WORD mx;
	WORD my;

	while (gui->win && (msg = (struct IntuiMessage *)GetMsg(gui->win->UserPort)) != NULL) {
		classValue = msg->Class;
		code = msg->Code;
		mx = msg->MouseX;
		my = msg->MouseY;
		ReplyMsg((struct Message *)msg);
		if (classValue == IDCMP_CLOSEWINDOW)
			gui->closeRequested = 1;
		else if (classValue == IDCMP_REFRESHWINDOW) {
			BeginRefresh(gui->win);
			GuiRedraw(gui);
			EndRefresh(gui->win, TRUE);
		} else if (classValue == IDCMP_MOUSEBUTTONS && code == SELECTUP) {
			int hit = HitTest(mx, my);
			if (hit != HIT_NONE) {
				gui->redrawRequested = hit;
				return;
			}
		}
	}
}

static void ChooseMp3(HelixAmp3Gui *gui)
{
	struct FileRequester *req;
	char path[HELIXAMP3_MAX_PATH];

	req = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Select MP3 for HelixAMP3",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.mp3",
		TAG_DONE);
	if (!req) {
		SafeCopy(gui->status, sizeof(gui->status), "Cannot allocate ASL file requester.");
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
		SafeCopy(gui->status, sizeof(gui->status), "Ready.");
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
	AddArg(player, "--buffer-seconds");
	sprintf(num, "%d", gui->bufferSeconds);
	AddArg(player, num);
	AddArg(player, "--rate");
	AddArg(player, kRates[gui->rateIndex]);
	if (gui->profile == 0) {
		AddArg(player, "--fast-mem");
		AddArg(player, "--play-fast-path");
	} else if (gui->profile == 1) {
		AddArg(player, "--fast-mem");
	}
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
		SafeCopy(gui->status, sizeof(gui->status), "Browse to an MP3 first.");
		return;
	}
	if (gGuiPlayer.running) {
		SafeCopy(gui->status, sizeof(gui->status), "Already playing; press Stop first.");
		return;
	}
	BuildPlaybackArgs(gui, &gGuiPlayer);
	gGuiPlayer.process = CreateNewProcTags(NP_Entry, (ULONG)PlaybackEntry,
		NP_Name, (ULONG)"HelixAMP3 playback",
		NP_StackSize, 32768,
		TAG_DONE);
	if (!gGuiPlayer.process) {
		SafeCopy(gui->status, sizeof(gui->status), "Cannot start playback process.");
		return;
	}
	gGuiPlayer.running = 1;
	SafeCopy(gui->status, sizeof(gui->status), "Playing through amiga_mp3dec Paula path.");
}

static void StopPlayback(HelixAmp3Gui *gui)
{
	if (!gGuiPlayer.running) {
		SafeCopy(gui->status, sizeof(gui->status), "Nothing is playing.");
		return;
	}
	gGuiPlayer.stopRequested = 1;
	gPlaybackInterrupted = 1;
	SafeCopy(gui->status, sizeof(gui->status), "Stop requested; waiting for playback loop to exit.");
}

static void HandleGuiAction(HelixAmp3Gui *gui, int action)
{
	switch (action) {
	case GID_BROWSE:
		ChooseMp3(gui);
		break;
	case GID_PROFILE:
		gui->profile = (gui->profile + 1) % 3;
		if (gui->profile == 0)
			SafeCopy(gui->status, sizeof(gui->status), "Fast: fast-mem and fast playback path enabled.");
		else if (gui->profile == 1)
			SafeCopy(gui->status, sizeof(gui->status), "Medium: fast-mem enabled, conservative speed options.");
		else
			SafeCopy(gui->status, sizeof(gui->status), "Slow: baseline playback options for best quality.");
		break;
	case GID_BUFFER_DOWN:
		if (gui->bufferSeconds > 1)
			gui->bufferSeconds--;
		break;
	case GID_BUFFER_UP:
		if (gui->bufferSeconds < 10)
			gui->bufferSeconds++;
		break;
	case GID_RATE:
		gui->rateIndex = (gui->rateIndex + 1) % 3;
		break;
	case GID_PLAY:
		StartPlayback(gui);
		break;
	case GID_STOP:
		StopPlayback(gui);
		break;
	}
	GuiRedraw(gui);
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
		if (gui.redrawRequested) {
			int action = gui.redrawRequested;
			gui.redrawRequested = 0;
			HandleGuiAction(&gui, action);
		} else if (!gGuiPlayer.running) {
			GuiRedraw(&gui);
		}
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
	fprintf(stderr, "HelixAMP3 GUI requires an AMIGA_M68K Intuition/ASL build.\n");
	fprintf(stderr, "Use amiga_mp3dec --play --rate 8287 --buffer-seconds 4 file.mp3 on this host.\n");
	return 1;
}

#endif
