#!/usr/bin/env python3
from pathlib import Path

path = Path("amiga_mp3gui.c")
text = path.read_text(encoding="utf-8")

replacements = [
(
"""#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       338    /* inner height */

#define GUI_MARGIN_L     8     /* left margin */
#define GUI_MARGIN_R     8     /* right margin */
#define GUI_TOP_Y       36     /* y of first gadget row */
""",
"""#define GUI_WIN_W       560    /* inner width; wide enough for all controls */
#define GUI_WIN_H       310    /* compact inner height */

#define GUI_MARGIN_L     8     /* left margin */
#define GUI_MARGIN_R     8     /* right margin */
#define GUI_TOP_Y        8     /* compact top margin */
""",
1
),
(
"""#define TEXT_COL_W      (ART_X - GUI_MARGIN_L - 8)
""",
"""#define TEXT_COL_W      (ART_X - GUI_MARGIN_L - 8)
#define META_X          (GUI_MARGIN_L + 60)
#define META_RIGHT      (ART_X - 8)
#define META_W          (META_RIGHT - META_X)
#define BROWSE_W        56
#define BROWSE_X        (ART_X - BROWSE_W)
#define FILE_W          (BROWSE_X - META_X - 4)
#define SLIDER_X        (GUI_MARGIN_L + 60)
#define BUFFER_SLIDER_W 300
#define VOLUME_SLIDER_W (GUI_WIN_W - SLIDER_X - GUI_MARGIN_R)
#define FILEINFO_X      (GUI_MARGIN_L + 84)
#define FILEINFO_W      (GUI_WIN_W - FILEINFO_X - GUI_MARGIN_R)
""",
1
),
(
"""\tstruct Gadget  *gadFastLowrate;
\tstruct Gadget  *gadRate;
""",
"""\tstruct Gadget  *gadFastLowrate;
\tstruct Gadget  *gadSuperfastLowrate;
\tstruct Gadget  *gadRate;
""",
1
),
(
"""static const STRPTR kSuperfastMonoRateLabels[] = {
\t(STRPTR)"8287",
\t(STRPTR)"8820",
\t(STRPTR)"11025",
\t(STRPTR)"22050",
\tNULL
};

static const STRPTR kSuperfastStereoRateLabels[] = {
\t(STRPTR)"8820",
\t(STRPTR)"11025",
\t(STRPTR)"22050",
\tNULL
};

static const STRPTR *SuperfastRateLabels(int mono)
{
\treturn mono ? kSuperfastMonoRateLabels : kSuperfastStereoRateLabels;
}

static int SuperfastActiveFromRateIndex(int rateIndex, int mono)
{
\treturn mono ? rateIndex : rateIndex - 1;
}

static int RateIndexFromSuperfastActive(int active, int mono)
{
\treturn mono ? active : active + 1;
}

""",
"""""",
1
),
(
"""\tgui->gadFile = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILE,
\t\tGUI_MARGIN_L + 48, ROW_FILE, TEXT_COL_W - 100, 16, "File:",
""",
"""\tgui->gadFile = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILE,
\t\tMETA_X, ROW_FILE, FILE_W, 16, "File:",
""",
1
),
(
"""\tgad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
\t\tART_X - 56, ROW_FILE - 1, 56, 16, "Browse",
""",
"""\tgad = MakeGadget(gui, gad, BUTTON_KIND, GID_BROWSE,
\t\tBROWSE_X, ROW_FILE - 1, BROWSE_W, 16, "Browse",
""",
1
),
(
"""\tgui->gadTitle = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TITLE,
\t\tGUI_MARGIN_L + 54, ROW_TITLE, TEXT_COL_W - 54, 16, "Title:",
""",
"""\tgui->gadTitle = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TITLE,
\t\tMETA_X, ROW_TITLE, META_W, 16, "Title:",
""",
1
),
(
"""\tgui->gadArtist = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ARTIST,
\t\tGUI_MARGIN_L + 60, ROW_ARTIST, TEXT_COL_W - 54, 16, "Artist:",
""",
"""\tgui->gadArtist = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ARTIST,
\t\tMETA_X, ROW_ARTIST, META_W, 16, "Artist:",
""",
1
),
(
"""\tgui->gadAlbum = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ALBUM,
\t\tGUI_MARGIN_L + 54, ROW_ALBUM, TEXT_COL_W - 54, 16, "Album:",
""",
"""\tgui->gadAlbum = gad = MakeGadget(gui, gad, TEXT_KIND, GID_ALBUM,
\t\tMETA_X, ROW_ALBUM, META_W, 16, "Album:",
""",
1
),
(
"""\tgui->gadTrack = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TRACK,
\t\tGUI_MARGIN_L + 54, ROW_TRACK, TEXT_COL_W - 54, 16, "Track:",
""",
"""\tgui->gadTrack = gad = MakeGadget(gui, gad, TEXT_KIND, GID_TRACK,
\t\tMETA_X, ROW_TRACK, META_W, 16, "Track:",
""",
1
),
(
"""\tgui->gadGenre = gad = MakeGadget(gui, gad, TEXT_KIND, GID_GENRE,
\t\tGUI_MARGIN_L + 54, ROW_GENRE, TEXT_COL_W - 54, 16, "Genre:",
""",
"""\tgui->gadGenre = gad = MakeGadget(gui, gad, TEXT_KIND, GID_GENRE,
\t\tMETA_X, ROW_GENRE, META_W, 16, "Genre:",
""",
1
),
(
"""\tgad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_SUPERFAST_LOWRATE,
""",
"""\tgui->gadSuperfastLowrate = gad = MakeGadget(gui, gad, CHECKBOX_KIND, GID_SUPERFAST_LOWRATE,
""",
1
),
(
"""\tgui->gadRate = gad = MakeGadget(gui, gad, CYCLE_KIND, GID_RATE,
\t\tGUI_MARGIN_L + 48, ROW_CYCLES, 80, 16, "Rate:",
\t\tGTCY_Labels, (ULONG)(gui->superfastLowrate ? SuperfastRateLabels(ChannelUsesMonoCost(gui)) : kRateLabels),
\t\tGTCY_Active, gui->superfastLowrate ? SuperfastActiveFromRateIndex(gui->rateIndex, ChannelUsesMonoCost(gui)) : gui->rateIndex,
""",
"""\tgui->gadRate = gad = MakeGadget(gui, gad, CYCLE_KIND, GID_RATE,
\t\tGUI_MARGIN_L + 48, ROW_CYCLES, 80, 16, "Rate:",
\t\tGTCY_Labels, (ULONG)kRateLabels,
\t\tGTCY_Active, gui->rateIndex,
""",
1
),
(
"""\tgui->gadBuffer = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_BUFFER,
\t\tGUI_MARGIN_L + 62, ROW_BUFFER,
\t\tGUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 80, 16, "Buffer:",
\t\tGTSL_Min, 1,
\t\tGTSL_Max, 30,
""",
"""\tgui->gadBuffer = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_BUFFER,
\t\tSLIDER_X, ROW_BUFFER,
\t\tBUFFER_SLIDER_W, 16, "Buffer:",
\t\tGTSL_Min, 1,
\t\tGTSL_Max, 10,
""",
1
),
(
"""\tgui->gadVolume = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_VOLUME,
\t\tGUI_MARGIN_L + 62, ROW_VOLUME,
\t\tGUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 80, 16, "Volume",
""",
"""\tgui->gadVolume = gad = MakeGadget(gui, gad, SLIDER_KIND, GID_VOLUME,
\t\tSLIDER_X, ROW_VOLUME,
\t\tVOLUME_SLIDER_W, 16, "Volume",
""",
1
),
(
"""\tgui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
\t\tGUI_MARGIN_L + 60, ROW_STATUS, GUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 80, 16, "Status:",
""",
"""\tgui->gadStatus = gad = MakeGadget(gui, gad, TEXT_KIND, GID_STATUS,
\t\tMETA_X, ROW_STATUS, GUI_WIN_W - META_X - GUI_MARGIN_R, 16, "Status:",
""",
1
),
(
"""\tgui->gadFileInfo = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILEINFO,
\t\tGUI_MARGIN_L + 68, ROW_FILEINFO, GUI_WIN_W - GUI_MARGIN_L - GUI_MARGIN_R - 88, 16, "File info:",
""",
"""\tgui->gadFileInfo = gad = MakeGadget(gui, gad, TEXT_KIND, GID_FILEINFO,
\t\tFILEINFO_X, ROW_FILEINFO, FILEINFO_W, 16, "File info:",
""",
1
),
(
"""\tgui->bufferSeconds = LoadEnvInt("BufferSeconds", 10, 1, 30);
""",
"""\tgui->bufferSeconds = LoadEnvInt("BufferSeconds", 10, 1, 10);
""",
1
),
(
"""\tif (seconds > 30)
\t\tseconds = 30;
""",
"""\tif (seconds > 10)
\t\tseconds = 10;
""",
1
),
(
"""\t\tif (gui->gadRate)
\t\t\tGT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
\t\t\t\tGTCY_Labels, (ULONG)(gui->superfastLowrate ? SuperfastRateLabels(ChannelUsesMonoCost(gui)) : kRateLabels),
\t\t\t\tGTCY_Active, gui->superfastLowrate ?
\t\t\t\t\tSuperfastActiveFromRateIndex(gui->rateIndex, ChannelUsesMonoCost(gui)) : gui->rateIndex,
\t\t\t\tTAG_DONE);
""",
"""\t\tif (gui->gadRate)
\t\t\tGT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
\t\t\t\tGTCY_Labels, (ULONG)kRateLabels,
\t\t\t\tGTCY_Active, gui->rateIndex,
\t\t\t\tTAG_DONE);
""",
2
),
(
"""\t\tif (gui->gadRate)
\t\t\tGT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
\t\t\t\tGTCY_Labels, (ULONG)(gui->superfastLowrate ?
\t\t\t\t\tSuperfastRateLabels(ChannelUsesMonoCost(gui)) : kRateLabels),
\t\t\t\tGTCY_Active, gui->superfastLowrate ?
\t\t\t\t\tSuperfastActiveFromRateIndex(gui->rateIndex,
\t\t\t\t\t\tChannelUsesMonoCost(gui)) : gui->rateIndex,
\t\t\t\tTAG_DONE);
""",
"""\t\tif (gui->gadRate)
\t\t\tGT_SetGadgetAttrs(gui->gadRate, gui->win, NULL,
\t\t\t\tGTCY_Labels, (ULONG)kRateLabels,
\t\t\t\tGTCY_Active, gui->rateIndex,
\t\t\t\tTAG_DONE);
""",
1
),
(
"""\tcase GID_RATE:
\t\tif (gui->playbackActive || gui->playbackDonePending) {
\t\t\tGT_SetGadgetAttrs(gad, gui->win, NULL,
\t\t\t\tGTCY_Active, gui->superfastLowrate ?
\t\t\t\t\tSuperfastActiveFromRateIndex(gui->rateIndex, ChannelUsesMonoCost(gui)) : gui->rateIndex,
\t\t\t\tTAG_DONE);
\t\t\tSetStatus(gui, "Stop playback before changing output rate.");
\t\t\tbreak;
\t\t}
\t\tif (gui->superfastLowrate)
\t\t\tgui->rateIndex = RateIndexFromSuperfastActive(code, ChannelUsesMonoCost(gui));
\t\telse {
\t\t\tgui->rateIndex = code;
\t\t\tif (gui->rateIndex < 0 || gui->rateIndex > 4)
\t\t\t\tgui->rateIndex = 2;
\t\t}
\t\tSetStatus(gui, "Output sample rate updated.");
\t\tSaveGuiSettings(gui);
\t\tbreak;
""",
"""\tcase GID_RATE:
\t\tif (gui->playbackActive || gui->playbackDonePending) {
\t\t\tGT_SetGadgetAttrs(gad, gui->win, NULL,
\t\t\t\tGTCY_Active, gui->rateIndex,
\t\t\t\tTAG_DONE);
\t\t\tSetStatus(gui, "Stop playback before changing output rate.");
\t\t\tbreak;
\t\t}
\t\tgui->rateIndex = code;
\t\tif (gui->rateIndex < 0 || gui->rateIndex > 4)
\t\t\tgui->rateIndex = 2;
\t\tif (gui->superfastLowrate &&
\t\t\t!RateIndexSupportsSuperfast(gui->rateIndex, ChannelUsesMonoCost(gui))) {
\t\t\tgui->superfastLowrate = 0;
\t\t\tif (gui->gadSuperfastLowrate)
\t\t\t\tGT_SetGadgetAttrs(gui->gadSuperfastLowrate, gui->win, NULL,
\t\t\t\t\tGTCB_Checked, FALSE, TAG_DONE);
\t\t\tSetStatus(gui, "Selected rate uses standard playback; Superfast disabled.");
\t\t} else {
\t\t\tSetStatus(gui, "Output sample rate updated.");
\t\t}
\t\tSaveGuiSettings(gui);
\t\tbreak;
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
