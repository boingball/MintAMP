/* Small SVG-subset decoder for MintAMP radio favicon artwork.
 *
 * Not a general SVG renderer: supports path/rect/circle/ellipse/
 * polygon/polyline/line with solid fills and simple strokes,
 * transform (translate/scale/rotate/skewX/skewY/matrix), opacity/
 * fill-opacity/stroke-opacity, and viewBox/width/height sizing.
 * Elliptical arc ("A") path commands degrade to a straight line to
 * their endpoint rather than a true arc.  Gradients, patterns,
 * filters, clip/mask, <use>, <image>, <text> and CSS <style> rules
 * are not resolved -- the affected paint or subtree is skipped
 * rather than failing the whole decode, unless the document has no
 * usable <svg> root/size, in which case decoding fails outright.
 *
 * All arithmetic is fixed-point (no floating point, no libm), and all
 * scratch state is static/bounded, matching this project's other
 * artwork decoders (lodepng.c, picojpeg.c).
 */
#ifndef SVGDEC_H
#define SVGDEC_H

#ifdef __cplusplus
extern "C" {
#endif

/* True if `data` looks like it starts an SVG document (allowing for a
 * UTF-8 BOM, XML prolog, comments and doctype ahead of the <svg> root). */
int SvgLooksLikeSvg(const unsigned char *data, int bytes);

/* Decodes an in-memory SVG document into the same downsampled grey/RGB
 * thumbnail buffers DecodePngToGrey()/DecodeJpegToGrey() produce.
 * outW/outH must both be <= 64.  Returns 0 on success, -1 on failure
 * (leaving greyOut/rgbOut untouched). */
int SvgDecodeToGrey(const unsigned char *svgData, unsigned long svgBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH);

#ifdef __cplusplus
}
#endif

#endif /* SVGDEC_H */
