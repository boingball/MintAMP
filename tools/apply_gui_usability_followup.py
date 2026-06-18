#!/usr/bin/env python3
from pathlib import Path

path = Path("amiga_mp3gui.c")
text = path.read_text(encoding="utf-8")

replacements = [
(
"""#include <intuition/screens.h>
#include <libraries/asl.h>
""",
"""#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/asl.h>
""",
1
),
(
"""#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       310    /* compact inner height */

#define GUI_MARGIN_L     8     /* left margin */
#define GUI_MARGIN_R     8     /* right margin */
#define GUI_TOP_Y        8     /* compact top margin */
""",
"""#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       322    /* compact inner height with safe title-bar clearance */

#define GUI_MARGIN_L     8     /* left margin */
#define GUI_MARGIN_R     8     /* right margin */
#define GUI_TOP_Y       20     /* leave breathing room below the title bar */
""",
1
),
(
"""#define SLIDER_X        (GUI_MARGIN_L + 60)
#define BUFFER_SLIDER_W 300
#define VOLUME_SLIDER_W (GUI_WIN_W - SLIDER_X - GUI_MARGIN_R)
#define FILEINFO_X      (GUI_MARGIN_L + 84)
""",
"""#define SLIDER_X        (GUI_MARGIN_L + 60)
#define BUFFER_SLIDER_W 300
#define VOLUME_SLIDER_W BUFFER_SLIDER_W
#define TRANSPORT_W     96
#define TRANSPORT_H     22
#define PLAY_X          (GUI_MARGIN_L + 126)
#define STOP_X          (GUI_MARGIN_L + 330)
#define FILEINFO_X      (GUI_MARGIN_L + 84)
""",
1
),
(
"""\tstruct Gadget  *gadContext;
\tstruct Gadget  *gadFile;
\tstruct Gadget  *gadTitle;
""",
"""\tstruct Gadget  *gadContext;
\tstruct Gadget  *gadFile;
\tstruct Gadget  *gadBrowse;
\tstruct Gadget  *gadTitle;
""",
1
),
(
"""static struct Gadget *MakeGadget(HelixAmp3Gui *gui, struct Gadget *prev,
\tULONG kind, UWORD id, WORD left, WORD top, WORD width, WORD height,
\tconst char *label, ULONG tag1, ULONG value1, ULONG tag2, ULONG value2,
\tULONG tag3, ULONG value3, ULONG tag4, ULONG value4)
{
\treturn MakeGadgetWithTextAttr(gui, prev, kind, id, left, top, width, height,
\t\tNULL, label, tag1, value1, tag2, value2, tag3, value3, tag4, value4);
}

""",
"""static struct Gadget *MakeGadget(HelixAmp3Gui *gui, struct Gadget *prev,
\tULONG kind, UWORD id, WORD left, WORD top, WORD width, WORD height,
\tconst char *label, ULONG tag1, ULONG value1, ULONG tag2, ULONG value2,
\tULONG tag3, ULONG value3, ULONG tag4, ULONG value4)
{
\treturn MakeGadgetWithTextAttr(gui, prev, kind, id, left, top, width, height,
\t\tNULL, label, tag1, value1, tag2, value2, tag3, value3, tag4, value4);
}

static struct Gadget *MakeSliderGadget(HelixAmp3Gui *gui, struct Gadget *prev,
\tUWORD id, WORD left, WORD top, WORD width, const char *label,
\tLONG minValue, LONG maxValue, LONG level, const char *format,
\tLONG maxLevelLen, LONG visible)
{
\tstruct NewGadget ng;

\tmemset(&ng, 0, sizeof(ng));
\tng.ng_LeftEdge = left;
\tng.ng_TopEdge = top;
\tng.ng_Width = width;
\tng.ng_Height = 16;
\tng.ng_GadgetText = (UBYTE *)label;
\tng.ng_GadgetID = id;
\tng.ng_Flags = PLACETEXT_LEFT;
\tng.ng_VisualInfo = gui->visualInfo;
\treturn CreateGadget(SLIDER_KIND, prev, &ng,
\t\tGA_Immediate, TRUE,
\t\tGA_RelVerify, TRUE,
\t\tGTSL_Min, minValue,
\t\tGTSL_Max, maxValue,
\t\tGTSL_Level, level,
\t\tGTSL_LevelFormat, (ULONG)format,
\t\tGTSL_LevelPlace, PLACETEXT_IN,
\t\tGTSL_MaxLevelLen, maxLevelLen,
\t\tPGA_Visible, visible,
\t\tTAG_DONE);
}

""",
1
),
(
"""\tgad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
\t\tBROWSE_X, ROW_FILE - 1, BROWSE_W, 16, "Browse",
""",
"""\tgui->gadBrowse = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
\t\tBROWSE_X, ROW_FILE - 1, BROWSE_W, 16, "Browse",
""",
1
),
(
"""\tgui->gadBuffer = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_BUFFER,
\t\tSLIDER_X, ROW_BUFFER,
\t\tBUFFER_SLIDER_W, 16, "Buffer:",
\t\tGTSL_Min, 1,
\t\tGTSL_Max, 10,
\t\tGTSL_Level, gui->bufferSeconds,
\t\tGTSL_LevelFormat, (ULONG)"%ld sec");
""",
"""\tgui->gadBuffer = gad = MakeSliderGadget(gui, gad, GID_BUFFER,
\t\tSLIDER_X, ROW_BUFFER, BUFFER_SLIDER_W, "Buffer:",
\t\t1, 10, gui->bufferSeconds, "%ld sec", 6, 2);
""",
1
),
(
"""\tgui->gadVolume = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_VOLUME,
\t\tSLIDER_X, ROW_VOLUME,
\t\tVOLUME_SLIDER_W, 16, "Volume",
\t\tGTSL_Min, 0,
\t\tGTSL_Max, 100,
\t\tGTSL_Level, gui->volumePercent,
\t\tGTSL_LevelFormat, (ULONG)"%ld%%");
""",
"""\tgui->gadVolume = gad = MakeSliderGadget(gui, gad, GID_VOLUME,
\t\tSLIDER_X, ROW_VOLUME, VOLUME_SLIDER_W, "Volume:",
\t\t0, 100, gui->volumePercent, "%ld%%", 4, 12);
""",
1
),
(
"""\tgui->gadPlay = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAY,
\t\tGUI_MARGIN_L + 120, ROW_BUTTONS, 80, 18, "Play",
""",
"""\tgui->gadPlay = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_PLAY,
\t\tPLAY_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "> PLAY",
""",
1
),
(
"""\tgui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
\t\tGUI_MARGIN_L + 300, ROW_BUTTONS, 80, 18, "Stop",
""",
"""\tgui->gadStop = gad = MakeGadget(gui, gad, BUTTON_KIND, GID_STOP,
\t\tSTOP_X, ROW_BUTTONS, TRANSPORT_W, TRANSPORT_H, "[] STOP",
""",
1
),
(
"""static void ChooseMp3(HelixAmp3Gui *gui)
{
\tstruct FileRequester *req;
\tchar path[HELIXAMP3_MAX_PATH];

\tif (!gui->lastDrawer[0] && gui->inputName[0])
""",
"""static void ChooseMp3(HelixAmp3Gui *gui)
{
\tstruct FileRequester *req;
\tchar path[HELIXAMP3_MAX_PATH];

\t/* Replacing tags/artwork while the playback child and artwork timer still
\t * reference the current selection can corrupt Intuition/ASL state and raise
\t * a recoverable alert.  File changes are intentionally a stopped-state action. */
\tif (gui->playbackActive || gui->playbackDonePending) {
\t\tSetStatus(gui, "Stop playback before choosing another file.");
\t\treturn;
\t}
\tif (!gui->lastDrawer[0] && gui->inputName[0])
""",
1
),
(
"""\tgui->playbackDonePending = 0;
\tgui->playbackStoppedByUser = 0;
\tgui->playbackActive = 1;
\tSetStatus(gui, gui->decodeThenPlay ?
""",
"""\tgui->playbackDonePending = 0;
\tgui->playbackStoppedByUser = 0;
\tgui->playbackActive = 1;
\tif (gui->gadBrowse)
\t\tGT_SetGadgetAttrs(gui->gadBrowse, gui->win, NULL,
\t\t\tGA_Disabled, TRUE, TAG_DONE);
\tSetStatus(gui, gui->decodeThenPlay ?
""",
1
),
(
"""\tgui->playbackDonePending = 0;
\tgui->playbackStoppedByUser = 0;
\tgui->playbackActive = 0;
\tgGuiPlayer.process = NULL;
""",
"""\tgui->playbackDonePending = 0;
\tgui->playbackStoppedByUser = 0;
\tgui->playbackActive = 0;
\tif (gui->win && gui->gadBrowse)
\t\tGT_SetGadgetAttrs(gui->gadBrowse, gui->win, NULL,
\t\t\tGA_Disabled, FALSE, TAG_DONE);
\tgGuiPlayer.process = NULL;
""",
1
),
]

for index, (old, new, expected) in enumerate(replacements, 1):
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"replacement {index}: expected {expected} matches, found {count}")
    text = text.replace(old, new)

path.write_text(text, encoding="utf-8")
