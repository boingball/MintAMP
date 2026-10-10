/* Classic Winamp bitmap skins. No Windows or toolkit dependencies. */
#ifndef MINTAMP_WINAMP_SKIN_H
#define MINTAMP_WINAMP_SKIN_H
#include <stddef.h>

#define SKIN_WIDTH 275
#define SKIN_HEIGHT 116
#define SKIN_MAX_FILE (8UL * 1024UL * 1024UL)
#define SKIN_MAX_BITMAP (2UL * 1024UL * 1024UL)
#define SKIN_MAX_TOTAL (4UL * 1024UL * 1024UL)

enum SkinAssetId {
    SKIN_MAIN, SKIN_BUTTONS, SKIN_TITLEBAR, SKIN_NUMBERS, SKIN_TEXT,
    SKIN_VOLUME, SKIN_BALANCE, SKIN_POSITION, SKIN_PLAYPAUS,
    SKIN_MONOSTER, SKIN_SHUFREP, SKIN_PLEDIT, SKIN_ASSET_COUNT
};
typedef struct SkinBitmap {
    unsigned width, height;
    unsigned char *rgb;
} SkinBitmap;
typedef struct WinampSkin {
    SkinBitmap assets[SKIN_ASSET_COUNT];
    unsigned playlist_normal, playlist_current, playlist_background, playlist_selected;
} WinampSkin;

/* Destination must be zero-initialised. Failure leaves it unchanged. */
int skin_load_memory(WinampSkin *skin, const unsigned char *zip, size_t size,
                     char *error, size_t error_size);
int skin_load_file(WinampSkin *skin, const char *path, char *error, size_t error_size);
int skin_decode_bmp(SkinBitmap *bmp, const unsigned char *data, size_t size,
                    char *error, size_t error_size);
void skin_free(WinampSkin *skin);

enum SkinAction {
    SKIN_NONE, SKIN_PLAY, SKIN_STOP, SKIN_PREVIOUS, SKIN_NEXT,
    SKIN_BROWSE, SKIN_SETTINGS, SKIN_PLAYLIST, SKIN_RADIO,
    SKIN_QUIT, SKIN_SELECT, SKIN_SIZE, SKIN_VOLUME_SET, SKIN_SEEK,
    SKIN_UNSUPPORTED, SKIN_DRAG, SKIN_TRACK_SELECT, SKIN_TRACK_PLAY,
    SKIN_PLAYLIST_OPTIONS, SKIN_PLAYLIST_CLOSE, SKIN_PLAYLIST_SCROLL
};
typedef struct SkinState {
    char title[256];
    int elapsed, total, volume, playing, mono, bitrate, rate;
    int pressed, scroll, playlist_visible;
} SkinState;
typedef void (*SkinBlit)(void *ctx, int asset, int sx, int sy,
                          int width, int height, int x, int y);
typedef void (*SkinFill)(void *ctx, unsigned rgb, int x, int y, int w, int h);

#define SKIN_PLAYLIST_HEIGHT 232
#define SKIN_PLAYLIST_ROWS 17
typedef struct SkinPlaylistState {
    int count, selected, current, top;
    char rows[SKIN_PLAYLIST_ROWS][80];
} SkinPlaylistState;
typedef void (*SkinLabel)(void *ctx, const char *text, unsigned rgb,
                         unsigned background, int x, int y, int width);
int skin_playlist_top(int top, int count);
int skin_playlist_hit_test(int x, int y);
void skin_playlist_render(const WinampSkin *skin, const SkinPlaylistState *now,
                          const SkinPlaylistState *old, SkinBlit blit,
                          SkinFill fill, SkinLabel label, void *ctx);

/* Coordinates use the unscaled 275x116 layout. Unsupported optional sheets
 * use neutral controls; missing mandatory background/buttons/text fail load. */
int skin_hit_test(int x, int y);
int skin_slider_value(int action, int x);
void skin_render(const WinampSkin *skin, const SkinState *now,
                 const SkinState *old, SkinBlit blit, SkinFill fill, void *ctx);
#endif
