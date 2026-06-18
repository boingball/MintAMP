from pathlib import Path

p = Path('amiga_mp3gui.c')
s = p.read_text()

if 'static void ApplyHardwareAudioFilter(HelixAmp3Gui *gui)' not in s:
    pos = s.find('static void DrawTransportIcons(HelixAmp3Gui *gui)')
    if pos < 0:
        raise SystemExit('DrawTransportIcons insertion point not found')
    fn = r'''static void ApplyHardwareAudioFilter(HelixAmp3Gui *gui)
{
#if defined(AMIGA_M68K)
	/* CIA-A port A bit 1 controls the Amiga/CD32 analogue audio filter via the
	 * power LED circuit.  This is global hardware, not owned by audio.device. */
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

'''
    s = s[:pos] + fn + s[pos:]

# Forward declarations if needed.
old = '''static void DrawArtPanel(HelixAmp3Gui *gui);
static void DrawTransportIcons(HelixAmp3Gui *gui);
static void HandleDoneSignal(HelixAmp3Gui *gui);
'''
new = '''static void DrawArtPanel(HelixAmp3Gui *gui);
static void DrawTransportIcons(HelixAmp3Gui *gui);
static void DrawFilterButton(HelixAmp3Gui *gui);
static void ApplyHardwareAudioFilter(HelixAmp3Gui *gui);
static void HandleDoneSignal(HelixAmp3Gui *gui);
'''
if new not in s and old in s:
    s = s.replace(old, new, 1)

# Draw the filter button wherever the transport icons are redrawn.
s = s.replace('DrawTransportIcons(gui);\n\tif (gui->timerOpen)', 'DrawTransportIcons(gui);\n\tDrawFilterButton(gui);\n\tif (gui->timerOpen)')
s = s.replace('DrawTransportIcons(gui);\n\t\t} else if (classValue == IDCMP_MOUSEMOVE)', 'DrawTransportIcons(gui);\n\t\t\tDrawFilterButton(gui);\n\t\t} else if (classValue == IDCMP_MOUSEMOVE)')
s = s.replace('DrawTransportIcons(gui);\n}', 'DrawTransportIcons(gui);\n\tDrawFilterButton(gui);\n}')

if 'case GID_HARDWARE_FILTER:' not in s:
    old = '''\tcase GID_STOP:
\t\tStopPlayback(gui);
\t\tbreak;
\t}
}
'''
    new = '''\tcase GID_STOP:
\t\tStopPlayback(gui);
\t\tbreak;
\tcase GID_HARDWARE_FILTER:
\t\tgui->hardwareFilter = !gui->hardwareFilter;
\t\tApplyHardwareAudioFilter(gui);
\t\tDrawFilterButton(gui);
\t\tSetStatus(gui, gui->hardwareFilter ?
\t\t\t"Hardware filter enabled." : "Hardware filter disabled.");
\t\tSaveGuiSettings(gui);
\t\tbreak;
\t}
}
'''
    if old not in s:
        raise SystemExit('HandleGuiAction stop block not found')
    s = s.replace(old, new, 1)

if 'ApplyHardwareAudioFilter(gui);' not in s[s.find('GuiOpen'):s.find('GuiOpen')+4000]:
    old = '\tUpdateChannelGadgetState(gui);\n'
    new = '\tUpdateChannelGadgetState(gui);\n\tApplyHardwareAudioFilter(gui);\n'
    if old not in s:
        raise SystemExit('GuiOpen channel update block not found')
    s = s.replace(old, new, 1)

p.write_text(s)
