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
    SKIN_MONOSTER, SKIN_SHUFREP, SKIN_ASSET_COUNT
};
typedef struct SkinBitmap {
    unsigned width, height;
    unsigned char *rgb;
} SkinBitmap;
typedef struct WinampSkin {
    SkinBitmap assets[SKIN_ASSET_COUNT];
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
    SKIN_UNSUPPORTED, SKIN_DRAG
};
typedef struct SkinState {
    char title[256];
    int elapsed, total, volume, playing, mono, bitrate, rate;
    int pressed, scroll;
} SkinState;
typedef void (*SkinBlit)(void *ctx, int asset, int sx, int sy,
                          int width, int height, int x, int y);
typedef void (*SkinFill)(void *ctx, unsigned rgb, int x, int y, int w, int h);

/* Coordinates use the unscaled 275x116 layout. Unsupported optional sheets
 * use neutral controls; missing mandatory background/buttons/text fail load. */
int skin_hit_test(int x, int y);
int skin_slider_value(int action, int x);
void skin_render(const WinampSkin *skin, const SkinState *now,
                 const SkinState *old, SkinBlit blit, SkinFill fill, void *ctx);
#endif
