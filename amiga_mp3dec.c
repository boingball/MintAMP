/* Minimal AmigaOS/m68k-friendly command-line MP3 decoder.
 *
 * Builds the public decoder (mp3dec.c, mp3tabs.c) plus the portable real C files and writes raw
 * PCM or Amiga IFF-8SVX audio.  The code intentionally uses plain C library
 * calls only so it can be compiled by m68k-amigaos-gcc for 68020 systems.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mp3dec.h"
#include "assembly.h"

#define READBUF_SIZE (1024 * 16)
#define OUTBUF_SAMPS (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

#define OUT_PCM16 0
#define OUT_S8    1
#define OUT_8SVX  2

#define SVX_COMP_NONE 0
#define SVX_COMP_FIBDELTA 1

typedef struct DecodeOptions {
	const char *inName;
	const char *outName;
	int outFormat;
	int mono;
	int compression;
	int bench;
	int decodeOnly;
	int noOutput;
	int selftestMulshift;
	int checksum;
	int outputRate;
	int help;
	int debugArgv;
} DecodeOptions;

typedef struct DecodeStats {
	unsigned long decodedFrames;
	unsigned long outputSamples;
	unsigned long pcmChecksum;
	int sampleRate;
	int channels;
	int bitrate;
} DecodeStats;

typedef struct TimingStats {
	clock_t frameDecode;
	clock_t pcmConvert;
	clock_t svxWrite;
	clock_t fibCompress;
	clock_t fileWrite;
} TimingStats;

typedef struct RateState {
	int inRate;
	int outRate;
	int channels;
	unsigned long phase;
} RateState;

typedef struct SvxWriter {
	FILE *fp;
	long formSizePos;
	long oneShotPos;
	long bodySizePos;
	unsigned long sourceSamples;
	unsigned long bodyBytes;
	int compression;
	int noOutput;
	int fibStarted;
	signed char fibPrev;
	int fibHaveHighNibble;
	unsigned char fibPending;
} SvxWriter;

#ifdef AMIGA_M68K
typedef struct NormalizedArgs {
	int argc;
	char **argv;
	char *storage;
} NormalizedArgs;

static const char *AmigaBaseName(const char *path)
{
	const char *base;

	base = path;
	while (*path) {
		if (*path == '/' || *path == ':' || *path == '\\')
			base = path + 1;
		path++;
	}

	return base;
}

static int AmigaAsciiLower(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';

	return c;
}

static int AmigaArgIsProgramName(const char *arg)
{
	const char *base;
	const char *prefix;

	if (!arg || !arg[0])
		return 0;

	base = AmigaBaseName(arg);
	prefix = "amiga_mp3dec";
	while (*prefix) {
		if (AmigaAsciiLower((unsigned char)*base) != *prefix)
			return 0;
		base++;
		prefix++;
	}

	return *base == '\0' || *base == '.';
}


static int AmigaIsArgSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int AmigaTailHasSplittableSpace(const char *arg)
{
	if (!arg)
		return 0;
	while (*arg) {
		if (AmigaIsArgSpace(*arg))
			return 1;
		arg++;
	}
	return 0;
}

static int AmigaCountTailTokens(const char *src)
{
	int tokens = 0;
	char quote = '\0';
	int inToken = 0;

	while (*src) {
		if (quote) {
			if (*src == quote)
				quote = '\0';
			inToken = 1;
		} else if (*src == '"' || *src == '\'') {
			quote = *src;
			if (!inToken) {
				tokens++;
				inToken = 1;
			}
		} else if (AmigaIsArgSpace(*src)) {
			inToken = 0;
		} else if (!inToken) {
			tokens++;
			inToken = 1;
		}
		src++;
	}

	return tokens;
}

static void AmigaSplitTail(char *src, char **argv, int *argc)
{
	char *read;
	char *write;
	char *token;
	char quote;

	read = src;
	while (*read) {
		while (AmigaIsArgSpace(*read))
			read++;
		if (!*read)
			break;

		token = read;
		write = read;
		quote = '\0';
		while (*read) {
			if (quote) {
				if (*read == quote) {
					quote = '\0';
					read++;
					continue;
				}
			} else if (*read == '"' || *read == '\'') {
				quote = *read++;
				continue;
			} else if (AmigaIsArgSpace(*read)) {
				break;
			}
			*write++ = *read++;
		}
		if (*read)
			read++;
		*write = '\0';
		argv[(*argc)++] = token;
		while (AmigaIsArgSpace(*read))
			read++;
	}
}

static int AmigaArgStringNeedsSplit(int argc, char **argv)
{
	if (!argv)
		return 0;

	if (argc == 1 && argv[0])
		return !AmigaArgIsProgramName(argv[0]) ||
			AmigaTailHasSplittableSpace(argv[0]);

	if (argc == 2 && argv[0] && argv[1] && AmigaArgIsProgramName(argv[0]))
		return AmigaTailHasSplittableSpace(argv[1]);

	return 0;
}


static int AmigaNormalizeArgs(int argc, char **argv, NormalizedArgs *normalized)
{
	const char *src;
	int tokens;
	int outArgc;

	normalized->argc = argc;
	normalized->argv = argv;
	normalized->storage = NULL;

	if (!AmigaArgStringNeedsSplit(argc, argv))
		return 0;

	if (argc == 2 && argv[0] && AmigaArgIsProgramName(argv[0]))
		src = argv[1];
	else
		src = argv[0];

	tokens = AmigaCountTailTokens(src);
	normalized->argv = (char **)malloc((tokens + 2) * sizeof(char *));
	if (!normalized->argv)
		return -1;

	normalized->storage = (char *)malloc(strlen(src) + 1);
	if (!normalized->storage) {
		free(normalized->argv);
		normalized->argv = argv;
		return -1;
	}

	strcpy(normalized->storage, src);
	normalized->argv[0] = (char *)"amiga_mp3dec";
	outArgc = 1;
	AmigaSplitTail(normalized->storage, normalized->argv, &outArgc);
	if (outArgc > 1 && AmigaArgIsProgramName(normalized->argv[1])) {
		int i;
		for (i = 1; i < outArgc; i++)
			normalized->argv[i] = normalized->argv[i + 1];
		outArgc--;
	}
	normalized->argv[outArgc] = NULL;
	normalized->argc = outArgc;

	return 0;
}


static void AmigaFreeNormalizedArgs(NormalizedArgs *normalized)
{
	if (normalized->storage) {
		free(normalized->storage);
		free(normalized->argv);
	}
	normalized->storage = NULL;
	normalized->argv = NULL;
	normalized->argc = 0;
}
#else
typedef struct NormalizedArgs {
	int argc;
	char **argv;
} NormalizedArgs;

static int AmigaNormalizeArgs(int argc, char **argv, NormalizedArgs *normalized)
{
	normalized->argc = argc;
	normalized->argv = argv;
	return 0;
}

static void AmigaFreeNormalizedArgs(NormalizedArgs *normalized)
{
	(void)normalized;
}
#endif

static void PrintArgvDebug(int argc, char **argv)
{
	int i;

	printf("argc: %d\n", argc);
	for (i = 0; i < argc; i++)
		printf("argv[%d]: %s\n", i, argv[i] ? argv[i] : "(null)");
}

static void PrintUsage(const char *prog)
{
	printf("usage: %s [options] infile.mp3 outfile\n", prog);
	printf("options:\n");
	printf("  --mono       mix stereo to mono before writing\n");
	printf("  --s8         write raw signed 8-bit PCM instead of signed 16-bit PCM\n");
	printf("  --8svx       write Amiga IFF-8SVX signed 8-bit output (implies mono)\n");
	printf("  --fibdelta   use 8SVX Fibonacci Delta compression (implies --8svx)\n");
	printf("  --bench      print elapsed decode/write time and realtime ratio\n");
	printf("  --decode-only decode frames only; skip PCM conversion and output\n");
	printf("  --no-output  run conversion/compression paths but discard output bytes\n");
	printf("  --rate HZ     output/downsample rate: 22050, 11025, or 8287 Hz\n");
	printf("  --selftest-mulshift compare C and optional asm MULSHIFT32 helpers\n");
	printf("  --checksum  print a 32-bit checksum of decoded PCM samples\n");
	printf("  --debug-argv print argc/argv after Amiga argument normalization\n");
	printf("  --show-argv  alias for --debug-argv\n");
	printf("\n");
	printf("default output is raw signed 16-bit big-endian PCM.\n");
	printf("outfile ending in :, /, or \\ is treated as a directory/volume.\n");
}

static int ParseOptions(int argc, char **argv, DecodeOptions *opt)
{
	int i;

	memset(opt, 0, sizeof(*opt));
	opt->outFormat = OUT_PCM16;
	opt->compression = SVX_COMP_NONE;
	opt->outputRate = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mono")) {
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--s8")) {
			opt->outFormat = OUT_S8;
		} else if (!strcmp(argv[i], "--8svx")) {
			opt->outFormat = OUT_8SVX;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--fibdelta")) {
			opt->outFormat = OUT_8SVX;
			opt->mono = 1;
			opt->compression = SVX_COMP_FIBDELTA;
		} else if (!strcmp(argv[i], "--bench")) {
			opt->bench = 1;
		} else if (!strcmp(argv[i], "--decode-only")) {
			opt->decodeOnly = 1;
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--no-output")) {
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--selftest-mulshift")) {
			opt->selftestMulshift = 1;
		} else if (!strcmp(argv[i], "--checksum")) {
			opt->checksum = 1;
		} else if (!strcmp(argv[i], "--rate")) {
			if (++i >= argc)
				return -1;
			opt->outputRate = atoi(argv[i]);
			if (opt->outputRate != 22050 && opt->outputRate != 11025 &&
				opt->outputRate != 8287)
				return -1;
		} else if (!strcmp(argv[i], "--debug-argv") ||
			!strcmp(argv[i], "--show-argv")) {
			opt->debugArgv = 1;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			opt->help = 1;
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return -1;
		} else if (!opt->inName) {
			opt->inName = argv[i];
		} else if (!opt->outName) {
			opt->outName = argv[i];
		} else {
			return -1;
		}
	}

	if (opt->help)
		return 0;

	if (opt->selftestMulshift)
		return 0;

	if (!opt->inName || (!opt->outName && !opt->noOutput))
		return -1;

	return 0;
}


static const char *PathBaseName(const char *path)
{
	const char *base;

	base = path;
	while (path && *path) {
		if (*path == '/' || *path == ':' || *path == '\\')
			base = path + 1;
		path++;
	}

	return base ? base : "";
}

static int OutputNameIsDirectory(const char *path)
{
	size_t len;

	if (!path || !path[0])
		return 0;
	len = strlen(path);
	return path[len - 1] == ':' || path[len - 1] == '/' || path[len - 1] == '\\';
}

static const char *DefaultOutputExtension(const DecodeOptions *opt)
{
	if (opt->outFormat == OUT_8SVX)
		return ".8svx";
	if (opt->outFormat == OUT_S8)
		return ".s8";
	return ".pcm";
}

static char *BuildDirectoryOutputName(const char *dir, const char *input,
	const DecodeOptions *opt)
{
	const char *base;
	const char *dot;
	const char *ext;
	size_t dirLen;
	size_t stemLen;
	size_t extLen;
	char *name;

	base = PathBaseName(input);
	if (!base[0])
		base = "output";
	dot = strrchr(base, '.');
	stemLen = dot && dot != base ? (size_t)(dot - base) : strlen(base);
	ext = DefaultOutputExtension(opt);
	dirLen = strlen(dir);
	extLen = strlen(ext);

	name = (char *)malloc(dirLen + stemLen + extLen + 1);
	if (!name)
		return NULL;
	memcpy(name, dir, dirLen);
	memcpy(name + dirLen, base, stemLen);
	memcpy(name + dirLen + stemLen, ext, extLen + 1);
	return name;
}

static int FillReadBuffer(unsigned char *readBuf, unsigned char *readPtr, int bufSize,
	int bytesLeft, FILE *infile)
{
	int nRead;

	memmove(readBuf, readPtr, bytesLeft);
	nRead = (int)fread(readBuf + bytesLeft, 1, bufSize - bytesLeft, infile);
	if (nRead < bufSize - bytesLeft) {
		memset(readBuf + bytesLeft + nRead, 0,
			bufSize - bytesLeft - nRead);
	}

	return nRead;
}

static TimingStats *gTiming;

static double ClocksToSeconds(clock_t c)
{
	if (CLOCKS_PER_SEC <= 0)
		return 0.0;
	return (double)c / (double)CLOCKS_PER_SEC;
}

static int TimedFputc(int c, FILE *fp)
{
	clock_t t0;
	int r;

	if (!fp)
		return c;
	t0 = clock();
	r = fputc(c, fp);
	if (gTiming)
		gTiming->fileWrite += clock() - t0;
	return r;
}

static size_t TimedFwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
	clock_t t0;
	size_t r;

	if (!fp)
		return nmemb;
	t0 = clock();
	r = fwrite(ptr, size, nmemb, fp);
	if (gTiming)
		gTiming->fileWrite += clock() - t0;
	return r;
}

static void WriteU16BE(FILE *fp, unsigned int v)
{
	TimedFputc((int)((v >> 8) & 0xff), fp);
	TimedFputc((int)(v & 0xff), fp);
}

static void WriteU32BE(FILE *fp, unsigned long v)
{
	TimedFputc((int)((v >> 24) & 0xff), fp);
	TimedFputc((int)((v >> 16) & 0xff), fp);
	TimedFputc((int)((v >> 8) & 0xff), fp);
	TimedFputc((int)(v & 0xff), fp);
}

static void PatchU32BE(FILE *fp, long pos, unsigned long v)
{
	long cur;

	cur = ftell(fp);
	fseek(fp, pos, SEEK_SET);
	WriteU32BE(fp, v);
	fseek(fp, cur, SEEK_SET);
}

static signed char Sample16ToS8(short s)
{
	return (signed char)(s >> 8);
}

static int MixFrame(const short *in, short *out, int inSamps, int channels, int mono)
{
	int i;
	int n;

	if (!mono || channels == 1) {
		memmove(out, in, inSamps * sizeof(short));
		return inSamps;
	}

	n = inSamps / 2;
	for (i = 0; i < n; i++) {
		out[i] = (short)(((int)in[2 * i] + (int)in[2 * i + 1]) / 2);
	}

	return n;
}

static int WriteRawSamples(FILE *fp, const short *pcm, int nSamps, int format)
{
	int i;

	if (format == OUT_S8) {
		for (i = 0; i < nSamps; i++)
			TimedFputc((int)(unsigned char)Sample16ToS8(pcm[i]), fp);
	} else {
		for (i = 0; i < nSamps; i++)
			WriteU16BE(fp, (unsigned int)(unsigned short)pcm[i]);
	}

	return (fp && ferror(fp)) ? -1 : 0;
}

static int SvxBegin(SvxWriter *svx, FILE *fp, int sampleRate, int compression)
{
	memset(svx, 0, sizeof(*svx));
	svx->fp = fp;
	svx->compression = compression;

	TimedFwrite("FORM", 1, 4, fp);
	svx->formSizePos = ftell(fp);
	WriteU32BE(fp, 0);
	TimedFwrite("8SVX", 1, 4, fp);

	TimedFwrite("VHDR", 1, 4, fp);
	WriteU32BE(fp, 20);
	svx->oneShotPos = ftell(fp);
	WriteU32BE(fp, 0);              /* oneShotHiSamples */
	WriteU32BE(fp, 0);              /* repeatHiSamples */
	WriteU32BE(fp, 0);              /* samplesPerHiCycle */
	WriteU16BE(fp, (unsigned int)sampleRate);
	TimedFputc(1, fp);                   /* ctOctave */
	TimedFputc(compression, fp);         /* sCompression */
	WriteU32BE(fp, 0x00010000UL);   /* volume */

	TimedFwrite("BODY", 1, 4, fp);
	svx->bodySizePos = ftell(fp);
	WriteU32BE(fp, 0);

	return ferror(fp) ? -1 : 0;
}

static void SvxWriteByte(SvxWriter *svx, unsigned char b)
{
	TimedFputc((int)b, svx->noOutput ? NULL : svx->fp);
	svx->bodyBytes++;
}

static int FibDeltaNibble(signed char prev, signed char sample)
{
	static const int deltaTable[16] = {
		-34, -21, -13, -8, -5, -3, -2, -1,
		0, 1, 2, 3, 5, 8, 13, 21
	};
	int best;
	int bestErr;
	int i;

	best = 0;
	bestErr = 32767;
	for (i = 0; i < 16; i++) {
		int predicted = (int)prev + deltaTable[i];
		int err;
		if (predicted < -128)
			predicted = -128;
		else if (predicted > 127)
			predicted = 127;
		err = predicted - (int)sample;
		if (err < 0)
			err = -err;
		if (err < bestErr) {
			bestErr = err;
			best = i;
		}
	}

	return best;
}

static signed char FibDeltaApply(signed char prev, int nibble)
{
	static const int deltaTable[16] = {
		-34, -21, -13, -8, -5, -3, -2, -1,
		0, 1, 2, 3, 5, 8, 13, 21
	};
	int v;

	v = (int)prev + deltaTable[nibble & 15];
	if (v < -128)
		v = -128;
	else if (v > 127)
		v = 127;
	return (signed char)v;
}

static void SvxStartFibDelta(SvxWriter *svx, signed char predictor)
{
	/*
	 * 8SVX Fibonacci Delta (D1) BODY data starts with two bytes before
	 * the packed nibble stream.  The D1 unpacker seeds its predictor from
	 * source[1], but it does not copy that byte to the output; every output
	 * sample must still be represented by a following delta nibble.
	 */
	SvxWriteByte(svx, 0);
	SvxWriteByte(svx, (unsigned char)predictor);
	svx->fibPrev = predictor;
	svx->fibStarted = 1;
}

static void SvxWriteFibSample(SvxWriter *svx, signed char sample)
{
	clock_t t0;
	int nibble;

	if (!svx->fibStarted)
		SvxStartFibDelta(svx, sample);

	t0 = clock();
	nibble = FibDeltaNibble(svx->fibPrev, sample);
	svx->fibPrev = FibDeltaApply(svx->fibPrev, nibble);
	if (gTiming)
		gTiming->fibCompress += clock() - t0;
	if (!svx->fibHaveHighNibble) {
		svx->fibPending = (unsigned char)((nibble & 15) << 4);
		svx->fibHaveHighNibble = 1;
	} else {
		SvxWriteByte(svx, (unsigned char)(svx->fibPending | (nibble & 15)));
		svx->fibHaveHighNibble = 0;
	}
}

static int SvxWriteSamples(SvxWriter *svx, const short *pcm, int nSamps)
{
	int i;

	for (i = 0; i < nSamps; i++) {
		signed char s8 = Sample16ToS8(pcm[i]);
		if (svx->compression == SVX_COMP_FIBDELTA)
			SvxWriteFibSample(svx, s8);
		else
			SvxWriteByte(svx, (unsigned char)s8);
		svx->sourceSamples++;
	}

	return (!svx->noOutput && ferror(svx->fp)) ? -1 : 0;
}

static int SvxEnd(SvxWriter *svx)
{
	unsigned long formSize;
	long endPos;

	if (svx->compression == SVX_COMP_FIBDELTA) {
		if (!svx->fibStarted)
			SvxStartFibDelta(svx, 0);
		if (svx->fibHaveHighNibble) {
			SvxWriteByte(svx, svx->fibPending);
			svx->fibHaveHighNibble = 0;
		}
	}

	if (svx->bodyBytes & 1)
		TimedFputc(0, svx->noOutput ? NULL : svx->fp);

	if (svx->noOutput)
		return 0;

	endPos = ftell(svx->fp);
	formSize = (unsigned long)(endPos - 8);
	PatchU32BE(svx->fp, svx->oneShotPos, svx->sourceSamples);
	PatchU32BE(svx->fp, svx->bodySizePos, svx->bodyBytes);
	PatchU32BE(svx->fp, svx->formSizePos, formSize);

	return ferror(svx->fp) ? -1 : 0;
}

static unsigned long UpdatePcmChecksum(unsigned long checksum, const short *pcm, int nSamps)
{
	int i;

	for (i = 0; i < nSamps; i++) {
		unsigned int sample = (unsigned int)(unsigned short)pcm[i];
		checksum ^= (unsigned long)(sample & 0xffU);
		checksum = (checksum * 16777619UL) & 0xffffffffUL;
		checksum ^= (unsigned long)((sample >> 8) & 0xffU);
		checksum = (checksum * 16777619UL) & 0xffffffffUL;
	}

	return checksum;
}

static void UpdateFirstFrameStats(DecodeStats *stats, const MP3FrameInfo *info)
{
	if (!stats->sampleRate && info->samprate)
		stats->sampleRate = info->samprate;
	if (!stats->channels && info->nChans)
		stats->channels = info->nChans;
	if (!stats->bitrate && info->bitrate)
		stats->bitrate = info->bitrate;
}

static int DownsampleFrame(RateState *rate, const short *in, short *out, int nSamps,
	int inRate, int outRate, int channels)
{
	unsigned long inFrames;
	unsigned long produced;
	unsigned long consume;

	if (outRate <= 0 || outRate >= inRate || channels <= 0) {
		if (out != in)
			memmove(out, in, nSamps * sizeof(short));
		return nSamps;
	}

	if (rate->inRate != inRate || rate->outRate != outRate ||
		rate->channels != channels) {
		rate->inRate = inRate;
		rate->outRate = outRate;
		rate->channels = channels;
		rate->phase = 0;
	}

	inFrames = (unsigned long)(nSamps / channels);
	produced = 0;
	while (rate->phase / (unsigned long)outRate < inFrames) {
		unsigned long srcFrame = rate->phase / (unsigned long)outRate;
		int ch;
		for (ch = 0; ch < channels; ch++)
			out[produced * (unsigned long)channels + (unsigned long)ch] =
				in[srcFrame * (unsigned long)channels + (unsigned long)ch];
		produced++;
		rate->phase += (unsigned long)inRate;
	}
	consume = inFrames * (unsigned long)outRate;
	if (rate->phase >= consume)
		rate->phase -= consume;
	else
		rate->phase = 0;

	return (int)(produced * (unsigned long)channels);
}

static void InitNoOutputSvx(SvxWriter *svx, int compression)
{
	memset(svx, 0, sizeof(*svx));
	svx->compression = compression;
	svx->noOutput = 1;
}

static unsigned long NextRand32(unsigned long *state)
{
	*state = (*state * 1664525UL) + 1013904223UL;
	return *state;
}

static int TestMulshiftPair(int x, int y, unsigned long index)
{
	int c = MULSHIFT32_C_REFERENCE(x, y);
#if MULSHIFT32_HAS_AMIGA_M68K_ASM
	int a = MULSHIFT32_AMIGA_M68K_ASM(x, y);
#else
	int a = c;
#endif
	if (a != c) {
		printf("MULSHIFT32 mismatch %lu: x=%ld y=%ld C=%ld asm=%ld\n",
			index, (long)x, (long)y, (long)c, (long)a);
		return -1;
	}
	return 0;
}

static int SelftestMulshift(void)
{
	static const int edges[] = {
		0, 1, -1, 2, -2, 0x7fffffffL, (int)0x80000000UL,
		0x40000000L, (int)0xc0000000UL, 0x12345678L, (int)0x87654321UL
	};
	unsigned long i;
	unsigned long failures = 0;
	unsigned long tested = 0;
	unsigned long seed = 0x1234abcdUL;

	for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		unsigned long j;
		for (j = 0; j < sizeof(edges) / sizeof(edges[0]); j++) {
			if (TestMulshiftPair(edges[i], edges[j], tested) != 0)
				failures++;
			tested++;
		}
	}

	for (i = 0; i < 100000UL; i++) {
		int x = (int)NextRand32(&seed);
		int y = (int)NextRand32(&seed);
		if (TestMulshiftPair(x, y, tested) != 0)
			failures++;
		tested++;
	}

	printf("MULSHIFT32 asm available: %s\n",
#if MULSHIFT32_HAS_AMIGA_M68K_ASM
		"yes"
#else
		"no (C reference path only in this build)"
#endif
	);
	printf("MULSHIFT32 selftest cases: %lu\n", tested);
	printf("MULSHIFT32 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	DecodeOptions opt;
	DecodeStats stats;
	unsigned char readBuf[READBUF_SIZE];
	unsigned char *readPtr;
	short decodeBuf[OUTBUF_SAMPS];
	short writeBuf[OUTBUF_SAMPS];
	short rateBuf[OUTBUF_SAMPS];
	FILE *infile;
	FILE *outfile;
	HMP3Decoder decoder;
	MP3FrameInfo info;
	SvxWriter svx;
	TimingStats timing;
	RateState rateState;
	int bytesLeft;
	int eofReached;
	int outOfData;
	int svxOpen;
	clock_t startClock;
	clock_t endClock;
	NormalizedArgs normalized;
	int debugArgv;
	int effectiveRate;
	char *resolvedOutName;

	resolvedOutName = NULL;

	if (AmigaNormalizeArgs(argc, argv, &normalized) != 0) {
		fprintf(stderr, "cannot normalize command arguments\n");
		return 1;
	}
	argc = normalized.argc;
	argv = normalized.argv;

	debugArgv = 0;
	{
		int i;
		for (i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--debug-argv") ||
				!strcmp(argv[i], "--show-argv")) {
				debugArgv = 1;
				break;
			}
		}
	}
	if (debugArgv)
		PrintArgvDebug(argc, argv);

	if (ParseOptions(argc, argv, &opt) != 0) {
		PrintUsage(argv && argv[0] ? argv[0] : "amiga_mp3dec");
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (opt.help) {
		PrintUsage(argv && argv[0] ? argv[0] : "amiga_mp3dec");
		AmigaFreeNormalizedArgs(&normalized);
		return 0;
	}
	if (opt.selftestMulshift) {
		int selftestErr = SelftestMulshift();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}

	if (opt.outName && OutputNameIsDirectory(opt.outName)) {
		resolvedOutName = BuildDirectoryOutputName(opt.outName, opt.inName, &opt);
		if (!resolvedOutName) {
			fprintf(stderr, "cannot build output path\n");
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		opt.outName = resolvedOutName;
	}

	memset(&stats, 0, sizeof(stats));
	if (opt.checksum)
		stats.pcmChecksum = 2166136261UL;
	memset(&timing, 0, sizeof(timing));
	memset(&rateState, 0, sizeof(rateState));
	memset(&info, 0, sizeof(info));

	infile = fopen(opt.inName, "rb");
	if (!infile) {
		fprintf(stderr, "cannot open input: %s\n", opt.inName);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	outfile = NULL;
	if (!opt.noOutput) {
		outfile = fopen(opt.outName, opt.outFormat == OUT_8SVX ? "wb+" : "wb");
		if (!outfile) {
			fprintf(stderr, "cannot open output: %s\n", opt.outName);
			fclose(infile);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
	}

	decoder = MP3InitDecoder();
	if (!decoder) {
		fprintf(stderr, "MP3InitDecoder failed\n");
		fclose(infile);
		if (outfile)
			fclose(outfile);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	bytesLeft = 0;
	eofReached = 0;
	outOfData = 0;
	svxOpen = 0;
	readPtr = readBuf;
	gTiming = &timing;
	MP3ResetDecodeCoreProfile();
	effectiveRate = 0;
	startClock = clock();

	while (!outOfData) {
		int nRead;
		int offset;
		int err;

		if (bytesLeft < 2 * MAINBUF_SIZE && !eofReached) {
			nRead = FillReadBuffer(readBuf, readPtr, READBUF_SIZE,
				bytesLeft, infile);
			bytesLeft += nRead;
			readPtr = readBuf;
			if (nRead == 0)
				eofReached = 1;
		}

		offset = MP3FindSyncWord(readPtr, bytesLeft);
		if (offset < 0)
			break;

		readPtr += offset;
		bytesLeft -= offset;

		{
			clock_t t0 = clock();
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
			timing.frameDecode += clock() - t0;
		}
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW) {
				outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stats.decodedFrames);
				outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(decoder, &info);
		UpdateFirstFrameStats(&stats, &info);
		if (opt.checksum)
			stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, decodeBuf,
				info.outputSamps);
		if (!effectiveRate)
			effectiveRate = (opt.outputRate && info.samprate > opt.outputRate) ? opt.outputRate : info.samprate;

		if (!opt.decodeOnly && opt.outFormat == OUT_8SVX && !svxOpen) {
			if (!info.samprate) {
				fprintf(stderr, "cannot write 8SVX before sample rate is known\n");
				outOfData = 1;
				break;
			}
			if (opt.noOutput) {
				InitNoOutputSvx(&svx, opt.compression);
			} else {
				clock_t t0 = clock();
				int beginErr = SvxBegin(&svx, outfile, effectiveRate, opt.compression);
				timing.svxWrite += clock() - t0;
				if (beginErr != 0) {
					fprintf(stderr, "cannot write 8SVX header\n");
					outOfData = 1;
					break;
				}
			}
			svxOpen = 1;
		}

		if (opt.decodeOnly) {
			stats.outputSamples += (unsigned long)info.outputSamps;
		} else {
			int outSamps;
			int outChannels;
			int writeErr;
			clock_t t0;

			t0 = clock();
			outSamps = MixFrame(decodeBuf, writeBuf, info.outputSamps,
				info.nChans, opt.mono);
			outChannels = (opt.mono || info.nChans <= 1) ? 1 : info.nChans;
			if (opt.outputRate && info.samprate > opt.outputRate) {
				outSamps = DownsampleFrame(&rateState, writeBuf, rateBuf, outSamps,
					info.samprate, opt.outputRate, outChannels);
				memmove(writeBuf, rateBuf, outSamps * sizeof(short));
			}
			timing.pcmConvert += clock() - t0;

			if (opt.outFormat == OUT_8SVX) {
				t0 = clock();
				writeErr = SvxWriteSamples(&svx, writeBuf, outSamps);
				timing.svxWrite += clock() - t0;
			} else {
				writeErr = WriteRawSamples(opt.noOutput ? NULL : outfile, writeBuf,
					outSamps, opt.outFormat);
			}

			if (writeErr != 0) {
				fprintf(stderr, "output write error\n");
				outOfData = 1;
				break;
			}
			stats.outputSamples += (unsigned long)outSamps;
		}

		stats.decodedFrames++;
	}

	if (svxOpen) {
		clock_t t0 = clock();
		if (SvxEnd(&svx) != 0)
			fprintf(stderr, "error finalizing 8SVX file\n");
		timing.svxWrite += clock() - t0;
	}

	endClock = clock();

	printf("sample rate: %d Hz\n", stats.sampleRate);
	if (opt.outputRate)
		printf("output rate: %d Hz\n", effectiveRate ? effectiveRate : opt.outputRate);
	printf("channels: %d%s\n", stats.channels, opt.mono ? " (mono output)" : "");
	printf("bitrate: %d bps\n", stats.bitrate);
	printf("decoded frames: %lu\n", stats.decodedFrames);
	printf("output samples: %lu\n", stats.outputSamples);
	if (opt.checksum)
		printf("decoded PCM checksum: %08lx\n", stats.pcmChecksum);

	if (opt.bench) {
		double elapsed = 0.0;
		double audioSeconds = 0.0;
		if (CLOCKS_PER_SEC > 0)
			elapsed = (double)(endClock - startClock) / (double)CLOCKS_PER_SEC;
		if ((opt.decodeOnly ? stats.sampleRate : (effectiveRate ? effectiveRate : stats.sampleRate)) > 0) {
			int outputChannels = opt.mono ? 1 : stats.channels;
			if (outputChannels <= 0)
				outputChannels = 1;
			audioSeconds = (double)stats.outputSamples /
				((double)(opt.decodeOnly ? stats.sampleRate :
				(effectiveRate ? effectiveRate : stats.sampleRate)) *
				(double)outputChannels);
		}
		printf("elapsed seconds: %.3f\n", elapsed);
		if (elapsed > 0.0 && audioSeconds > 0.0)
			printf("decode speed: %.2fx realtime\n", audioSeconds / elapsed);
		{
			MP3DecodeCoreProfile coreProfile;

			MP3GetDecodeCoreProfile(&coreProfile);
			printf("decode-core profiling: %s\n",
				MP3DecodeCoreProfileIsEnabled() ? "enabled" : "disabled");
			if (MP3DecodeCoreProfileIsEnabled()) {
				printf("timing core bitstream/frame parsing: %.3f s\n",
					ClocksToSeconds(coreProfile.bitstreamFrameParsing));
				printf("timing core huffman: %.3f s\n",
					ClocksToSeconds(coreProfile.huffman));
				printf("timing core dequant: %.3f s\n",
					ClocksToSeconds(coreProfile.dequant));
				printf("timing core stereo/post: %.3f s\n",
					ClocksToSeconds(coreProfile.stereoPost));
				printf("timing core imdct: %.3f s\n",
					ClocksToSeconds(coreProfile.imdct));
				printf("timing core subband/dct32: %.3f s\n",
					ClocksToSeconds(coreProfile.subbandDct32));
				printf("timing core polyphase: %.3f s\n",
					ClocksToSeconds(coreProfile.polyphase));
			}
		}
		printf("timing frame decode: %.3f s\n", ClocksToSeconds(timing.frameDecode));
		printf("timing PCM conversion: %.3f s\n", ClocksToSeconds(timing.pcmConvert));
		printf("timing 8SVX write: %.3f s\n", ClocksToSeconds(timing.svxWrite));
		printf("timing Fibonacci compression: %.3f s\n", ClocksToSeconds(timing.fibCompress));
		printf("timing file writing: %.3f s\n", ClocksToSeconds(timing.fileWrite));
	}

	MP3FreeDecoder(decoder);
	fclose(infile);
	if (outfile)
		fclose(outfile);
	gTiming = NULL;
	free(resolvedOutName);
	AmigaFreeNormalizedArgs(&normalized);

	return 0;
}
