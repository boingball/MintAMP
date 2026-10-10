#define gSupportedExtPattern "#?.(mp3|flac|aac|ogg|oga|wav|wma|8svx|iff|svx|aif|aiff)"
#include "skin_playlist_actions.inc"
#undef gSupportedExtPattern
static int MrSkinAdd(void *ctx,const char *path,const char *title)
{ return PlaylistAddEntry((MrApp *)ctx,path,title); }
static void MrSkinFiles(void *ctx) { PlaylistAddFiles((MrApp *)ctx); }
static void MrSkinSave(void *ctx)
{ MrApp *app=(MrApp *)ctx; PlaylistSaveM3U(app); skin_player_notice(gSkinPlayer,app->shownStatus); }
static void MrSkinRefresh(void *ctx) { RefreshPlaylistView((MrApp *)ctx); }
static SkinPlaylistOps MrSkinOps(MrApp *app)
{
    SkinPlaylistOps o;
    memset(&o,0,sizeof(o)); o.ctx=app;
    o.add=MrSkinAdd; o.files=MrSkinFiles; o.save=MrSkinSave; o.refresh=MrSkinRefresh;
    o.list.paths=&app->playlist[0][0]; o.list.path_size=sizeof(app->playlist[0]);
    o.list.titles=&app->playlistTitles[0][0]; o.list.title_size=sizeof(app->playlistTitles[0]);

    o.list.durations=app->playlistDurations;
    o.list.count=&app->playlistCount; o.list.current=&app->playlistCurrent; o.list.selected=&app->playlistSelected;
    o.list.selection=skin_player_selection(gSkinPlayer,app->playlistCount,app->playlistSelected);
    return o;
}
/* Included only by the MintAMP-SR frontend. Reuse its established actions. */
static void MrSkinHideSettings(MrApp *app)
{
    if (!app->win) return;
    SyncFromGadgets(app);
    RA_CloseWindow(app->winObj); app->win=NULL;
}
static const char *MrSkinPlaylistName(void *ctx,int index)
{
    MrApp *app=(MrApp *)ctx;
    return app->playlistTitles[index][0] ? app->playlistTitles[index] :
        (MrIsRadioInput(app->playlist[index]) ? app->playlist[index] : PlaylistBaseName(app->playlist[index]));
}
static void MrSkinPlaylistUpdate(MrApp *app)
{
    if (app->playlistCurrent>=0 && app->playlistCurrent<app->playlistCount && app->totalSecs>0 && !MrIsRadioInput(app->inputName)) app->playlistDurations[app->playlistCurrent]=app->totalSecs;
    skin_player_playlist_durations(gSkinPlayer,app->playlistDurations);
    skin_player_playlist_update(gSkinPlayer,app->playlistCount,app->playlistSelected,
        app->playlistCurrent,MrSkinPlaylistName,app);
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
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1; BrowseForFile(app); break;
        case SKIN_PLAY:
            if (gSkinAudio.pause_requested || gSkinAudio.paused) { skin_player_wake("MintAMP playback",0); break; }
            if (app->playbackActive) break;
            gSkinPendingTrack=-1;
            if (app->win) SyncFromGadgets(app);
            if (!app->inputName[0]) {
                if (app->playlistCount>0) {
                    skin_player_order_cancel(gSkinPlayer);
                    gSkinPendingTrack=app->playlistSelected>=0 && app->playlistSelected<app->playlistCount ? app->playlistSelected : 0;
                    app->queuedStreamUrl[0]=0; app->playlistNextPending=0;
                    break;
                }
                skin_player_notice(gSkinPlayer,"OPEN AUDIO OR CHOOSE INTERNET RADIO"); break;
            }
            if (MrIsRadioInput(app->inputName)) RadioProbeUrlAndStart(app,app->inputName,NULL);
            else StartPlayback(app);
            break;
        case SKIN_PAUSE:
            if (app->playbackActive && !app->playbackDonePending &&
                (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING || gGuiPlaybackStatus.phase==GUIPLAY_PHASE_UNDERRUN)) {
                skin_player_wake("MintAMP playback",!gSkinAudio.pause_requested);
                skin_player_notice(gSkinPlayer,gSkinAudio.pause_requested ? "PAUSE PENDING - QUEUED AUDIO WILL FINISH" : "RESUMING");
            } else skin_player_notice(gSkinPlayer,"PAUSE REQUIRES PLAYBACK");
            break;
        case SKIN_BALANCE_SET:
            if (app->playbackActive && !gSkinAudio.output_stereo) skin_player_notice(gSkinPlayer,"BALANCE REQUIRES STEREO PAULA OUTPUT");
            else { gSkinAudio.balance=e.value; if (e.released) skin_player_save(gSkinPlayer); }
            break;
        case SKIN_DEBUG: {
            char station[128]; MrCopyVolatileString(station,sizeof(station),gGuiPlaybackStatus.radioStationName);
            skin_player_debug(gSkinPlayer,gGuiPlaybackStatus.halfBufferMs,gGuiPlaybackStatus.spareMs,
                gGuiPlaybackStatus.underruns,gGuiPlaybackStatus.radioBufferedBytes,station); break;
        }
        case SKIN_LIST_ADD: case SKIN_LIST_REM: case SKIN_LIST_SEL: case SKIN_LIST_MISC: case SKIN_LIST_FILE:
        case SKIN_TRACK_MOVE: case SKIN_TRACK_REMOVE: {
            SkinPlaylistOps o=MrSkinOps(app);
            gSkinPendingTrack=-1; SkinPlaylistAction(&o,&e); MrSkinPlaylistUpdate(app); break;
        }
        case SKIN_STOP:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1; StopPlayback(app); break;
        case SKIN_TRACK_SELECT:
            if (e.value>=0 && e.value<app->playlistCount) {
                skin_selection_set(skin_player_selection(gSkinPlayer,app->playlistCount,app->playlistSelected),e.value,e.selection);
                app->playlistSelected=e.value; RefreshPlaylistView(app); MrSkinPlaylistUpdate(app);
            }
            break;
        case SKIN_TRACK_PLAY: case SKIN_PREVIOUS: case SKIN_NEXT: {
            int index=e.action==SKIN_TRACK_PLAY ? e.value :
                skin_player_order(gSkinPlayer,app->playlistCount,app->playlistCurrent,e.action==SKIN_NEXT ? 1 : -1,0);
            if (index<0 || index>=app->playlistCount) {
                skin_player_notice(gSkinPlayer,"NO PLAYLIST TRACK IN THAT DIRECTION"); break;
            }
            if (e.action==SKIN_TRACK_PLAY) skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=index;
            app->queuedStreamUrl[0]=0; app->playlistNextPending=0;
            if (app->playbackActive || app->playbackDonePending) {
                StopPlayback(app);
            }
            break;
        }
        case SKIN_RADIO:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1;
            if (app->hasNetwork) OpenRadioWindow(app);
            else skin_player_notice(gSkinPlayer,"NO TCP/IP STACK AVAILABLE");
            break;
        case SKIN_PLAYLIST:
            MrSkinPlaylistUpdate(app);
            if (skin_player_playlist_toggle(gSkinPlayer)) break;
            if (app->plWinObj) ClosePlaylistWindow(app); else OpenPlaylistWindow(app); break;
        case SKIN_PLAYLIST_OPTIONS:
            if (!app->plWinObj) OpenPlaylistWindow(app);
            if (app->plWin) { WindowToFront(app->plWin); ActivateWindow(app->plWin); }
            break;
        case SKIN_VOLUME_SET:
            if (app->volumePercent!=e.value) {
                app->volumePercent=e.value;
                gMiniAmp3RequestedVolume=(unsigned short)e.value; ++gMiniAmp3VolumeSequence;
                MrSetGadgetAttr(app->volumeGad,app->win,SLIDER_Level,(ULONG)e.value);
            }
            if (e.released) SaveSettings(app);
            break;
        case SKIN_SEEK:
            if (gSkinAudio.pause_requested || !gSkinAudio.eq_supported || MrIsRadioInput(app->inputName) || app->totalSecs<=0 || !app->playbackActive)
                skin_player_notice(gSkinPlayer,"SEEK REQUIRES AN UNPAUSED LOCAL MP3");
            else MrSeekRelative(app,(int)((long)app->totalSecs*e.value/100)-app->elapsedSecs);
            break;
        case SKIN_UNSUPPORTED:
            skin_player_notice(gSkinPlayer,"CONTROL NOT AVAILABLE"); break;
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
    if (!state.title[0] && MrIsRadioInput(app->inputName))
        MrCopyVolatileString(state.title,sizeof(state.title),gGuiPlaybackStatus.radioStationName);
    if (!state.title[0] && app->shownTitle[0] && strcmp(app->shownTitle,"-"))
        SafeCopy(state.title,sizeof(state.title),app->shownTitle);
    if (!state.title[0]) SafeCopy(state.title,sizeof(state.title),
        app->inputName[0] ? app->inputName : "MINTAMP - RIGHT CLICK FOR MENU");
    state.elapsed=app->elapsedSecs; if (gSkinAudio.position_valid && app->playbackActive) state.elapsed=(int)(gSkinAudio.position_ms/1000UL);
    if (!app->playbackActive && !gSkinAudio.completed_ok) state.elapsed=0;
    state.total=app->totalSecs; state.volume=app->volumePercent;
    state.playing=app->playbackActive && (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING || gGuiPlaybackStatus.phase==GUIPLAY_PHASE_UNDERRUN);
    state.paused=gSkinAudio.paused;
    state.mono=gSkinAudio.channels==1; state.rate=gGuiPlaybackStatus.sampleRate;
    state.bitrate=gSkinAudio.bitrate ? gSkinAudio.bitrate : gGuiPlaybackStatus.radioBitrateKbps;
    snprintf(state.debug_text,sizeof(state.debug_text),"BUF %luMS SPARE %ldMS UNDERRUN %lu",gGuiPlaybackStatus.halfBufferMs,gGuiPlaybackStatus.spareMs,gGuiPlaybackStatus.underruns);
    skin_player_update(gSkinPlayer,&state,tick);
    MrSkinPlaylistUpdate(app);
}
