/*
 * webpdec -- small self-contained WebP decoder (see webpdec.h).
 *
 * Supports the two "simple file format" bitstreams real favicons use:
 *   - lossless VP8L
 *   - lossy    VP8
 * plus enough VP8X (extended) container parsing to locate the image chunk.
 *
 * The bitstream constant tables and the arithmetic of the transforms/
 * predictors are taken from Google's libwebp reference decoder (BSD-3), which
 * is the authority on the format; the surrounding structure is a compact
 * re-implementation tuned for decoding one small in-memory image to packed
 * 24-bit RGB.  Everything is assembled from individual bytes so the result is
 * identical on the little-endian test host and the big-endian m68k target.
 *
 * Test harness build (host):
 *   gcc -std=gnu89 -Wall -Wextra -DWEBPDEC_TEST webpdec.c -o /tmp/webpdec_test
 *   /tmp/webpdec_test in.webp out.ppm
 */

#include "webpdec.h"

#include <stdlib.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static unsigned webp_rd_le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long webp_rd_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- container ------------------------------------------------------------ */

/* Locates the primary image chunk ("VP8 " lossy or "VP8L" lossless) inside a
 * RIFF/WEBP container.  Handles the plain simple formats and the VP8X extended
 * container (walking its chunk list, skipping ALPH/ANIM/etc.).  On success
 * sets *is_lossless, *chunk (points at the chunk payload) and *chunk_size. */
static int webp_find_image_chunk(const unsigned char *data, unsigned long size,
                                 int *is_lossless,
                                 const unsigned char **chunk,
                                 unsigned long *chunk_size)
{
    unsigned long riff_size;
    unsigned long pos;

    if (!data || size < 12) return WEBP_ERR_NOT_WEBP;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
        return WEBP_ERR_NOT_WEBP;
    riff_size = webp_rd_le32(data + 4);
    /* The RIFF size counts everything after the first 8 bytes; clamp to the
     * buffer we actually have so a lying header can't walk us off the end. */
    if (riff_size + 8 < size)
        size = riff_size + 8;

    pos = 12;
    while (pos + 8 <= size) {
        const unsigned char *id = data + pos;
        unsigned long clen = webp_rd_le32(data + pos + 4);
        unsigned long body = pos + 8;

        if (body + clen > size) clen = size - body; /* tolerate truncation */

        if (memcmp(id, "VP8L", 4) == 0) {
            *is_lossless = 1;
            *chunk = data + body;
            *chunk_size = clen;
            return WEBP_OK;
        }
        if (memcmp(id, "VP8 ", 4) == 0) {
            *is_lossless = 0;
            *chunk = data + body;
            *chunk_size = clen;
            return WEBP_OK;
        }
        /* VP8X and any other chunk (ALPH, ANIM, ANMF, ICCP, EXIF, XMP): skip.
         * (ANMF would nest frames, but a still favicon never uses it; if we hit
         * one we just keep scanning for a top-level VP8/VP8L.) */
        pos = body + clen + (clen & 1); /* chunks are padded to even size */
    }
    return WEBP_ERR_UNSUPPORTED;
}

int webp_is_webp(const unsigned char *data, unsigned long size)
{
    return data && size >= 12 &&
           memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
}

/* ======================================================================== *
 *  Lossless (VP8L)
 * ======================================================================== */

#define VP8L_MAGIC          0x2f
#define L_MAX_HUFF_BITS     15
#define NUM_LITERAL_CODES   256
#define NUM_LENGTH_CODES    24
#define NUM_DISTANCE_CODES  40
#define NUM_CODE_LENGTH_CODES 19
#define HUFF_CODES_PER_GROUP  5

/* LSB-first bit reader over an in-memory buffer. */
typedef struct {
    const unsigned char *buf;
    unsigned long len;
    unsigned long bytepos;
    unsigned bitpos;      /* 0..7 */
    int eos;
} LBits;

static void lbits_init(LBits *b, const unsigned char *buf, unsigned long len)
{
    b->buf = buf; b->len = len; b->bytepos = 0; b->bitpos = 0; b->eos = 0;
}

static unsigned lbits_read(LBits *b, int n)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < n; i++) {
        unsigned bit;
        if (b->bytepos >= b->len) { b->eos = 1; bit = 0; }
        else {
            bit = (b->buf[b->bytepos] >> b->bitpos) & 1u;
            if (++b->bitpos == 8) { b->bitpos = 0; b->bytepos++; }
        }
        v |= bit << i;
    }
    return v;
}

/* Canonical (DEFLATE-style) Huffman decode, after Mark Adler's "puff".
 * Symbols are sorted by (length, symbol); decode() walks the code lengths
 * accumulating one bit at a time.  Valid for VP8L, which uses the same
 * canonical prefix coding as DEFLATE read LSB-first. */
typedef struct {
    short count[L_MAX_HUFF_BITS + 1];
    short *symbol;    /* length == alphabet size */
    int nsym;
    int single;       /* >=0: a 0-bit code that always yields this symbol */
} LHuff;

/* Builds counts/symbol[] from per-symbol code lengths.  Returns 0 on success,
 * <0 if the code is over-subscribed/invalid.  A single-symbol code (only one
 * non-zero length, or exactly one used symbol) is accepted as a special case
 * (VP8L "simple" codes and degenerate trees). */
static int lhuff_build(LHuff *h, const int *lengths, int n, short *symstore)
{
    int len, sym, left, used, offs[L_MAX_HUFF_BITS + 1];

    h->symbol = symstore;
    h->nsym = n;
    h->single = -1;
    for (len = 0; len <= L_MAX_HUFF_BITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) {
        if (lengths[sym] < 0 || lengths[sym] > L_MAX_HUFF_BITS) return -1;
        h->count[lengths[sym]]++;
    }
    used = n - h->count[0];
    if (used == 0) return -1;  /* all-zero code is invalid (matches libwebp) */

    /* place symbols sorted by (length, symbol) for the canonical decoder */
    offs[1] = 0;
    for (len = 1; len < L_MAX_HUFF_BITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (sym = 0; sym < n; sym++)
        if (lengths[sym] != 0) h->symbol[offs[lengths[sym]]++] = (short)sym;

    /* A code with exactly one used symbol is a 0-bit code that always yields
     * that symbol (libwebp's "special case code with only one value"). */
    if (used == 1) {
        h->single = h->symbol[0];
        return 0;
    }

    /* reject over-subscribed length sets */
    left = 1;
    for (len = 1; len <= L_MAX_HUFF_BITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    return 0;
}

static int lhuff_decode(LHuff *h, LBits *b)
{
    int len, code = 0, first = 0, index = 0;

    if (h->single >= 0) return h->single;   /* 0-bit single-symbol code */

    for (len = 1; len <= L_MAX_HUFF_BITS; len++) {
        unsigned bit;
        if (b->bytepos < b->len) {
            bit = (b->buf[b->bytepos] >> b->bitpos) & 1u;
            if (++b->bitpos == 8) { b->bitpos = 0; b->bytepos++; }
        } else {
            b->eos = 1;
            bit = 0;
            if (++b->bitpos == 8) { b->bitpos = 0; b->bytepos++; }
        }
        code |= (int)bit;
        {
            int count = h->count[len];
            if (code - count < first) return h->symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }
    return -1;
}

/* VP8L transform bookkeeping. */
typedef enum {
    L_PREDICTOR = 0, L_CROSS_COLOR = 1, L_SUBTRACT_GREEN = 2, L_COLOR_INDEX = 3
} LTransformType;

typedef struct {
    int type;
    int bits;            /* tile size bits (predictor/color); or index packing */
    int xsize, ysize;    /* size of the image this transform applies to */
    unsigned long *data; /* entropy sub-image (predictor/color) or color map   */
    int num_colors;      /* color-index only */
} LTransform;

/* One meta-huffman group: 5 canonical trees. */
typedef struct {
    LHuff htrees[HUFF_CODES_PER_GROUP];
} LHGroup;

typedef struct {
    LBits br;
    int xsize, ysize;

    LTransform transforms[4];
    int num_transforms;
    unsigned transforms_seen;

    /* color cache */
    int cache_bits;
    int cache_size;
    unsigned long *cache;   /* argb entries, or NULL */
} LDecoder;

static int l_subsample_size(int size, int bits)
{
    return (size + (1 << bits) - 1) >> bits;
}

/* ---- color cache ---------------------------------------------------------- */

static unsigned l_cache_index(unsigned long argb, int bits)
{
    /* (kHashMul * argb) >> (32 - bits), all in 32-bit. */
    unsigned long h = (argb * 0x1e35a7bdUL) & 0xffffffffUL;
    return (unsigned)(h >> (32 - bits));
}

/* ---- huffman code reading ------------------------------------------------- */

static const unsigned char kCodeLengthCodeOrder[NUM_CODE_LENGTH_CODES] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
static const unsigned char kCodeLengthExtraBits[3] = { 2, 3, 7 };
static const unsigned char kCodeLengthRepeatOffsets[3] = { 3, 3, 11 };

/* Reads the code lengths for one alphabet (the normal, non-"simple" path). */
static int l_read_code_lengths(LDecoder *d, int *code_lengths, int alphabet_size)
{
    LBits *br = &d->br;
    int ok = 0;
    int symbol, max_symbol, prev_code_len;
    int cl_lengths[NUM_CODE_LENGTH_CODES];
    LHuff cl_tree;
    short cl_syms[NUM_CODE_LENGTH_CODES];
    int num_codes, i;

    num_codes = (int)lbits_read(br, 4) + 4;
    if (num_codes > NUM_CODE_LENGTH_CODES) return -1;

    for (i = 0; i < NUM_CODE_LENGTH_CODES; i++) cl_lengths[i] = 0;
    for (i = 0; i < num_codes; i++) {
        cl_lengths[kCodeLengthCodeOrder[i]] = (int)lbits_read(br, 3);
    }
    if (lhuff_build(&cl_tree, cl_lengths, NUM_CODE_LENGTH_CODES, cl_syms) < 0)
        return -1;

    for (i = 0; i < alphabet_size; i++) code_lengths[i] = 0;

    /* optional max_symbol */
    if (lbits_read(br, 1)) {
        int length_nbits = 2 + 2 * (int)lbits_read(br, 3);
        max_symbol = 2 + (int)lbits_read(br, length_nbits);
        if (max_symbol > alphabet_size) return -1;
    } else {
        max_symbol = alphabet_size;
    }

    symbol = 0;
    prev_code_len = 8; /* default repeat value */
    while (symbol < alphabet_size) {
        int code_len;
        if (max_symbol-- == 0) break;
        code_len = lhuff_decode(&cl_tree, br);
        if (code_len < 0) return -1;
        if (code_len < 16) {
            code_lengths[symbol++] = code_len;
            if (code_len != 0) prev_code_len = code_len;
        } else {
            int use_prev = (code_len == 16);
            int slot = code_len - 16;
            int extra = kCodeLengthExtraBits[slot];
            int repeat = (int)lbits_read(br, extra) + kCodeLengthRepeatOffsets[slot];
            int fill = use_prev ? prev_code_len : 0;
            if (symbol + repeat > alphabet_size) return -1;
            while (repeat-- > 0) code_lengths[symbol++] = fill;
        }
    }
    ok = 1;
    (void)ok;
    return 0;
}

/* Reads one Huffman code (simple or normal) into 'h', using length storage in
 * 'lenbuf' (alphabet_size ints) and symbol storage 'symstore'. */
static int l_read_huffman_code(LDecoder *d, int alphabet_size,
                               LHuff *h, int *lenbuf, short *symstore)
{
    LBits *br = &d->br;
    int simple = (int)lbits_read(br, 1);
    int i;

    for (i = 0; i < alphabet_size; i++) lenbuf[i] = 0;

    if (simple) {
        /* Symbols/codes given directly.  Each named symbol gets code length 1;
         * if only one symbol is named it becomes a 0-bit code (handled by
         * lhuff_build's single-symbol detection). */
        int num_symbols = (int)lbits_read(br, 1) + 1;
        int first_is_8 = (int)lbits_read(br, 1);
        int s0 = (int)lbits_read(br, first_is_8 ? 8 : 1);
        if (s0 >= alphabet_size) return -1;
        lenbuf[s0] = 1;
        if (num_symbols == 2) {
            int s1 = (int)lbits_read(br, 8);
            if (s1 >= alphabet_size) return -1;
            lenbuf[s1] = 1;
        }
    } else {
        if (l_read_code_lengths(d, lenbuf, alphabet_size) < 0) return -1;
    }
    return lhuff_build(h, lenbuf, alphabet_size, symstore);
}

/* ---- prefix (length/distance) decoding ------------------------------------ */

static int l_get_copy_value(int sym, LBits *br)
{
    int extra_bits, offset;
    if (sym < 4) return sym + 1;
    extra_bits = (sym - 2) >> 1;
    offset = (2 + (sym & 1)) << extra_bits;
    return offset + (int)lbits_read(br, extra_bits) + 1;
}

#define L_CODE_TO_PLANE_CODES 120
static const unsigned char kCodeToPlane[L_CODE_TO_PLANE_CODES] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a,
    0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04,
    0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
    0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d,
    0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e,
    0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e,
    0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
    0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70
};

static int l_plane_code_to_distance(int xsize, int plane_code)
{
    if (plane_code > L_CODE_TO_PLANE_CODES) {
        return plane_code - L_CODE_TO_PLANE_CODES;
    } else {
        int dist_code = kCodeToPlane[plane_code - 1];
        int yoffset = dist_code >> 4;
        int xoffset = 8 - (dist_code & 0xf);
        int dist = yoffset * xsize + xoffset;
        return (dist >= 1) ? dist : 1;
    }
}

/* ---- meta-huffman group storage ------------------------------------------- */

typedef struct {
    LHGroup *groups;
    int num_groups;
    /* per-group backing storage for symbols/lengths */
    short *symstore;
    int *lenstore;
    /* meta-huffman image (maps tile -> group index), NULL if single group */
    unsigned long *huffman_image;
    int huffman_bits;
    int huffman_xsize;
    int color_cache_size;   /* for the green alphabet size */
} LHuffData;

static void l_free_huffdata(LHuffData *hd)
{
    if (!hd) return;
    if (hd->groups) free(hd->groups);
    if (hd->symstore) free(hd->symstore);
    if (hd->lenstore) free(hd->lenstore);
    if (hd->huffman_image) free(hd->huffman_image);
    hd->groups = NULL; hd->symstore = NULL; hd->lenstore = NULL;
    hd->huffman_image = NULL;
}

static int l_alphabet_size(int which, int color_cache_size)
{
    switch (which) {
    case 0: return NUM_LITERAL_CODES + NUM_LENGTH_CODES + color_cache_size; /* green */
    case 1: return NUM_LITERAL_CODES; /* red */
    case 2: return NUM_LITERAL_CODES; /* blue */
    case 3: return NUM_LITERAL_CODES; /* alpha */
    default: return NUM_DISTANCE_CODES; /* dist */
    }
}

static int l_decode_image_stream(LDecoder *d, int xsize, int ysize,
                                 int is_level0, unsigned long **out);

/* Reads all meta-huffman groups for an image of size xsize*ysize. */
static int l_read_huffman_codes(LDecoder *d, int xsize, int ysize,
                                int color_cache_size, int allow_recursion,
                                LHuffData *hd)
{
    LBits *br = &d->br;
    int num_groups = 1;
    int max_alpha, g, k;
    int rc = WEBP_ERR_CORRUPT;

    memset(hd, 0, sizeof(*hd));
    hd->color_cache_size = color_cache_size;

    if (allow_recursion && lbits_read(br, 1)) {
        /* use meta huffman: a sub-image assigns a group index per tile */
        int huffman_precision = (int)lbits_read(br, 3) + 2;
        int huffman_xsize = l_subsample_size(xsize, huffman_precision);
        int huffman_ysize = l_subsample_size(ysize, huffman_precision);
        unsigned long *himg = NULL;
        int i, npix, maxg = 0;

        if (l_decode_image_stream(d, huffman_xsize, huffman_ysize, 0, &himg) != WEBP_OK)
            return WEBP_ERR_CORRUPT;
        /* group index is stored in the (red<<8 | green) of each pixel */
        npix = huffman_xsize * huffman_ysize;
        for (i = 0; i < npix; i++) {
            int idx = (int)((himg[i] >> 8) & 0xffff);
            himg[i] = (unsigned long)idx;   /* store decoded group index */
            if (idx > maxg) maxg = idx;
        }
        num_groups = maxg + 1;
        hd->huffman_image = himg;
        hd->huffman_bits = huffman_precision;
        hd->huffman_xsize = huffman_xsize;
    }

    hd->num_groups = num_groups;
    hd->groups = (LHGroup *)calloc((size_t)num_groups, sizeof(LHGroup));
    if (!hd->groups) { rc = WEBP_ERR_NOMEM; goto fail; }

    /* backing storage: worst-case alphabet is green's. */
    max_alpha = l_alphabet_size(0, color_cache_size);
    hd->symstore = (short *)malloc(sizeof(short) * (size_t)max_alpha *
                                   (size_t)num_groups * HUFF_CODES_PER_GROUP);
    hd->lenstore = (int *)malloc(sizeof(int) * (size_t)max_alpha);
    if (!hd->symstore || !hd->lenstore) { rc = WEBP_ERR_NOMEM; goto fail; }

    for (g = 0; g < num_groups; g++) {
        for (k = 0; k < HUFF_CODES_PER_GROUP; k++) {
            int alpha = l_alphabet_size(k, color_cache_size);
            short *sstore = hd->symstore +
                ((size_t)(g * HUFF_CODES_PER_GROUP + k)) * (size_t)max_alpha;
            if (l_read_huffman_code(d, alpha, &hd->groups[g].htrees[k],
                                    hd->lenstore, sstore) < 0) {
                rc = WEBP_ERR_CORRUPT;
                goto fail;
            }
        }
    }
    return WEBP_OK;

fail:
    l_free_huffdata(hd);
    return rc;
}

static LHGroup *l_group_for_pos(LHuffData *hd, int x, int y)
{
    int idx;
    if (hd->num_groups == 1 || !hd->huffman_image) return &hd->groups[0];
    idx = (int)hd->huffman_image[(y >> hd->huffman_bits) * hd->huffman_xsize +
                                 (x >> hd->huffman_bits)];
    if (idx < 0 || idx >= hd->num_groups) idx = 0;
    return &hd->groups[idx];
}

/* Decodes the entropy-coded ARGB pixel data (with LZ77 + color cache). */
static int l_decode_pixels(LDecoder *d, int xsize, int ysize,
                           LHuffData *hd, unsigned long *out)
{
    LBits *br = &d->br;
    int len_code_limit = NUM_LITERAL_CODES + NUM_LENGTH_CODES;
    int color_cache_limit = len_code_limit + d->cache_size;
    int npix = xsize * ysize;
    int pos = 0;
    int x = 0, y = 0;
    int last_cached = 0;

    while (pos < npix) {
        LHGroup *grp = l_group_for_pos(hd, x, y);
        int code = lhuff_decode(&grp->htrees[0], br);
        if (br->eos || code < 0) return WEBP_ERR_CORRUPT;

        if (code < NUM_LITERAL_CODES) {           /* literal */
            int green = code;
            int red = lhuff_decode(&grp->htrees[1], br);
            int blue = lhuff_decode(&grp->htrees[2], br);
            int alpha = lhuff_decode(&grp->htrees[3], br);
            if (red < 0 || blue < 0 || alpha < 0) return WEBP_ERR_CORRUPT;
            out[pos] = ((unsigned long)alpha << 24) |
                       ((unsigned long)red << 16) |
                       ((unsigned long)green << 8) |
                       (unsigned long)blue;
            pos++; x++;
            if (x >= xsize) { x = 0; y++; }
        } else if (code < len_code_limit) {       /* backward reference */
            int length_sym = code - NUM_LITERAL_CODES;
            int length = l_get_copy_value(length_sym, br);
            int dist_symbol = lhuff_decode(&grp->htrees[4], br);
            int dist_code, dist, i;
            if (dist_symbol < 0) return WEBP_ERR_CORRUPT;
            dist_code = l_get_copy_value(dist_symbol, br);
            dist = l_plane_code_to_distance(xsize, dist_code);
            if (pos < dist || pos + length > npix) return WEBP_ERR_CORRUPT;
            for (i = 0; i < length; i++) {
                out[pos] = out[pos - dist];
                pos++;
            }
            x = pos % xsize; y = pos / xsize;
        } else if (code < color_cache_limit) {     /* color cache */
            int key = code - len_code_limit;
            /* insert everything produced since last cache sync */
            while (last_cached < pos) {
                d->cache[l_cache_index(out[last_cached], d->cache_bits)] =
                    out[last_cached];
                last_cached++;
            }
            if (key < 0 || key >= d->cache_size) return WEBP_ERR_CORRUPT;
            out[pos] = d->cache[key];
            pos++; x++;
            if (x >= xsize) { x = 0; y++; }
        } else {
            return WEBP_ERR_CORRUPT;
        }

        /* keep the color cache in sync up to the current position */
        if (d->cache) {
            while (last_cached < pos) {
                d->cache[l_cache_index(out[last_cached], d->cache_bits)] =
                    out[last_cached];
                last_cached++;
            }
        }
    }
    return WEBP_OK;
}

/* Decodes an image stream (transforms only at level0), returning the raw
 * (pre-... actually post-)transform ARGB pixels of size xsize*ysize. */
static int l_decode_image_stream(LDecoder *d, int xsize, int ysize,
                                 int is_level0, unsigned long **out)
{
    LBits *br = &d->br;
    LHuffData hd;
    unsigned long *pixels = NULL;
    int transform_xsize = xsize;
    int color_cache_bits = 0;
    int rc;

    /* Transforms only exist at the top level and only mutate the *width* seen
     * by the entropy decoder (color indexing). We handle them in the caller
     * for level0; sub-images (is_level0==0) never carry transforms. */
    (void)is_level0;

    /* color cache */
    if (lbits_read(br, 1)) {
        color_cache_bits = (int)lbits_read(br, 4);
        if (color_cache_bits < 1 || color_cache_bits > 11) return WEBP_ERR_CORRUPT;
    }

    rc = l_read_huffman_codes(d, transform_xsize, ysize,
                              color_cache_bits ? (1 << color_cache_bits) : 0,
                              is_level0, &hd);
    if (rc != WEBP_OK) return rc;

    /* set up color cache for this stream */
    d->cache_bits = color_cache_bits;
    d->cache_size = color_cache_bits ? (1 << color_cache_bits) : 0;
    d->cache = NULL;
    if (d->cache_size) {
        d->cache = (unsigned long *)calloc((size_t)d->cache_size, sizeof(unsigned long));
        if (!d->cache) { l_free_huffdata(&hd); return WEBP_ERR_NOMEM; }
    }

    pixels = (unsigned long *)malloc(sizeof(unsigned long) *
                                     (size_t)transform_xsize * (size_t)ysize);
    if (!pixels) {
        if (d->cache) { free(d->cache); d->cache = NULL; }
        l_free_huffdata(&hd);
        return WEBP_ERR_NOMEM;
    }

    rc = l_decode_pixels(d, transform_xsize, ysize, &hd, pixels);

    if (d->cache) { free(d->cache); d->cache = NULL; }
    l_free_huffdata(&hd);

    if (rc != WEBP_OK) { free(pixels); return rc; }
    *out = pixels;
    return WEBP_OK;
}

/* ---- inverse transforms --------------------------------------------------- */

static unsigned long l_add_pixels(unsigned long a, unsigned long b)
{
    unsigned long ag = (a & 0xff00ff00UL) + (b & 0xff00ff00UL);
    unsigned long rb = (a & 0x00ff00ffUL) + (b & 0x00ff00ffUL);
    return (ag & 0xff00ff00UL) | (rb & 0x00ff00ffUL);
}

static unsigned long l_average2(unsigned long a0, unsigned long a1)
{
    return (((a0 ^ a1) & 0xfefefefeUL) >> 1) + (a0 & a1);
}

static unsigned l_clip255(int a)
{
    if (a < 0) return 0;
    if (a > 255) return 255;
    return (unsigned)a;
}

static int l_addsub_full(int a, int b, int c) { return (int)l_clip255(a + b - c); }
static int l_addsub_half(int a, int b) { return (int)l_clip255(a + (a - b) / 2); }

static unsigned long l_clamped_add_sub_full(unsigned long c0, unsigned long c1, unsigned long c2)
{
    int a = l_addsub_full((int)(c0 >> 24), (int)(c1 >> 24), (int)(c2 >> 24));
    int r = l_addsub_full((int)((c0 >> 16) & 0xff), (int)((c1 >> 16) & 0xff), (int)((c2 >> 16) & 0xff));
    int g = l_addsub_full((int)((c0 >> 8) & 0xff), (int)((c1 >> 8) & 0xff), (int)((c2 >> 8) & 0xff));
    int b = l_addsub_full((int)(c0 & 0xff), (int)(c1 & 0xff), (int)(c2 & 0xff));
    return ((unsigned long)a << 24) | ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

static unsigned long l_clamped_add_sub_half(unsigned long c0, unsigned long c1, unsigned long c2)
{
    unsigned long ave = l_average2(c0, c1);
    int a = l_addsub_half((int)(ave >> 24), (int)(c2 >> 24));
    int r = l_addsub_half((int)((ave >> 16) & 0xff), (int)((c2 >> 16) & 0xff));
    int g = l_addsub_half((int)((ave >> 8) & 0xff), (int)((c2 >> 8) & 0xff));
    int b = l_addsub_half((int)(ave & 0xff), (int)(c2 & 0xff));
    return ((unsigned long)a << 24) | ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

static int l_sub3(int a, int b, int c)
{
    int pb = b - c, pa = a - c;
    if (pb < 0) pb = -pb;
    if (pa < 0) pa = -pa;
    return pb - pa;
}

static unsigned long l_select(unsigned long a, unsigned long b, unsigned long c)
{
    int pa_minus_pb =
        l_sub3((int)(a >> 24), (int)(b >> 24), (int)(c >> 24)) +
        l_sub3((int)((a >> 16) & 0xff), (int)((b >> 16) & 0xff), (int)((c >> 16) & 0xff)) +
        l_sub3((int)((a >> 8) & 0xff), (int)((b >> 8) & 0xff), (int)((c >> 8) & 0xff)) +
        l_sub3((int)(a & 0xff), (int)(b & 0xff), (int)(c & 0xff));
    return (pa_minus_pb <= 0) ? a : b;
}

#define ARGB_BLACK 0xff000000UL

static unsigned long l_predict(int mode, unsigned long left,
                               const unsigned long *top /* points at pixel above */)
{
    /* top[-1]=top-left, top[0]=top, top[1]=top-right */
    switch (mode) {
    case 0:  return ARGB_BLACK;
    case 1:  return left;
    case 2:  return top[0];
    case 3:  return top[1];
    case 4:  return top[-1];
    case 5:  return l_average2(l_average2(left, top[1]), top[0]);
    case 6:  return l_average2(left, top[-1]);
    case 7:  return l_average2(left, top[0]);
    case 8:  return l_average2(top[-1], top[0]);
    case 9:  return l_average2(top[0], top[1]);
    case 10: return l_average2(l_average2(left, top[-1]), l_average2(top[0], top[1]));
    case 11: return l_select(top[0], left, top[-1]);
    case 12: return l_clamped_add_sub_full(left, top[0], top[-1]);
    case 13: return l_clamped_add_sub_half(left, top[0], top[-1]);
    default: return ARGB_BLACK;
    }
}

/* predictor inverse transform, in place over 'data' (xsize*ysize argb) */
static void l_inverse_predictor(LTransform *t, unsigned long *data)
{
    int width = t->xsize, height = t->ysize;
    int tile_bits = t->bits;
    int tiles_per_row = l_subsample_size(width, tile_bits);
    int x, y;

    /* first pixel: black-add */
    data[0] = l_add_pixels(data[0], ARGB_BLACK);
    /* rest of first row: predict from left (mode 1) */
    for (x = 1; x < width; x++)
        data[x] = l_add_pixels(data[x], data[x - 1]);

    for (y = 1; y < height; y++) {
        unsigned long *row = data + (size_t)y * width;
        unsigned long *prev = data + (size_t)(y - 1) * width;
        const unsigned long *modes = t->data + (size_t)(y >> tile_bits) * tiles_per_row;
        /* first column: predict from top (mode 2) */
        row[0] = l_add_pixels(row[0], prev[0]);
        for (x = 1; x < width; x++) {
            int mode = (int)((modes[x >> tile_bits] >> 8) & 0xf);
            unsigned long pred = l_predict(mode, row[x - 1], prev + x);
            row[x] = l_add_pixels(row[x], pred);
        }
    }
}

static int l_color_delta(signed char pred, signed char color)
{
    return ((int)pred * (int)color) >> 5;
}

static void l_inverse_color(LTransform *t, unsigned long *data)
{
    int width = t->xsize, height = t->ysize;
    int tile_bits = t->bits;
    int tiles_per_row = l_subsample_size(width, tile_bits);
    int x, y;

    for (y = 0; y < height; y++) {
        unsigned long *row = data + (size_t)y * width;
        const unsigned long *codes = t->data + (size_t)(y >> tile_bits) * tiles_per_row;
        for (x = 0; x < width; x++) {
            unsigned long code = codes[x >> tile_bits];
            signed char g2r = (signed char)(code & 0xff);
            signed char g2b = (signed char)((code >> 8) & 0xff);
            signed char r2b = (signed char)((code >> 16) & 0xff);
            unsigned long argb = row[x];
            signed char green = (signed char)((argb >> 8) & 0xff);
            int new_red = (int)((argb >> 16) & 0xff);
            int new_blue = (int)(argb & 0xff);
            new_red += l_color_delta(g2r, green);
            new_red &= 0xff;
            new_blue += l_color_delta(g2b, green);
            new_blue += l_color_delta(r2b, (signed char)new_red);
            new_blue &= 0xff;
            row[x] = (argb & 0xff00ff00UL) | ((unsigned long)new_red << 16) | (unsigned long)new_blue;
        }
    }
}

static void l_inverse_subtract_green(int npix, unsigned long *data)
{
    int i;
    for (i = 0; i < npix; i++) {
        unsigned long argb = data[i];
        unsigned long green = (argb >> 8) & 0xff;
        unsigned long rb = argb & 0x00ff00ffUL;
        rb += (green << 16) | green;
        rb &= 0x00ff00ffUL;
        data[i] = (argb & 0xff00ff00UL) | rb;
    }
}

/* color-index inverse: expands packed indices back to argb via the palette.
 * 'src' has (subsampled) width t->xsize; 'dst' has full width full_width. */
static void l_inverse_color_index(LTransform *t, const unsigned long *src,
                                  unsigned long *dst, int full_width, int height)
{
    int bits = t->bits; /* 0,1,2,3 -> bits_per_pixel 8,4,2,1 */
    int bits_per_pixel = 8 >> bits;
    unsigned long *map = t->data;
    int y;

    if (bits_per_pixel == 8) {
        int n = full_width * height, i;
        for (i = 0; i < n; i++) {
            int idx = (int)(src[i] >> 8) & 0xff;
            dst[i] = map[idx];
        }
        return;
    }
    for (y = 0; y < height; y++) {
        const unsigned long *s = src + (size_t)y * t->xsize;
        unsigned long *dd = dst + (size_t)y * full_width;
        int x = 0, sx = 0;
        int per_byte = 1 << bits;
        int mask = (1 << bits_per_pixel) - 1;
        while (x < full_width) {
            int packed = (int)((s[sx++] >> 8) & 0xff);
            int k;
            for (k = 0; k < per_byte && x < full_width; k++) {
                dd[x++] = map[packed & mask];
                packed >>= bits_per_pixel;
            }
        }
    }
}

/* ======================================================================== */

static int l_decode(const unsigned char *chunk, unsigned long chunk_size,
                    int max_dim, unsigned char **out_rgb,
                    unsigned *out_w, unsigned *out_h)
{
    LDecoder d;
    LBits *br;
    int width, height, alpha, version;
    int transform_xsize;
    unsigned long *pixels = NULL;
    int rc, t;
    int color_index_full_width = 0;
    int have_color_index = 0;

    memset(&d, 0, sizeof(d));
    lbits_init(&d.br, chunk, chunk_size);
    br = &d.br;

    if (lbits_read(br, 8) != VP8L_MAGIC) return WEBP_ERR_CORRUPT;
    width = (int)lbits_read(br, 14) + 1;
    height = (int)lbits_read(br, 14) + 1;
    alpha = (int)lbits_read(br, 1);
    version = (int)lbits_read(br, 3);
    (void)alpha;
    if (version != 0) return WEBP_ERR_UNSUPPORTED;
    if (max_dim > 0 && (width > max_dim || height > max_dim))
        return WEBP_ERR_TOO_LARGE;

    d.xsize = width; d.ysize = height;
    transform_xsize = width;

    /* read transforms (level 0 only) */
    d.num_transforms = 0;
    d.transforms_seen = 0;
    while (lbits_read(br, 1)) {
        int type = (int)lbits_read(br, 2);
        LTransform *tr;
        if (d.num_transforms >= 4) return WEBP_ERR_CORRUPT;
        if (d.transforms_seen & (1u << type)) return WEBP_ERR_CORRUPT;
        d.transforms_seen |= (1u << type);
        tr = &d.transforms[d.num_transforms++];
        tr->type = type;
        tr->xsize = transform_xsize;
        tr->ysize = height;
        tr->data = NULL;
        tr->bits = 0;
        tr->num_colors = 0;

        if (type == L_PREDICTOR || type == L_CROSS_COLOR) {
            tr->bits = 2 + (int)lbits_read(br, 3);
            rc = l_decode_image_stream(&d, l_subsample_size(transform_xsize, tr->bits),
                                       l_subsample_size(height, tr->bits), 0, &tr->data);
            if (rc != WEBP_OK) goto cleanup;
        } else if (type == L_COLOR_INDEX) {
            int num_colors = (int)lbits_read(br, 8) + 1;
            int bits = (num_colors > 16) ? 0 : (num_colors > 4) ? 1 :
                       (num_colors > 2) ? 2 : 3;
            unsigned long *rawmap = NULL;
            int final_colors = 1 << (8 >> bits);
            unsigned long *fullmap;
            int i;

            tr->num_colors = num_colors;
            tr->bits = bits;
            color_index_full_width = transform_xsize;   /* width BEFORE bundling */
            have_color_index = 1;
            tr->xsize = l_subsample_size(transform_xsize, bits); /* bundled width */

            rc = l_decode_image_stream(&d, num_colors, 1, 0, &rawmap);
            if (rc != WEBP_OK) goto cleanup;
            /* expand palette: cumulative add, then black tail */
            fullmap = (unsigned long *)calloc((size_t)final_colors, sizeof(unsigned long));
            if (!fullmap) { free(rawmap); rc = WEBP_ERR_NOMEM; goto cleanup; }
            fullmap[0] = rawmap[0];
            for (i = 1; i < num_colors; i++)
                fullmap[i] = l_add_pixels(rawmap[i], fullmap[i - 1]);
            for (; i < final_colors; i++) fullmap[i] = 0;
            free(rawmap);
            tr->data = fullmap;
            transform_xsize = tr->xsize;   /* entropy image now uses bundled width */
        } else if (type == L_SUBTRACT_GREEN) {
            /* no data */
        } else {
            rc = WEBP_ERR_CORRUPT; goto cleanup;
        }
    }

    /* main entropy-coded image at (transform_xsize x height) */
    rc = l_decode_image_stream(&d, transform_xsize, height, 1, &pixels);
    if (rc != WEBP_OK) goto cleanup;

    /* apply inverse transforms in reverse order */
    for (t = d.num_transforms - 1; t >= 0; t--) {
        LTransform *tr = &d.transforms[t];
        if (tr->type == L_COLOR_INDEX) {
            unsigned long *expanded = (unsigned long *)malloc(
                sizeof(unsigned long) * (size_t)color_index_full_width * (size_t)height);
            if (!expanded) { rc = WEBP_ERR_NOMEM; goto cleanup; }
            l_inverse_color_index(tr, pixels, expanded, color_index_full_width, height);
            free(pixels);
            pixels = expanded;
            transform_xsize = color_index_full_width;
        } else if (tr->type == L_PREDICTOR) {
            tr->xsize = transform_xsize; tr->ysize = height;
            l_inverse_predictor(tr, pixels);
        } else if (tr->type == L_CROSS_COLOR) {
            tr->xsize = transform_xsize; tr->ysize = height;
            l_inverse_color(tr, pixels);
        } else if (tr->type == L_SUBTRACT_GREEN) {
            l_inverse_subtract_green(transform_xsize * height, pixels);
        }
    }
    (void)have_color_index;

    /* pack to RGB (drop alpha); argb stored as 0xAARRGGBB */
    {
        unsigned char *rgb = (unsigned char *)malloc((size_t)width * height * 3);
        int i, n = width * height;
        if (!rgb) { rc = WEBP_ERR_NOMEM; goto cleanup; }
        for (i = 0; i < n; i++) {
            unsigned long argb = pixels[i];
            rgb[i * 3 + 0] = (unsigned char)((argb >> 16) & 0xff);
            rgb[i * 3 + 1] = (unsigned char)((argb >> 8) & 0xff);
            rgb[i * 3 + 2] = (unsigned char)(argb & 0xff);
        }
        *out_rgb = rgb;
        *out_w = (unsigned)width;
        *out_h = (unsigned)height;
        rc = WEBP_OK;
    }

cleanup:
    if (pixels) free(pixels);
    for (t = 0; t < d.num_transforms; t++)
        if (d.transforms[t].data) free(d.transforms[t].data);
    return rc;
}

/* ======================================================================== *
 *  Lossy (VP8) -- implemented in a later step
 * ======================================================================== */

static int vp8_decode(const unsigned char *chunk, unsigned long chunk_size,
                      int max_dim, unsigned char **out_rgb,
                      unsigned *out_w, unsigned *out_h);

/* ======================================================================== *
 *  Public entry points
 * ======================================================================== */

int webp_get_info(const unsigned char *data, unsigned long size,
                  unsigned *out_w, unsigned *out_h)
{
    const unsigned char *chunk;
    unsigned long chunk_size;
    int is_lossless, rc;

    rc = webp_find_image_chunk(data, size, &is_lossless, &chunk, &chunk_size);
    if (rc != WEBP_OK) return rc;

    if (is_lossless) {
        LBits br;
        int w, h;
        if (chunk_size < 5) return WEBP_ERR_TRUNCATED;
        lbits_init(&br, chunk, chunk_size);
        if (lbits_read(&br, 8) != VP8L_MAGIC) return WEBP_ERR_CORRUPT;
        w = (int)lbits_read(&br, 14) + 1;
        h = (int)lbits_read(&br, 14) + 1;
        if (out_w) *out_w = (unsigned)w;
        if (out_h) *out_h = (unsigned)h;
        return WEBP_OK;
    } else {
        /* VP8 lossy key-frame header: 3 bytes frame tag, then 00 9d 01 2a
         * start code, then 14-bit width/height (with 2-bit scale). */
        if (chunk_size < 10) return WEBP_ERR_TRUNCATED;
        if (chunk[3] != 0x9d || chunk[4] != 0x01 || chunk[5] != 0x2a)
            return WEBP_ERR_CORRUPT;
        if (out_w) *out_w = webp_rd_le16(chunk + 6) & 0x3fff;
        if (out_h) *out_h = webp_rd_le16(chunk + 8) & 0x3fff;
        return WEBP_OK;
    }
}

int webp_decode_rgb(const unsigned char *data, unsigned long size,
                    int max_dim,
                    unsigned char **out_rgb, unsigned *out_w, unsigned *out_h)
{
    const unsigned char *chunk;
    unsigned long chunk_size;
    int is_lossless, rc;

    if (!data || !out_rgb || !out_w || !out_h) return WEBP_ERR_BAD_ARG;
    *out_rgb = NULL; *out_w = 0; *out_h = 0;

    rc = webp_find_image_chunk(data, size, &is_lossless, &chunk, &chunk_size);
    if (rc != WEBP_OK) return rc;

    if (is_lossless)
        return l_decode(chunk, chunk_size, max_dim, out_rgb, out_w, out_h);
    return vp8_decode(chunk, chunk_size, max_dim, out_rgb, out_w, out_h);
}

/* ======================================================================== *
 *  Lossy (VP8) key-frame intra decoder.
 *
 *  The constant tables (webp_vp8_tables) and the transform/prediction kernels
 *  (webp_vp8_dsp) are lifted verbatim from libwebp's reference C decoder
 *  (BSD-3); the surrounding bitstream/reconstruction driver is a compact
 *  re-implementation for still images.  The in-loop deblocking filter is
 *  intentionally omitted: favicons are downsampled to a 64x64 (or smaller)
 *  thumbnail, where the filter's sub-pixel edge smoothing is not visible, and
 *  leaving it out removes a large, fiddly chunk of code.  Output is therefore
 *  within a pixel or two of a reference decode -- imperceptible once scaled.
 * ======================================================================== */

#define BPS 32
#define WEBP_TRANSFORM_AC3_C1 20091
#define WEBP_TRANSFORM_AC3_C2 35468
#define WEBP_TRANSFORM_AC3_MUL1(a) ((((a) * WEBP_TRANSFORM_AC3_C1) >> 16) + (a))
#define WEBP_TRANSFORM_AC3_MUL2(a) (((a) * WEBP_TRANSFORM_AC3_C2) >> 16)

static unsigned char v_clip_8b(int v)
{
    return (!(v & ~0xff)) ? (unsigned char)v : (v < 0) ? 0 : 255;
}
#define clip_8b v_clip_8b
#define STORE(x, y, vv) \
  dst[(x) + (y) * BPS] = clip_8b(dst[(x) + (y) * BPS] + ((vv) >> 3))
#define STORE2(y, dc, d, c) \
  do { const int DC = (dc); STORE(0, y, DC + (d)); STORE(1, y, DC + (c)); \
       STORE(2, y, DC - (c)); STORE(3, y, DC - (d)); } while (0)
#define DST(x, y) dst[(x) + (y) * BPS]
#define AVG3(a, b, c) ((unsigned char)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) (((a) + (b) + 1) >> 1)

/* Endian-safe little-endian 32-bit store (the predictor helpers only ever pass
 * a byte replicated 4x, so byte order is immaterial, but keep it explicit). */
static void WebPUint32ToMem(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}
/* Auto-extracted verbatim from libwebp (BSD-3); do not edit. */
static const signed char w_kYModesIntra4[18] = {
    -0, 1, -1, 2, -2, 3,
    4,          6, -3, 5, -4, -5,
    -6, 7, -7, 8, -8, -9};

static const unsigned char w_CoeffsProba0[4][8][3][11] = {
    {{{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128},
      {189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128},
      {106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128}},
     {
         {1, 98, 248, 255, 236, 226, 255, 255, 128, 128, 128},
         {181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128},
         {78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128},
     },
     {
         {1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128},
         {184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128},
         {77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128},
     },
     {{1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128},
      {170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128},
      {37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128}},
     {{1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128},
      {207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128},
      {102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128}},
     {{1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128},
      {177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128},
      {80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128}},
     {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {246, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}},
    {{{198, 35, 237, 223, 193, 187, 162, 160, 145, 155, 62},
      {131, 45, 198, 221, 172, 176, 220, 157, 252, 221, 1},
      {68, 47, 146, 208, 149, 167, 221, 162, 255, 223, 128}},
     {{1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128},
      {184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128},
      {81, 99, 181, 242, 176, 190, 249, 202, 255, 255, 128}},
     {{1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128},
      {99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128},
      {23, 91, 163, 242, 170, 187, 247, 210, 255, 255, 128}},
     {{1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128},
      {109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128},
      {44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128}},
     {{1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128},
      {94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128},
      {22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128}},
     {{1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128},
      {124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128},
      {35, 77, 181, 251, 193, 211, 255, 205, 128, 128, 128}},
     {{1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128},
      {121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128},
      {45, 99, 188, 251, 195, 217, 255, 224, 128, 128, 128}},
     {{1, 1, 251, 255, 213, 255, 128, 128, 128, 128, 128},
      {203, 1, 248, 255, 255, 128, 128, 128, 128, 128, 128},
      {137, 1, 177, 255, 224, 255, 128, 128, 128, 128, 128}}},
    {{{253, 9, 248, 251, 207, 208, 255, 192, 128, 128, 128},
      {175, 13, 224, 243, 193, 185, 249, 198, 255, 255, 128},
      {73, 17, 171, 221, 161, 179, 236, 167, 255, 234, 128}},
     {{1, 95, 247, 253, 212, 183, 255, 255, 128, 128, 128},
      {239, 90, 244, 250, 211, 209, 255, 255, 128, 128, 128},
      {155, 77, 195, 248, 188, 195, 255, 255, 128, 128, 128}},
     {{1, 24, 239, 251, 218, 219, 255, 205, 128, 128, 128},
      {201, 51, 219, 255, 196, 186, 128, 128, 128, 128, 128},
      {69, 46, 190, 239, 201, 218, 255, 228, 128, 128, 128}},
     {{1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128},
      {223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128},
      {141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128}},
     {{1, 16, 248, 255, 255, 128, 128, 128, 128, 128, 128},
      {190, 36, 230, 255, 236, 255, 128, 128, 128, 128, 128},
      {149, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128},
      {213, 62, 250, 255, 255, 128, 128, 128, 128, 128, 128},
      {55, 93, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}},
    {{{202, 24, 213, 235, 186, 191, 220, 160, 240, 175, 255},
      {126, 38, 182, 232, 169, 184, 228, 174, 255, 187, 128},
      {61, 46, 138, 219, 151, 178, 240, 170, 255, 216, 128}},
     {{1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128},
      {166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128},
      {39, 77, 162, 232, 172, 180, 245, 178, 255, 255, 128}},
     {{1, 52, 220, 246, 198, 199, 249, 220, 255, 255, 128},
      {124, 74, 191, 243, 183, 193, 250, 221, 255, 255, 128},
      {24, 71, 130, 219, 154, 170, 243, 182, 255, 255, 128}},
     {{1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128},
      {149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128},
      {28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128}},
     {{1, 81, 230, 252, 204, 203, 255, 192, 128, 128, 128},
      {123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128},
      {20, 95, 153, 243, 164, 173, 255, 203, 128, 128, 128}},
     {{1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128},
      {168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128},
      {47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128}},
     {{1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128},
      {141, 84, 213, 252, 201, 202, 255, 219, 128, 128, 128},
      {42, 80, 160, 240, 162, 185, 255, 205, 128, 128, 128}},
     {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {244, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {238, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}}}};

static const unsigned char w_kBModesProba[10][10][10 - 1] = {
    {{231, 120, 48, 89, 115, 113, 120, 152, 112},
     {152, 179, 64, 126, 170, 118, 46, 70, 95},
     {175, 69, 143, 80, 85, 82, 72, 155, 103},
     {56, 58, 10, 171, 218, 189, 17, 13, 152},
     {114, 26, 17, 163, 44, 195, 21, 10, 173},
     {121, 24, 80, 195, 26, 62, 44, 64, 85},
     {144, 71, 10, 38, 171, 213, 144, 34, 26},
     {170, 46, 55, 19, 136, 160, 33, 206, 71},
     {63, 20, 8, 114, 114, 208, 12, 9, 226},
     {81, 40, 11, 96, 182, 84, 29, 16, 36}},
    {{134, 183, 89, 137, 98, 101, 106, 165, 148},
     {72, 187, 100, 130, 157, 111, 32, 75, 80},
     {66, 102, 167, 99, 74, 62, 40, 234, 128},
     {41, 53, 9, 178, 241, 141, 26, 8, 107},
     {74, 43, 26, 146, 73, 166, 49, 23, 157},
     {65, 38, 105, 160, 51, 52, 31, 115, 128},
     {104, 79, 12, 27, 217, 255, 87, 17, 7},
     {87, 68, 71, 44, 114, 51, 15, 186, 23},
     {47, 41, 14, 110, 182, 183, 21, 17, 194},
     {66, 45, 25, 102, 197, 189, 23, 18, 22}},
    {{88, 88, 147, 150, 42, 46, 45, 196, 205},
     {43, 97, 183, 117, 85, 38, 35, 179, 61},
     {39, 53, 200, 87, 26, 21, 43, 232, 171},
     {56, 34, 51, 104, 114, 102, 29, 93, 77},
     {39, 28, 85, 171, 58, 165, 90, 98, 64},
     {34, 22, 116, 206, 23, 34, 43, 166, 73},
     {107, 54, 32, 26, 51, 1, 81, 43, 31},
     {68, 25, 106, 22, 64, 171, 36, 225, 114},
     {34, 19, 21, 102, 132, 188, 16, 76, 124},
     {62, 18, 78, 95, 85, 57, 50, 48, 51}},
    {{193, 101, 35, 159, 215, 111, 89, 46, 111},
     {60, 148, 31, 172, 219, 228, 21, 18, 111},
     {112, 113, 77, 85, 179, 255, 38, 120, 114},
     {40, 42, 1, 196, 245, 209, 10, 25, 109},
     {88, 43, 29, 140, 166, 213, 37, 43, 154},
     {61, 63, 30, 155, 67, 45, 68, 1, 209},
     {100, 80, 8, 43, 154, 1, 51, 26, 71},
     {142, 78, 78, 16, 255, 128, 34, 197, 171},
     {41, 40, 5, 102, 211, 183, 4, 1, 221},
     {51, 50, 17, 168, 209, 192, 23, 25, 82}},
    {{138, 31, 36, 171, 27, 166, 38, 44, 229},
     {67, 87, 58, 169, 82, 115, 26, 59, 179},
     {63, 59, 90, 180, 59, 166, 93, 73, 154},
     {40, 40, 21, 116, 143, 209, 34, 39, 175},
     {47, 15, 16, 183, 34, 223, 49, 45, 183},
     {46, 17, 33, 183, 6, 98, 15, 32, 183},
     {57, 46, 22, 24, 128, 1, 54, 17, 37},
     {65, 32, 73, 115, 28, 128, 23, 128, 205},
     {40, 3, 9, 115, 51, 192, 18, 6, 223},
     {87, 37, 9, 115, 59, 77, 64, 21, 47}},
    {{104, 55, 44, 218, 9, 54, 53, 130, 226},
     {64, 90, 70, 205, 40, 41, 23, 26, 57},
     {54, 57, 112, 184, 5, 41, 38, 166, 213},
     {30, 34, 26, 133, 152, 116, 10, 32, 134},
     {39, 19, 53, 221, 26, 114, 32, 73, 255},
     {31, 9, 65, 234, 2, 15, 1, 118, 73},
     {75, 32, 12, 51, 192, 255, 160, 43, 51},
     {88, 31, 35, 67, 102, 85, 55, 186, 85},
     {56, 21, 23, 111, 59, 205, 45, 37, 192},
     {55, 38, 70, 124, 73, 102, 1, 34, 98}},
    {{125, 98, 42, 88, 104, 85, 117, 175, 82},
     {95, 84, 53, 89, 128, 100, 113, 101, 45},
     {75, 79, 123, 47, 51, 128, 81, 171, 1},
     {57, 17, 5, 71, 102, 57, 53, 41, 49},
     {38, 33, 13, 121, 57, 73, 26, 1, 85},
     {41, 10, 67, 138, 77, 110, 90, 47, 114},
     {115, 21, 2, 10, 102, 255, 166, 23, 6},
     {101, 29, 16, 10, 85, 128, 101, 196, 26},
     {57, 18, 10, 102, 102, 213, 34, 20, 43},
     {117, 20, 15, 36, 163, 128, 68, 1, 26}},
    {{102, 61, 71, 37, 34, 53, 31, 243, 192},
     {69, 60, 71, 38, 73, 119, 28, 222, 37},
     {68, 45, 128, 34, 1, 47, 11, 245, 171},
     {62, 17, 19, 70, 146, 85, 55, 62, 70},
     {37, 43, 37, 154, 100, 163, 85, 160, 1},
     {63, 9, 92, 136, 28, 64, 32, 201, 85},
     {75, 15, 9, 9, 64, 255, 184, 119, 16},
     {86, 6, 28, 5, 64, 255, 25, 248, 1},
     {56, 8, 17, 132, 137, 255, 55, 116, 128},
     {58, 15, 20, 82, 135, 57, 26, 121, 40}},
    {{164, 50, 31, 137, 154, 133, 25, 35, 218},
     {51, 103, 44, 131, 131, 123, 31, 6, 158},
     {86, 40, 64, 135, 148, 224, 45, 183, 128},
     {22, 26, 17, 131, 240, 154, 14, 1, 209},
     {45, 16, 21, 91, 64, 222, 7, 1, 197},
     {56, 21, 39, 155, 60, 138, 23, 102, 213},
     {83, 12, 13, 54, 192, 255, 68, 47, 28},
     {85, 26, 85, 85, 128, 128, 32, 146, 171},
     {18, 11, 7, 63, 144, 171, 4, 4, 246},
     {35, 27, 10, 146, 174, 171, 12, 26, 128}},
    {{190, 80, 35, 99, 180, 80, 126, 54, 45},
     {85, 126, 47, 87, 176, 51, 41, 20, 32},
     {101, 75, 128, 139, 118, 146, 116, 128, 85},
     {56, 41, 15, 176, 236, 85, 37, 9, 62},
     {71, 30, 17, 119, 118, 255, 17, 18, 138},
     {101, 38, 60, 138, 55, 70, 43, 26, 142},
     {146, 36, 19, 30, 171, 255, 97, 27, 20},
     {138, 45, 61, 62, 219, 1, 81, 188, 64},
     {32, 41, 20, 117, 151, 142, 20, 21, 163},
     {112, 19, 12, 61, 195, 128, 48, 4, 24}}};

static const unsigned char
    w_CoeffsUpdateProba[4][8][3][11] = {
        {{{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255},
          {249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255},
          {234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255},
          {250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255},
          {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
        {{{217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255},
          {234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255}},
         {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255},
          {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
        {{{186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255},
          {234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255},
          {251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255}},
         {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255}},
         {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
        {{{248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255},
          {248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255},
          {246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255},
          {252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255}},
         {{255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255},
          {248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255},
          {253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255},
          {252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255},
          {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
         {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}}};

static const unsigned char w_kBands[16 + 1] = {
    0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7,
    0  // extra entry as sentinel
};

static const unsigned char w_kCat3[] = {173, 148, 140, 0};
static const unsigned char w_kCat4[] = {176, 155, 140, 135, 0};
static const unsigned char w_kCat5[] = {180, 157, 141, 134, 130, 0};
static const unsigned char w_kCat6[] = {254, 254, 243, 230, 196, 177,
                                153, 140, 133, 130, 129, 0};
static const unsigned char* const w_kCat3456[] = {w_kCat3, w_kCat4, w_kCat5, w_kCat6};
static const unsigned char w_kZigzag[16] = {0, 1,  4,  8,  5, 2,  3,  6,
                                    9, 12, 13, 10, 7, 11, 14, 15};

static const unsigned char w_kDcTable[128] = {
    4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,
    17,  18,  19,  20,  20,  21,  21,  22,  22,  23,  23,  24,  25,  25,  26,
    27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,
    55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
    70,  71,  72,  73,  74,  75,  76,  76,  77,  78,  79,  80,  81,  82,  83,
    84,  85,  86,  87,  88,  89,  91,  93,  95,  96,  98,  100, 101, 102, 104,
    106, 108, 110, 112, 114, 116, 118, 122, 124, 126, 128, 130, 132, 134, 136,
    138, 140, 143, 145, 148, 151, 154, 157};

static const unsigned short w_kAcTable[128] = {
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,
    19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  62,  64,  66,  68,
    70,  72,  74,  76,  78,  80,  82,  84,  86,  88,  90,  92,  94,  96,  98,
    100, 102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134,
    137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173, 177, 181,
    185, 189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229, 234, 239, 245,
    249, 254, 259, 264, 269, 274, 279, 284};

static void TransformOne_C(const short*  in,
                           unsigned char*  dst) {
  int C[4 * 4], *tmp;
  int i;
  tmp = C;
  for (i = 0; i < 4; ++i) {       // vertical pass
    const int a = in[0] + in[8];  // [-4096, 4094]
    const int b = in[0] - in[8];  // [-4095, 4095]
    const int c = WEBP_TRANSFORM_AC3_MUL2(in[4]) -
                  WEBP_TRANSFORM_AC3_MUL1(in[12]);  // [-3783, 3783]
    const int d = WEBP_TRANSFORM_AC3_MUL1(in[4]) +
                  WEBP_TRANSFORM_AC3_MUL2(in[12]);  // [-3785, 3781]
    tmp[0] = a + d;                                 // [-7881, 7875]
    tmp[1] = b + c;                                 // [-7878, 7878]
    tmp[2] = b - c;                                 // [-7878, 7878]
    tmp[3] = a - d;                                 // [-7877, 7879]
    tmp += 4;
    in++;
  }
  // Each pass is expanding the dynamic range by ~3.85 (upper bound).
  // The exact value is (2. + (20091 + 35468) / 65536).
  // After the second pass, maximum interval is [-3794, 3794], assuming
  // an input in [-2048, 2047] interval. We then need to add a dst value
  // in the [0, 255] range.
  // In the worst case scenario, the input to clip_8b() can be as large as
  // [-60713, 60968].
  tmp = C;
  for (i = 0; i < 4; ++i) {  // horizontal pass
    const int dc = tmp[0] + 4;
    const int a = dc + tmp[8];
    const int b = dc - tmp[8];
    const int c =
        WEBP_TRANSFORM_AC3_MUL2(tmp[4]) - WEBP_TRANSFORM_AC3_MUL1(tmp[12]);
    const int d =
        WEBP_TRANSFORM_AC3_MUL1(tmp[4]) + WEBP_TRANSFORM_AC3_MUL2(tmp[12]);
    STORE(0, 0, a + d);
    STORE(1, 0, b + c);
    STORE(2, 0, b - c);
    STORE(3, 0, a - d);
    tmp++;
    dst += BPS;
  }
}

// Simplified transform when only in[0], in[1] and in[4] are non-zero

static void TransformWHT_C(const short*  in,
                           short*  out) {
  int tmp[16];
  int i;
  for (i = 0; i < 4; ++i) {
    const int a0 = in[0 + i] + in[12 + i];
    const int a1 = in[4 + i] + in[8 + i];
    const int a2 = in[4 + i] - in[8 + i];
    const int a3 = in[0 + i] - in[12 + i];
    tmp[0 + i] = a0 + a1;
    tmp[8 + i] = a0 - a1;
    tmp[4 + i] = a3 + a2;
    tmp[12 + i] = a3 - a2;
  }
  for (i = 0; i < 4; ++i) {
    const int dc = tmp[0 + i * 4] + 3;  // w/ rounder
    const int a0 = dc + tmp[3 + i * 4];
    const int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
    const int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
    const int a3 = dc - tmp[3 + i * 4];
    out[0] = (a0 + a1) >> 3;
    out[16] = (a3 + a2) >> 3;
    out[32] = (a0 - a1) >> 3;
    out[48] = (a3 - a2) >> 3;
    out += 64;
  }
}


static void TrueMotion(unsigned char* dst, int size) {
  const unsigned char* const top = dst - BPS;   /* fixed top row (row -1) */
  const int tl = top[-1];
  int y;
  for (y = 0; y < size; ++y) {
    const int left = dst[-1];
    int x;
    for (x = 0; x < size; ++x) {
      dst[x] = v_clip_8b(left + top[x] - tl);
    }
    dst += BPS;
  }
}
static void TM4_C(unsigned char* dst) { TrueMotion(dst, 4); }
static void TM8uv_C(unsigned char* dst) { TrueMotion(dst, 8); }
static void TM16_C(unsigned char* dst) { TrueMotion(dst, 16); }

//------------------------------------------------------------------------------
// 16x16

static void VE16_C(unsigned char* dst) {  // vertical
  int j;
  for (j = 0; j < 16; ++j) {
    memcpy(dst + j * BPS, dst - BPS, 16);
  }
}

static void HE16_C(unsigned char* dst) {  // horizontal
  int j;
  for (j = 16; j > 0; --j) {
    memset(dst, dst[-1], 16);
    dst += BPS;
  }
}

static  void Put16(int v, unsigned char* dst) {
  int j;
  for (j = 0; j < 16; ++j) {
    memset(dst + j * BPS, v, 16);
  }
}

static void DC16_C(unsigned char* dst) {  // DC
  int DC = 16;
  int j;
  for (j = 0; j < 16; ++j) {
    DC += dst[-1 + j * BPS] + dst[j - BPS];
  }
  Put16(DC >> 5, dst);
}

static void DC16NoTop_C(unsigned char* dst) {  // DC with top samples not available
  int DC = 8;
  int j;
  for (j = 0; j < 16; ++j) {
    DC += dst[-1 + j * BPS];
  }
  Put16(DC >> 4, dst);
}

static void DC16NoLeft_C(unsigned char* dst) {  // DC with left samples not available
  int DC = 8;
  int i;
  for (i = 0; i < 16; ++i) {
    DC += dst[i - BPS];
  }
  Put16(DC >> 4, dst);
}

static void DC16NoTopLeft_C(unsigned char* dst) {  // DC with no top and left samples
  Put16(0x80, dst);
}


//------------------------------------------------------------------------------
// 4x4

#define AVG3(a, b, c) ((unsigned char)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) (((a) + (b) + 1) >> 1)

static void VE4_C(unsigned char* dst) {  // vertical
  const unsigned char* top = dst - BPS;
  const unsigned char vals[4] = {
      AVG3(top[-1], top[0], top[1]),
      AVG3(top[0], top[1], top[2]),
      AVG3(top[1], top[2], top[3]),
      AVG3(top[2], top[3], top[4]),
  };
  int i;
  for (i = 0; i < 4; ++i) {
    memcpy(dst + i * BPS, vals, sizeof(vals));
  }
}

static void HE4_C(unsigned char* dst) {  // horizontal
  const int A = dst[-1 - BPS];
  const int B = dst[-1];
  const int C = dst[-1 + BPS];
  const int D = dst[-1 + 2 * BPS];
  const int E = dst[-1 + 3 * BPS];
  WebPUint32ToMem(dst + 0 * BPS, 0x01010101U * AVG3(A, B, C));
  WebPUint32ToMem(dst + 1 * BPS, 0x01010101U * AVG3(B, C, D));
  WebPUint32ToMem(dst + 2 * BPS, 0x01010101U * AVG3(C, D, E));
  WebPUint32ToMem(dst + 3 * BPS, 0x01010101U * AVG3(D, E, E));
}

static void DC4_C(unsigned char* dst) {  // DC
  unsigned int dc = 4;
  int i;
  for (i = 0; i < 4; ++i) dc += dst[i - BPS] + dst[-1 + i * BPS];
  dc >>= 3;
  for (i = 0; i < 4; ++i) memset(dst + i * BPS, dc, 4);
}

static void RD4_C(unsigned char* dst) {  // Down-right
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int L = dst[-1 + 3 * BPS];
  const int X = dst[-1 - BPS];
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  DST(0, 3) = AVG3(J, K, L);
  DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
  DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
  DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(A, X, I);
  DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(B, A, X);
  DST(3, 1) = DST(2, 0) = AVG3(C, B, A);
  DST(3, 0) = AVG3(D, C, B);
}

static void LD4_C(unsigned char* dst) {  // Down-Left
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  const int E = dst[4 - BPS];
  const int F = dst[5 - BPS];
  const int G = dst[6 - BPS];
  const int H = dst[7 - BPS];
  DST(0, 0) = AVG3(A, B, C);
  DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
  DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
  DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
  DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
  DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
  DST(3, 3) = AVG3(G, H, H);
}

static void VR4_C(unsigned char* dst) {  // Vertical-Right
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int X = dst[-1 - BPS];
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  DST(0, 0) = DST(1, 2) = AVG2(X, A);
  DST(1, 0) = DST(2, 2) = AVG2(A, B);
  DST(2, 0) = DST(3, 2) = AVG2(B, C);
  DST(3, 0) = AVG2(C, D);

  DST(0, 3) = AVG3(K, J, I);
  DST(0, 2) = AVG3(J, I, X);
  DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
  DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
  DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
  DST(3, 1) = AVG3(B, C, D);
}

static void VL4_C(unsigned char* dst) {  // Vertical-Left
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  const int E = dst[4 - BPS];
  const int F = dst[5 - BPS];
  const int G = dst[6 - BPS];
  const int H = dst[7 - BPS];
  DST(0, 0) = AVG2(A, B);
  DST(1, 0) = DST(0, 2) = AVG2(B, C);
  DST(2, 0) = DST(1, 2) = AVG2(C, D);
  DST(3, 0) = DST(2, 2) = AVG2(D, E);

  DST(0, 1) = AVG3(A, B, C);
  DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
  DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
  DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
  DST(3, 2) = AVG3(E, F, G);
  DST(3, 3) = AVG3(F, G, H);
}

static void HU4_C(unsigned char* dst) {  // Horizontal-Up
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int L = dst[-1 + 3 * BPS];
  DST(0, 0) = AVG2(I, J);
  DST(2, 0) = DST(0, 1) = AVG2(J, K);
  DST(2, 1) = DST(0, 2) = AVG2(K, L);
  DST(1, 0) = AVG3(I, J, K);
  DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
  DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
  DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = L;
}

static void HD4_C(unsigned char* dst) {  // Horizontal-Down
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int L = dst[-1 + 3 * BPS];
  const int X = dst[-1 - BPS];
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];

  DST(0, 0) = DST(2, 1) = AVG2(I, X);
  DST(0, 1) = DST(2, 2) = AVG2(J, I);
  DST(0, 2) = DST(2, 3) = AVG2(K, J);
  DST(0, 3) = AVG2(L, K);

  DST(3, 0) = AVG3(A, B, C);
  DST(2, 0) = AVG3(X, A, B);
  DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
  DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
  DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
  DST(1, 3) = AVG3(L, K, J);
}

#undef DST
#undef AVG3
#undef AVG2


//------------------------------------------------------------------------------
// Chroma

static void VE8uv_C(unsigned char* dst) {  // vertical
  int j;
  for (j = 0; j < 8; ++j) {
    memcpy(dst + j * BPS, dst - BPS, 8);
  }
}

static void HE8uv_C(unsigned char* dst) {  // horizontal
  int j;
  for (j = 0; j < 8; ++j) {
    memset(dst, dst[-1], 8);
    dst += BPS;
  }
}

// helper for chroma-DC predictions
static  void Put8x8uv(unsigned char value, unsigned char* dst) {
  int j;
  for (j = 0; j < 8; ++j) {
    memset(dst + j * BPS, value, 8);
  }
}

static void DC8uv_C(unsigned char* dst) {  // DC
  int dc0 = 8;
  int i;
  for (i = 0; i < 8; ++i) {
    dc0 += dst[i - BPS] + dst[-1 + i * BPS];
  }
  Put8x8uv(dc0 >> 4, dst);
}

static void DC8uvNoLeft_C(unsigned char* dst) {  // DC with no left samples
  int dc0 = 4;
  int i;
  for (i = 0; i < 8; ++i) {
    dc0 += dst[i - BPS];
  }
  Put8x8uv(dc0 >> 3, dst);
}

static void DC8uvNoTop_C(unsigned char* dst) {  // DC with no top samples
  int dc0 = 4;
  int i;
  for (i = 0; i < 8; ++i) {
    dc0 += dst[-1 + i * BPS];
  }
  Put8x8uv(dc0 >> 3, dst);
}

static void DC8uvNoTopLeft_C(unsigned char* dst) {  // DC with nothing
  Put8x8uv(0x80, dst);
}


/* ---- predictor dispatch tables (assigned from the *_C kernels above) ---- */
typedef void (*VPredFunc)(unsigned char *dst);
static VPredFunc VP8PredLuma16[7];
static VPredFunc VP8PredLuma4[10];
static VPredFunc VP8PredChroma8[7];
static int vp8_dsp_ready = 0;
static void vp8_dsp_init(void)
{
    if (vp8_dsp_ready) return;
    VP8PredLuma16[0] = DC16_C;   VP8PredLuma16[1] = TM16_C;
    VP8PredLuma16[2] = VE16_C;   VP8PredLuma16[3] = HE16_C;
    VP8PredLuma16[4] = DC16NoTop_C; VP8PredLuma16[5] = DC16NoLeft_C;
    VP8PredLuma16[6] = DC16NoTopLeft_C;
    VP8PredLuma4[0] = DC4_C; VP8PredLuma4[1] = TM4_C; VP8PredLuma4[2] = VE4_C;
    VP8PredLuma4[3] = HE4_C; VP8PredLuma4[4] = RD4_C; VP8PredLuma4[5] = VR4_C;
    VP8PredLuma4[6] = LD4_C; VP8PredLuma4[7] = VL4_C; VP8PredLuma4[8] = HD4_C;
    VP8PredLuma4[9] = HU4_C;
    VP8PredChroma8[0] = DC8uv_C; VP8PredChroma8[1] = TM8uv_C;
    VP8PredChroma8[2] = VE8uv_C; VP8PredChroma8[3] = HE8uv_C;
    VP8PredChroma8[4] = DC8uvNoTop_C; VP8PredChroma8[5] = DC8uvNoLeft_C;
    VP8PredChroma8[6] = DC8uvNoTopLeft_C;
    vp8_dsp_ready = 1;
}

/* ---- boolean (arithmetic) decoder, RFC 6386 section 7 ---- */
typedef struct {
    const unsigned char *buf, *end;
    unsigned long value;
    int range;
    int bits;
    int eof;
} VBool;

static int vbool_byte(VBool *d)
{
    if (d->buf < d->end) return *d->buf++;
    d->eof = 1;
    return 0;
}

static void vbool_init(VBool *d, const unsigned char *b, unsigned long n)
{
    d->buf = b; d->end = b + n; d->eof = 0;
    d->value = ((unsigned long)vbool_byte(d) << 8) | (unsigned long)vbool_byte(d);
    d->range = 255;
    d->bits = 0;
}

static int vbool_bit(VBool *d, int prob)
{
    unsigned long split = 1 + (((unsigned long)(d->range - 1) * (unsigned long)prob) >> 8);
    unsigned long bigsplit = split << 8;
    int ret;
    if (d->value >= bigsplit) { ret = 1; d->range -= (int)split; d->value -= bigsplit; }
    else { ret = 0; d->range = (int)split; }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bits == 8) { d->bits = 0; d->value |= (unsigned long)vbool_byte(d); }
    }
    return ret;
}

static int vbool_lit(VBool *d, int n)     /* n literal bits, MSB first, prob 128 */
{
    int v = 0;
    while (n-- > 0) v = (v << 1) | vbool_bit(d, 128);
    return v;
}

static int vbool_slit(VBool *d, int n)    /* signed literal: magnitude then sign */
{
    int v = vbool_lit(d, n);
    return vbool_bit(d, 128) ? -v : v;
}

/* ---- decoder state ---- */
typedef struct { int probas[3][11]; } VBandProbas;
typedef struct {
    VBandProbas bands[4][8];
    VBandProbas *bandptr[4][17];
} VProba;

typedef struct { int y1[2], y2[2], uv[2]; } VQuant; /* [dc, ac] */

/* Per-column non-zero context.  'nz' packs the luma (bits 0-3) and both
 * chroma planes (bits 4+) sub-block flags, so it must be a full 32-bit word --
 * an 8-bit field would drop the chroma context and corrupt colour decoding
 * across macroblocks. */
typedef struct { unsigned int nz; unsigned char nz_dc; } VMBnz;

typedef struct {
    unsigned char y[16], u[8], v[8];
} VTop;

typedef struct {
    int is_i4x4;
    int segment;
    int skip;
    unsigned char imodes[16];
    int uvmode;
    short coeffs[384];
    unsigned int non_zero_y, non_zero_uv;
} VMBData;

typedef struct {
    VBool br;                 /* partition 0 (header + modes) */
    VBool parts[8];           /* token partitions */
    int num_parts;

    int mb_w, mb_h;
    int width, height;

    /* segmentation */
    int use_segment, update_map, absolute_delta;
    int seg_quant[4];
    int seg_prob[3];

    /* quant */
    VQuant dqm[4];

    /* token probabilities */
    VProba proba;
    int use_skip_proba, skip_p;

    /* per-column contexts */
    unsigned char *intra_t;   /* 4*mb_w top 4x4 modes */
    unsigned char intra_l[4];
    VMBnz *mb_info;           /* mb_w+1, [0]=left sentinel */
    VTop *top;                /* mb_w top samples */

    VMBData mb;               /* current macroblock */
} VP8Dec;

static const unsigned short kScan[16] = {
    0 + 0 * BPS,  4 + 0 * BPS,  8 + 0 * BPS,  12 + 0 * BPS,
    0 + 4 * BPS,  4 + 4 * BPS,  8 + 4 * BPS,  12 + 4 * BPS,
    0 + 8 * BPS,  4 + 8 * BPS,  8 + 8 * BPS,  12 + 8 * BPS,
    0 + 12 * BPS, 4 + 12 * BPS, 8 + 12 * BPS, 12 + 12 * BPS
};

static int v_clip_q(int q, int hi) { return q < 0 ? 0 : (q > hi ? hi : q); }

static void vp8_parse_segments(VBool *br, VP8Dec *d)
{
    d->use_segment = vbool_bit(br, 128);
    if (d->use_segment) {
        int s;
        d->update_map = vbool_bit(br, 128);
        if (vbool_bit(br, 128)) {          /* update data */
            d->absolute_delta = vbool_bit(br, 128);
            for (s = 0; s < 4; s++)
                d->seg_quant[s] = vbool_bit(br, 128) ? vbool_slit(br, 7) : 0;
            for (s = 0; s < 4; s++)
                (void)(vbool_bit(br, 128) ? vbool_slit(br, 6) : 0); /* filter str (unused) */
        }
        if (d->update_map)
            for (s = 0; s < 3; s++)
                d->seg_prob[s] = vbool_bit(br, 128) ? vbool_lit(br, 8) : 255;
    } else {
        d->update_map = 0;
    }
}

static void vp8_parse_filter_hdr(VBool *br)
{
    int use_lf_delta;
    (void)vbool_bit(br, 128);              /* simple */
    (void)vbool_lit(br, 6);                /* level  */
    (void)vbool_lit(br, 3);                /* sharpness */
    use_lf_delta = vbool_bit(br, 128);
    if (use_lf_delta && vbool_bit(br, 128)) {
        int i;
        for (i = 0; i < 4; i++) if (vbool_bit(br, 128)) (void)vbool_slit(br, 6);
        for (i = 0; i < 4; i++) if (vbool_bit(br, 128)) (void)vbool_slit(br, 6);
    }
}

static int vp8_parse_partitions(VP8Dec *d, const unsigned char *buf, unsigned long size)
{
    const unsigned char *sz = buf, *part_start;
    unsigned long size_left = size, last;
    unsigned long p;

    d->num_parts = 1 << vbool_lit(&d->br, 2);
    last = (unsigned long)d->num_parts - 1;
    if (size < 3 * last) return WEBP_ERR_TRUNCATED;
    part_start = buf + last * 3;
    size_left -= last * 3;
    for (p = 0; p < last; p++) {
        unsigned long psize = (unsigned long)sz[0] | ((unsigned long)sz[1] << 8) |
                              ((unsigned long)sz[2] << 16);
        if (psize > size_left) psize = size_left;
        vbool_init(&d->parts[p], part_start, psize);
        part_start += psize;
        size_left -= psize;
        sz += 3;
    }
    vbool_init(&d->parts[last], part_start, size_left);
    return WEBP_OK;
}

static void vp8_parse_quant(VP8Dec *d)
{
    VBool *br = &d->br;
    int base = vbool_lit(br, 7);
    int dqy1_dc = vbool_bit(br, 128) ? vbool_slit(br, 4) : 0;
    int dqy2_dc = vbool_bit(br, 128) ? vbool_slit(br, 4) : 0;
    int dqy2_ac = vbool_bit(br, 128) ? vbool_slit(br, 4) : 0;
    int dquv_dc = vbool_bit(br, 128) ? vbool_slit(br, 4) : 0;
    int dquv_ac = vbool_bit(br, 128) ? vbool_slit(br, 4) : 0;
    int i;
    for (i = 0; i < 4; i++) {
        int q;
        VQuant *m = &d->dqm[i];
        if (d->use_segment) {
            q = d->seg_quant[i];
            if (!d->absolute_delta) q += base;
        } else {
            if (i > 0) { d->dqm[i] = d->dqm[0]; continue; }
            q = base;
        }
        m->y1[0] = w_kDcTable[v_clip_q(q + dqy1_dc, 127)];
        m->y1[1] = w_kAcTable[v_clip_q(q, 127)];
        m->y2[0] = w_kDcTable[v_clip_q(q + dqy2_dc, 127)] * 2;
        m->y2[1] = (w_kAcTable[v_clip_q(q + dqy2_ac, 127)] * 101581) >> 16;
        if (m->y2[1] < 8) m->y2[1] = 8;
        m->uv[0] = w_kDcTable[v_clip_q(q + dquv_dc, 117)];
        m->uv[1] = w_kAcTable[v_clip_q(q + dquv_ac, 127)];
    }
}

static void vp8_parse_proba(VP8Dec *d)
{
    VBool *br = &d->br;
    VProba *proba = &d->proba;
    int t, b, c, p;
    for (t = 0; t < 4; t++) {
        for (b = 0; b < 8; b++)
            for (c = 0; c < 3; c++)
                for (p = 0; p < 11; p++) {
                    int v = vbool_bit(br, w_CoeffsUpdateProba[t][b][c][p])
                            ? vbool_lit(br, 8) : w_CoeffsProba0[t][b][c][p];
                    proba->bands[t][b].probas[c][p] = v;
                }
        for (b = 0; b < 17; b++)
            proba->bandptr[t][b] = &proba->bands[t][w_kBands[b]];
    }
    d->use_skip_proba = vbool_bit(br, 128);
    if (d->use_skip_proba) d->skip_p = vbool_lit(br, 8);
}

/* ---- coefficient (token) decoding ---- */
static int vp8_get_large(VBool *br, const int *p)
{
    int v;
    if (!vbool_bit(br, p[3])) {
        if (!vbool_bit(br, p[4])) v = 2;
        else v = 3 + vbool_bit(br, p[5]);
    } else {
        if (!vbool_bit(br, p[6])) {
            if (!vbool_bit(br, p[7])) v = 5 + vbool_bit(br, 159);
            else { v = 7 + 2 * vbool_bit(br, 165); v += vbool_bit(br, 145); }
        } else {
            const unsigned char *tab;
            int bit1 = vbool_bit(br, p[8]);
            int bit0 = vbool_bit(br, p[9 + bit1]);
            int cat = 2 * bit1 + bit0;
            v = 0;
            for (tab = w_kCat3456[cat]; *tab; ++tab)
                v += v + vbool_bit(br, *tab);
            v += 3 + (8 << cat);
        }
    }
    return v;
}

/* Reads one 4x4 block of coefficients into out[] (zigzag-expanded, dequantized).
 * Returns position of last non-zero coeff + 1. */
static int vp8_get_coeffs(VBool *br, VBandProbas *const prob[], int ctx,
                          const int *dq, int n, short *out)
{
    const int *p = prob[n]->probas[ctx];
    for (; n < 16; ++n) {
        if (!vbool_bit(br, p[0])) return n;
        while (!vbool_bit(br, p[1])) {
            p = prob[++n]->probas[0];
            if (n == 16) return 16;
        }
        {
            int (*p_ctx)[11] = prob[n + 1]->probas;
            int v;
            if (!vbool_bit(br, p[2])) { v = 1; p = p_ctx[1]; }
            else { v = vp8_get_large(br, p); p = p_ctx[2]; }
            {
                int sv = vbool_bit(br, 128) ? -v : v;   /* VP8GetSigned */
                out[w_kZigzag[n]] = (short)(sv * dq[n > 0]);
            }
        }
    }
    return 16;
}

static unsigned int vp8_nz_bits(unsigned int nz_coeffs, int nz, int dc_nz)
{
    nz_coeffs <<= 2;
    nz_coeffs |= (nz > 3) ? 3 : (nz > 1) ? 2 : (unsigned int)dc_nz;
    return nz_coeffs;
}

static void vp8_parse_residuals(VP8Dec *d, int mb_x, VBool *token_br)
{
    VBandProbas *(*bands)[17] = d->proba.bandptr;
    VBandProbas *const *ac_proba;
    VMBData *block = &d->mb;
    VQuant *q = &d->dqm[block->segment];
    short *dst = block->coeffs;
    VMBnz *mb = &d->mb_info[mb_x + 1];     /* per-column top context           */
    VMBnz *left_mb = &d->mb_info[0];       /* single running left sentinel      */
    unsigned char tnz, lnz;
    unsigned int non_zero_y = 0, non_zero_uv = 0;
    int x, y, ch, first;

    memset(dst, 0, 384 * sizeof(short));
    if (!block->is_i4x4) {
        short dc[16];
        int ctx = mb->nz_dc + left_mb->nz_dc;
        int nz;
        memset(dc, 0, sizeof(dc));
        nz = vp8_get_coeffs(token_br, bands[1], ctx, q->y2, 0, dc);
        mb->nz_dc = left_mb->nz_dc = (unsigned char)(nz > 0);
        if (nz > 1) {
            TransformWHT_C(dc, dst);
        } else {
            int i, dc0 = (dc[0] + 3) >> 3;
            for (i = 0; i < 16 * 16; i += 16) dst[i] = (short)dc0;
        }
        first = 1;
        ac_proba = bands[0];
    } else {
        first = 0;
        ac_proba = bands[3];
    }

    tnz = (unsigned char)(mb->nz & 0x0f);
    lnz = (unsigned char)(left_mb->nz & 0x0f);
    for (y = 0; y < 4; ++y) {
        int l = lnz & 1;
        unsigned int nz_coeffs = 0;
        for (x = 0; x < 4; ++x) {
            int ctx = l + (tnz & 1);
            int nz = vp8_get_coeffs(token_br, ac_proba, ctx, q->y1, first, dst);
            l = (nz > first);
            tnz = (unsigned char)((tnz >> 1) | (l << 7));
            nz_coeffs = vp8_nz_bits(nz_coeffs, nz, dst[0] != 0);
            dst += 16;
        }
        tnz >>= 4;
        lnz = (unsigned char)((lnz >> 1) | (l << 7));
        non_zero_y = (non_zero_y << 8) | nz_coeffs;
    }
    {
        unsigned int out_t_nz = tnz, out_l_nz = lnz >> 4;
        for (ch = 0; ch < 4; ch += 2) {
            unsigned int nz_coeffs = 0;
            tnz = (unsigned char)(mb->nz >> (4 + ch));
            lnz = (unsigned char)(left_mb->nz >> (4 + ch));
            for (y = 0; y < 2; ++y) {
                int l = lnz & 1;
                for (x = 0; x < 2; ++x) {
                    int ctx = l + (tnz & 1);
                    int nz = vp8_get_coeffs(token_br, bands[2], ctx, q->uv, 0, dst);
                    l = (nz > 0);
                    tnz = (unsigned char)((tnz >> 1) | (l << 3));
                    nz_coeffs = vp8_nz_bits(nz_coeffs, nz, dst[0] != 0);
                    dst += 16;
                }
                tnz >>= 2;
                lnz = (unsigned char)((lnz >> 1) | (l << 5));
            }
            non_zero_uv |= nz_coeffs << (4 * ch);
            out_t_nz |= (unsigned int)((tnz << 4) << ch);
            out_l_nz |= (unsigned int)((lnz & 0xf0) << ch);
        }
        mb->nz = out_t_nz;
        left_mb->nz = out_l_nz;
    }
    block->non_zero_y = non_zero_y;
    block->non_zero_uv = non_zero_uv;
}

/* ---- per-macroblock intra mode parsing ---- */
static void vp8_parse_intra_modes(VP8Dec *d, int mb_x)
{
    VBool *br = &d->br;
    unsigned char *top = d->intra_t + 4 * mb_x;
    unsigned char *left = d->intra_l;
    VMBData *block = &d->mb;

    if (d->update_map) {
        block->segment = !vbool_bit(br, d->seg_prob[0])
            ? vbool_bit(br, d->seg_prob[1])
            : vbool_bit(br, d->seg_prob[2]) + 2;
    } else {
        block->segment = 0;
    }
    block->skip = d->use_skip_proba ? vbool_bit(br, d->skip_p) : 0;
    block->is_i4x4 = !vbool_bit(br, 145);
    if (!block->is_i4x4) {
        int ymode = vbool_bit(br, 156) ? (vbool_bit(br, 128) ? 1 : 3)
                                       : (vbool_bit(br, 163) ? 2 : 0);
        int i;
        block->imodes[0] = (unsigned char)ymode;
        for (i = 0; i < 4; i++) { top[i] = (unsigned char)ymode; left[i] = (unsigned char)ymode; }
    } else {
        unsigned char *modes = block->imodes;
        int y;
        for (y = 0; y < 4; ++y) {
            int ymode = left[y];
            int x;
            for (x = 0; x < 4; ++x) {
                const unsigned char *prob = w_kBModesProba[top[x]][ymode];
                int i = w_kYModesIntra4[vbool_bit(br, prob[0])];
                while (i > 0) i = w_kYModesIntra4[2 * i + vbool_bit(br, prob[i])];
                ymode = -i;
                top[x] = (unsigned char)ymode;
            }
            for (x = 0; x < 4; x++) modes[x] = top[x];
            modes += 4;
            left[y] = (unsigned char)ymode;
        }
    }
    block->uvmode = !vbool_bit(br, 142) ? 0
                    : !vbool_bit(br, 114) ? 2
                    : vbool_bit(br, 183) ? 1 : 3;
}

static int vp8_check_mode(int mb_x, int mb_y, int mode)
{
    if (mode == 0) {                       /* B_DC_PRED / DC_PRED */
        if (mb_x == 0) return (mb_y == 0) ? 6 : 5;   /* NOTOPLEFT : NOLEFT */
        else return (mb_y == 0) ? 4 : 0;             /* NOTOP : DC */
    }
    return mode;
}

#define Y_OFF (BPS * 1 + 8)
#define U_OFF (Y_OFF + BPS * 16 + BPS)
#define V_OFF (U_OFF + 16)
#define YUV_B_SIZE (V_OFF + 8 * BPS + 16)

/* Full 4x4 inverse transform of one block (skips the DC/AC fast paths; a
 * zero block adds nothing, so this is exact). */
static void vp8_do_transform(unsigned int bits, const short *src, unsigned char *dst)
{
    if (bits >> 30) TransformOne_C(src, dst);
}

static void vp8_recon_row(VP8Dec *d, int mb_y, unsigned char *mem,
                          unsigned char *Y, unsigned char *U, unsigned char *V,
                          int ystride, int uvstride, VBool *token_br)
{
    unsigned char *y_dst = mem + Y_OFF;
    unsigned char *u_dst = mem + U_OFF;
    unsigned char *v_dst = mem + V_OFF;
    int j, mb_x, n;

    for (j = 0; j < 16; ++j) y_dst[j * BPS - 1] = 129;
    for (j = 0; j < 8; ++j) { u_dst[j * BPS - 1] = 129; v_dst[j * BPS - 1] = 129; }
    if (mb_y > 0) {
        y_dst[-1 - BPS] = u_dst[-1 - BPS] = v_dst[-1 - BPS] = 129;
    } else {
        memset(y_dst - BPS - 1, 127, 16 + 4 + 1);
        memset(u_dst - BPS - 1, 127, 8 + 1);
        memset(v_dst - BPS - 1, 127, 8 + 1);
    }

    for (mb_x = 0; mb_x < d->mb_w; ++mb_x) {
        VMBData *block = &d->mb;
        VTop *top = &d->top[mb_x];

        /* parse this macroblock */
        vp8_parse_intra_modes(d, mb_x);
        if (!(d->use_skip_proba && block->skip)) {
            vp8_parse_residuals(d, mb_x, token_br);
        } else {
            VMBnz *mb = &d->mb_info[mb_x + 1];
            VMBnz *left = &d->mb_info[0];
            left->nz = mb->nz = 0;
            if (!block->is_i4x4) { left->nz_dc = mb->nz_dc = 0; }
            block->non_zero_y = 0;
            block->non_zero_uv = 0;
            memset(block->coeffs, 0, sizeof(block->coeffs));
        }

        /* rotate in left samples from the previously decoded block */
        if (mb_x > 0) {
            for (j = -1; j < 16; ++j) memcpy(&y_dst[j * BPS - 4], &y_dst[j * BPS + 12], 4);
            for (j = -1; j < 8; ++j) {
                memcpy(&u_dst[j * BPS - 4], &u_dst[j * BPS + 4], 4);
                memcpy(&v_dst[j * BPS - 4], &v_dst[j * BPS + 4], 4);
            }
        }
        if (mb_y > 0) {
            memcpy(y_dst - BPS, top->y, 16);
            memcpy(u_dst - BPS, top->u, 8);
            memcpy(v_dst - BPS, top->v, 8);
        }

        {
            const short *coeffs = block->coeffs;
            unsigned int bits = block->non_zero_y;
            if (block->is_i4x4) {
                unsigned char *tr = y_dst - BPS + 16;
                if (mb_y > 0) {
                    if (mb_x >= d->mb_w - 1) memset(tr, top->y[15], 4);
                    else memcpy(tr, d->top[mb_x + 1].y, 4);
                }
                for (j = 0; j < 4; ++j) { tr[4 * BPS + j] = tr[8 * BPS + j] = tr[12 * BPS + j] = tr[j]; }
                for (n = 0; n < 16; ++n, bits <<= 2) {
                    unsigned char *dd = y_dst + kScan[n];
                    VP8PredLuma4[block->imodes[n]](dd);
                    vp8_do_transform(bits, coeffs + n * 16, dd);
                }
            } else {
                int pf = vp8_check_mode(mb_x, mb_y, block->imodes[0]);
                VP8PredLuma16[pf](y_dst);
                if (bits != 0)
                    for (n = 0; n < 16; ++n, bits <<= 2)
                        vp8_do_transform(bits, coeffs + n * 16, y_dst + kScan[n]);
            }
            {
                unsigned int bits_uv = block->non_zero_uv;
                int pf = vp8_check_mode(mb_x, mb_y, block->uvmode);
                VP8PredChroma8[pf](u_dst);
                VP8PredChroma8[pf](v_dst);
                /* U/V each hold a 2x2 grid of 4x4 blocks; transform all four
                 * when the plane has any non-zero coeff (a fully-zero block's
                 * inverse transform adds nothing, so we needn't gate each one
                 * individually -- and mustn't, since the packed bit order does
                 * not line up one-to-one with block order). */
                if (bits_uv & 0x00ff) {
                    TransformOne_C(coeffs + 16 * 16, u_dst);
                    TransformOne_C(coeffs + 17 * 16, u_dst + 4);
                    TransformOne_C(coeffs + 18 * 16, u_dst + 4 * BPS);
                    TransformOne_C(coeffs + 19 * 16, u_dst + 4 * BPS + 4);
                }
                if (bits_uv & 0xff00) {
                    TransformOne_C(coeffs + 20 * 16, v_dst);
                    TransformOne_C(coeffs + 21 * 16, v_dst + 4);
                    TransformOne_C(coeffs + 22 * 16, v_dst + 4 * BPS);
                    TransformOne_C(coeffs + 23 * 16, v_dst + 4 * BPS + 4);
                }
            }
        }

        /* stash bottom samples as next row's top */
        if (mb_y < d->mb_h - 1) {
            memcpy(top->y, y_dst + 15 * BPS, 16);
            memcpy(top->u, u_dst + 7 * BPS, 8);
            memcpy(top->v, v_dst + 7 * BPS, 8);
        }

        /* copy reconstructed samples to the output planes */
        for (j = 0; j < 16; ++j)
            memcpy(Y + (mb_y * 16 + j) * ystride + mb_x * 16, y_dst + j * BPS, 16);
        for (j = 0; j < 8; ++j) {
            memcpy(U + (mb_y * 8 + j) * uvstride + mb_x * 8, u_dst + j * BPS, 8);
            memcpy(V + (mb_y * 8 + j) * uvstride + mb_x * 8, v_dst + j * BPS, 8);
        }
    }
}

/* YUV (BT.601, libwebp fixed-point) -> RGB, one pixel. */
static int vp8_clip8(int v)
{
    int m = (256 << 6) - 1;
    return ((v & ~m) == 0) ? (v >> 6) : (v < 0 ? 0 : 255);
}
static void vp8_yuv_to_rgb(int y, int u, int v, unsigned char *rgb)
{
    int yy = ((y * 19077) >> 8);
    rgb[0] = (unsigned char)vp8_clip8(yy + ((v * 26149) >> 8) - 14234);
    rgb[1] = (unsigned char)vp8_clip8(yy - ((u * 6419) >> 8) - ((v * 13320) >> 8) + 8708);
    rgb[2] = (unsigned char)vp8_clip8(yy + ((u * 33050) >> 8) - 17685);
}

static int vp8_decode(const unsigned char *chunk, unsigned long chunk_size,
                      int max_dim, unsigned char **out_rgb,
                      unsigned *out_w, unsigned *out_h)
{
    VP8Dec *d;
    const unsigned char *buf = chunk;
    unsigned long size = chunk_size;
    unsigned long part_len;
    unsigned char *mem = NULL, *Y = NULL, *U = NULL, *V = NULL, *rgb = NULL;
    int ystride, uvstride, mb_y, x, yy, rc = WEBP_ERR_CORRUPT;
    int width, height;

    vp8_dsp_init();
    if (size < 10) return WEBP_ERR_TRUNCATED;

    d = (VP8Dec *)calloc(1, sizeof(VP8Dec));
    if (!d) return WEBP_ERR_NOMEM;

    /* frame tag (3 bytes) */
    {
        unsigned long tag = (unsigned long)buf[0] | ((unsigned long)buf[1] << 8) |
                            ((unsigned long)buf[2] << 16);
        int key_frame = !(tag & 1);
        int show = (tag >> 4) & 1;
        part_len = tag >> 5;
        if (!key_frame || !show) { free(d); return WEBP_ERR_UNSUPPORTED; }
    }
    if (buf[3] != 0x9d || buf[4] != 0x01 || buf[5] != 0x2a) { free(d); return WEBP_ERR_CORRUPT; }
    width = ((buf[7] << 8) | buf[6]) & 0x3fff;
    height = ((buf[9] << 8) | buf[8]) & 0x3fff;
    if (width < 1 || height < 1) { free(d); return WEBP_ERR_CORRUPT; }
    if (max_dim > 0 && (width > max_dim || height > max_dim)) { free(d); return WEBP_ERR_TOO_LARGE; }
    buf += 10; size -= 10;

    d->width = width; d->height = height;
    d->mb_w = (width + 15) >> 4;
    d->mb_h = (height + 15) >> 4;

    if (part_len > size) { free(d); return WEBP_ERR_TRUNCATED; }
    vbool_init(&d->br, buf, part_len);
    buf += part_len; size -= part_len;

    (void)vbool_bit(&d->br, 128);   /* colorspace */
    (void)vbool_bit(&d->br, 128);   /* clamp type */
    vp8_parse_segments(&d->br, d);
    vp8_parse_filter_hdr(&d->br);
    rc = vp8_parse_partitions(d, buf, size);
    if (rc != WEBP_OK) { free(d); return rc; }
    vp8_parse_quant(d);
    (void)vbool_bit(&d->br, 128);   /* update_proba */
    vp8_parse_proba(d);

    ystride = d->mb_w * 16;
    uvstride = d->mb_w * 8;
    mem = (unsigned char *)calloc(1, YUV_B_SIZE);
    Y = (unsigned char *)calloc((size_t)ystride * d->mb_h * 16, 1);
    U = (unsigned char *)calloc((size_t)uvstride * d->mb_h * 8, 1);
    V = (unsigned char *)calloc((size_t)uvstride * d->mb_h * 8, 1);
    d->intra_t = (unsigned char *)calloc((size_t)4 * d->mb_w, 1);
    d->mb_info = (VMBnz *)calloc((size_t)d->mb_w + 1, sizeof(VMBnz));
    d->top = (VTop *)calloc((size_t)d->mb_w, sizeof(VTop));
    if (!mem || !Y || !U || !V || !d->intra_t || !d->mb_info || !d->top) {
        rc = WEBP_ERR_NOMEM; goto done;
    }

    for (mb_y = 0; mb_y < d->mb_h; ++mb_y) {
        VBool *token_br = &d->parts[mb_y & (d->num_parts - 1)];
        int i;
        for (i = 0; i < 4; i++) d->intra_l[i] = 0;
        d->mb_info[0].nz = 0; d->mb_info[0].nz_dc = 0;   /* left sentinel */
        vp8_recon_row(d, mb_y, mem, Y, U, V, ystride, uvstride, token_br);
    }

    rgb = (unsigned char *)malloc((size_t)width * height * 3);
    if (!rgb) { rc = WEBP_ERR_NOMEM; goto done; }
    for (yy = 0; yy < height; ++yy) {
        for (x = 0; x < width; ++x) {
            int Yv = Y[yy * ystride + x];
            int Uv = U[(yy >> 1) * uvstride + (x >> 1)];
            int Vv = V[(yy >> 1) * uvstride + (x >> 1)];
            vp8_yuv_to_rgb(Yv, Uv, Vv, rgb + (yy * width + x) * 3);
        }
    }
    *out_rgb = rgb;
    *out_w = (unsigned)width;
    *out_h = (unsigned)height;
    rc = WEBP_OK;

done:
    if (mem) free(mem);
    if (Y) free(Y);
    if (U) free(U);
    if (V) free(V);
    if (d->intra_t) free(d->intra_t);
    if (d->mb_info) free(d->mb_info);
    if (d->top) free(d->top);
    free(d);
    return rc;
}


#ifdef WEBPDEC_TEST
#include <stdio.h>
int main(int argc, char **argv)
{
    FILE *f; unsigned char *buf; long n;
    unsigned char *rgb; unsigned w, h; int rc;

    if (argc < 3) { fprintf(stderr, "usage: %s in.webp out.ppm\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read\n"); return 2; }
    fclose(f);

    rc = webp_decode_rgb(buf, (unsigned long)n, 4096, &rgb, &w, &h);
    fprintf(stderr, "decode rc=%d w=%u h=%u\n", rc, w, h);
    if (rc != WEBP_OK) return 1;

    f = fopen(argv[2], "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(rgb); free(buf);
    return 0;
}
#endif
