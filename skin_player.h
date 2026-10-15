#ifndef MINTAMP_SKIN_PLAYER_H
#define MINTAMP_SKIN_PLAYER_H
#include "winamp_skin.h"
#include "skin_controls.h"
#include <exec/types.h>

typedef struct SkinPlayer SkinPlayer;
typedef struct SkinEvent { int action, value, released, target, selection; } SkinEvent;
SkinPlayer *skin_player_open(const char *name, int argc, char **argv);
void skin_player_close(SkinPlayer *player);
/* Close for a live switch to Classic: ENVARC persistence waits for the next
 * skin_player_close() or skin_player_shutdown() after audio retirement. */
void skin_player_suspend(SkinPlayer *player);
ULONG skin_player_signal(SkinPlayer *player);
struct Window *skin_player_window(SkinPlayer *player);
int skin_player_poll(SkinPlayer *player, SkinEvent *event);
void skin_player_update(SkinPlayer *player, const SkinState *state, int tick);
void skin_player_notice(SkinPlayer *player, const char *message);
/* Returns zero when artwork/screen/memory requires the native fallback. */
int skin_player_playlist_toggle(SkinPlayer *player);
typedef const char *(*SkinPlaylistName)(void *ctx, int index);
void skin_player_playlist_update(SkinPlayer *player, int count, int selected,
                                int current, SkinPlaylistName name, void *ctx);
int skin_player_order(SkinPlayer *player,int count,int current,int direction,int natural);
int skin_player_paths(SkinPlayer *player,const char *paths,unsigned stride,int count,int current,int focus);
void skin_player_order_cancel(SkinPlayer *player);
void skin_player_order_reset(SkinPlayer *player,int count,int current);
SkinSelection *skin_player_selection(SkinPlayer *player,int count,int focus);
void skin_player_save(SkinPlayer *player);
void skin_player_wake(const char *task_name,int pause);
int skin_player_choice(SkinPlayer *player,const char *text,const char *choices);
void skin_player_debug(SkinPlayer *player,long buffer_ms,long spare_ms,unsigned long underruns,
                       unsigned long radio_bytes,const char *station);
void skin_player_playlist_durations(SkinPlayer *player,const int *durations);
const char *skin_player_audio_arg(int argc, char **argv);
/* 64x64 RGB artwork from the frontend; hashed only while the window shows. */
void skin_player_artwork(SkinPlayer *player,const unsigned char *rgb,int valid,int loading,int radio);
/* Modal single-line requester on the skin's screen; 1 when accepted. */
int skin_player_request_text(SkinPlayer *player,const char *title,char *buffer,int size);
/* Classic start: apply the saved skin balance/EQ without opening a skin. */
void skin_player_restore_audio(const char *name);
/* Final exit, after the last skin_player_close(). */
void skin_player_shutdown(void);
int skin_player_is_skin(const char *path);
#endif
