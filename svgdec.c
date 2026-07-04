/* See svgdec.h for the supported feature subset and rationale. */
#include "svgdec.h"

#include <string.h>

#define SVG_MAX_DIM        64  /* must match callers' outW/outH cap */
#define SVG_MAX_DEPTH       24  /* nested <g>/<svg> style-stack depth */
#define SVG_MAX_SHAPE_PTS  512  /* flattened points per path/shape element */
#define SVG_MAX_SUBPATHS    24  /* subpaths ("M ... Z M ... Z ...") per shape */
#define SVG_MAX_SHAPES     600  /* painted shapes per document (soft cap) */
#define SVG_SCAN_LIMIT    (256L * 1024L) /* bytes actually walked */

/* ---- fixed point (Q16.16) ------------------------------------------- */

typedef long Fx;
#define FX_SHIFT 16
#define FX_ONE   (1L << FX_SHIFT)

static Fx FxMul(Fx a, Fx b)
{
	return (Fx)(((long long)a * (long long)b) >> FX_SHIFT);
}

static Fx FxDiv(Fx a, Fx b)
{
	if (b == 0)
		return 0;
	return (Fx)(((long long)a << FX_SHIFT) / (long long)b);
}

static Fx FxFromInt(long i)
{
	if (i > 30000) i = 30000;
	if (i < -30000) i = -30000;
	return (Fx)(i << FX_SHIFT);
}

static Fx FxLerp(Fx a, Fx b, Fx t)
{
	return a + FxMul(b - a, t);
}

/* Integer Newton's-method sqrt on a Q16.16 value, returning a Q16.16
 * result -- used to estimate a transform's linear scale factor for
 * stroke widths, without floating point. */
static Fx FxSqrt(Fx v)
{
	long long n;
	long long x, y;
	if (v <= 0)
		return 0;
	n = (long long)v << FX_SHIFT; /* v is Q16.16; sqrt(v) needs v scaled by 2^16 again */
	x = n;
	y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2;
	}
	return (Fx)x;
}

/* sin(0..90 degrees) * 65536, rounded; other quadrants derived by symmetry. */
static const unsigned long sSin90[91] = {
	0, 1144, 2287, 3430, 4572, 5712, 6850, 7987, 9121, 10252,
	11380, 12505, 13626, 14742, 15855, 16962, 18064, 19161, 20252, 21336,
	22415, 23486, 24550, 25607, 26656, 27697, 28729, 29753, 30767, 31772,
	32768, 33754, 34729, 35693, 36647, 37590, 38521, 39441, 40348, 41243,
	42126, 42995, 43852, 44695, 45525, 46341, 47143, 47930, 48703, 49461,
	50203, 50931, 51643, 52339, 53020, 53684, 54332, 54963, 55578, 56175,
	56756, 57319, 57865, 58393, 58903, 59396, 59870, 60326, 60764, 61183,
	61584, 61966, 62328, 62672, 62997, 63303, 63589, 63856, 64104, 64332,
	64540, 64729, 64898, 65048, 65177, 65287, 65376, 65446, 65496, 65526,
	65536
};

static Fx FxSinDeg(long deg)
{
	long d = deg % 360;
	if (d < 0) d += 360;
	if (d <= 90) return (Fx)sSin90[d];
	if (d <= 180) return (Fx)sSin90[180 - d];
	if (d <= 270) return -(Fx)sSin90[d - 180];
	return -(Fx)sSin90[360 - d];
}

static Fx FxCosDeg(long deg) { return FxSinDeg(deg + 90); }
static Fx FxTanDeg(long deg)
{
	Fx c = FxCosDeg(deg);
	if (c == 0) c = 1;
	return FxDiv(FxSinDeg(deg), c);
}

/* ---- 2x3 affine matrix: x' = a*x + c*y + e ; y' = b*x + d*y + f ------ */

typedef struct { Fx a, b, c, d, e, f; } Mat2x3;

static void MatIdentity(Mat2x3 *m)
{
	m->a = FX_ONE; m->b = 0; m->c = 0; m->d = FX_ONE; m->e = 0; m->f = 0;
}

/* out = m1 (outer/applied second) composed with m2 (inner/applied first) */
static void MatMul(Mat2x3 *out, const Mat2x3 *m1, const Mat2x3 *m2)
{
	Mat2x3 r;
	r.a = FxMul(m1->a, m2->a) + FxMul(m1->c, m2->b);
	r.b = FxMul(m1->b, m2->a) + FxMul(m1->d, m2->b);
	r.c = FxMul(m1->a, m2->c) + FxMul(m1->c, m2->d);
	r.d = FxMul(m1->b, m2->c) + FxMul(m1->d, m2->d);
	r.e = FxMul(m1->a, m2->e) + FxMul(m1->c, m2->f) + m1->e;
	r.f = FxMul(m1->b, m2->e) + FxMul(m1->d, m2->f) + m1->f;
	*out = r;
}

static void MatApply(const Mat2x3 *m, Fx x, Fx y, Fx *ox, Fx *oy)
{
	*ox = FxMul(m->a, x) + FxMul(m->c, y) + m->e;
	*oy = FxMul(m->b, x) + FxMul(m->d, y) + m->f;
}

/* Rough uniform-scale estimate of a matrix, used only to size stroke
 * widths in output-pixel space; anisotropic scale is approximated by
 * its geometric mean via the determinant. */
static Fx MatScaleEstimate(const Mat2x3 *m)
{
	Fx det = FxMul(m->a, m->d) - FxMul(m->b, m->c);
	if (det < 0) det = -det;
	return FxSqrt(det);
}

/* ---- number / token parsing ------------------------------------------ */

static int SvgIsSpaceOrComma(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',';
}

static const char *SvgSkipWs(const char *s, const char *end)
{
	while (s < end && SvgIsSpaceOrComma(*s)) s++;
	return s;
}

/* Parses one fixed-point number (optional sign, digits, optional
 * fraction, optional small integer exponent).  Returns the position
 * just past the number, or NULL if none was found at `s`. */
static const char *SvgParseNumber(const char *s, const char *end, Fx *out)
{
	int sign = 1, any = 0, expSign = 1, expVal = 0;
	long ip = 0, fracNum = 0, fracDen = 1;
	Fx val;

	if (s >= end) return NULL;
	if (*s == '+') s++;
	else if (*s == '-') { sign = -1; s++; }

	while (s < end && *s >= '0' && *s <= '9') {
		if (ip < 1000000L) ip = ip * 10 + (*s - '0');
		s++; any = 1;
	}
	if (s < end && *s == '.') {
		s++;
		while (s < end && *s >= '0' && *s <= '9') {
			if (fracDen < 1000000L) { fracNum = fracNum * 10 + (*s - '0'); fracDen *= 10; }
			s++; any = 1;
		}
	}
	if (!any) return NULL;
	if (s < end && (*s == 'e' || *s == 'E')) {
		const char *save = s;
		const char *t = s + 1;
		int esign = 1, eval = 0, eany = 0;
		if (t < end && (*t == '+' || *t == '-')) { esign = (*t == '-') ? -1 : 1; t++; }
		while (t < end && *t >= '0' && *t <= '9') { eval = eval * 10 + (*t - '0'); t++; eany = 1; }
		if (eany) { s = t; expSign = esign; expVal = eval; } else s = save;
	}

	val = FxFromInt(ip);
	if (fracNum) val += (Fx)(((long long)fracNum << FX_SHIFT) / fracDen);
	if (expVal) {
		int i, n = expVal > 8 ? 8 : expVal;
		for (i = 0; i < n; i++) val = (expSign > 0) ? val * 10 : val / 10;
	}
	/* A trailing unit suffix (px, pt, %, em, ...) is left for the caller
	 * to skip; only bare numbers and percentages are meaningful here. */
	if (s < end && *s == '%') s++;
	*out = sign < 0 ? -val : val;
	return s;
}

static int SvgStrEqN(const char *a, int alen, const char *b)
{
	int blen = (int)strlen(b);
	return alen == blen && memcmp(a, b, (size_t)alen) == 0;
}

/* ---- attribute lookup (re-scans the tag's raw attribute text) -------- */

/* Finds attribute `name` within [attrs, attrsEnd) (the raw text between
 * the tag name and its closing '>' or "/>").  Returns 1 and sets
 * val/vlen on success. */
static int SvgFindAttr(const char *attrs, const char *attrsEnd,
	const char *name, const char **val, int *vlen)
{
	const char *s = attrs;
	int nlen = (int)strlen(name);
	while (s < attrsEnd) {
		const char *nameStart;
		int thisLen;
		char quote;
		const char *vstart, *vend;
		s = SvgSkipWs(s, attrsEnd);
		if (s >= attrsEnd || *s == '/' ) break;
		nameStart = s;
		while (s < attrsEnd && *s != '=' && !SvgIsSpaceOrComma(*s) && *s != '/') s++;
		thisLen = (int)(s - nameStart);
		s = SvgSkipWs(s, attrsEnd);
		if (s >= attrsEnd || *s != '=') {
			/* malformed/valueless attribute; bail to avoid looping */
			break;
		}
		s++;
		s = SvgSkipWs(s, attrsEnd);
		if (s >= attrsEnd || (*s != '"' && *s != '\'')) break;
		quote = *s; s++;
		vstart = s;
		while (s < attrsEnd && *s != quote) s++;
		vend = s;
		if (s < attrsEnd) s++; /* closing quote */
		if (thisLen == nlen && memcmp(nameStart, name, (size_t)nlen) == 0) {
			*val = vstart; *vlen = (int)(vend - vstart);
			return 1;
		}
	}
	return 0;
}

/* ---- color parsing ---------------------------------------------------- */

typedef struct { const char *name; unsigned char r, g, b; } SvgNamedColor;
static const SvgNamedColor sNamedColors[] = {
	{ "black", 0, 0, 0 }, { "white", 255, 255, 255 },
	{ "red", 255, 0, 0 }, { "green", 0, 128, 0 }, { "blue", 0, 0, 255 },
	{ "yellow", 255, 255, 0 }, { "cyan", 0, 255, 255 }, { "magenta", 255, 0, 255 },
	{ "gray", 128, 128, 128 }, { "grey", 128, 128, 128 },
	{ "silver", 192, 192, 192 }, { "orange", 255, 165, 0 },
	{ "purple", 128, 0, 128 }, { "navy", 0, 0, 128 },
	{ "lime", 0, 255, 0 }, { "maroon", 128, 0, 0 },
	{ "olive", 128, 128, 0 }, { "teal", 0, 128, 128 },
	{ "pink", 255, 192, 203 }, { "brown", 165, 42, 42 },
	{ "gold", 255, 215, 0 }, { "indigo", 75, 0, 130 },
	{ "violet", 238, 130, 238 }, { "transparent", 0, 0, 0 }
};
#define SVG_NAMED_COLOR_COUNT (int)(sizeof(sNamedColors) / sizeof(sNamedColors[0]))

static int SvgHexNibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Parses a "fill"/"stroke" paint value.  Returns 0 (color set), 1 (none/
 * transparent), or 2 (unresolvable reference such as url(#...) -- caller
 * should treat as none). */
static int SvgParseColor(const char *s, int len, unsigned char *r, unsigned char *g, unsigned char *b)
{
	const char *end = s + len;
	s = SvgSkipWs(s, end);
	len = (int)(end - s);
	if (len == 0) return 1;
	if (SvgStrEqN(s, len, "none")) return 1;
	if (len >= 3 && memcmp(s, "url", 3) == 0) return 2;
	if (SvgStrEqN(s, len, "currentColor")) { *r = *g = *b = 0; return 0; }
	if (SvgStrEqN(s, len, "transparent")) return 1;
	if (*s == '#') {
		int h[6], i, n = 0;
		for (i = 1; i < len && n < 6; i++) {
			int v = SvgHexNibble(s[i]);
			if (v < 0) break;
			h[n++] = v;
		}
		if (n == 3) { *r = (unsigned char)(h[0] * 17); *g = (unsigned char)(h[1] * 17); *b = (unsigned char)(h[2] * 17); return 0; }
		if (n == 6) { *r = (unsigned char)(h[0] * 16 + h[1]); *g = (unsigned char)(h[2] * 16 + h[3]); *b = (unsigned char)(h[4] * 16 + h[5]); return 0; }
		return 1;
	}
	if (len >= 4 && memcmp(s, "rgb", 3) == 0) {
		const char *p = s + 3;
		const char *pe = end;
		int comp[3], i;
		if (*p == 'a') p++;
		while (p < pe && *p != '(') p++;
		if (p < pe) p++;
		for (i = 0; i < 3; i++) {
			Fx v;
			p = SvgSkipWs(p, pe);
			p = SvgParseNumber(p, pe, &v);
			if (!p) return 1;
			comp[i] = (int)(v >> FX_SHIFT);
			if (comp[i] < 0) comp[i] = 0;
			if (comp[i] > 255) comp[i] = 255;
			p = SvgSkipWs(p, pe);
			if (p < pe && *p == ',') p++;
		}
		*r = (unsigned char)comp[0]; *g = (unsigned char)comp[1]; *b = (unsigned char)comp[2];
		return 0;
	}
	{
		int i;
		for (i = 0; i < SVG_NAMED_COLOR_COUNT; i++) {
			if (SvgStrEqN(s, len, sNamedColors[i].name)) {
				*r = sNamedColors[i].r; *g = sNamedColors[i].g; *b = sNamedColors[i].b;
				return SvgStrEqN(s, len, "transparent") ? 1 : 0;
			}
		}
	}
	return 1; /* unknown keyword: treat as no paint rather than guessing black */
}

/* Parses an opacity value (0..1, or a percentage) into 0..255. */
static int SvgParseOpacity255(const char *s, int len)
{
	const char *end = s + len;
	Fx v;
	long i;
	if (!SvgParseNumber(s, end, &v)) return 255;
	i = (v * 255 + (FX_ONE / 2)) >> FX_SHIFT;
	if (i < 0) i = 0;
	if (i > 255) i = 255;
	return (int)i;
}

/* ---- transform list ---------------------------------------------------- */

static void SvgParseTransform(const char *s, int len, Mat2x3 *out)
{
	const char *end = s + len;
	MatIdentity(out);
	s = SvgSkipWs(s, end);
	while (s < end) {
		const char *nameStart = s;
		int nlen;
		Fx args[6]; int nargs = 0;
		Mat2x3 local;
		while (s < end && *s != '(') s++;
		nlen = (int)(s - nameStart);
		if (s >= end) break;
		s++; /* '(' */
		while (s < end && *s != ')' && nargs < 6) {
			const char *next;
			s = SvgSkipWs(s, end);
			next = SvgParseNumber(s, end, &args[nargs]);
			if (!next) break;
			s = next; nargs++;
			s = SvgSkipWs(s, end);
		}
		while (s < end && *s != ')') s++;
		if (s < end) s++; /* ')' */

		MatIdentity(&local);
		if (SvgStrEqN(nameStart, nlen, "translate") && nargs >= 1) {
			local.e = args[0]; local.f = (nargs >= 2) ? args[1] : 0;
		} else if (SvgStrEqN(nameStart, nlen, "scale") && nargs >= 1) {
			local.a = args[0]; local.d = (nargs >= 2) ? args[1] : args[0];
		} else if (SvgStrEqN(nameStart, nlen, "rotate") && nargs >= 1) {
			long deg = args[0] >> FX_SHIFT;
			Fx cs = FxCosDeg(deg), sn = FxSinDeg(deg);
			Mat2x3 rot, pre, post;
			rot.a = cs; rot.b = sn; rot.c = -sn; rot.d = cs; rot.e = 0; rot.f = 0;
			if (nargs >= 3) {
				MatIdentity(&pre); pre.e = args[1]; pre.f = args[2];
				MatIdentity(&post); post.e = -args[1]; post.f = -args[2];
				MatMul(&local, &pre, &rot);
				MatMul(&local, &local, &post);
			} else {
				local = rot;
			}
		} else if (SvgStrEqN(nameStart, nlen, "skewX") && nargs >= 1) {
			local.c = FxTanDeg(args[0] >> FX_SHIFT);
		} else if (SvgStrEqN(nameStart, nlen, "skewY") && nargs >= 1) {
			local.b = FxTanDeg(args[0] >> FX_SHIFT);
		} else if (SvgStrEqN(nameStart, nlen, "matrix") && nargs >= 6) {
			local.a = args[0]; local.b = args[1]; local.c = args[2];
			local.d = args[3]; local.e = args[4]; local.f = args[5];
		}
		MatMul(out, out, &local);
		s = SvgSkipWs(s, end);
	}
}

/* ---- rendering state --------------------------------------------------- */

typedef struct {
	Mat2x3 m;
	int hasFill; unsigned char fillR, fillG, fillB; int fillAlpha;
	int hasStroke; unsigned char strokeR, strokeG, strokeB; int strokeAlpha;
	Fx strokeWidth;
	int evenOdd;
} SvgStyle;

typedef struct {
	unsigned char *rgb;
	unsigned char *touched;
	int w, h;
	long testBudget;
} SvgCtx;

static void SvgStyleDefault(SvgStyle *st)
{
	MatIdentity(&st->m);
	st->hasFill = 1; st->fillR = st->fillG = st->fillB = 0; st->fillAlpha = 255;
	st->hasStroke = 0; st->strokeR = st->strokeG = st->strokeB = 0; st->strokeAlpha = 255;
	st->strokeWidth = FxFromInt(1);
	st->evenOdd = 0;
}

/* Applies presentation attributes found on one tag onto a style already
 * seeded with the parent's (inherited) values. */
static void SvgApplyAttrs(SvgStyle *st, const char *attrs, const char *attrsEnd)
{
	const char *v; int vlen;
	int opacity255 = 255;

	if (SvgFindAttr(attrs, attrsEnd, "transform", &v, &vlen)) {
		Mat2x3 local;
		SvgParseTransform(v, vlen, &local);
		MatMul(&st->m, &st->m, &local);
	}
	if (SvgFindAttr(attrs, attrsEnd, "fill", &v, &vlen)) {
		unsigned char r, g, b;
		int rc = SvgParseColor(v, vlen, &r, &g, &b);
		if (rc == 0) { st->hasFill = 1; st->fillR = r; st->fillG = g; st->fillB = b; }
		else st->hasFill = 0;
	}
	if (SvgFindAttr(attrs, attrsEnd, "stroke", &v, &vlen)) {
		unsigned char r, g, b;
		int rc = SvgParseColor(v, vlen, &r, &g, &b);
		if (rc == 0) { st->hasStroke = 1; st->strokeR = r; st->strokeG = g; st->strokeB = b; }
		else st->hasStroke = 0;
	}
	if (SvgFindAttr(attrs, attrsEnd, "stroke-width", &v, &vlen)) {
		Fx sw;
		if (SvgParseNumber(v, v + vlen, &sw)) st->strokeWidth = sw;
	}
	if (SvgFindAttr(attrs, attrsEnd, "fill-rule", &v, &vlen))
		st->evenOdd = SvgStrEqN(v, vlen, "evenodd");
	if (SvgFindAttr(attrs, attrsEnd, "fill-opacity", &v, &vlen))
		st->fillAlpha = (st->fillAlpha * SvgParseOpacity255(v, vlen)) / 255;
	if (SvgFindAttr(attrs, attrsEnd, "stroke-opacity", &v, &vlen))
		st->strokeAlpha = (st->strokeAlpha * SvgParseOpacity255(v, vlen)) / 255;
	if (SvgFindAttr(attrs, attrsEnd, "opacity", &v, &vlen))
		opacity255 = SvgParseOpacity255(v, vlen);
	if (opacity255 != 255) {
		st->fillAlpha = (st->fillAlpha * opacity255) / 255;
		st->strokeAlpha = (st->strokeAlpha * opacity255) / 255;
	}
}

/* ---- scanline fill with 2x2 supersampled coverage ---------------------- */

typedef struct { Fx x, y; } FxPt;

static int SvgPointInPoly(const FxPt *pts, const int *subStart, const int *subCount,
	int nSub, int evenOdd, Fx px, Fx py)
{
	int winding = 0, si;
	for (si = 0; si < nSub; si++) {
		int start = subStart[si], count = subCount[si], i;
		for (i = 0; i < count; i++) {
			FxPt p0 = pts[start + i];
			FxPt p1 = pts[start + ((i + 1) % count)];
			int dir = 0;
			if (p0.y <= py && p1.y > py) dir = 1;
			else if (p1.y <= py && p0.y > py) dir = -1;
			if (dir) {
				Fx t = FxDiv(py - p0.y, p1.y - p0.y);
				Fx xCross = FxLerp(p0.x, p1.x, t);
				if (xCross > px) winding += dir;
			}
		}
	}
	return evenOdd ? (winding & 1) : (winding != 0);
}

static void SvgBlendPixel(SvgCtx *ctx, int px, int py, unsigned char r, unsigned char g,
	unsigned char b, int alpha255)
{
	int idx;
	unsigned char *dst;
	if (px < 0 || py < 0 || px >= ctx->w || py >= ctx->h || alpha255 <= 0)
		return;
	idx = py * ctx->w + px;
	dst = ctx->rgb + idx * 3;
	if (alpha255 >= 255) {
		dst[0] = r; dst[1] = g; dst[2] = b;
	} else {
		dst[0] = (unsigned char)((dst[0] * (255 - alpha255) + r * alpha255) / 255);
		dst[1] = (unsigned char)((dst[1] * (255 - alpha255) + g * alpha255) / 255);
		dst[2] = (unsigned char)((dst[2] * (255 - alpha255) + b * alpha255) / 255);
	}
	ctx->touched[idx] = 1;
}

/* Fills the polygon set (pts/subStart/subCount, nSub subpaths) with a
 * solid color, 2x2 supersampled per output pixel for cheap antialiasing.
 * Consumes ctx->testBudget; returns 0 if the budget ran out mid-shape
 * (caller stops issuing further shapes but the decode still succeeds). */
static int SvgFillPoly(SvgCtx *ctx, const FxPt *pts, const int *subStart, const int *subCount,
	int nSub, int evenOdd, unsigned char r, unsigned char g, unsigned char b, int alpha255)
{
	Fx minX, maxX, minY, maxY;
	int x0, x1, y0, y1, x, y, si, i, totalEdges = 0;
	if (alpha255 <= 0 || nSub <= 0) return 1;
	minX = maxX = pts[subStart[0]].x;
	minY = maxY = pts[subStart[0]].y;
	for (si = 0; si < nSub; si++) {
		totalEdges += subCount[si];
		for (i = 0; i < subCount[si]; i++) {
			FxPt p = pts[subStart[si] + i];
			if (p.x < minX) minX = p.x;
			if (p.x > maxX) maxX = p.x;
			if (p.y < minY) minY = p.y;
			if (p.y > maxY) maxY = p.y;
		}
	}
	x0 = (int)(minX >> FX_SHIFT); if (x0 < 0) x0 = 0;
	y0 = (int)(minY >> FX_SHIFT); if (y0 < 0) y0 = 0;
	x1 = (int)(maxX >> FX_SHIFT) + 1; if (x1 > ctx->w) x1 = ctx->w;
	y1 = (int)(maxY >> FX_SHIFT) + 1; if (y1 > ctx->h) y1 = ctx->h;

	for (y = y0; y < y1; y++) {
		for (x = x0; x < x1; x++) {
			int cov = 0, s;
			static const Fx off[4] = { FX_ONE / 4, (FX_ONE * 3) / 4, FX_ONE / 4, (FX_ONE * 3) / 4 };
			static const Fx offY[4] = { FX_ONE / 4, FX_ONE / 4, (FX_ONE * 3) / 4, (FX_ONE * 3) / 4 };
			if (ctx->testBudget <= 0) return 0;
			for (s = 0; s < 4; s++) {
				Fx sx = FxFromInt(x) + off[s];
				Fx sy = FxFromInt(y) + offY[s];
				ctx->testBudget -= totalEdges; /* proxy for the O(edges) cost of one test */
				if (SvgPointInPoly(pts, subStart, subCount, nSub, evenOdd, sx, sy))
					cov++;
			}
			if (cov > 0)
				SvgBlendPixel(ctx, x, y, r, g, b, (alpha255 * cov) / 4);
		}
		if (ctx->testBudget <= 0) return 0;
	}
	return 1;
}

/* ---- shape point-list builder ------------------------------------------ */

typedef struct {
	FxPt pts[SVG_MAX_SHAPE_PTS];
	int subStart[SVG_MAX_SUBPATHS];
	int subCount[SVG_MAX_SUBPATHS];
	int subClosed[SVG_MAX_SUBPATHS];
	int nSub;
	int nPts;
} SvgShape;

static void SvgShapeBegin(SvgShape *sh) { sh->nSub = 0; sh->nPts = 0; }

static void SvgShapeNewSub(SvgShape *sh)
{
	if (sh->nSub >= SVG_MAX_SUBPATHS) return;
	sh->subStart[sh->nSub] = sh->nPts;
	sh->subCount[sh->nSub] = 0;
	sh->subClosed[sh->nSub] = 0;
	sh->nSub++;
}

static void SvgShapeCloseSub(SvgShape *sh)
{
	if (sh->nSub > 0) sh->subClosed[sh->nSub - 1] = 1;
}

/* Shared scratch shape reused by every SvgHandle*() call below -- shapes
 * are processed and rasterized one at a time, never concurrently or
 * recursively, so a single static instance avoids putting a ~4KB point
 * buffer on the stack per call (real Amiga process stacks are small). */
static SvgShape sSvgShape;

static void SvgShapeAddPt(SvgShape *sh, const Mat2x3 *m, Fx ux, Fx uy)
{
	Fx ox, oy;
	if (sh->nSub == 0 || sh->nPts >= SVG_MAX_SHAPE_PTS) return;
	MatApply(m, ux, uy, &ox, &oy);
	sh->pts[sh->nPts].x = ox; sh->pts[sh->nPts].y = oy;
	sh->nPts++;
	sh->subCount[sh->nSub - 1]++;
}

/* Renders a completed shape's fill and (approximate) stroke. */
static void SvgShapeRender(SvgCtx *ctx, const SvgShape *sh, const SvgStyle *st, const Mat2x3 *m)
{
	if (sh->nSub <= 0) return;
	if (st->hasFill && st->fillAlpha > 0) {
		if (!SvgFillPoly(ctx, sh->pts, sh->subStart, sh->subCount, sh->nSub, st->evenOdd,
			st->fillR, st->fillG, st->fillB, st->fillAlpha))
			return;
	}
	if (st->hasStroke && st->strokeAlpha > 0) {
		Fx wOut = FxMul(st->strokeWidth, MatScaleEstimate(m));
		Fx half = wOut / 2;
		int si;
		if (half < FX_ONE / 2) half = FX_ONE / 2; /* keep hairlines visible */
		for (si = 0; si < sh->nSub; si++) {
			int start = sh->subStart[si], count = sh->subCount[si], i;
			int segs = sh->subClosed[si] ? count : count - 1;
			if (count < 2) continue;
			for (i = 0; i < segs; i++) {
				FxPt p0 = sh->pts[start + i];
				FxPt p1 = sh->pts[start + (i + 1) % count];
				Fx dx = p1.x - p0.x, dy = p1.y - p0.y;
				Fx len = FxSqrt(FxMul(dx, dx) + FxMul(dy, dy));
				Fx nx, ny;
				FxPt quad[4]; int qs[1] = { 0 }; int qc[1] = { 4 };
				if (len < 1) continue;
				nx = FxDiv(-dy, len); ny = FxDiv(dx, len);
				quad[0].x = p0.x + FxMul(nx, half); quad[0].y = p0.y + FxMul(ny, half);
				quad[1].x = p1.x + FxMul(nx, half); quad[1].y = p1.y + FxMul(ny, half);
				quad[2].x = p1.x - FxMul(nx, half); quad[2].y = p1.y - FxMul(ny, half);
				quad[3].x = p0.x - FxMul(nx, half); quad[3].y = p0.y - FxMul(ny, half);
				if (!SvgFillPoly(ctx, quad, qs, qc, 1, 0, st->strokeR, st->strokeG, st->strokeB, st->strokeAlpha))
					return;
			}
		}
	}
}

/* Appends a circular arc's worth of points (used for circle/ellipse and
 * rounded rect corners) using the fixed sin/cos table; startDeg..endDeg
 * sweep counter-clockwise in user-space units, steps taken every ~11
 * degrees (33 samples for a full turn) which is smooth enough at the
 * thumbnail sizes this decoder targets. */
static void SvgShapeAddEllipseArc(SvgShape *sh, const Mat2x3 *m, Fx cx, Fx cy, Fx rx, Fx ry,
	long startDeg, long endDeg)
{
	long deg;
	long step = 11;
	if (endDeg < startDeg) step = -step;
	for (deg = startDeg; (step > 0) ? (deg <= endDeg) : (deg >= endDeg); deg += step) {
		Fx x = cx + FxMul(rx, FxCosDeg(deg));
		Fx y = cy + FxMul(ry, FxSinDeg(deg));
		SvgShapeAddPt(sh, m, x, y);
	}
	{
		Fx x = cx + FxMul(rx, FxCosDeg(endDeg));
		Fx y = cy + FxMul(ry, FxSinDeg(endDeg));
		SvgShapeAddPt(sh, m, x, y);
	}
}

/* ---- path "d" data ------------------------------------------------------ */

static void SvgFlattenCubic(SvgShape *sh, const Mat2x3 *m, Fx x0, Fx y0, Fx x1, Fx y1,
	Fx x2, Fx y2, Fx x3, Fx y3)
{
	int i;
	for (i = 1; i <= 8; i++) {
		Fx t = FxDiv(FxFromInt(i), FxFromInt(8));
		Fx ax = FxLerp(x0, x1, t), ay = FxLerp(y0, y1, t);
		Fx bx = FxLerp(x1, x2, t), by = FxLerp(y1, y2, t);
		Fx cx = FxLerp(x2, x3, t), cy = FxLerp(y2, y3, t);
		Fx dx = FxLerp(ax, bx, t), dy = FxLerp(ay, by, t);
		Fx ex = FxLerp(bx, cx, t), ey = FxLerp(by, cy, t);
		Fx fx = FxLerp(dx, ex, t), fy = FxLerp(dy, ey, t);
		SvgShapeAddPt(sh, m, fx, fy);
	}
}

static void SvgFlattenQuad(SvgShape *sh, const Mat2x3 *m, Fx x0, Fx y0, Fx x1, Fx y1, Fx x2, Fx y2)
{
	int i;
	for (i = 1; i <= 6; i++) {
		Fx t = FxDiv(FxFromInt(i), FxFromInt(6));
		Fx ax = FxLerp(x0, x1, t), ay = FxLerp(y0, y1, t);
		Fx bx = FxLerp(x1, x2, t), by = FxLerp(y1, y2, t);
		Fx cx = FxLerp(ax, bx, t), cy = FxLerp(ay, by, t);
		SvgShapeAddPt(sh, m, cx, cy);
	}
}

static const char *SvgSkipCmdWs(const char *s, const char *end)
{
	while (s < end && (SvgIsSpaceOrComma(*s))) s++;
	return s;
}

static int SvgLooksLikeNumber(const char *s, const char *end)
{
	return s < end && (*s == '+' || *s == '-' || *s == '.' || (*s >= '0' && *s <= '9'));
}

static void SvgParsePathData(SvgShape *sh, const Mat2x3 *m, const char *d, int dlen)
{
	const char *s = d, *end = d + dlen;
	Fx cx = 0, cy = 0, subStartX = 0, subStartY = 0;
	Fx lastCtrlX = 0, lastCtrlY = 0;
	char cmd = 0, lastCmd = 0;
	int haveSub = 0;

	while (s < end) {
		Fx nums[7]; int n, need;
		char c;
		s = SvgSkipCmdWs(s, end);
		if (s >= end) break;
		if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
			cmd = *s; s++;
		} else if (cmd == 0) {
			break; /* garbage before any command */
		}
		c = cmd;
		switch (c) {
		case 'M': case 'm': need = 2; break;
		case 'L': case 'l': need = 2; break;
		case 'H': case 'h': need = 1; break;
		case 'V': case 'v': need = 1; break;
		case 'C': case 'c': need = 6; break;
		case 'S': case 's': need = 4; break;
		case 'Q': case 'q': need = 4; break;
		case 'T': case 't': need = 2; break;
		case 'A': case 'a': need = 7; break;
		case 'Z': case 'z': need = 0; break;
		default: return; /* unknown command: stop, keep whatever we have */
		}
		n = 0;
		while (n < need) {
			const char *next;
			s = SvgSkipCmdWs(s, end);
			if (!SvgLooksLikeNumber(s, end)) break;
			next = SvgParseNumber(s, end, &nums[n]);
			if (!next) break;
			s = next; n++;
		}
		if (n < need && need > 0) return; /* malformed args: stop cleanly */

		switch (c) {
		case 'M': case 'm': {
			Fx nx = (c == 'm') ? cx + nums[0] : nums[0];
			Fx ny = (c == 'm') ? cy + nums[1] : nums[1];
			SvgShapeNewSub(sh);
			SvgShapeAddPt(sh, m, nx, ny);
			cx = nx; cy = ny; subStartX = nx; subStartY = ny; haveSub = 1;
			cmd = (c == 'm') ? 'l' : 'L'; /* subsequent implicit pairs are lineto */
			break;
		}
		case 'L': case 'l': {
			Fx nx = (c == 'l') ? cx + nums[0] : nums[0];
			Fx ny = (c == 'l') ? cy + nums[1] : nums[1];
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgShapeAddPt(sh, m, nx, ny);
			cx = nx; cy = ny;
			break;
		}
		case 'H': case 'h': {
			Fx nx = (c == 'h') ? cx + nums[0] : nums[0];
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgShapeAddPt(sh, m, nx, cy);
			cx = nx;
			break;
		}
		case 'V': case 'v': {
			Fx ny = (c == 'v') ? cy + nums[0] : nums[0];
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgShapeAddPt(sh, m, cx, ny);
			cy = ny;
			break;
		}
		case 'C': case 'c': {
			Fx x1 = nums[0], y1 = nums[1], x2 = nums[2], y2 = nums[3], x3 = nums[4], y3 = nums[5];
			if (c == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x3 += cx; y3 += cy; }
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgFlattenCubic(sh, m, cx, cy, x1, y1, x2, y2, x3, y3);
			lastCtrlX = x2; lastCtrlY = y2; cx = x3; cy = y3;
			break;
		}
		case 'S': case 's': {
			Fx x2 = nums[0], y2 = nums[1], x3 = nums[2], y3 = nums[3];
			Fx x1, y1;
			if (c == 's') { x2 += cx; y2 += cy; x3 += cx; y3 += cy; }
			if (lastCmd == 'C' || lastCmd == 'c' || lastCmd == 'S' || lastCmd == 's') {
				x1 = 2 * cx - lastCtrlX; y1 = 2 * cy - lastCtrlY;
			} else { x1 = cx; y1 = cy; }
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgFlattenCubic(sh, m, cx, cy, x1, y1, x2, y2, x3, y3);
			lastCtrlX = x2; lastCtrlY = y2; cx = x3; cy = y3;
			break;
		}
		case 'Q': case 'q': {
			Fx x1 = nums[0], y1 = nums[1], x2 = nums[2], y2 = nums[3];
			if (c == 'q') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; }
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgFlattenQuad(sh, m, cx, cy, x1, y1, x2, y2);
			lastCtrlX = x1; lastCtrlY = y1; cx = x2; cy = y2;
			break;
		}
		case 'T': case 't': {
			Fx x2 = nums[0], y2 = nums[1];
			Fx x1, y1;
			if (c == 't') { x2 += cx; y2 += cy; }
			if (lastCmd == 'Q' || lastCmd == 'q' || lastCmd == 'T' || lastCmd == 't') {
				x1 = 2 * cx - lastCtrlX; y1 = 2 * cy - lastCtrlY;
			} else { x1 = cx; y1 = cy; }
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgFlattenQuad(sh, m, cx, cy, x1, y1, x2, y2);
			lastCtrlX = x1; lastCtrlY = y1; cx = x2; cy = y2;
			break;
		}
		case 'A': case 'a': {
			/* Elliptical arcs degrade to a straight line to the endpoint
			 * (see svgdec.h) -- avoids an atan2 implementation for a
			 * command favicon art rarely relies on for legibility. */
			Fx nx = (c == 'a') ? cx + nums[5] : nums[5];
			Fx ny = (c == 'a') ? cy + nums[6] : nums[6];
			if (!haveSub) { SvgShapeNewSub(sh); SvgShapeAddPt(sh, m, cx, cy); haveSub = 1; }
			SvgShapeAddPt(sh, m, nx, ny);
			cx = nx; cy = ny;
			break;
		}
		case 'Z': case 'z':
			SvgShapeCloseSub(sh);
			cx = subStartX; cy = subStartY;
			haveSub = 0;
			break;
		}
		lastCmd = c;
	}
}

/* ---- tag scanning -------------------------------------------------------- */

typedef struct {
	const unsigned char *buf;
	long len;
	long pos;
} SvgScan;

static void SvgScanSkipMisc(SvgScan *sc)
{
	for (;;) {
		if (sc->pos + 4 <= sc->len && memcmp(sc->buf + sc->pos, "<!--", 4) == 0) {
			long p = sc->pos + 4;
			while (p + 3 <= sc->len && memcmp(sc->buf + p, "-->", 3) != 0) p++;
			sc->pos = (p + 3 <= sc->len) ? p + 3 : sc->len;
			continue;
		}
		if (sc->pos + 2 <= sc->len && memcmp(sc->buf + sc->pos, "<?", 2) == 0) {
			long p = sc->pos + 2;
			while (p + 2 <= sc->len && memcmp(sc->buf + p, "?>", 2) != 0) p++;
			sc->pos = (p + 2 <= sc->len) ? p + 2 : sc->len;
			continue;
		}
		if (sc->pos + 2 <= sc->len && memcmp(sc->buf + sc->pos, "<!", 2) == 0
			&& (sc->pos + 4 > sc->len || memcmp(sc->buf + sc->pos, "<![C", 4) != 0)) {
			long p = sc->pos + 2, depth = 1;
			while (p < sc->len && depth > 0) {
				if (sc->buf[p] == '<') depth++;
				else if (sc->buf[p] == '>') depth--;
				p++;
			}
			sc->pos = p;
			continue;
		}
		if (sc->pos + 9 <= sc->len && memcmp(sc->buf + sc->pos, "<![CDATA[", 9) == 0) {
			long p = sc->pos + 9;
			while (p + 3 <= sc->len && memcmp(sc->buf + p, "]]>", 3) != 0) p++;
			sc->pos = (p + 3 <= sc->len) ? p + 3 : sc->len;
			continue;
		}
		break;
	}
}

/* Names whose entire subtree is ignored: not rendered, and their
 * contents (which may include nested tags) never reach the shape
 * dispatch below. */
static int SvgIsSkipName(const char *name, int len)
{
	static const char *names[] = {
		"defs", "style", "symbol", "clipPath", "mask", "marker", "pattern",
		"linearGradient", "radialGradient", "filter", "text", "tspan",
		"metadata", "title", "desc", "image", "use", "foreignObject", "script"
	};
	int i;
	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
		if (SvgStrEqN(name, len, names[i])) return 1;
	return 0;
}

static void SvgHandleRect(SvgCtx *ctx, const SvgStyle *st, const char *attrs, const char *attrsEnd)
{
	const char *v; int vlen;
	Fx x = 0, y = 0, w = 0, h = 0, rx = -1, ry = -1;
	SvgShape *sh = &sSvgShape;
	if (SvgFindAttr(attrs, attrsEnd, "x", &v, &vlen)) SvgParseNumber(v, v + vlen, &x);
	if (SvgFindAttr(attrs, attrsEnd, "y", &v, &vlen)) SvgParseNumber(v, v + vlen, &y);
	if (SvgFindAttr(attrs, attrsEnd, "width", &v, &vlen)) SvgParseNumber(v, v + vlen, &w);
	if (SvgFindAttr(attrs, attrsEnd, "height", &v, &vlen)) SvgParseNumber(v, v + vlen, &h);
	if (SvgFindAttr(attrs, attrsEnd, "rx", &v, &vlen)) SvgParseNumber(v, v + vlen, &rx);
	if (SvgFindAttr(attrs, attrsEnd, "ry", &v, &vlen)) SvgParseNumber(v, v + vlen, &ry);
	if (w <= 0 || h <= 0) return;
	if (rx < 0 && ry >= 0) rx = ry;
	if (ry < 0 && rx >= 0) ry = rx;
	if (rx < 0) rx = 0;
	if (ry < 0) ry = 0;
	if (rx > w / 2) rx = w / 2;
	if (ry > h / 2) ry = h / 2;

	SvgShapeBegin(sh);
	if (rx <= 0 || ry <= 0) {
		SvgShapeNewSub(sh);
		SvgShapeAddPt(sh, &st->m, x, y);
		SvgShapeAddPt(sh, &st->m, x + w, y);
		SvgShapeAddPt(sh, &st->m, x + w, y + h);
		SvgShapeAddPt(sh, &st->m, x, y + h);
	} else {
		SvgShapeNewSub(sh);
		SvgShapeAddEllipseArc(sh, &st->m, x + w - rx, y + ry, rx, ry, -90, 0);
		SvgShapeAddEllipseArc(sh, &st->m, x + w - rx, y + h - ry, rx, ry, 0, 90);
		SvgShapeAddEllipseArc(sh, &st->m, x + rx, y + h - ry, rx, ry, 90, 180);
		SvgShapeAddEllipseArc(sh, &st->m, x + rx, y + ry, rx, ry, 180, 270);
	}
	SvgShapeCloseSub(sh);
	SvgShapeRender(ctx, sh, st, &st->m);
}

static void SvgHandleCircleOrEllipse(SvgCtx *ctx, const SvgStyle *st, const char *attrs,
	const char *attrsEnd, int ellipse)
{
	const char *v; int vlen;
	Fx cx = 0, cy = 0, rx = 0, ry = 0;
	SvgShape *sh = &sSvgShape;
	if (SvgFindAttr(attrs, attrsEnd, "cx", &v, &vlen)) SvgParseNumber(v, v + vlen, &cx);
	if (SvgFindAttr(attrs, attrsEnd, "cy", &v, &vlen)) SvgParseNumber(v, v + vlen, &cy);
	if (ellipse) {
		if (SvgFindAttr(attrs, attrsEnd, "rx", &v, &vlen)) SvgParseNumber(v, v + vlen, &rx);
		if (SvgFindAttr(attrs, attrsEnd, "ry", &v, &vlen)) SvgParseNumber(v, v + vlen, &ry);
	} else {
		if (SvgFindAttr(attrs, attrsEnd, "r", &v, &vlen)) SvgParseNumber(v, v + vlen, &rx);
		ry = rx;
	}
	if (rx <= 0 || ry <= 0) return;
	SvgShapeBegin(sh);
	SvgShapeNewSub(sh);
	SvgShapeAddEllipseArc(sh, &st->m, cx, cy, rx, ry, 0, 359);
	SvgShapeCloseSub(sh);
	SvgShapeRender(ctx, sh, st, &st->m);
}

static void SvgHandleLine(SvgCtx *ctx, const SvgStyle *st, const char *attrs, const char *attrsEnd)
{
	const char *v; int vlen;
	Fx x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	SvgShape *sh = &sSvgShape;
	if (SvgFindAttr(attrs, attrsEnd, "x1", &v, &vlen)) SvgParseNumber(v, v + vlen, &x1);
	if (SvgFindAttr(attrs, attrsEnd, "y1", &v, &vlen)) SvgParseNumber(v, v + vlen, &y1);
	if (SvgFindAttr(attrs, attrsEnd, "x2", &v, &vlen)) SvgParseNumber(v, v + vlen, &x2);
	if (SvgFindAttr(attrs, attrsEnd, "y2", &v, &vlen)) SvgParseNumber(v, v + vlen, &y2);
	if (!st->hasStroke) return; /* lines have no fill */
	SvgShapeBegin(sh);
	SvgShapeNewSub(sh);
	SvgShapeAddPt(sh, &st->m, x1, y1);
	SvgShapeAddPt(sh, &st->m, x2, y2);
	SvgShapeRender(ctx, sh, st, &st->m);
}

static void SvgHandlePoly(SvgCtx *ctx, const SvgStyle *st, const char *attrs, const char *attrsEnd,
	int closed)
{
	const char *v; int vlen;
	SvgShape *sh = &sSvgShape;
	if (!SvgFindAttr(attrs, attrsEnd, "points", &v, &vlen)) return;
	SvgShapeBegin(sh);
	SvgShapeNewSub(sh);
	{
		const char *s = v, *end = v + vlen;
		while (s < end) {
			Fx x, y;
			const char *next;
			s = SvgSkipWs(s, end);
			next = SvgParseNumber(s, end, &x);
			if (!next) break;
			s = SvgSkipWs(next, end);
			next = SvgParseNumber(s, end, &y);
			if (!next) break;
			s = next;
			SvgShapeAddPt(sh, &st->m, x, y);
		}
	}
	if (closed) SvgShapeCloseSub(sh);
	SvgShapeRender(ctx, sh, st, &st->m);
}

static void SvgHandlePath(SvgCtx *ctx, const SvgStyle *st, const char *attrs, const char *attrsEnd)
{
	const char *v; int vlen;
	SvgShape *sh = &sSvgShape;
	if (!SvgFindAttr(attrs, attrsEnd, "d", &v, &vlen)) return;
	SvgShapeBegin(sh);
	SvgParsePathData(sh, &st->m, v, vlen);
	SvgShapeRender(ctx, sh, st, &st->m);
}

/* ---- main document walk -------------------------------------------------- */

static int SvgReadTagName(const unsigned char *buf, long len, long pos, const char **name, int *nlen)
{
	long s = pos;
	*name = (const char *)buf + s;
	while (s < len && buf[s] != ' ' && buf[s] != '\t' && buf[s] != '\r' && buf[s] != '\n'
		&& buf[s] != '/' && buf[s] != '>')
		s++;
	*nlen = (int)(s - pos);
	return (int)s;
}

int SvgLooksLikeSvg(const unsigned char *data, int bytes)
{
	long i, limit;
	if (!data || bytes < 5) return 0;
	limit = bytes < 4096 ? bytes : 4096;
	for (i = 0; i + 4 < limit; i++) {
		if (data[i] == '<' && data[i + 1] == 's' && data[i + 2] == 'v' && data[i + 3] == 'g') {
			unsigned char c = data[i + 4];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
				return 1;
		}
	}
	return 0;
}

/* Static scratch for SvgDecodeToGrey(): a style-stack entry, the RGB
 * working canvas, and its coverage map, all sized for the SVG_MAX_DIM
 * cap. Kept off the C stack for the same real-Amiga-stack-budget reason
 * as sSvgShape above -- only one decode runs at a time. */
static SvgStyle sSvgStyleStack[SVG_MAX_DEPTH];
static unsigned char sSvgRgbBuf[SVG_MAX_DIM * SVG_MAX_DIM * 3];
static unsigned char sSvgTouchedBuf[SVG_MAX_DIM * SVG_MAX_DIM];

int SvgDecodeToGrey(const unsigned char *svgData, unsigned long svgBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	SvgScan sc;
	SvgCtx ctx;
	SvgStyle *styleStack = sSvgStyleStack;
	unsigned char *rgbBuf = sSvgRgbBuf;
	unsigned char *touchedBuf = sSvgTouchedBuf;
	long tagDepth = 0, styleTop = 0, skipUntilDepth = -1;
	long docLen;
	Fx vbMinX = 0, vbMinY = 0, vbW = 0, vbH = 0;
	int haveSize = 0;
	int shapesLeft = SVG_MAX_SHAPES;
	int i;

	if (!svgData || svgBytes < 5 || !greyOut || !rgbOut ||
		outW <= 0 || outW > SVG_MAX_DIM || outH <= 0 || outH > SVG_MAX_DIM)
		return -1;
	if (!SvgLooksLikeSvg(svgData, (int)(svgBytes < 4096 ? svgBytes : 4096)))
		return -1;

	docLen = (long)(svgBytes < (unsigned long)SVG_SCAN_LIMIT ? svgBytes : (unsigned long)SVG_SCAN_LIMIT);
	sc.buf = svgData; sc.len = docLen; sc.pos = 0;

	memset(rgbBuf, 0x80, sizeof(sSvgRgbBuf));
	memset(touchedBuf, 0, sizeof(sSvgTouchedBuf));
	ctx.rgb = rgbBuf; ctx.touched = touchedBuf; ctx.w = outW; ctx.h = outH;
	/* Bounds total (pixel x subsample x edge) coverage-test work across the
	 * whole document -- a proxy for wall-clock on real 68k hardware, not a
	 * hard guarantee.  Typical favicon-sized SVGs (a handful of shapes,
	 * dozens of points) use a small fraction of this; once exhausted, any
	 * remaining shapes are simply left undrawn and the decode still
	 * succeeds with whatever was painted so far. */
	ctx.testBudget = 8000000L;

	SvgStyleDefault(&styleStack[0]);

	/* Find and consume the root <svg ...> tag to establish the
	 * user-space -> output-pixel transform (viewBox, or width/height,
	 * stretched to exactly fill outW x outH; no letterboxing, matching
	 * how raster favicons are already stretched to the square art
	 * panel elsewhere in this codebase). */
	for (;;) {
		SvgScanSkipMisc(&sc);
		if (sc.pos >= sc.len) return -1;
		if (sc.buf[sc.pos] != '<') { sc.pos++; continue; }
		if (sc.pos + 4 <= sc.len && sc.buf[sc.pos + 1] == 's' && sc.buf[sc.pos + 2] == 'v'
			&& sc.buf[sc.pos + 3] == 'g') {
			const char *attrs = (const char *)sc.buf + sc.pos + 4;
			const char *ae = (const char *)sc.buf + sc.len;
			const char *tagEnd = attrs;
			const char *v; int vlen;
			while (tagEnd < ae && *tagEnd != '>') tagEnd++;
			if (SvgFindAttr(attrs, tagEnd, "viewBox", &v, &vlen)) {
				const char *p = v, *pe = v + vlen; Fx nums[4]; int n = 0;
				while (n < 4) {
					const char *next;
					p = SvgSkipWs(p, pe);
					next = SvgParseNumber(p, pe, &nums[n]);
					if (!next) break;
					p = next; n++;
				}
				if (n == 4 && nums[2] > 0 && nums[3] > 0) {
					vbMinX = nums[0]; vbMinY = nums[1]; vbW = nums[2]; vbH = nums[3];
					haveSize = 1;
				}
			}
			if (!haveSize && SvgFindAttr(attrs, tagEnd, "width", &v, &vlen)) {
				Fx w = 0, h = 0;
				SvgParseNumber(v, v + vlen, &w);
				if (SvgFindAttr(attrs, tagEnd, "height", &v, &vlen))
					SvgParseNumber(v, v + vlen, &h);
				if (w > 0 && h > 0) { vbMinX = 0; vbMinY = 0; vbW = w; vbH = h; haveSize = 1; }
			}
			sc.pos = (int)(tagEnd - (const char *)sc.buf) + 1;
			break;
		}
		sc.pos++;
	}
	if (!haveSize) return -1;

	{
		Fx sx = FxDiv(FxFromInt(outW), vbW);
		Fx sy = FxDiv(FxFromInt(outH), vbH);
		styleStack[0].m.a = sx; styleStack[0].m.b = 0;
		styleStack[0].m.c = 0; styleStack[0].m.d = sy;
		styleStack[0].m.e = -FxMul(vbMinX, sx);
		styleStack[0].m.f = -FxMul(vbMinY, sy);
	}
	tagDepth = 0; styleTop = 0;

	while (sc.pos < sc.len && shapesLeft > 0 && ctx.testBudget > 0) {
		SvgScanSkipMisc(&sc);
		if (sc.pos >= sc.len) break;
		if (sc.buf[sc.pos] != '<') { sc.pos++; continue; }

		if (sc.pos + 1 < sc.len && sc.buf[sc.pos + 1] == '/') {
			const char *name; int nlen;
			long p = SvgReadTagName(sc.buf, sc.len, sc.pos + 2, &name, &nlen);
			while (p < sc.len && sc.buf[p] != '>') p++;
			sc.pos = p + 1;
			if (skipUntilDepth >= 0) {
				if (tagDepth == skipUntilDepth) skipUntilDepth = -1;
			} else if (styleTop > 0 && tagDepth == styleTop) {
				styleTop--;
			}
			if (tagDepth > 0) tagDepth--;
			continue;
		}

		{
			const char *name; int nlen;
			long p = SvgReadTagName(sc.buf, sc.len, sc.pos + 1, &name, &nlen);
			const char *attrs = (const char *)sc.buf + p;
			const char *ae = (const char *)sc.buf + sc.len;
			const char *tagEnd = attrs;
			int selfClose;
			while (tagEnd < ae && *tagEnd != '>') tagEnd++;
			selfClose = (tagEnd > attrs && tagEnd[-1] == '/');
			sc.pos = (int)(tagEnd - (const char *)sc.buf) + 1;

			if (skipUntilDepth >= 0) {
				if (!selfClose) tagDepth++;
				continue;
			}
			if (nlen == 0) continue;

			if (SvgIsSkipName(name, nlen)) {
				if (!selfClose) { tagDepth++; skipUntilDepth = tagDepth; }
				continue;
			}

			if (SvgStrEqN(name, nlen, "g") || SvgStrEqN(name, nlen, "svg") || SvgStrEqN(name, nlen, "a")) {
				if (!selfClose && styleTop + 1 < SVG_MAX_DEPTH) {
					styleStack[styleTop + 1] = styleStack[styleTop];
					SvgApplyAttrs(&styleStack[styleTop + 1], attrs, (const char *)tagEnd - (selfClose ? 1 : 0));
					styleTop++;
					tagDepth++;
				} else if (!selfClose) {
					tagDepth++; /* depth budget exceeded: keep nesting in sync, no new style */
				}
				continue;
			}

			{
				SvgStyle local = styleStack[styleTop];
				const char *attrsEnd = (const char *)tagEnd - (selfClose ? 1 : 0);
				SvgApplyAttrs(&local, attrs, attrsEnd);

				if (SvgStrEqN(name, nlen, "path")) SvgHandlePath(&ctx, &local, attrs, attrsEnd);
				else if (SvgStrEqN(name, nlen, "rect")) SvgHandleRect(&ctx, &local, attrs, attrsEnd);
				else if (SvgStrEqN(name, nlen, "circle")) SvgHandleCircleOrEllipse(&ctx, &local, attrs, attrsEnd, 0);
				else if (SvgStrEqN(name, nlen, "ellipse")) SvgHandleCircleOrEllipse(&ctx, &local, attrs, attrsEnd, 1);
				else if (SvgStrEqN(name, nlen, "line")) SvgHandleLine(&ctx, &local, attrs, attrsEnd);
				else if (SvgStrEqN(name, nlen, "polygon"))
					SvgHandlePoly(&ctx, &local, attrs, attrsEnd, 1);
				else if (SvgStrEqN(name, nlen, "polyline"))
					SvgHandlePoly(&ctx, &local, attrs, attrsEnd, 0);
				else if (!selfClose && styleTop + 1 < SVG_MAX_DEPTH) {
					/* Unknown-but-not-skip-listed container (e.g. <switch>):
					 * keep walking its children with inherited style. */
					styleStack[styleTop + 1] = local;
					styleTop++;
					tagDepth++;
					continue;
				}
				shapesLeft--;
				if (!selfClose) tagDepth++;
			}
		}
	}

	for (i = 0; i < outW * outH; i++) {
		if (touchedBuf[i]) {
			unsigned char r = rgbBuf[i * 3], g = rgbBuf[i * 3 + 1], b = rgbBuf[i * 3 + 2];
			rgbOut[i * 3] = r; rgbOut[i * 3 + 1] = g; rgbOut[i * 3 + 2] = b;
			greyOut[i] = (unsigned char)((77UL * r + 150UL * g + 29UL * b + 128UL) >> 8);
		} else {
			rgbOut[i * 3] = rgbOut[i * 3 + 1] = rgbOut[i * 3 + 2] = 0x80;
			greyOut[i] = 0x80;
		}
	}
	return 0;
}
