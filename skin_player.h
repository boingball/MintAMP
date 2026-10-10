#ifndef MINTAMP_SKIN_PLAYER_H
#define MINTAMP_SKIN_PLAYER_H
#include "winamp_skin.h"
#include <exec/types.h>

typedef struct SkinPlayer SkinPlayer;
typedef struct SkinEvent { int action, value, released; } SkinEvent;
SkinPlayer *skin_player_open(const char *name, int argc, char **argv);
void skin_player_close(SkinPlayer *player);
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
const char *skin_player_audio_arg(int argc, char **argv);
int skin_player_is_skin(const char *path);
#endif
