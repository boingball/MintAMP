from pathlib import Path

path = Path('amiga_mp3gui.c')
text = path.read_text()

marker = '''extern GuiPlaybackStatus gGuiPlaybackStatus;
extern volatile int gMiniAmp3EmbeddedPlayback;
'''
helper = '''extern GuiPlaybackStatus gGuiPlaybackStatus;
extern volatile int gMiniAmp3EmbeddedPlayback;

static const char *GuiStartupStageLabel(int stage)
{
\tswitch (stage) {
\tcase GUISTART_NONE: return "none";
\tcase GUISTART_CHILD_ENTERED: return "child entered";
\tcase GUISTART_ARGS_READY: return "args ready";
\tcase GUISTART_INPUT_OPEN: return "input open";
\tcase GUISTART_INPUT_FOPEN_BEFORE: return "before fopen";
\tcase GUISTART_INPUT_FOPEN_AFTER: return "after fopen";
\tcase GUISTART_INPUT_PRELOAD_FASTMEM: return "Fast RAM preload";
\tcase GUISTART_INPUT_PREPARE: return "input prepare";
\tcase GUISTART_DECODER_ALLOC: return "decoder alloc";
\tcase GUISTART_DECODER_CONFIG: return "decoder config";
\tcase GUISTART_FASTLOWRATE_WARN_BEFORE: return "before rate warning";
\tcase GUISTART_FASTLOWRATE_WARN_AFTER: return "after rate warning";
\tcase GUISTART_PROBE_RATE: return "probe rate";
\tcase GUISTART_PROBE_RATE_DONE: return "probe rate done";
\tcase GUISTART_STREAM_INIT: return "stream init";
\tcase GUISTART_PREFILL: return "prefill";
\tcase GUISTART_PREFILL_DONE: return "prefill done";
\tcase GUISTART_AUDIO_SETUP: return "audio setup";
\tcase GUISTART_CREATE_PORT: return "create audio port";
\tcase GUISTART_ALLOC_CHIP_BUFFERS: return "allocate chip buffers";
\tcase GUISTART_CREATE_IOREQUESTS: return "create IO requests";
\tcase GUISTART_OPEN_DEVICE: return "open audio.device";
\tcase GUISTART_OPEN_DEVICE_DONE: return "audio.device opened";
\tcase GUISTART_ALLOC_WORK_BUFFERS: return "allocate work buffers";
\tcase GUISTART_AUDIO_SETUP_DONE: return "audio setup done";
\tcase GUISTART_FILL_BUFFER_A: return "fill buffer A";
\tcase GUISTART_FILL_BUFFER_A_DONE: return "buffer A filled";
\tcase GUISTART_FILL_BUFFER_B: return "fill buffer B";
\tcase GUISTART_FILL_BUFFER_B_DONE: return "buffer B filled";
\tcase GUISTART_PREPARE_A: return "prepare A";
\tcase GUISTART_PREPARE_B: return "prepare B";
\tcase GUISTART_COMMIT_A: return "submit first buffer";
\tcase GUISTART_PLAYING: return "playing";
\tcase GUISTART_FAILED: return "failed";
\tcase GUISTART_CLEANUP: return "cleanup";
\tdefault: return "unknown";
\t}
}
'''
if 'GuiStartupStageLabel' not in text:
    if marker not in text:
        raise SystemExit('extern marker not found')
    text = text.replace(marker, helper, 1)

old = '''#else
\t\t\tif (gui->startupStageStableTicks >= 5 && !gui->startupStallShown) {
\t\t\t\tSetStatus(gui, "Playback startup is taking longer than expected.");
\t\t\t\tgui->startupStallShown = 1;
\t\t\t} else if (phaseChanged || stageChanged) {
\t\t\t\tif (stage == GUISTART_INPUT_PRELOAD_FASTMEM)
\t\t\t\t\tSetStatus(gui, "Copying file to Fast RAM...");
\t\t\t\telse if (stage >= GUISTART_AUDIO_SETUP)
\t\t\t\t\tSetStatus(gui, "Buffering...");
\t\t\t\telse
\t\t\t\t\tSetStatus(gui, "Starting playback...");
\t\t\t}
#endif
'''
new = '''#else
\t\t\tif (phaseChanged || stageChanged || gui->startupStageStableTicks == 3 ||
\t\t\t\tgui->startupStageStableTicks == 10) {
\t\t\t\tchar buf[128];
\t\t\t\tsprintf(buf, "Startup %lu: %s [%d], err=%d dev=%d",
\t\t\t\t\tgui->playbackRunId, GuiStartupStageLabel(stage), stage,
\t\t\t\t\tgGuiPlaybackStatus.lastError,
\t\t\t\t\tgGuiPlaybackStatus.openDeviceResult);
\t\t\t\tSetStatus(gui, buf);
\t\t\t\tif (gui->startupStageStableTicks >= 3)
\t\t\t\t\tgui->startupStallShown = 1;
\t\t\t}
#endif
'''
if old not in text:
    if 'Startup %lu: %s [%d], err=%d dev=%d' not in text:
        raise SystemExit('non-debug startup block not found')
else:
    text = text.replace(old, new, 1)

path.write_text(text)
