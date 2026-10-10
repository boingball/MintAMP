#include "skin_playlist_actions.inc"
static int GuiSkinAdd(void *ctx,const char *path,const char *title)
{ return PlaylistAddEntry(&((HelixAmp3Gui *)ctx)->playlist,path,title); }
static void GuiSkinFiles(void *ctx) { PlaylistAddFiles((HelixAmp3Gui *)ctx); }
static void GuiSkinSave(void *ctx)
{ HelixAmp3Gui *gui=(HelixAmp3Gui *)ctx; PlaylistSaveM3U(gui); skin_player_notice(gSkinPlayer,gui->statusText); }
static void GuiSkinRefresh(void *ctx) { RefreshPlaylistView((HelixAmp3Gui *)ctx); }
static SkinPlaylistOps GuiSkinOps(HelixAmp3Gui *gui)
{
    SkinPlaylistOps o;
    memset(&o,0,sizeof(o)); o.ctx=gui;
    o.add=GuiSkinAdd; o.files=GuiSkinFiles; o.save=GuiSkinSave; o.refresh=GuiSkinRefresh;
    o.list.paths=&gui->playlist.paths[0][0]; o.list.path_size=sizeof(gui->playlist.paths[0]);
    o.list.titles=&gui->playlist.titles[0][0]; o.list.title_size=sizeof(gui->playlist.titles[0]);
    o.list.names=&gui->playlist.names[0][0]; o.list.name_size=sizeof(gui->playlist.names[0]);
    o.list.durations=gui->playlist.durations;
    o.list.count=&gui->playlist.count; o.list.current=&gui->playlist.current; o.list.selected=&gui->playlist.selected;
    o.list.selection=skin_player_selection(gSkinPlayer,gui->playlist.count,gui->playlist.selected);
    return o;
}
/* Included only by the MintAMP-SGT frontend. Reuse its established actions. */
static void GuiSkinHideSettings(HelixAmp3Gui *gui)
{
    if (!gui->win) return;
    gui->iconifyLeft=gui->win->LeftEdge; gui->iconifyTop=gui->win->TopEdge;
    if (gui->menuStrip) ClearMenuStrip(gui->win);
    if (gui->gadgets) RemoveGList(gui->win,gui->gadgets,-1);
    DrainWindowMessages(gui); CloseWindow(gui->win); gui->win=NULL;
    /* Reopening uses the existing AppIcon restore path, without requiring
     * an AppIcon or a standard edition's .info file just to hide settings. */
    gui->iconified=1;
}
static const char *GuiSkinPlaylistName(void *ctx,int index)
{
    HelixAmp3Gui *gui=(HelixAmp3Gui *)ctx;
    return gui->playlist.names[index];
}
static void GuiSkinPlaylistUpdate(HelixAmp3Gui *gui)
{
    if (gui->playlist.current>=0 && gui->playlist.current<gui->playlist.count && gui->totalSecs>0 && !IsRadioInputName(gui->inputName)) gui->playlist.durations[gui->playlist.current]=gui->totalSecs;
    skin_player_playlist_durations(gSkinPlayer,gui->playlist.durations);
    skin_player_playlist_update(gSkinPlayer,gui->playlist.count,gui->playlist.selected,
        gui->playlist.current,GuiSkinPlaylistName,gui);
}
static void GuiSkinPoll(HelixAmp3Gui *gui)
{
    SkinEvent e;
    while (skin_player_poll(gSkinPlayer,&e)) {
        struct Gadget gad;
        memset(&gad,0,sizeof(gad));
        switch (e.action) {
        case SKIN_QUIT: gui->closeRequested=1; return;
        case SKIN_SETTINGS:
            if (gui->win) GuiSkinHideSettings(gui);
            else GuiRestoreFromAppIcon(gui);
            break;
        case SKIN_BROWSE:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1; ChooseMp3(gui); break;
        case SKIN_PLAY:
            if (gSkinAudio.pause_requested || gSkinAudio.paused) { skin_player_wake("MintAMP-GT playback",0); break; }
            if (gui->playbackActive) break;
            gSkinPendingTrack=-1;
            if (!gui->inputName[0] && !gui->queuedInputName[0]) {
                skin_player_notice(gSkinPlayer,"OPEN AUDIO OR CHOOSE INTERNET RADIO"); break;
            }
            gad.GadgetID=GID_PLAY; HandleGuiAction(gui,&gad,0,IDCMP_GADGETUP,TRUE); break;
        case SKIN_PAUSE:
            if (gui->playbackActive && !gui->playbackDonePending &&
                (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING || gGuiPlaybackStatus.phase==GUIPLAY_PHASE_UNDERRUN)) {
                skin_player_wake("MintAMP-GT playback",!gSkinAudio.pause_requested);
                skin_player_notice(gSkinPlayer,gSkinAudio.pause_requested ? "PAUSE PENDING - QUEUED AUDIO WILL FINISH" : "RESUMING");
            } else skin_player_notice(gSkinPlayer,"PAUSE REQUIRES PLAYBACK");
            break;
        case SKIN_BALANCE_SET:
            if (gui->playbackActive && !gSkinAudio.output_stereo) skin_player_notice(gSkinPlayer,"BALANCE REQUIRES STEREO PAULA OUTPUT");
            else { gSkinAudio.balance=e.value; if (e.released) skin_player_save(gSkinPlayer); }
            break;
        case SKIN_DEBUG: {
            char station[128]; CopyVolatileGuiString(station,sizeof(station),gGuiPlaybackStatus.radioStationName);
            skin_player_debug(gSkinPlayer,gGuiPlaybackStatus.halfBufferMs,gGuiPlaybackStatus.spareMs,
                gGuiPlaybackStatus.underruns,gGuiPlaybackStatus.radioBufferedBytes,station); break;
        }
        case SKIN_LIST_ADD: case SKIN_LIST_REM: case SKIN_LIST_SEL: case SKIN_LIST_MISC: case SKIN_LIST_FILE:
        case SKIN_TRACK_MOVE: case SKIN_TRACK_REMOVE: {
            SkinPlaylistOps o=GuiSkinOps(gui);
            gSkinPendingTrack=-1; SkinPlaylistAction(&o,&e); GuiSkinPlaylistUpdate(gui); break;
        }
        case SKIN_STOP:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1; StopPlayback(gui); break;
        case SKIN_TRACK_SELECT:
            if (e.value>=0 && e.value<gui->playlist.count) {
                skin_selection_set(skin_player_selection(gSkinPlayer,gui->playlist.count,gui->playlist.selected),e.value,e.selection);
                gui->playlist.selected=e.value; RefreshPlaylistView(gui); GuiSkinPlaylistUpdate(gui);
            }
            break;
        case SKIN_TRACK_PLAY: case SKIN_PREVIOUS: case SKIN_NEXT: {
            int index=e.action==SKIN_TRACK_PLAY ? e.value :
                skin_player_order(gSkinPlayer,gui->playlist.count,gui->playlist.current,e.action==SKIN_NEXT ? 1 : -1,0);
            if (index<0 || index>=gui->playlist.count) {
                skin_player_notice(gSkinPlayer,"NO PLAYLIST TRACK IN THAT DIRECTION"); break;
            }
            if (e.action==SKIN_TRACK_PLAY) skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=index;
            gui->queuedInputName[0]=0; gui->queuedPlayPending=0;
            gui->playlistNextPending=0;
            if (gui->playbackActive || gui->playbackDonePending) StopPlayback(gui);
            break;
        }
        case SKIN_RADIO:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1;
            if (gui->hasNetwork) OpenRadioWindow(gui);
            else skin_player_notice(gSkinPlayer,"NO TCP/IP STACK AVAILABLE");
            break;
        case SKIN_PLAYLIST:
            GuiSkinPlaylistUpdate(gui);
            if (skin_player_playlist_toggle(gSkinPlayer)) break;
            /* Missing artwork or a small screen uses the full native view. */
            if (gui->plWin) ClosePlaylistWindow(gui); else OpenPlaylistWindow(gui); break;
        case SKIN_PLAYLIST_OPTIONS:
            if (!gui->plWin) OpenPlaylistWindow(gui);
            if (gui->plWin) { WindowToFront(gui->plWin); ActivateWindow(gui->plWin); }
            break;
        case SKIN_VOLUME_SET:
            SetGuiVolume(gui,e.value,e.released,IDCMP_MOUSEMOVE,0); break;
        case SKIN_SEEK:
            if (gSkinAudio.pause_requested || !gSkinAudio.eq_supported || IsRadioInputName(gui->inputName) || gui->totalSecs<=0 || !gui->playbackActive)
                skin_player_notice(gSkinPlayer,"SEEK REQUIRES AN UNPAUSED LOCAL MP3");
            else GuiSeekRelative(gui,(int)((long)gui->totalSecs*e.value/100)-
                                      (gui->elapsedSecs-gui->launchBufferSecs));
            break;
        case SKIN_UNSUPPORTED:
            skin_player_notice(gSkinPlayer,"CONTROL NOT AVAILABLE"); break;
        }
    }
    if (!gui->closeRequested && gSkinPendingTrack>=0 && !gui->playbackActive && !gui->playbackDonePending) {
        int index=gSkinPendingTrack; gSkinPendingTrack=-1;
        if (index<gui->playlist.count) { PlaylistLoadAndShow(gui,index); PlaylistStartCurrent(gui); }
    }
}
static void GuiSkinUpdate(HelixAmp3Gui *gui,int tick)
{
    SkinState state;
    memset(&state,0,sizeof(state));
    if (IsRadioInputName(gui->inputName))
        CopyVolatileGuiString(state.title,sizeof(state.title),gGuiPlaybackStatus.radioTitle);
    if (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_ERROR)
        SafeCopy(state.title,sizeof(state.title),gui->statusText);
    if (!state.title[0] && IsRadioInputName(gui->inputName))
        CopyVolatileGuiString(state.title,sizeof(state.title),gGuiPlaybackStatus.radioStationName);
    if (!state.title[0] && gui->tags.title[0] && strcmp(gui->tags.title,"-"))
        SafeCopy(state.title,sizeof(state.title),gui->tags.title);
    if (!state.title[0]) SafeCopy(state.title,sizeof(state.title),
        gui->inputName[0] ? gui->inputName : "MINTAMP - RIGHT CLICK FOR MENU");
    state.elapsed=gui->elapsedSecs-gui->launchBufferSecs;
    if (gSkinAudio.position_valid && gui->playbackActive) state.elapsed=(int)(gSkinAudio.position_ms/1000UL);
    if (!gui->playbackActive && !gSkinAudio.completed_ok) state.elapsed=0;
    state.total=gui->totalSecs; state.volume=gui->volumePercent;
    state.playing=gui->playbackActive && (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING || gGuiPlaybackStatus.phase==GUIPLAY_PHASE_UNDERRUN);
    state.paused=gSkinAudio.paused;
    state.mono=gSkinAudio.channels==1; state.rate=gGuiPlaybackStatus.sampleRate;
    state.bitrate=gSkinAudio.bitrate ? gSkinAudio.bitrate : gGuiPlaybackStatus.radioBitrateKbps;
    snprintf(state.debug_text,sizeof(state.debug_text),"BUF %luMS SPARE %ldMS UNDERRUN %lu",gGuiPlaybackStatus.halfBufferMs,gGuiPlaybackStatus.spareMs,gGuiPlaybackStatus.underruns);
    skin_player_update(gSkinPlayer,&state,tick);
    GuiSkinPlaylistUpdate(gui);
}
