/* Included only by the MintAMP-SR frontend. Reuse its established actions. */
static void MrSkinHideSettings(MrApp *app)
{
    if (!app->win) return;
    SyncFromGadgets(app);
    RA_CloseWindow(app->winObj); app->win=NULL;
}
static void MrSkinPoll(MrApp *app,int *done)
{
    SkinEvent e;
    while (skin_player_poll(gSkinPlayer,&e)) {
        switch (e.action) {
        case SKIN_QUIT: *done=1; return;
        case SKIN_SETTINGS:
            if (app->win) MrSkinHideSettings(app); else MrUniconify(app); break;
        case SKIN_BROWSE:
            gSkinPendingTrack=-1; BrowseForFile(app); break;
        case SKIN_PLAY:
            gSkinPendingTrack=-1;
            if (app->win) SyncFromGadgets(app);
            if (!app->inputName[0]) {
                skin_player_notice(gSkinPlayer,"OPEN AUDIO OR CHOOSE INTERNET RADIO"); break;
            }
            if (MrIsRadioInput(app->inputName)) RadioProbeUrlAndStart(app,app->inputName,NULL);
            else StartPlayback(app);
            break;
        case SKIN_STOP:
            gSkinPendingTrack=-1; StopPlayback(app); break;
        case SKIN_PREVIOUS: case SKIN_NEXT: {
            int index=app->playlistCurrent+(e.action==SKIN_NEXT ? 1 : -1);
            if (index<0 || index>=app->playlistCount) {
                skin_player_notice(gSkinPlayer,"NO PLAYLIST TRACK IN THAT DIRECTION"); break;
            }
            gSkinPendingTrack=index;
            app->queuedStreamUrl[0]=0; app->playlistNextPending=0;
            if (app->playbackActive || app->playbackDonePending) {
                StopPlayback(app);
            }
            break;
        }
        case SKIN_RADIO:
            gSkinPendingTrack=-1;
            if (app->hasNetwork) OpenRadioWindow(app);
            else skin_player_notice(gSkinPlayer,"NO TCP/IP STACK AVAILABLE");
            break;
        case SKIN_PLAYLIST:
            gSkinPendingTrack=-1;
            if (app->plWinObj) ClosePlaylistWindow(app); else OpenPlaylistWindow(app); break;
        case SKIN_VOLUME_SET:
            if (app->volumePercent!=e.value) {
                app->volumePercent=e.value;
                gMiniAmp3RequestedVolume=(unsigned short)e.value; ++gMiniAmp3VolumeSequence;
                MrSetGadgetAttr(app->volumeGad,app->win,SLIDER_Level,(ULONG)e.value);
            }
            if (e.released) SaveSettings(app);
            break;
        case SKIN_SEEK:
            if (MrIsRadioInput(app->inputName) || app->totalSecs<=0 || !app->playbackActive)
                skin_player_notice(gSkinPlayer,"SEEK NEEDS A PLAYING LOCAL TRACK");
            else MrSeekRelative(app,(int)((long)app->totalSecs*e.value/100)-app->elapsedSecs);
            break;
        case SKIN_UNSUPPORTED:
            skin_player_notice(gSkinPlayer,"PAUSE BALANCE SHUFFLE REPEAT NOT SUPPORTED"); break;
        }
    }
    if (!*done && gSkinPendingTrack>=0 && !app->playbackActive && !app->playbackDonePending) {
        int index=gSkinPendingTrack; gSkinPendingTrack=-1;
        if (index<app->playlistCount) PlaylistLoadCurrent(app,index,1);
    }
}
static void MrSkinUpdate(MrApp *app,int tick)
{
    SkinState state;
    memset(&state,0,sizeof(state));
    if (MrIsRadioInput(app->inputName))
        MrCopyVolatileString(state.title,sizeof(state.title),gGuiPlaybackStatus.radioTitle);
    if (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_ERROR)
        SafeCopy(state.title,sizeof(state.title),app->shownStatus);
    if (!state.title[0] && app->shownTitle[0] && strcmp(app->shownTitle,"-"))
        SafeCopy(state.title,sizeof(state.title),app->shownTitle);
    if (!state.title[0]) SafeCopy(state.title,sizeof(state.title),
        app->inputName[0] ? app->inputName : "MINTAMP - RIGHT CLICK FOR MENU");
    state.elapsed=app->elapsedSecs; state.total=app->totalSecs; state.volume=app->volumePercent;
    state.playing=app->playbackActive && gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING;
    state.mono=app->mono; state.rate=gGuiPlaybackStatus.sampleRate;
    state.bitrate=gGuiPlaybackStatus.radioBitrateKbps;
    skin_player_update(gSkinPlayer,&state,tick);
}
