/*
 * Minimal AmigaOS Intuition frontend for the Helix fixed-point MP3 decoder.
 *
 * This program intentionally links the same decoder objects as amiga_mp3dec.c
 * instead of requiring a separate library build.  It writes signed 16-bit
 * big-endian PCM and keeps the UI deliberately small: a status window with the
 * standard close gadget, which doubles as an abort button during decoding.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp3dec.h"

#define GUI_READBUF_SIZE (1024 * 16)
#define GUI_OUTBUF_SAMPS (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

#ifdef AMIGA_M68K
#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>


typedef struct GuiState {
	struct Window *win;
	char line1[80];
	char line2[80];
	int abortRequested;
} GuiState;

static void GuiRedraw(GuiState *gui);

static int GuiOpen(GuiState *gui)
{
	struct NewWindow nw;

	memset(gui, 0, sizeof(*gui));
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = 40;
	nw.TopEdge = 30;
	nw.Width = 520;
	nw.Height = 96;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW;
	nw.Flags = WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
		WFLG_ACTIVATE | WFLG_SIMPLE_REFRESH;
	nw.FirstGadget = NULL;
	nw.CheckMark = NULL;
	nw.Title = (UBYTE *)"Helix MP3 Decoder";
	nw.Screen = NULL;
	nw.BitMap = NULL;
	nw.MinWidth = 240;
	nw.MinHeight = 60;
	nw.MaxWidth = 640;
	nw.MaxHeight = 200;
	nw.Type = WBENCHSCREEN;

	gui->win = OpenWindow(&nw);
	if (!gui->win) {
		fprintf(stderr, "cannot open Intuition window\n");
		return -1;
	}
	return 0;
}

static void GuiClose(GuiState *gui)
{
	if (gui->win) {
		CloseWindow(gui->win);
		gui->win = NULL;
	}
}


static void GuiPoll(GuiState *gui)
{
	struct IntuiMessage *msg;
	ULONG classValue;

	if (!gui->win || !gui->win->UserPort)
		return;
	while ((msg = (struct IntuiMessage *)GetMsg(gui->win->UserPort)) != NULL) {
		classValue = msg->Class;
		ReplyMsg((struct Message *)msg);
		if (classValue == IDCMP_CLOSEWINDOW)
			gui->abortRequested = 1;
		else if (classValue == IDCMP_REFRESHWINDOW && gui->win) {
			BeginRefresh(gui->win);
			GuiRedraw(gui);
			EndRefresh(gui->win, TRUE);
		}
	}
}

static void GuiRedraw(GuiState *gui)
{
	struct RastPort *rp;

	if (!gui->win)
		return;
	rp = gui->win->RPort;
	SetAPen(rp, 0);
	RectFill(rp, 8, 18, gui->win->Width - 9, gui->win->Height - 9);
	SetAPen(rp, 1);
	Move(rp, 16, 36);
	Text(rp, gui->line1, (LONG)strlen(gui->line1));
	Move(rp, 16, 58);
	Text(rp, gui->line2, (LONG)strlen(gui->line2));
}

static void GuiSetStatus(GuiState *gui, const char *line1, const char *line2)
{
	if (line1) {
		strncpy(gui->line1, line1, sizeof(gui->line1) - 1);
		gui->line1[sizeof(gui->line1) - 1] = '\0';
	}
	if (line2) {
		strncpy(gui->line2, line2, sizeof(gui->line2) - 1);
		gui->line2[sizeof(gui->line2) - 1] = '\0';
	}
	GuiRedraw(gui);
	GuiPoll(gui);
}

static int GuiWaitForClose(GuiState *gui)
{
	while (!gui->abortRequested) {
		Wait(1UL << gui->win->UserPort->mp_SigBit);
		GuiPoll(gui);
	}
	return 0;
}
#else

typedef struct GuiState {
	int abortRequested;
} GuiState;

static int GuiOpen(GuiState *gui)
{
	memset(gui, 0, sizeof(*gui));
	fprintf(stderr, "amiga_mp3gui needs an AMIGA_M68K Intuition build\n");
	return -1;
}

static void GuiClose(GuiState *gui)
{
	(void)gui;
}

static void GuiSetStatus(GuiState *gui, const char *line1, const char *line2)
{
	(void)gui;
	if (line1)
		fprintf(stderr, "%s\n", line1);
	if (line2)
		fprintf(stderr, "%s\n", line2);
}
#endif

static int FillReadBuffer(unsigned char *buffer, unsigned char **readPtr,
	int *bytesLeft, FILE *input, int *eofReached)
{
	int nRead;

	if (*bytesLeft > 0 && *readPtr != buffer)
		memmove(buffer, *readPtr, *bytesLeft);
	*readPtr = buffer;
	nRead = (int)fread(buffer + *bytesLeft, 1,
		GUI_READBUF_SIZE - *bytesLeft, input);
	if (nRead <= 0)
		*eofReached = 1;
	else
		*bytesLeft += nRead;
	return nRead;
}

static int WriteBigEndianPcm(FILE *output, const short *samples, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		unsigned short sample = (unsigned short)samples[i];
		if (fputc((sample >> 8) & 0xff, output) == EOF ||
			fputc(sample & 0xff, output) == EOF) {
			return -1;
		}
	}
	return 0;
}

static int DecodeMp3ToPcm(const char *inputName, const char *outputName,
	GuiState *gui)
{
	FILE *input;
	FILE *output;
	HMP3Decoder decoder;
	MP3FrameInfo info;
	unsigned char readBuf[GUI_READBUF_SIZE];
	unsigned char *readPtr;
	short decodeBuf[GUI_OUTBUF_SAMPS];
	int bytesLeft;
	int eofReached;
	unsigned long frames;
	unsigned long samples;
	char status[80];
	int result;

	input = NULL;
	output = NULL;
	decoder = NULL;
	readPtr = readBuf;
	bytesLeft = 0;
	eofReached = 0;
	frames = 0;
	samples = 0;
	result = 1;
	memset(&info, 0, sizeof(info));

	input = fopen(inputName, "rb");
	if (!input) {
		GuiSetStatus(gui, "Cannot open MP3 input", inputName);
		goto cleanup;
	}
	output = fopen(outputName, "wb");
	if (!output) {
		GuiSetStatus(gui, "Cannot create PCM output", outputName);
		goto cleanup;
	}
	decoder = MP3InitDecoder();
	if (!decoder) {
		GuiSetStatus(gui, "MP3InitDecoder failed", "Out of memory?");
		goto cleanup;
	}

	GuiSetStatus(gui, "Decoding MP3 to signed 16-bit PCM", "Close window to abort");
	while (!gui->abortRequested) {
		int offset;
		int err;

		if (bytesLeft < 2 * MAINBUF_SIZE && !eofReached)
			FillReadBuffer(readBuf, &readPtr, &bytesLeft, input, &eofReached);

		offset = MP3FindSyncWord(readPtr, bytesLeft);
		if (offset < 0) {
			if (eofReached)
				break;
			if (bytesLeft > 3) {
				readPtr += bytesLeft - 3;
				bytesLeft = 3;
			}
			continue;
		}

		readPtr += offset;
		bytesLeft -= offset;
		err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW && !eofReached)
				continue;
			if (err == ERR_MP3_MAINDATA_UNDERFLOW)
				continue;
			sprintf(status, "Decode error %d after %lu frames", err, frames);
			GuiSetStatus(gui, status, "Output file is incomplete");
			goto cleanup;
		}

		MP3GetLastFrameInfo(decoder, &info);
		if (WriteBigEndianPcm(output, decodeBuf, info.outputSamps) != 0) {
			GuiSetStatus(gui, "Write error", outputName);
			goto cleanup;
		}
		frames++;
		samples += (unsigned long)info.outputSamps;
		if ((frames & 15UL) == 0) {
			sprintf(status, "%lu frames, %lu samples", frames, samples);
			GuiSetStatus(gui, status, "Close window to abort");
		}
	}

	if (gui->abortRequested) {
		GuiSetStatus(gui, "Decode aborted", "Output file is incomplete");
		goto cleanup;
	}

	sprintf(status, "%lu frames, %lu samples", frames, samples);
	GuiSetStatus(gui, "Decode complete", status);
	result = 0;

cleanup:
	if (decoder)
		MP3FreeDecoder(decoder);
	if (output)
		fclose(output);
	if (input)
		fclose(input);
	return result;
}

int main(int argc, char **argv)
{
	GuiState gui;
	int result;

	if (GuiOpen(&gui) != 0)
		return 1;

	if (argc != 3) {
		GuiSetStatus(&gui, "Usage: amiga_mp3gui infile.mp3 outfile.pcm",
			"Close this window to exit");
#ifdef AMIGA_M68K
		GuiWaitForClose(&gui);
#endif
		GuiClose(&gui);
		return 1;
	}

	result = DecodeMp3ToPcm(argv[1], argv[2], &gui);
#ifdef AMIGA_M68K
	GuiWaitForClose(&gui);
#endif
	GuiClose(&gui);
	return result;
}
