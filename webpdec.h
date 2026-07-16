#ifndef WEBPDEC_H
#define WEBPDEC_H

/*
 * webpdec -- a small, self-contained WebP decoder for the Radio Browser
 * station-favicon artwork path (see amiga_mp3gui.c / minimp3r.c).  It is a
 * decoder only, targeting the two "simple file format" bitstreams that real
 * favicons use -- lossy VP8 and lossless VP8L -- plus enough VP8X (extended)
 * container parsing to locate the image chunk (any ALPHA/animation chunks are
 * ignored; favicons are downsampled to opaque thumbnails anyway).
 *
 * The decoder is written to be endian-neutral: every multi-byte field in the
 * bitstream is assembled from individual bytes, so it behaves the same on the
 * little-endian host used for testing and on the big-endian m68k target.
 *
 * Output is a freshly malloc()'d, tightly packed 24-bit RGB buffer
 * (w*h*3 bytes, row-major, top-to-bottom).  The caller owns it and must
 * free() it.  Alpha is not produced (favicon thumbnails are opaque).
 */

/* Return codes (0 == success, negatives are errors). */
#define WEBP_OK              0
#define WEBP_ERR_BAD_ARG   (-1)
#define WEBP_ERR_NOT_WEBP  (-2)   /* missing RIFF/WEBP container signature   */
#define WEBP_ERR_UNSUPPORTED (-3) /* a WebP feature this decoder omits       */
#define WEBP_ERR_TRUNCATED (-4)   /* ran off the end of the input            */
#define WEBP_ERR_CORRUPT   (-5)   /* malformed bitstream                     */
#define WEBP_ERR_NOMEM     (-6)   /* out of memory                           */
#define WEBP_ERR_TOO_LARGE (-7)   /* declared dimensions exceed max_dim      */

/* True (non-zero) if the buffer starts with a RIFF....WEBP container. */
int webp_is_webp(const unsigned char *data, unsigned long size);

/* Reads just the container/header far enough to report the pixel dimensions,
 * without allocating or decoding the image body.  Returns WEBP_OK and fills
 * out_w/out_h, or a negative error.  Either out pointer may be NULL. */
int webp_get_info(const unsigned char *data, unsigned long size,
                  unsigned *out_w, unsigned *out_h);

/* Decodes a WebP image to a malloc()'d 24-bit RGB buffer.  On success returns
 * WEBP_OK, sets out_rgb to the buffer (caller free()s) and out_w/out_h to
 * the dimensions.  Images whose width or height exceed max_dim are rejected
 * with WEBP_ERR_TOO_LARGE before any large allocation (favicon callers pass a
 * small cap so a tiny compressed file can't declare a huge canvas).  A
 * max_dim <= 0 disables the check. */
int webp_decode_rgb(const unsigned char *data, unsigned long size,
                    int max_dim,
                    unsigned char **out_rgb, unsigned *out_w, unsigned *out_h);

#endif /* WEBPDEC_H */
