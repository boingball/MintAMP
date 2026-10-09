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
            gSkinPendingTrack=-1; ChooseMp3(gui); break;
        case SKIN_PLAY:
            gSkinPendingTrack=-1;
            if (!gui->inputName[0] && !gui->queuedInputName[0]) {
                skin_player_notice(gSkinPlayer,"OPEN AUDIO OR CHOOSE INTERNET RADIO"); break;
            }
            gad.GadgetID=GID_PLAY; HandleGuiAction(gui,&gad,0,IDCMP_GADGETUP,TRUE); break;
        case SKIN_STOP:
            gSkinPendingTrack=-1; StopPlayback(gui); break;
        case SKIN_PREVIOUS: case SKIN_NEXT: {
            int index=gui->playlist.current+(e.action==SKIN_NEXT ? 1 : -1);
            if (index<0 || index>=gui->playlist.count) {
                skin_player_notice(gSkinPlayer,"NO PLAYLIST TRACK IN THAT DIRECTION"); break;
            }
            gSkinPendingTrack=index;
            gui->queuedInputName[0]=0; gui->queuedPlayPending=0;
            gui->playlistNextPending=0;
            if (gui->playbackActive || gui->playbackDonePending) StopPlayback(gui);
            break;
        }
        case SKIN_RADIO:
            gSkinPendingTrack=-1;
            if (gui->hasNetwork) OpenRadioWindow(gui);
            else skin_player_notice(gSkinPlayer,"NO TCP/IP STACK AVAILABLE");
            break;
        case SKIN_PLAYLIST:
            gSkinPendingTrack=-1;
            if (gui->plWin) ClosePlaylistWindow(gui); else OpenPlaylistWindow(gui); break;
        case SKIN_VOLUME_SET:
            SetGuiVolume(gui,e.value,e.released,IDCMP_MOUSEMOVE,0); break;
        case SKIN_SEEK:
            if (IsRadioInputName(gui->inputName) || gui->totalSecs<=0 || !gui->playbackActive)
                skin_player_notice(gSkinPlayer,"SEEK NEEDS A PLAYING LOCAL TRACK");
            else GuiSeekRelative(gui,(int)((long)gui->totalSecs*e.value/100)-
                                      (gui->elapsedSecs-gui->launchBufferSecs));
            break;
        case SKIN_UNSUPPORTED:
            skin_player_notice(gSkinPlayer,"PAUSE BALANCE SHUFFLE REPEAT NOT SUPPORTED"); break;
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
    if (!state.title[0] && gui->tags.title[0] && strcmp(gui->tags.title,"-"))
        SafeCopy(state.title,sizeof(state.title),gui->tags.title);
    if (!state.title[0]) SafeCopy(state.title,sizeof(state.title),
        gui->inputName[0] ? gui->inputName : "MINTAMP - RIGHT CLICK FOR MENU");
    state.elapsed=gui->elapsedSecs-gui->launchBufferSecs;
    state.total=gui->totalSecs; state.volume=gui->volumePercent;
    state.playing=gui->playbackActive && gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING;
    state.mono=gui->mono; state.rate=gGuiPlaybackStatus.sampleRate;
    state.bitrate=gGuiPlaybackStatus.radioBitrateKbps;
    skin_player_update(gSkinPlayer,&state,tick);
}
