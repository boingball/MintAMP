from pathlib import Path

path = Path('amiga_mp3gui.c')
text = path.read_text()
old = '''\tgui->donePort = CreateMsgPort();
\tif (gui->donePort) {
\t\tmemset(&gDoneMsg, 0, sizeof(gDoneMsg));
\t\tgDoneMsg.mn_Length = sizeof(gDoneMsg);
\t\tgDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
\t}
'''
new = '''\tgui->donePort = CreateMsgPort();
'''
if old in text:
    text = text.replace(old, new, 1)
elif 'gDoneMsg' in text:
    raise SystemExit('legacy gDoneMsg reference remains but expected GuiOpen block was not found')
path.write_text(text)
