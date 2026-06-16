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
#include <stdarg.h>
#ifndef AMIGA_M68K
#include <signal.h>
#endif

#if defined(AMIGA_M68K) && (defined(__amigaos__) || defined(__AMIGA__) || defined(__MORPHOS__))
#define HAVE_AMIGA_AUDIO_DEVICE 1
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/audio.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#ifndef AUDIONAME
#define AUDIONAME "audio.device"
#endif
#endif

#include "mp3dec.h"
#include "assembly.h"
#include "statname.h"

volatile int gMiniAmp3EmbeddedPlayback;

static int MiniAmp3ConsoleSuppressed(void)
{
	return gMiniAmp3EmbeddedPlayback != 0;
}

static int MiniAmp3Printf(const char *fmt, ...)
{
	int r;
	va_list ap;
	if (MiniAmp3ConsoleSuppressed())
		return 0;
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}

static int MiniAmp3Fprintf(FILE *stream, const char *fmt, ...)
{
	int r;
	va_list ap;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return 0;
	va_start(ap, fmt);
	r = vfprintf(stream, fmt, ap);
	va_end(ap);
	return r;
}

static int MiniAmp3Fputs(const char *s, FILE *stream)
{
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return 0;
	return fputs(s, stream);
}

static int MiniAmp3Puts(const char *s)
{
	if (MiniAmp3ConsoleSuppressed())
		return 0;
	return puts(s);
}

static int MiniAmp3Putchar(int c)
{
	if (MiniAmp3ConsoleSuppressed())
		return c;
	return putchar(c);
}

static int MiniAmp3Fflush(FILE *stream)
{
	if (MiniAmp3ConsoleSuppressed() && (!stream || stream == stdout || stream == stderr))
		return 0;
	return fflush(stream);
}

static size_t MiniAmp3Fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return nmemb;
	return fwrite(ptr, size, nmemb, stream);
}

#define printf MiniAmp3Printf
#define fprintf MiniAmp3Fprintf
#define fputs MiniAmp3Fputs
#define puts MiniAmp3Puts
#define putchar MiniAmp3Putchar
#define fflush MiniAmp3Fflush
#define fwrite MiniAmp3Fwrite

#if defined(AMIGA_M68K)
/* Tell AmigaOS to provide at least 250 KB of stack for this executable. */
static const char amigaStackCookie[] __attribute__((used)) = "$STACK:250000";
#endif

void STATNAME(FDCT32)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32_C_REFERENCE)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32Half)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32Half_TEST_ACTIVE)(int *x, int *d, int offset, int oddBlock, int gb);
int STATNAME(FDCT32Half_AMIGA_M68K_ASM_RUNTIME)(void);
void STATNAME(FDCT32Quarter)(int *x, int *d, int offset, int oddBlock, int gb, int phase, int stride);
int STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
void STATNAME(AntiAlias_C_REFERENCE)(int *x, int nBfly);
void STATNAME(AntiAlias_TEST_ACTIVE)(int *x, int nBfly);
int STATNAME(AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(IMDCT36_C_REFERENCE)(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int STATNAME(IMDCT36_TEST_ACTIVE)(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(IMDCTThinOutputSelftest)(void);
int STATNAME(IMDCTSubbandCapSelftest)(void);
void STATNAME(PolyphaseMonoFast_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
void STATNAME(PolyphaseMonoFast_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseMonoFastLowrateStride2_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseMonoFastLowrateStride4_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride4_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseStereoFastLowrateStride2_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_POLYPHASE)
extern void StereoFastPolyphaseStride4Half_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase) __asm__("StereoFastPolyphaseStride4Half_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase0_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase1_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase2_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase3_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase3_Amiga_m68k")
	__attribute__((weak));
extern volatile unsigned long StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[4]
	__asm__("StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts")
	__attribute__((weak));
#endif
int STATNAME(AmigaM68KPolyphaseMonoFast_IsActive)(void);
int STATNAME(AmigaM68KPolyphaseMonoFastStride2_IsActive)(void);
int STATNAME(DecodeHuffmanPairs_C_REFERENCE)(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset);
int STATNAME(DecodeHuffmanPairs_TEST_ACTIVE)(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset);
int STATNAME(DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
const char *STATNAME(DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE)(void);
int STATNAME(DequantBlock_C_REFERENCE)(int *inbuf, int *outbuf, int num, int scale);
int STATNAME(DequantBlock_TEST_ACTIVE)(int *inbuf, int *outbuf, int num, int scale);
int STATNAME(DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(BitstreamRefillSelftest)(void);
extern const int STATNAME(polyCoef)[264];
#define AMIGA_FDCT32 STATNAME(FDCT32)
#define AMIGA_FDCT32_C_REFERENCE STATNAME(FDCT32_C_REFERENCE)
#define AMIGA_FDCT32_HALF STATNAME(FDCT32Half)
#define AMIGA_FDCT32_HALF_TEST_ACTIVE STATNAME(FDCT32Half_TEST_ACTIVE)
#define AMIGA_FDCT32_HALF_HAS_ASM STATNAME(FDCT32Half_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_FDCT32_QUARTER STATNAME(FDCT32Quarter)
#define AMIGA_FDCT32_HAS_ASM STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_ANTIALIAS_C_REFERENCE STATNAME(AntiAlias_C_REFERENCE)
#define AMIGA_ANTIALIAS_TEST_ACTIVE STATNAME(AntiAlias_TEST_ACTIVE)
#define AMIGA_ANTIALIAS_HAS_ASM STATNAME(AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_IMDCT36_C_REFERENCE STATNAME(IMDCT36_C_REFERENCE)
#define AMIGA_IMDCT36_TEST_ACTIVE STATNAME(IMDCT36_TEST_ACTIVE)
#define AMIGA_IMDCT36_HAS_ASM STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_IMDCT_THIN_SELFTEST STATNAME(IMDCTThinOutputSelftest)
#define AMIGA_IMDCT_SUBBAND_CAP_SELFTEST STATNAME(IMDCTSubbandCapSelftest)
#define AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE STATNAME(PolyphaseMonoFast_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_TEST_ACTIVE STATNAME(PolyphaseMonoFast_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_HAS_ASM STATNAME(PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride2_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride2_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_HAS_ASM STATNAME(PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride4_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_HAS_ASM STATNAME(PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride2_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride2_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_HAS_ASM STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_REDUCED_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)
#define AMIGA_M68K_POLYPHASE_MONO_FAST_IS_ACTIVE STATNAME(AmigaM68KPolyphaseMonoFast_IsActive)
#define AMIGA_M68K_POLYPHASE_MONO_FAST_STRIDE2_IS_ACTIVE STATNAME(AmigaM68KPolyphaseMonoFastStride2_IsActive)
#define AMIGA_HUFFMAN_PAIRS_C_REFERENCE STATNAME(DecodeHuffmanPairs_C_REFERENCE)
#define AMIGA_HUFFMAN_PAIRS_TEST_ACTIVE STATNAME(DecodeHuffmanPairs_TEST_ACTIVE)
#define AMIGA_HUFFMAN_PAIRS_HAS_ASM STATNAME(DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_HUFFMAN_PAIRS_ASM_NOTE STATNAME(DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE)
#define AMIGA_DEQUANT_BLOCK_C_REFERENCE STATNAME(DequantBlock_C_REFERENCE)
#define AMIGA_DEQUANT_BLOCK_TEST_ACTIVE STATNAME(DequantBlock_TEST_ACTIVE)
#define AMIGA_DEQUANT_BLOCK_HAS_ASM STATNAME(DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_BITSTREAM_REFILL_SELFTEST STATNAME(BitstreamRefillSelftest)
#define AMIGA_POLY_COEF STATNAME(polyCoef)

#define READBUF_SIZE (1024 * 16)
#define OUTBUF_SAMPS (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)
#define AMIGA_IMDCT_BLOCK_SIZE 18
#define AMIGA_IMDCT_NBANDS 32
#define AMIGA_POLYPHASE_NBANDS 32
#define AMIGA_POLYPHASE_VBUF_LENGTH (17 * 2 * AMIGA_POLYPHASE_NBANDS)
#define AMIGA_AUDIO_MAX_CHANNEL_BYTES 65534UL

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
	int selftestClz;
	int selftestFdct32;
	int selftestFdct32Half;
	int selftestFdct32HalfDebug;
	int selftestVerbose;
	int selftestImdct;
	int selftestImdctThin;
	int selftestSubbandCap;
	int selftestAntialias;
	int selftestPolyphase;
	int selftestPolyphaseStride2;
	int selftestPolyphaseStride4;
	int selftestPolyphaseStride4Stereo;
	int selftestPolyphaseStride2Stereo;
	int forceCPolyphaseStride2Stereo;
	int selftestFastLowrate;
	int selftestReducedTaps;
	int selftestFdct32Quarter;
	int selftestHuffman;
	int selftestDequant;
	int selftestBitstream;
	int selftestMonoFastLowrateStereo;
	int selftestQuality;
	int checksum;
	int outputRate;
	int fastLowrate;
	int superfastLowrate;
	int quality;
	int qualitySpecified;
	int expPoly;
	int expHuff;
	int expImdctThin;
	int expReducedTaps;
	int expFdct32Quarter;
	int help;
	int debugArgv;
	int debugFastLowrate;
	int debugPlay;
	int debugCleanup;
	int play;
	int stereo;
	int decodeThenPlay;
	int playLifecycleTest;
	int audioOpenSilentTest;
	int startupVolumeSelftest;
	int bufferSeconds;
	int volumePercent;
	int fastMem;
	int info;
	int noMonoMSSideSkip;
} DecodeOptions;

typedef struct Mp3InputInfo {
	int id3v2Detected;
	int id3v2Major;
	int id3v2Revision;
	int id3v2Flags;
	unsigned long id3v2SkipBytes;
	int firstFrameFound;
	unsigned long firstFrameOffset;
	MP3FrameInfo firstFrameInfo;
} Mp3InputInfo;

typedef struct InputSource {
	FILE *file;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	BPTR amigaFile;
	int useAmigaDos;
#endif
	unsigned char *memory;
	unsigned long memorySize;
	unsigned long memoryPos;
	Mp3InputInfo info;
} InputSource;

static void InputSourceInit(InputSource *input, FILE *file);
#ifdef HAVE_AMIGA_AUDIO_DEVICE
static void InputSourceInitAmigaDos(InputSource *input, BPTR amigaFile);
#endif
static int InputSourcePrepareMp3(InputSource *input);

typedef struct DecodeStats {
	unsigned long decodedFrames;
	unsigned long outputSamples;
	unsigned long pcmChecksum;
	int sampleRate;
	int outputSampleRate;
	int channels;
	int outputChannels;
	int bitrate;
	unsigned long underruns;
	unsigned long underrunBuffers[3];
	unsigned long lateBuffers;
	long minimumSpareMilliseconds;
	int spareTimeMeasured;
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
	printf("  --info       print MP3/ID3 metadata; alone, inspect without decoding\n");
	printf("  --play       AmigaOS experimental audio.device Paula playback (mono s8)\n");
	printf("  --stereo     opt-in experimental --play stereo output (s8 per channel)\n");
	printf("               stereo rates: 8820, 11025, 22050, or PAL-top 28600 Hz\n");
	printf("               mono rates: 8287 default, 8820, 11025, 22050, or PAL-top 28600 Hz\n");
	printf("  --play-fast-path accepted alias; --play already uses reduced-overhead playback\n");
	printf("  --decode-then-play decode whole MP3 to RAM, then play (debug for --play)\n");
	printf("  --selftest-play-cleanup open/submit/cleanup audio.device five times\n");
	printf("  --selftest-startup-volume verify startup CMD_WRITE volume setup\n");
	printf("  --play-lifecycle-test legacy alias for --selftest-play-cleanup\n");
	printf("  --buffer-seconds N playback seconds per half-buffer (default 4, clamped 1-10)\n");
	printf("  --volume N   audio.device master volume percent for --play (0-100, default 100)\n");
	printf("  --fast-mem   preload the compressed MP3 into Fast RAM before decoding/playback\n");
	printf("  --decode-only decode frames only; skip PCM conversion and output\n");
	printf("  --no-output  run conversion/compression paths but discard output bytes\n");
	printf("  --rate HZ    output/downsample rate: 28600, 22050, 11025, 8820, or 8287 Hz\n");
	printf("               28600/22050 playback is experimental/high CPU and may underrun\n");
	printf("  --fast-lowrate lower-quality Amiga conversion; requires --rate\n");
	printf("  --superfast-lowrate sparse low-rate mode; use --rate 11025 or --rate 22050\n");
	printf("                 defaults to 11025 if no --rate is specified\n");
	printf("  --quality N set quality/speed level (0 fastest, 1 fast, 2 balanced, 3 accurate)\n");
	printf("               default: 1 for --fast-lowrate --rate 11025, otherwise 3\n");
	printf("               0 enables Superfast FDCT32 quarter + Huffman; 1 keeps IMDCT thin opt-in; 3 is original behavior\n");
	printf("               individual --exp-* flags may still be enabled independently\n");
	printf("  --exp-poly  use experimental 68030 asm mono polyphase when compiled in\n");
	printf("  --exp-huff  use experimental 68030 inline-asm Huffman pair refill when compiled in\n");
	printf("  --exp-imdct-thin request experimental fast-lowrate IMDCT output thinning\n");
	printf("  --exp-reduced-taps use experimental 8-segment stride-4 low-rate dewindow\n");
	printf("  --exp-fdct32-quarter use experimental stride-4 quarter-rate FDCT32 approximation\n");
	printf("  --selftest-mulshift compare C and optional asm MULSHIFT32 helpers\n");
	printf("  --selftest-clz compare C and optional m68k bfffo CLZ helpers\n");
	printf("  --selftest-fdct32 compare C reference and optional m68k asm FDCT32 path\n");
	printf("  --selftest-fdct32half compare FDCT32Half even-row stores against full FDCT32\n");
	printf("  --selftest-fdct32half-debug print first FDCT32Half mismatch dependencies\n");
	printf("  --selftest-verbose print every selftest mismatch instead of the first only\n");
	printf("  --selftest-imdct compare C reference and optional m68k asm long IMDCT path\n");
	printf("  --selftest-imdct-thin verify exact selected IMDCT bands and deterministic sparse output\n");
	printf("  --selftest-subband-cap verify low-rate mono IMDCT subband cap behavior\n");
	printf("  --selftest-antialias compare C reference and optional m68k asm antialias path\n");
	printf("  --selftest-polyphase compare C fast mono polyphase and optional m68k asm path\n");
	printf("  --selftest-polyphase-stride2 compare C and optional asm stride-2 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride4 compare C and optional asm stride-4 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride4-stereo compare stereo stride-4 compact polyphase output\n");
	printf("  --selftest-polyphase-stride2-stereo compare stereo stride-2 compact polyphase output\n");
	printf("  --force-c-polyphase-stride2-stereo benchmark stereo stride-2 C fallback in this binary\n");
	printf("  --selftest-fastlowrate compare synthetic stride decimation paths\n");
	printf("  --selftest-reduced-taps compare full and reduced stride-4 dewindow paths\n");
	printf("  --selftest-fdct32-quarter inspect lossy stride-4 quarter-rate FDCT32 scatter\n");
	printf("  --selftest-huffman compare C and active Huffman pair decode paths\n");
	printf("  --selftest-dequant compare C and optional m68k asm dequant block paths\n");
	printf("  --selftest-bitstream compare C and optional m68k move.l bitstream refill paths\n");
	printf("  --selftest-mono-fastlowrate-stereo verify stereo-to-mono low-rate accounting\n");
	printf("  --selftest-quality verify --quality flag mapping and auto-default selection\n");
	printf("  --checksum  print a 32-bit checksum of decoded PCM samples\n");
	printf("  --no-ms-mono-skip force full two-channel M/S decode before mono regression checks\n");
	printf("  --debug-fastlowrate print per-frame/granule fast-lowrate placement\n");
	printf("  --debug-play print audio.device playback startup diagnostics\n");
	printf("  --debug-cleanup print playback resource cleanup diagnostics\n");
	printf("  --debug-argv print argc/argv after Amiga argument normalization\n");
	printf("  --show-argv  alias for --debug-argv\n");
	printf("\n");
	printf("default output is raw signed 16-bit big-endian PCM.\n");
	printf("outfile ending in :, /, or \\ is treated as a directory/volume.\n");
}

static int ParseBufferSecondsOption(const char *arg, int *outSeconds)
{
	char *end;
	long value;

	if (!arg || !arg[0])
		return -1;
	value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value <= 0)
		return -1;
	if (value > 10)
		value = 10;
	*outSeconds = (int)value;
	return 0;
}

static int ParseVolumeOption(const char *arg, int *outPercent)
{
	char *end;
	long value;

	if (!arg || !arg[0])
		return -1;
	value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value < 0 || value > 100)
		return -1;
	*outPercent = (int)value;
	return 0;
}

#ifndef HAVE_AMIGA_AUDIO_DEVICE
typedef unsigned short UWORD;
#endif
#define AMIGA_AUDIO_DEVICE_MAX_VOLUME 64U

static UWORD VolumePercentToAudioDevice(int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	return (UWORD)(((unsigned int)percent * AMIGA_AUDIO_DEVICE_MAX_VOLUME + 50U) / 100U);
}

volatile unsigned short gMiniAmp3RequestedVolume = 100;
volatile unsigned long gMiniAmp3VolumeSequence;

static void ApplyQualityOptions(DecodeOptions *opt)
{
	int quality;

	quality = opt->qualitySpecified ? opt->quality :
		(opt->fastLowrate && opt->outputRate == 11025 ? 1 : 3);
	opt->quality = quality;

	switch (quality) {
	case 0:
		opt->expHuff = 1;
		opt->expFdct32Quarter = 1;
		/* fall through */
	case 1:
		opt->expReducedTaps = 1;
		/* fall through */
	case 2:
		opt->expPoly = 1;
		break;
	case 3:
	default:
		break;
	}
}

static int ParseOptions(int argc, char **argv, DecodeOptions *opt)
{
	int i;

	memset(opt, 0, sizeof(*opt));
	opt->outFormat = OUT_PCM16;
	opt->compression = SVX_COMP_NONE;
	opt->outputRate = 0;
	opt->quality = 3;
	opt->qualitySpecified = 0;
	opt->bufferSeconds = 4;
	opt->volumePercent = 100;

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
		} else if (!strcmp(argv[i], "--info")) {
			opt->info = 1;
		} else if (!strcmp(argv[i], "--play")) {
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--stereo")) {
			opt->stereo = 1;
		} else if (!strcmp(argv[i], "--play-fast-path")) {
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--decode-then-play")) {
			opt->play = 1;
			opt->decodeThenPlay = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--selftest-audio-open-silent")) {
			opt->audioOpenSilentTest = 1;
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--selftest-startup-volume")) {
			opt->startupVolumeSelftest = 1;
		} else if (!strcmp(argv[i], "--selftest-play-cleanup") ||
			!strcmp(argv[i], "--play-lifecycle-test")) {
			opt->play = 1;
			opt->playLifecycleTest = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--buffer-seconds")) {
			if (++i >= argc)
				return -1;
			if (ParseBufferSecondsOption(argv[i], &opt->bufferSeconds) != 0) {
				fprintf(stderr, "--buffer-seconds requires a positive integer (1-10 seconds)\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--volume")) {
			if (++i >= argc)
				return -1;
			if (ParseVolumeOption(argv[i], &opt->volumePercent) != 0) {
				fprintf(stderr, "--volume requires an integer from 0 to 100\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--fast-mem")) {
			opt->fastMem = 1;
		} else if (!strcmp(argv[i], "--decode-only")) {
			opt->decodeOnly = 1;
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--no-output")) {
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--selftest-mulshift")) {
			opt->selftestMulshift = 1;
		} else if (!strcmp(argv[i], "--selftest-clz")) {
			opt->selftestClz = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32")) {
			opt->selftestFdct32 = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32half")) {
			opt->selftestFdct32Half = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32half-debug")) {
			opt->selftestFdct32Half = 1;
			opt->selftestFdct32HalfDebug = 1;
		} else if (!strcmp(argv[i], "--selftest-verbose")) {
			opt->selftestVerbose = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct")) {
			opt->selftestImdct = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct-thin")) {
			opt->selftestImdctThin = 1;
		} else if (!strcmp(argv[i], "--selftest-subband-cap")) {
			opt->selftestSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-antialias")) {
			opt->selftestAntialias = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase")) {
			opt->selftestPolyphase = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2")) {
			opt->selftestPolyphaseStride2 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4")) {
			opt->selftestPolyphaseStride4 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-stereo")) {
			opt->selftestPolyphaseStride4Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-stereo")) {
			opt->selftestPolyphaseStride2Stereo = 1;
		} else if (!strcmp(argv[i], "--force-c-polyphase-stride2-stereo")) {
			opt->forceCPolyphaseStride2Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-fastlowrate")) {
			opt->selftestFastLowrate = 1;
		} else if (!strcmp(argv[i], "--selftest-reduced-taps")) {
			opt->selftestReducedTaps = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32-quarter")) {
			opt->selftestFdct32Quarter = 1;
		} else if (!strcmp(argv[i], "--selftest-huffman")) {
			opt->selftestHuffman = 1;
		} else if (!strcmp(argv[i], "--selftest-dequant")) {
			opt->selftestDequant = 1;
		} else if (!strcmp(argv[i], "--selftest-bitstream")) {
			opt->selftestBitstream = 1;
		} else if (!strcmp(argv[i], "--selftest-mono-fastlowrate-stereo")) {
			opt->selftestMonoFastLowrateStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-quality")) {
			opt->selftestQuality = 1;
		} else if (!strcmp(argv[i], "--checksum")) {
			opt->checksum = 1;
		} else if (!strcmp(argv[i], "--fast-lowrate")) {
			opt->fastLowrate = 1;
		} else if (!strcmp(argv[i], "--superfast-lowrate")) {
			opt->fastLowrate = 1;
			opt->superfastLowrate = 1;
		} else if (!strcmp(argv[i], "--exp-poly")) {
			opt->expPoly = 1;
		} else if (!strcmp(argv[i], "--exp-huff")) {
			opt->expHuff = 1;
		} else if (!strcmp(argv[i], "--exp-imdct-thin")) {
			opt->expImdctThin = 1;
		} else if (!strcmp(argv[i], "--no-ms-mono-skip")) {
			opt->noMonoMSSideSkip = 1;
		} else if (!strcmp(argv[i], "--exp-reduced-taps")) {
			opt->expReducedTaps = 1;
		} else if (!strcmp(argv[i], "--exp-fdct32-quarter")) {
			opt->expFdct32Quarter = 1;
		} else if (!strcmp(argv[i], "--quality")) {
			if (++i >= argc)
				return -1;
			if (argv[i][0] < '0' || argv[i][0] > '3' || argv[i][1] != '\0') {
				fprintf(stderr, "--quality requires 0, 1, 2, or 3\n");
				return -1;
			}
			opt->quality = argv[i][0] - '0';
			opt->qualitySpecified = 1;
		} else if (!strcmp(argv[i], "--rate")) {
			if (++i >= argc)
				return -1;
			opt->outputRate = atoi(argv[i]);
			if (opt->outputRate != 28600 && opt->outputRate != 22050 &&
				opt->outputRate != 11025 && opt->outputRate != 8820 &&
				opt->outputRate != 8287)
				return -1;
		} else if (!strcmp(argv[i], "--debug-fastlowrate")) {
			opt->debugFastLowrate = 1;
		} else if (!strcmp(argv[i], "--debug-play")) {
			opt->debugPlay = 1;
		} else if (!strcmp(argv[i], "--debug-cleanup")) {
			opt->debugCleanup = 1;
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

if (opt->selftestMulshift ||
    opt->selftestClz ||
    opt->selftestFdct32 ||
    opt->selftestFdct32Half ||
    opt->selftestImdct ||
    opt->selftestImdctThin ||
    opt->selftestSubbandCap ||
    opt->selftestAntialias ||
    opt->selftestPolyphase ||
    opt->selftestPolyphaseStride2 ||
    opt->selftestPolyphaseStride4 ||
    opt->selftestPolyphaseStride4Stereo ||
    opt->selftestPolyphaseStride2Stereo ||
    opt->selftestFastLowrate ||
    opt->selftestReducedTaps ||
    opt->selftestFdct32Quarter ||
    opt->selftestHuffman ||
    opt->selftestDequant ||
    opt->selftestBitstream ||
    opt->selftestMonoFastLowrateStereo ||
    opt->selftestQuality)
		return 0;

	if (opt->stereo && !opt->play) {
		fprintf(stderr, "--stereo is only supported with --play\n");
		return -1;
	}

	if (opt->superfastLowrate && !opt->outputRate)
		opt->outputRate = 11025;

	if (opt->play && !opt->outputRate)
		opt->outputRate = opt->stereo ? 8820 : 8287;

	if (opt->play && opt->outputRate != 8287 && opt->outputRate != 8820 &&
		opt->outputRate != 11025 && opt->outputRate != 22050 &&
		opt->outputRate != 28600) {
		fprintf(stderr, "--play supports --rate 8287, 8820, 11025, 22050, or 28600 only\n");
		return -1;
	}
	if (opt->stereo && opt->outputRate == 8287) {
		fprintf(stderr, "--stereo supports --rate 8820, 11025, 22050, or PAL-top 28600 only\n");
		return -1;
	}
	if (opt->play) {
		opt->mono = opt->stereo ? 0 : 1;
		opt->outFormat = OUT_S8;
		if (opt->outputRate != 28600)
			opt->fastLowrate = 1;
		opt->noOutput = 1;
	}

	if (opt->superfastLowrate && opt->outputRate != 11025 && opt->outputRate != 22050) {
		fprintf(stderr, "--superfast-lowrate supports only --rate 11025 or --rate 22050\n");
		return -1;
	}
	if (opt->fastLowrate && (opt->outputRate != 22050 &&
		opt->outputRate != 11025 && opt->outputRate != 8820 &&
		opt->outputRate != 8287)) {
		fprintf(stderr, "--fast-lowrate requires --rate 22050, 11025, 8820, or 8287\n");
		return -1;
	}

	ApplyQualityOptions(opt);

	if (opt->playLifecycleTest || opt->audioOpenSilentTest)
		return 0;

	if (!opt->inName || (!opt->outName && !opt->noOutput && !opt->play && !opt->info))
		return -1;

	return 0;
}

static unsigned long SynchsafeSize(const unsigned char *p)
{
	return ((unsigned long)(p[0] & 0x7f) << 21) |
		((unsigned long)(p[1] & 0x7f) << 14) |
		((unsigned long)(p[2] & 0x7f) << 7) |
		(unsigned long)(p[3] & 0x7f);
}

static unsigned long BigEndianSize(const unsigned char *p, int bytes)
{
	unsigned long value;
	int i;

	value = 0;
	for (i = 0; i < bytes; i++)
		value = (value << 8) | p[i];
	return value;
}

static void PrintTagText(const char *label, const unsigned char *data,
	unsigned long bytes)
{
	unsigned long i;
	int encoding;
	int bigEndian;
	int printed;

	if (!bytes)
		return;
	encoding = data[0];
	data++;
	bytes--;
	printf("%s: ", label);
	printed = 0;
	if (encoding == 1 || encoding == 2) {
		bigEndian = encoding == 2;
		if (bytes >= 2 && data[0] == 0xfe && data[1] == 0xff) {
			bigEndian = 1;
			data += 2;
			bytes -= 2;
		} else if (bytes >= 2 && data[0] == 0xff && data[1] == 0xfe) {
			bigEndian = 0;
			data += 2;
			bytes -= 2;
		}
		for (i = 0; i + 1 < bytes; i += 2) {
			unsigned int ch;
			ch = bigEndian ? ((unsigned int)data[i] << 8) | data[i + 1] :
				((unsigned int)data[i + 1] << 8) | data[i];
			if (!ch)
				break;
			putchar(ch >= 32 && ch <= 126 ? (int)ch : '?');
			printed = 1;
		}
	} else {
		for (i = 0; i < bytes && data[i]; i++) {
			unsigned char ch;
			ch = data[i];
			putchar((ch >= 32 && ch != 127) ? ch : ' ');
			printed = 1;
		}
	}
	if (!printed)
		printf("(empty)");
	putchar('\n');
}

static const char *TagFrameLabel(const char *id)
{
	if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) return "title";
	if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) return "artist";
	if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) return "album";
	if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) return "track";
	if (!strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE")) return "year";
	if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) return "genre";
	if (!strcmp(id, "TCOM") || !strcmp(id, "TCM")) return "composer";
	if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) return "album artist";
	if (!strcmp(id, "TPUB") || !strcmp(id, "TPB")) return "publisher";
	if (!strcmp(id, "TCOP") || !strcmp(id, "TCR")) return "copyright";
	return id;
}

static void PrintCommentTag(const unsigned char *data, unsigned long bytes)
{
	unsigned long pos;
	unsigned long terminatorBytes;
	unsigned char *text;

	if (bytes < 5)
		return;
	terminatorBytes = (data[0] == 1 || data[0] == 2) ? 2UL : 1UL;
	pos = 4;
	while (pos + terminatorBytes <= bytes) {
		if (data[pos] == 0 && (terminatorBytes == 1 || data[pos + 1] == 0)) {
			pos += terminatorBytes;
			break;
		}
		pos += terminatorBytes;
	}
	if (pos >= bytes)
		return;
	text = (unsigned char *)malloc((size_t)(bytes - pos + 1));
	if (!text)
		return;
	text[0] = data[0];
	memcpy(text + 1, data + pos, (size_t)(bytes - pos));
	PrintTagText("comment", text, bytes - pos + 1);
	free(text);
}

static unsigned long PrintId3v2(FILE *fp)
{
	unsigned char header[10];
	unsigned long tagBytes;
	unsigned long pos;
	int major;

	if (fseek(fp, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), fp) != sizeof(header) ||
		memcmp(header, "ID3", 3) != 0)
		return 0;
	major = header[3];
	tagBytes = SynchsafeSize(header + 6);
	printf("ID3v2: 2.%d.%d (%lu bytes)\n", major, header[4], tagBytes);
	pos = 0;
	if ((header[5] & 0x40) && major >= 3) {
		unsigned char extSize[4];
		unsigned long skipBytes;
		if (fread(extSize, 1, sizeof(extSize), fp) != sizeof(extSize))
			return 10UL + tagBytes;
		skipBytes = major == 4 ? SynchsafeSize(extSize) : BigEndianSize(extSize, 4) + 4UL;
		if (skipBytes < 4 || skipBytes > tagBytes ||
			fseek(fp, (long)(skipBytes - 4UL), SEEK_CUR) != 0)
			return 10UL + tagBytes;
		pos = skipBytes;
	}
	while (pos + (major == 2 ? 6UL : 10UL) <= tagBytes) {
		unsigned char frameHeader[10];
		unsigned char *frame;
		unsigned long frameBytes;
		int headerBytes;
		char id[5];

		headerBytes = major == 2 ? 6 : 10;
		if (fread(frameHeader, 1, (size_t)headerBytes, fp) != (size_t)headerBytes)
			break;
		pos += (unsigned long)headerBytes;
		if (!frameHeader[0])
			break;
		memset(id, 0, sizeof(id));
		memcpy(id, frameHeader, major == 2 ? 3 : 4);
		if (major == 2)
			frameBytes = BigEndianSize(frameHeader + 3, 3);
		else if (major == 4)
			frameBytes = SynchsafeSize(frameHeader + 4);
		else
			frameBytes = BigEndianSize(frameHeader + 4, 4);
		if (!frameBytes || frameBytes > tagBytes - pos)
			break;
		if ((id[0] == 'T' || !strcmp(id, "COMM") || !strcmp(id, "COM")) &&
			frameBytes <= 1024UL * 1024UL) {
			frame = (unsigned char *)malloc((size_t)frameBytes);
			if (!frame || fread(frame, 1, (size_t)frameBytes, fp) != (size_t)frameBytes) {
				free(frame);
				break;
			}
			if (!strcmp(id, "COMM") || !strcmp(id, "COM"))
				PrintCommentTag(frame, frameBytes);
			else
				PrintTagText(TagFrameLabel(id), frame, frameBytes);
			free(frame);
		} else {
			if (!strcmp(id, "APIC") || !strcmp(id, "PIC"))
				printf("embedded artwork: %lu bytes\n", frameBytes);
			if (fseek(fp, (long)frameBytes, SEEK_CUR) != 0)
				break;
		}
		pos += frameBytes;
	}
	return 10UL + tagBytes + ((header[5] & 0x10) ? 10UL : 0UL);
}

static void PrintFixedId3v1Text(const char *label, const unsigned char *data, int bytes)
{
	int end;
	int i;

	end = bytes;
	while (end > 0 && (data[end - 1] == 0 || data[end - 1] == ' '))
		end--;
	if (!end)
		return;
	printf("ID3v1 %s: ", label);
	for (i = 0; i < end; i++)
		putchar(data[i] >= 32 && data[i] != 127 ? data[i] : ' ');
	putchar('\n');
}

static void PrintId3v1(FILE *fp, long fileSize)
{
	unsigned char tag[128];

	if (fileSize < 128 || fseek(fp, fileSize - 128, SEEK_SET) != 0 ||
		fread(tag, 1, sizeof(tag), fp) != sizeof(tag) || memcmp(tag, "TAG", 3) != 0)
		return;
	printf("ID3v1: present\n");
	PrintFixedId3v1Text("title", tag + 3, 30);
	PrintFixedId3v1Text("artist", tag + 33, 30);
	PrintFixedId3v1Text("album", tag + 63, 30);
	PrintFixedId3v1Text("year", tag + 93, 4);
	if (tag[125] == 0 && tag[126] != 0)
		printf("ID3v1 track: %u\n", (unsigned int)tag[126]);
	printf("ID3v1 genre index: %u\n", (unsigned int)tag[127]);
}

static void PrintFirstFrameInfo(const Mp3InputInfo *inputInfo)
{
	const MP3FrameInfo *info;
	const char *version;

	if (!inputInfo->firstFrameFound) {
		printf("first MPEG frame offset: not found\n");
		printf("MPEG audio: no valid frame found after tags\n");
		return;
	}
	info = &inputInfo->firstFrameInfo;
	version = info->version == MPEG1 ? "1" : (info->version == MPEG2 ? "2" : "2.5");
	printf("first MPEG frame offset: %lu\n", inputInfo->firstFrameOffset);
	printf("MPEG audio: version %s, layer %d\n", version, info->layer);
	printf("sample rate: %d Hz\n", info->samprate);
	printf("channels: %d\n", info->nChans);
	printf("bitrate: %d bps\n", info->bitrate);
}

static void PrintMp3Info(FILE *fp, const char *name)
{
	long fileSize;
	InputSource input;

	fileSize = -1;
	if (fseek(fp, 0, SEEK_END) == 0)
		fileSize = ftell(fp);
	printf("file: %s\n", name);
	if (fileSize >= 0)
		printf("file size: %lu bytes\n", (unsigned long)fileSize);
	InputSourceInit(&input, fp);
	InputSourcePrepareMp3(&input);
	printf("ID3v2 detected: %s\n", input.info.id3v2Detected ? "yes" : "no");
	if (input.info.id3v2Detected)
		printf("ID3v2 version: 2.%d.%d\n", input.info.id3v2Major,
			input.info.id3v2Revision);
	printf("ID3v2 size skipped: %lu bytes\n", input.info.id3v2SkipBytes);
	PrintId3v2(fp);
	PrintFirstFrameInfo(&input.info);
	if (fileSize >= 0)
		PrintId3v1(fp, fileSize);
	fseek(fp, 0, SEEK_SET);
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

static void *AllocFastInputMemory(unsigned long bytes)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	return AllocMem(bytes, MEMF_FAST);
#else
	return malloc((size_t)bytes);
#endif
}

static void FreeFastInputMemory(void *memory, unsigned long bytes)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (memory)
		FreeMem(memory, bytes);
#else
	(void)bytes;
	free(memory);
#endif
}

static void InputSourceInit(InputSource *input, FILE *file)
{
	memset(input, 0, sizeof(*input));
	input->file = file;
}

#ifdef HAVE_AMIGA_AUDIO_DEVICE
static void InputSourceInitAmigaDos(InputSource *input, BPTR amigaFile)
{
	memset(input, 0, sizeof(*input));
	input->amigaFile = amigaFile;
	input->useAmigaDos = amigaFile ? 1 : 0;
}
#endif

static void InputSourceClose(InputSource *input)
{
	FreeFastInputMemory(input->memory, input->memorySize);
	input->memory = NULL;
	input->memorySize = 0;
	input->memoryPos = 0;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos && input->amigaFile) {
		Close(input->amigaFile);
		input->amigaFile = (BPTR)0;
	}
	input->useAmigaDos = 0;
#endif
}

static void CloseInputFile(FILE **file, int debugCleanup)
{
	if (!file || !*file)
		return;
	fclose(*file);
	*file = NULL;
	if (debugCleanup)
		printf("debug-cleanup: input file closed: yes\n");
}

static size_t InputSourceRead(InputSource *input, void *dest, size_t bytes)
{
	if (input->memory) {
		unsigned long available;

		available = input->memorySize - input->memoryPos;
		if (bytes > (size_t)available)
			bytes = (size_t)available;
		if (bytes > 0) {
			memcpy(dest, input->memory + input->memoryPos, bytes);
			input->memoryPos += (unsigned long)bytes;
		}
		return bytes;
	}
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos) {
		LONG nRead;

		if (!input->amigaFile || bytes == 0)
			return 0;
		if (bytes > (size_t)2147483647L)
			bytes = (size_t)2147483647L;
		nRead = Read(input->amigaFile, dest, (LONG)bytes);
		return nRead < 0 ? 0 : (size_t)nRead;
	}
#endif
	return fread(dest, 1, bytes, input->file);
}

static unsigned long InputSourceTell(const InputSource *input)
{
	if (input->memory)
		return input->memoryPos;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos) {
		LONG pos = input->amigaFile ? Seek(input->amigaFile, 0, OFFSET_CURRENT) : -1;
		return pos < 0 ? 0UL : (unsigned long)pos;
	}
#endif
	{
		long pos = ftell(input->file);
		return pos < 0 ? 0UL : (unsigned long)pos;
	}
}

static void InputSourceSeek(InputSource *input, unsigned long pos)
{
	if (input->memory) {
		input->memoryPos = pos <= input->memorySize ? pos : input->memorySize;
	} else {
#ifdef HAVE_AMIGA_AUDIO_DEVICE
		if (input->useAmigaDos) {
			if (input->amigaFile)
				Seek(input->amigaFile, (LONG)pos, OFFSET_BEGINNING);
		} else
#endif
		{
			fseek(input->file, (long)pos, SEEK_SET);
		}
	}
}

static int InputSourcePreloadFastMemory(InputSource *input)
{
	long fileSize;
	unsigned char *memory;
	size_t nRead;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos) {
		LONG oldPos;
		LONG endPos;

		if (!input->amigaFile)
			return -1;
		oldPos = Seek(input->amigaFile, 0, OFFSET_END);
		if (oldPos < 0) {
			Seek(input->amigaFile, 0, OFFSET_BEGINNING);
			return -1;
		}
		endPos = Seek(input->amigaFile, 0, OFFSET_CURRENT);
		if (endPos <= 0 || (unsigned long)endPos > (unsigned long)(size_t)-1) {
			Seek(input->amigaFile, 0, OFFSET_BEGINNING);
			return -1;
		}
		fileSize = endPos;
		if (Seek(input->amigaFile, 0, OFFSET_BEGINNING) < 0)
			return -1;
		memory = (unsigned char *)AllocFastInputMemory((unsigned long)fileSize);
		if (!memory)
			return -1;
		nRead = InputSourceRead(input, memory, (size_t)fileSize);
		if (nRead != (size_t)fileSize) {
			FreeFastInputMemory(memory, (unsigned long)fileSize);
			Seek(input->amigaFile, 0, OFFSET_BEGINNING);
			return -1;
		}
		input->memory = memory;
		input->memorySize = (unsigned long)fileSize;
		input->memoryPos = 0;
		printf("fast-mem input preload: copying %lu bytes to Fast RAM\n", input->memorySize);
		return 0;
	}
#endif
	if (fseek(input->file, 0, SEEK_END) != 0)
		return -1;
	fileSize = ftell(input->file);
	if (fileSize <= 0 || (unsigned long)fileSize > (unsigned long)(size_t)-1) {
		fseek(input->file, 0, SEEK_SET);
		return -1;
	}
	if (fseek(input->file, 0, SEEK_SET) != 0)
		return -1;
	memory = (unsigned char *)AllocFastInputMemory((unsigned long)fileSize);
	if (!memory)
		return -1;
	nRead = fread(memory, 1, (size_t)fileSize, input->file);
	if (nRead != (size_t)fileSize) {
		FreeFastInputMemory(memory, (unsigned long)fileSize);
		fseek(input->file, 0, SEEK_SET);
		return -1;
	}
	input->memory = memory;
	input->memorySize = (unsigned long)fileSize;
	input->memoryPos = 0;
	printf("fast-mem input preload: copying %lu bytes to Fast RAM\n", input->memorySize);
	return 0;
}

static int MpegHeaderLooksValid(const unsigned char *header)
{
	if (header[0] != 0xff || (header[1] & 0xe0) != 0xe0)
		return 0;
	/* Reject reserved MPEG version, anything other than Layer III, the reserved
	 * bitrate index, and the reserved sample-rate index. */
	if ((header[1] & 0x18) == 0x08 || (header[1] & 0x06) != 0x02)
		return 0;
	if ((header[2] & 0xf0) == 0xf0 || (header[2] & 0x0c) == 0x0c)
		return 0;
	return 1;
}

static int FindValidatedMpegSync(const unsigned char *buf, int nBytes)
{
	int i;

	for (i = 0; i <= nBytes - 4; i++) {
		if (MpegHeaderLooksValid(buf + i))
			return i;
	}
	return -1;
}

static int InputSourcePrepareMp3(InputSource *input)
{
	unsigned char header[10];
	unsigned char scan[READBUF_SIZE];
	unsigned long scanBase;
	unsigned long tagBytes;
	size_t nRead;
	int keep;
	HMP3Decoder decoder;

	memset(&input->info, 0, sizeof(input->info));
	InputSourceSeek(input, 0);
	nRead = InputSourceRead(input, header, sizeof(header));
	if (nRead == sizeof(header) && memcmp(header, "ID3", 3) == 0) {
		input->info.id3v2Detected = 1;
		input->info.id3v2Major = header[3];
		input->info.id3v2Revision = header[4];
		input->info.id3v2Flags = header[5];
		if (!(header[6] & 0x80) && !(header[7] & 0x80) &&
			!(header[8] & 0x80) && !(header[9] & 0x80)) {
			tagBytes = SynchsafeSize(header + 6);
			input->info.id3v2SkipBytes = 10UL + tagBytes;
			if (header[5] & 0x10)
				input->info.id3v2SkipBytes += 10UL;
		}
	}

	InputSourceSeek(input, input->info.id3v2SkipBytes);
	scanBase = input->info.id3v2SkipBytes;
	keep = 0;
	decoder = MP3InitDecoder();
	if (!decoder)
		return -1;
	for (;;) {
		int offset;
		int available;

		nRead = InputSourceRead(input, scan + keep, sizeof(scan) - (size_t)keep);
		available = keep + (int)nRead;
		offset = FindValidatedMpegSync(scan, available);
		while (offset >= 0) {
			MP3FrameInfo frameInfo;
			if (MP3GetNextFrameInfo(decoder, &frameInfo, scan + offset) == ERR_MP3_NONE) {
				input->info.firstFrameFound = 1;
				input->info.firstFrameOffset = scanBase + (unsigned long)offset;
				input->info.firstFrameInfo = frameInfo;
				MP3FreeDecoder(decoder);
				InputSourceSeek(input, input->info.firstFrameOffset);
				return 0;
			}
			offset++;
			if (offset > available - 4)
				break;
			{
				int next = FindValidatedMpegSync(scan + offset, available - offset);
				offset = next < 0 ? -1 : offset + next;
			}
		}
		if (nRead == 0)
			break;
		keep = available < 3 ? available : 3;
		memmove(scan, scan + available - keep, (size_t)keep);
		scanBase += (unsigned long)(available - keep);
	}
	MP3FreeDecoder(decoder);
	InputSourceSeek(input, input->info.id3v2SkipBytes);
	return 0;
}

static int FillReadBuffer(unsigned char *readBuf, unsigned char *readPtr, int bufSize,
	int bytesLeft, InputSource *input)
{
	int nRead;

	memmove(readBuf, readPtr, bytesLeft);
	nRead = (int)InputSourceRead(input, readBuf + bytesLeft,
		(size_t)(bufSize - bytesLeft));
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
	if (!gTiming)
		return fputc(c, fp);
	t0 = clock();
	r = fputc(c, fp);
	gTiming->fileWrite += clock() - t0;
	return r;
}

static size_t TimedFwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
	clock_t t0;
	size_t r;

	if (!fp)
		return nmemb;
	if (!gTiming)
		return fwrite(ptr, size, nmemb, fp);
	t0 = clock();
	r = fwrite(ptr, size, nmemb, fp);
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

	if (gTiming) {
		t0 = clock();
		nibble = FibDeltaNibble(svx->fibPrev, sample);
		svx->fibPrev = FibDeltaApply(svx->fibPrev, nibble);
		gTiming->fibCompress += clock() - t0;
	} else {
		nibble = FibDeltaNibble(svx->fibPrev, sample);
		svx->fibPrev = FibDeltaApply(svx->fibPrev, nibble);
	}
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

static int FastLowrateStrideForOutputRate(int outputRate)
{
	if (outputRate == 22050)
		return 2;
	if (outputRate == 11025)
		return 4;
	return 5;
}

static int FastLowrateActualOutputRate(const DecodeOptions *opt, int inputSampleRate)
{
	int stride;

	if (inputSampleRate <= 0)
		return opt->outputRate;

	stride = FastLowrateStrideForOutputRate(opt->outputRate);
	return inputSampleRate / stride;
}

static int EffectiveOutputSampleRate(const DecodeOptions *opt, int inputSampleRate)
{
	if (inputSampleRate <= 0)
		return opt->outputRate;
	if (opt->fastLowrate)
		return FastLowrateActualOutputRate(opt, inputSampleRate);
	if (!opt->decodeOnly && opt->outputRate && inputSampleRate > opt->outputRate)
		return opt->outputRate;

	return inputSampleRate;
}

static int PlaybackOutputSampleRate(const DecodeOptions *opt, const DecodeStats *stats)
{
	if (stats->outputSampleRate > 0)
		return stats->outputSampleRate;
	if (opt->fastLowrate && stats->sampleRate > 0)
		return FastLowrateActualOutputRate(opt, stats->sampleRate);
	if (opt->outputRate > 0)
		return opt->outputRate;
	return stats->sampleRate;
}

static void PrintFastLowrateOutputRateDifference(const DecodeOptions *opt,
	int actualOutputRate)
{
	if (opt->fastLowrate && opt->outputRate > 0 && actualOutputRate > 0 &&
		actualOutputRate != opt->outputRate) {
		printf("requested output rate: %d Hz\n", opt->outputRate);
		printf("actual fast-lowrate output rate: %d Hz\n", actualOutputRate);
	}
}

static int OutputChannelCount(const DecodeOptions *opt, const DecodeStats *stats)
{
	int outputChannels;

	if (stats->outputChannels > 0)
		return stats->outputChannels;

	if (opt->stereo)
		outputChannels = 2;
	else if (opt->mono)
		outputChannels = 1;
	else
		outputChannels = stats->channels;

	if (outputChannels <= 0)
		outputChannels = 1;

	return outputChannels;
}

static unsigned long PerChannelEmittedSamples(const DecodeOptions *opt,
	const DecodeStats *stats)
{
	int outputChannels;

	outputChannels = OutputChannelCount(opt, stats);
	return outputChannels > 1 ?
		stats->outputSamples / (unsigned long)outputChannels : stats->outputSamples;
}

static double DecodedAudioSeconds(const DecodeOptions *opt,
	const DecodeStats *stats)
{
	int sampleRate;
	unsigned long perChannelSamples;

	if (stats->outputSamples == 0)
		return 0.0;

	if (opt->fastLowrate)
		sampleRate = PlaybackOutputSampleRate(opt, stats);
	else
		sampleRate = stats->outputSampleRate ?
			stats->outputSampleRate : stats->sampleRate;

	if (sampleRate <= 0)
		return 0.0;

	perChannelSamples = PerChannelEmittedSamples(opt, stats);
	return (double)perChannelSamples / (double)sampleRate;
}

static void PrintOutputStats(const DecodeOptions *opt, const DecodeStats *stats)
{
	unsigned long perChannelSamples;
	int outputChannels;
	double audioSeconds;

	outputChannels = OutputChannelCount(opt, stats);
	perChannelSamples = PerChannelEmittedSamples(opt, stats);
	audioSeconds = DecodedAudioSeconds(opt, stats);

	printf("input channels: %d\n", stats->channels);
	printf("output channels: %d\n", outputChannels);
	printf("total emitted samples: %lu\n", stats->outputSamples);
	printf("per-channel emitted samples: %lu\n", perChannelSamples);
	printf("decoded audio seconds used for realtime calculation: %.6f\n", audioSeconds);
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


static int FastLowrateSelectFrame(int *phase, const short *in, short *out,
	int nSamps, int stride, int channels)
{
	int inFrames;
	int frame;
	int produced;

	if (stride < 2 || channels <= 0) {
		if (out != in)
			memmove(out, in, nSamps * sizeof(short));
		return nSamps;
	}

	inFrames = nSamps / channels;
	produced = 0;
	for (frame = 0; frame < inFrames; frame++) {
		if (*phase == 0) {
			int ch;
			for (ch = 0; ch < channels; ch++)
				out[produced * channels + ch] = in[frame * channels + ch];
			produced++;
		}
		(*phase)++;
		if (*phase >= stride)
			*phase = 0;
	}
	return produced * channels;
}


static int QualitySelftestExpect(const char *name, DecodeOptions opt,
	int expReducedTaps, int expFdct32Quarter, int expImdctThin,
	int expPoly, int expHuff, int expectedQuality)
{
	ApplyQualityOptions(&opt);
	if (opt.quality != expectedQuality ||
		opt.expReducedTaps != expReducedTaps ||
		opt.expFdct32Quarter != expFdct32Quarter ||
		opt.expImdctThin != expImdctThin ||
		opt.expPoly != expPoly || opt.expHuff != expHuff) {
		fprintf(stderr,
			"quality selftest %s mismatch: quality=%d reduced=%d fdct32q=%d imdctThin=%d poly=%d huff=%d\n",
			name, opt.quality, opt.expReducedTaps, opt.expFdct32Quarter,
			opt.expImdctThin, opt.expPoly, opt.expHuff);
		fprintf(stderr,
			"quality selftest %s expected: quality=%d reduced=%d fdct32q=%d imdctThin=%d poly=%d huff=%d\n",
			name, expectedQuality, expReducedTaps, expFdct32Quarter,
			expImdctThin, expPoly, expHuff);
		return -1;
	}
	return 0;
}

static int SelftestQuality(void)
{
	DecodeOptions opt;
	int failures;

	failures = 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 0;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality0", opt, 1, 1, 0, 1, 1, 0) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 1;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality1", opt, 1, 0, 0, 1, 0, 1) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 2;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality2", opt, 0, 0, 0, 1, 0, 2) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 3;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality3", opt, 0, 0, 0, 0, 0, 3) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.expHuff = 1;
	opt.expFdct32Quarter = 1;
	opt.expReducedTaps = 1;
	opt.expImdctThin = 1;
	opt.expPoly = 1;
	opt.quality = 3;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality3-explicit-flags", opt, 1, 1, 1, 1, 1, 3) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.fastLowrate = 1;
	opt.outputRate = 11025;
	failures += QualitySelftestExpect("auto-fast-lowrate-11025", opt, 1, 0, 0, 1, 0, 1) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.fastLowrate = 1;
	opt.outputRate = 8820;
	failures += QualitySelftestExpect("auto-fast-lowrate-8820", opt, 0, 0, 0, 0, 0, 3) != 0;

	memset(&opt, 0, sizeof(opt));
	failures += QualitySelftestExpect("auto-default", opt, 0, 0, 0, 0, 0, 3) != 0;

	printf("Quality selftest cases: %d\n", 7);
	printf("Quality selftest failures: %d\n", failures);
	if (!failures)
		printf("Quality selftest passed\n");
	return failures ? -1 : 0;
}

static int SelftestStartupVolume(void)
{
	static const int percents[] = { 0, 50, 100 };
	static const UWORD expected[] = { 0, 32, 64 };
	int failures = 0;
	int stereo;
	unsigned int i;

	for (stereo = 0; stereo <= 1; stereo++) {
		for (i = 0; i < sizeof(percents) / sizeof(percents[0]); i++) {
			UWORD mapped = VolumePercentToAudioDevice(percents[i]);
			UWORD request0 = mapped;
			UWORD request1 = mapped;
			if (mapped != expected[i]) {
				fprintf(stderr,
					"startup volume selftest %s %d%% mapped %u expected %u\n",
					stereo ? "stereo" : "mono", percents[i],
					(unsigned int)mapped, (unsigned int)expected[i]);
				failures++;
			}
			if (request0 != mapped || (stereo && request1 != mapped)) {
				fprintf(stderr,
					"startup volume selftest %s %d%% request mismatch ch0=%u ch1=%u mapped=%u\n",
					stereo ? "stereo" : "mono", percents[i],
					(unsigned int)request0, (unsigned int)request1,
					(unsigned int)mapped);
				failures++;
			}
			printf("startup volume selftest: %s %d%% -> ioa_Volume %u%s\n",
				stereo ? "stereo" : "mono", percents[i],
				(unsigned int)mapped,
				stereo ? " on both channel requests" : "");
		}
	}
	printf("startup volume selftest failures: %d\n", failures);
	return failures ? -1 : 0;
}

static int SelftestFastLowrate(void)
{
	enum { CHANNELS = 1, TOTAL_FRAMES = 2304, CHUNK_FRAMES = 576 };
	short input[TOTAL_FRAMES * CHANNELS];
	short normal[TOTAL_FRAMES * CHANNELS];
	short fast[TOTAL_FRAMES * CHANNELS];
	RateState rateState;
	int fastPhase;
	int normalCount;
	int fastCount;
	int offset;
	int i;
	int failures;
	int inSamps;

	for (i = 0; i < TOTAL_FRAMES; i++) {
		input[i] = (short)((i % 257) * 127 - 16384);
		if ((i % 509) == 0)
			input[i] = 30000;
	}

	memset(&rateState, 0, sizeof(rateState));
	fastPhase = 0;
	normalCount = 0;
	fastCount = 0;
	for (offset = 0; offset < TOTAL_FRAMES; offset += CHUNK_FRAMES) {
		inSamps = CHUNK_FRAMES * CHANNELS;
		normalCount += DownsampleFrame(&rateState, input + offset * CHANNELS,
			normal + normalCount, inSamps, 44100, 11025, CHANNELS);
		fastCount += FastLowrateSelectFrame(&fastPhase, input + offset * CHANNELS,
			fast + fastCount, inSamps, 4, CHANNELS);
	}

	failures = 0;
	if (normalCount != fastCount) {
		fprintf(stderr, "fast-lowrate selftest count mismatch: normal=%d fast=%d\n",
			normalCount, fastCount);
		failures++;
	}
	for (i = 0; i < normalCount && i < fastCount; i++) {
		if (normal[i] != fast[i]) {
			fprintf(stderr, "fast-lowrate selftest mismatch at %d: normal=%d fast=%d\n",
				i, normal[i], fast[i]);
			failures++;
			break;
		}
	}
	if (!failures) {
		printf("fast-lowrate selftest passed: stride 4 selects the same positions "
			"as 44100->11025 normal decimation across chunk boundaries (%d samples)\n",
			normalCount);
		printf("note: --rate 8820/8287 uses fixed stride 5; 8287 intentionally differs "
			"from rational 44100->8287 normal --rate positions.\n");
	}
	return failures ? 1 : 0;
}



static int gSelftestVerbose;
static int gSelftestFdct32HalfDebug;

static const char *DescribeFdct32HalfDest(int destIndex, int offset, int oddBlock, int *row, const char **expr)
{
	int oddBase = oddBlock ? AMIGA_POLYPHASE_VBUF_LENGTH : 0;
	int evenBase = oddBlock ? 0 : AMIGA_POLYPHASE_VBUF_LENGTH;
	int delayOff = (offset - oddBlock) & 7;
	int highBase = offset + oddBase;
	int lowBase = 16 + delayOff + evenBase;
	static const char *highExpr[8] = {
		"buf[1]", "buf[9]+buf[13]", "buf[5]", "buf[13]+buf[11]",
		"buf[3]", "buf[11]+buf[15]", "buf[7]", "buf[15]"
	};
	static const char *lowExpr[8] = {
		"buf[1]", "buf[14]+buf[9]", "buf[6]", "buf[10]+buf[14]",
		"buf[2]", "buf[12]+buf[10]", "buf[4]", "buf[8]+buf[12]"
	};
	int k;
	if (destIndex == 64 * 16 + delayOff + evenBase || destIndex == 64 * 16 + delayOff + evenBase + 8) {
		*row = 0;
		*expr = "buf[0]";
		return "CENTRE";
	}
	for (k = 0; k < 8; k++) {
		if (destIndex == highBase + 128 * k || destIndex == highBase + 128 * k + 8) {
			*row = 16 + 2 * k;
			*expr = highExpr[k];
			return "HIGH";
		}
		if (destIndex == lowBase + 128 * k || destIndex == lowBase + 128 * k + 8) {
			*row = 16 - 2 * k;
			*expr = lowExpr[k];
			return "LOW";
		}
	}
	*row = -1;
	*expr = "unmapped destination";
	return "UNKNOWN";
}

static void PrintFdct32HalfDebug(unsigned long index, int destIndex, int offset, int oddBlock, int gb,
	const int *cbuf, const int *hbuf, const int *xbuf, int fullValue, int cHalfValue, int asmHalfValue)
{
	int row;
	const char *expr;
	const char *lane = DescribeFdct32HalfDest(destIndex, offset, oddBlock, &row, &expr);
	printf("FDCT32Half debug first mismatch: case=%lu offset=%d oddBlock=%d gb=%d dest=%d lane=%s row=%d\n",
		index, offset, oddBlock, gb, destIndex, lane, row);
	printf("  C half=%ld asm half=%ld full FDCT32=%ld dependency=%s\n",
		(long)cHalfValue, (long)asmHalfValue, (long)fullValue, expr);
	printf("  C intermediate buf[0..15]:");
	for (row = 0; row < 16; row++) printf(" %ld", (long)hbuf[row]);
	printf("\n  asm intermediate buf[0..15]:");
	for (row = 0; row < 16; row++) printf(" %ld", (long)xbuf[row]);
	printf("\n  full intermediate buf[0..31]:");
	for (row = 0; row < 32; row++) printf(" %ld", (long)cbuf[row]);
	printf("\n");
}

static int TestFdct32Case(unsigned long index, unsigned long seed, int offset,
	int oddBlock, int gb)
{
	static int cbuf[32];
	static int abuf[32];
	static int hbuf[32];
	static int cdest[4096];
	static int adest[4096];
	static int hdest[4096];
	static int xbuf[32];
	static int xdest[4096];
	int i;
	int halfWrites;
	int failed;

	failed = 0;
	for (i = 0; i < 32; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		cbuf[i] = ((int)seed) >> 8;
		abuf[i] = cbuf[i];
		hbuf[i] = cbuf[i];
		xbuf[i] = cbuf[i];
	}
	for (i = 0; i < 4096; i++) {
		cdest[i] = (int)(0x55aa0000UL ^ (unsigned long)i);
		adest[i] = cdest[i];
		hdest[i] = cdest[i];
		xdest[i] = cdest[i];
	}

	AMIGA_FDCT32_C_REFERENCE(cbuf, cdest, offset, oddBlock, gb);
	AMIGA_FDCT32(abuf, adest, offset, oddBlock, gb);
	AMIGA_FDCT32_HALF(hbuf, hdest, offset, oddBlock, gb);
	AMIGA_FDCT32_HALF_TEST_ACTIVE(xbuf, xdest, offset, oddBlock, gb);

	for (i = 0; i < 32; i++) {
		if (abuf[i] != cbuf[i]) {
			printf("FDCT32 buffer mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)cbuf[i], (long)abuf[i], offset, oddBlock, gb);
			failed = 1;
			if (!gSelftestVerbose) return -1;
		}
	}
	for (i = 0; i < 4096; i++) {
		if (adest[i] != cdest[i]) {
			printf("FDCT32 dest mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)cdest[i], (long)adest[i], offset, oddBlock, gb);
			failed = 1;
			if (!gSelftestVerbose) return -1;
		}
	}
	halfWrites = 0;
	for (i = 0; i < 4096; i++) {
		if (hdest[i] != (int)(0x55aa0000UL ^ (unsigned long)i)) {
			halfWrites++;
			if (hdest[i] != cdest[i]) {
				printf("FDCT32 half dest mismatch %lu[%d]: full=%ld half=%ld offset=%d odd=%d gb=%d\n",
					index, i, (long)cdest[i], (long)hdest[i], offset, oddBlock, gb);
				failed = 1;
				if (!gSelftestVerbose) return -1;
			}
		}
	}
	for (i = 0; i < 4096; i++) {
		if (xdest[i] != hdest[i]) {
			printf("FDCT32 half asm dest mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)hdest[i], (long)xdest[i], offset, oddBlock, gb);
			if (gSelftestFdct32HalfDebug)
				PrintFdct32HalfDebug(index, i, offset, oddBlock, gb, cbuf, hbuf, xbuf, cdest[i], hdest[i], xdest[i]);
			failed = 1;
			if (!gSelftestVerbose) return -1;
		}
	}
	if (halfWrites != 34) {
		printf("FDCT32 half write count mismatch %lu: got=%d expected=34 offset=%d odd=%d gb=%d\n",
			index, halfWrites, offset, oddBlock, gb);
		failed = 1;
		if (!gSelftestVerbose) return -1;
	}
	return failed ? -1 : 0;
}

static int SelftestFdct32Half(void);

static int SelftestFdct32(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;

	failures = 0;
	seed = 0x31415926UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (TestFdct32Case(i, seed, (int)(seed & 7), (int)((seed >> 3) & 1),
			(int)((seed >> 4) % 8)) != 0)
			failures++;
	}

	printf("FDCT32 asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_FDCT32
		"yes"
#else
		"no"
#endif
	);
	printf("FDCT32 asm active: %s\n", AMIGA_FDCT32_HAS_ASM() ? "yes" : "no");
	printf("FDCT32 selftest cases: %lu\n", i);
	printf("FDCT32 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int SelftestFdct32Half(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;

	failures = 0;
	seed = 0x32483248UL;
	i = 0;
	{
		int offset, oddBlock, gb, variant;
		for (variant = 0; variant < 32; variant++) {
			for (gb = 0; gb < 8; gb++) {
				for (oddBlock = 0; oddBlock < 2; oddBlock++) {
					for (offset = 0; offset < 8; offset++) {
						seed = seed * 1664525UL + 1013904223UL;
						if (TestFdct32Case(i, seed, offset, oddBlock, gb) != 0)
							failures++;
						i++;
					}
				}
			}
		}
	}

	printf("FDCT32Half asm requested: %s\n",
#if defined(AMIGA_M68K_ASM_FDCT32) && !defined(AMIGA_FORCE_FDCT32_HALF_C)
		"yes"
#else
		"no"
#endif
	);
	printf("FDCT32Half asm active: %s\n", AMIGA_FDCT32_HALF_HAS_ASM() ? "yes" : "no");
	printf("FDCT32Half selftest cases: %lu\n", i);
	printf("FDCT32Half selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static void FillImdctSentinel(int *y)
{
	int i;
	for (i = 0; i < AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS; i++)
		y[i] = (int)(0x13570000UL ^ (unsigned long)i);
}

static int TestImdctCase(unsigned long index, int pattern, unsigned long seed,
	int btCurr, int btPrev, int blockIdx, int gb)
{
	static int cx[18];
	static int ax[18];
	static int cp[9];
	static int ap[9];
	static int cy[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	static int ay[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	int i;
	int cm;
	int am;

	for (i = 0; i < 18; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cx[i] = 0;
		else if (pattern == 1)
			cx[i] = ((int)seed) >> 10;
		else
			cx[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		ax[i] = cx[i];
	}
	for (i = 0; i < 9; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cp[i] = 0;
		else if (pattern == 1)
			cp[i] = ((int)seed) >> 12;
		else
			cp[i] = (i & 1) ? 0x01ffffff : (int)0xfe000000UL;
		ap[i] = cp[i];
	}
	FillImdctSentinel(cy);
	FillImdctSentinel(ay);

	cm = AMIGA_IMDCT36_C_REFERENCE(cx, cp, cy + blockIdx, btCurr, btPrev, blockIdx, gb);
	am = AMIGA_IMDCT36_TEST_ACTIVE(ax, ap, ay + blockIdx, btCurr, btPrev, blockIdx, gb);
	if (am != cm) {
		printf("IMDCT36 mOut mismatch %lu: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
			index, (long)cm, (long)am, btCurr, btPrev, blockIdx, gb);
		return -1;
	}
	for (i = 0; i < 18; i++) {
		if (ax[i] != cx[i]) {
			printf("IMDCT36 input mismatch %lu[%d]: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cx[i], (long)ax[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	for (i = 0; i < 9; i++) {
		if (ap[i] != cp[i]) {
			printf("IMDCT36 overlap mismatch %lu[%d]: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cp[i], (long)ap[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS; i++) {
		if (ay[i] != cy[i]) {
			printf("IMDCT36 output mismatch %lu[%d]: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cy[i], (long)ay[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	return 0;
}

static int TestAntialiasCase(unsigned long index, unsigned long seed, int pattern, int nBfly)
{
	static int cx[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	static int ax[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	int i;
	int nSamps;

	nSamps = (nBfly + 1) * AMIGA_IMDCT_BLOCK_SIZE;
	for (i = 0; i < nSamps; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cx[i] = 0;
		else if (pattern == 1)
			cx[i] = ((int)seed) >> 7;
		else
			cx[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		ax[i] = cx[i];
	}

	AMIGA_ANTIALIAS_C_REFERENCE(cx, nBfly);
	AMIGA_ANTIALIAS_TEST_ACTIVE(ax, nBfly);

	for (i = 0; i < nSamps; i++) {
		if (ax[i] != cx[i]) {
			printf("AntiAlias mismatch %lu[%d]: first=%ld second=%ld nBfly=%d pattern=%d\n",
				index, i, (long)cx[i], (long)ax[i], nBfly, pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestAntialias(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int nBfly;

	failures = 0;
	seed = 0x0aa51aa5UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		nBfly = (int)(seed % AMIGA_IMDCT_NBANDS);
		if (TestAntialiasCase(i, seed, pattern, nBfly) != 0)
			failures++;
	}

	printf("AntiAlias asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_ANTIALIAS
		"yes"
#else
		"no"
#endif
	);
	printf("AntiAlias asm active: %s\n", AMIGA_ANTIALIAS_HAS_ASM() ? "yes" : "no");
	printf("AntiAlias selftest cases: %lu\n", 4096UL);
	printf("AntiAlias selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int SelftestImdct(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	unsigned long fallbackCases;
	int pattern;
	int btCurr;
	int btPrev;
	int blockIdx;
	int gb;
	
	failures = 0;
	fallbackCases = 0;
	seed = 0x27182818UL;

	/* Zero, edge-value, and deterministic random long-block cases. */
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		btCurr = 0;
		btPrev = 0;
		blockIdx = (int)((seed >> 8) & 31);
		gb = (int)((seed >> 13) % 8);
		if (TestImdctCase(i, pattern, seed, btCurr, btPrev, blockIdx, gb) != 0)
			failures++;
	}

	/* Non-common long windows, and the block types used around mixed/short transitions,
	 * must route through the C fallback and remain bit-identical.
	 */
	for (i = 0; i < 256UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (int)(seed % 3UL);
		/* Cycle through every non-common current/previous window pair. */
		btCurr = (int)((i >> 2) & 3UL);
		btPrev = (int)(i & 3UL);
		if (btCurr == 0 && btPrev == 0)
			btPrev = 1;
		blockIdx = (int)((seed >> 10) & 31);
		gb = (int)((seed >> 15) % 8);
		if (TestImdctCase(4096UL + i, pattern, seed, btCurr, btPrev, blockIdx, gb) != 0)
			failures++;
		fallbackCases++;
	}

	printf("IMDCT asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_IMDCT
		"yes"
#else
		"no"
#endif
	);
	printf("IMDCT asm active: %s\n", AMIGA_IMDCT36_HAS_ASM() ? "yes" : "no");
	printf("IMDCT selftest long cases: %lu\n", 4096UL);
	printf("IMDCT selftest fallback cases: %lu\n", fallbackCases);
	printf("IMDCT selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int TestPolyphaseCase(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x6000 + i);
		apcm[i] = (short)(0x6000 + i);
	}

	AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	AMIGA_POLYPHASE_MONO_FAST_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphase(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x16180339UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		if (TestPolyphaseCase(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase asm active: %s\n", AMIGA_POLYPHASE_MONO_FAST_HAS_ASM() ? "yes" : "no");
	printf("Polyphase selftest cases: %lu\n", i);
	printf("Polyphase selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestPolyphaseStride2Case(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x7000 + i);
		apcm[i] = (short)(0x7000 + i);
	}

	ccount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	if (ccount != 16 || acount != 16) {
		printf("PolyphaseMonoFast stride2 count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast stride2 vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast stride2 output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride2(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x27182818UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		if (TestPolyphaseStride2Case(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride2 asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride2 asm active: %s\n",
		AMIGA_POLYPHASE_MONO_FAST_STRIDE2_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride2 selftest cases: %lu\n", i);
	printf("Polyphase stride2 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestPolyphaseStride4Case(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x7100 + i);
		apcm[i] = (short)(0x7100 + i);
	}

	ccount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	if (ccount != 8 || acount != 8) {
		printf("PolyphaseMonoFast stride4 count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast stride4 vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast stride4 output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride4(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x31415926UL;
	for (i = 0; i < 500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 8UL) ? 0 : ((i < 16UL) ? 2 : 1);
		if (TestPolyphaseStride4Case(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride4 asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride4 asm active: %s\n",
		AMIGA_POLYPHASE_MONO_FAST_STRIDE4_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride4 selftest cases: %lu\n", i);
	printf("Polyphase stride4 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestPolyphaseStride4StereoCase(unsigned long index, unsigned long seed, int pattern, int phase)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short apcm[AMIGA_POLYPHASE_NBANDS * 2];
	int ccount;
	int acount;
	int i;
	int lane;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0) {
			cvbuf[i] = 0;
		} else if (pattern == 1) {
			cvbuf[i] = ((int)seed) >> 9;
		} else if (pattern == 2) {
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		} else if (pattern == 3) {
			cvbuf[i] = (i == (int)((index + (unsigned long)phase * 37UL) %
				AMIGA_POLYPHASE_VBUF_LENGTH)) ? 0x02000000 : 0;
		} else {
			/* Left/right asymmetric stereo addressing check: every 64-int
			 * block stores 32 left entries followed by 32 right entries.
			 */
			lane = i & 63;
			if (lane < 32)
				cvbuf[i] = (int)(0x01000000 + ((i * 97) & 0x000fffff));
			else
				cvbuf[i] = (int)(0xff000000UL + ((i * 193) & 0x000fffff));
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		cpcm[i] = (short)(0x7200 + i);
		apcm[i] = (short)(0x7200 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF, phase);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF, phase);

	if (ccount != 16 || acount != 16) {
		printf("PolyphaseStereoFast stride4 count mismatch %lu phase=%d: first=%d second=%d pattern=%d\n",
			index, phase, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride4 vbuf mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseStereoFast stride4 output mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_POLYPHASE)
static void InitPolyphaseStride4StereoDiagnosticVector(int *vbuf, short *pcm,
	unsigned long seed, int pattern, int phase)
{
	int i;
	int lane;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			vbuf[i] = 0;
		else if (pattern == 1)
			vbuf[i] = ((int)seed) >> 9;
		else if (pattern == 2)
			vbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		else if (pattern == 3)
			vbuf[i] = (i == (phase * 41 + 17)) ? 0x02000000 : 0;
		else {
			lane = i & 63;
			vbuf[i] = (lane < 32) ?
				(int)(0x01000000 + ((i * 97) & 0x000fffff)) :
				(int)(0xff000000UL + ((i * 193) & 0x000fffff));
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++)
		pcm[i] = (short)(0x7300 + i);
}

static int PolyphaseStride4StereoDiagnosticMatches(const short *refPcm,
	const int *refVbuf, int refCount, const short *testPcm,
	const int *testVbuf, int testCount)
{
	int i;

	if (testCount != refCount)
		return 0;
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		if (testPcm[i] != refPcm[i])
			return 0;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (testVbuf[i] != refVbuf[i])
			return 0;
	}
	return 1;
}

static void SelftestPolyphaseStride4StereoKernelMapping(void)
{
	static int baseVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int refVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int kernelVbuf[4][AMIGA_POLYPHASE_VBUF_LENGTH];
	static int dispatcherVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short basePcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short refPcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short kernelPcm[4][AMIGA_POLYPHASE_NBANDS * 2];
	static short dispatcherPcm[AMIGA_POLYPHASE_NBANDS * 2];
	int matches[4][4];
	int dispatcherMatches[4];
	int phase;
	int kernel;
	int i;
	int refCount;
	int dispatcherCount;
	void (*kernelFns[4])(short *, int *, const int *);

	if (!StereoFastPolyphaseStride4Half_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase0_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase1_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase2_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase3_Amiga_m68k)
		return;

	kernelFns[0] = StereoFastPolyphaseStride4Phase0_Amiga_m68k;
	kernelFns[1] = StereoFastPolyphaseStride4Phase1_Amiga_m68k;
	kernelFns[2] = StereoFastPolyphaseStride4Phase2_Amiga_m68k;
	kernelFns[3] = StereoFastPolyphaseStride4Phase3_Amiga_m68k;

	for (phase = 0; phase < 4; phase++) {
		for (kernel = 0; kernel < 4; kernel++)
			matches[phase][kernel] = 1;
		dispatcherMatches[phase] = 1;
	}

	if (StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts) {
		for (phase = 0; phase < 4; phase++)
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[phase] = 0;
	}

	for (phase = 0; phase < 4; phase++) {
		for (i = 0; i < 5; i++) {
			InitPolyphaseStride4StereoDiagnosticVector(baseVbuf, basePcm,
				0x2468ace0UL + (unsigned long)i * 0x10203UL, i, phase);
			memcpy(refVbuf, baseVbuf, sizeof(refVbuf));
			memcpy(refPcm, basePcm, sizeof(refPcm));
			refCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE(refPcm,
				refVbuf, AMIGA_POLY_COEF, phase);
			for (kernel = 0; kernel < 4; kernel++) {
				memcpy(kernelVbuf[kernel], baseVbuf, sizeof(baseVbuf));
				memcpy(kernelPcm[kernel], basePcm, sizeof(basePcm));
				kernelFns[kernel](kernelPcm[kernel], kernelVbuf[kernel],
					AMIGA_POLY_COEF);
				if (!PolyphaseStride4StereoDiagnosticMatches(refPcm, refVbuf,
					refCount, kernelPcm[kernel], kernelVbuf[kernel], 16))
					matches[phase][kernel] = 0;
			}
			memcpy(dispatcherVbuf, baseVbuf, sizeof(baseVbuf));
			memcpy(dispatcherPcm, basePcm, sizeof(basePcm));
			StereoFastPolyphaseStride4Half_Amiga_m68k(dispatcherPcm,
				dispatcherVbuf, AMIGA_POLY_COEF, phase);
			dispatcherCount = 16;
			if (!PolyphaseStride4StereoDiagnosticMatches(refPcm, refVbuf,
				refCount, dispatcherPcm, dispatcherVbuf, dispatcherCount))
				dispatcherMatches[phase] = 0;
		}
	}

	printf("Polyphase stride4 stereo direct kernel mapping diagnostic:\n");
	for (phase = 0; phase < 4; phase++) {
		printf("logical phase %d: kernel0=%s kernel1=%s kernel2=%s kernel3=%s dispatcher=%s\n",
			phase,
			matches[phase][0] ? "yes" : "no",
			matches[phase][1] ? "yes" : "no",
			matches[phase][2] ? "yes" : "no",
			matches[phase][3] ? "yes" : "no",
			dispatcherMatches[phase] ? "yes" : "no");
	}
	if (StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts) {
		printf("Polyphase stride4 stereo dispatcher phase counts: phase0=%lu phase1=%lu phase2=%lu phase3=%lu\n",
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[0],
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[1],
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[2],
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[3]);
	}
}
#else
static void SelftestPolyphaseStride4StereoKernelMapping(void)
{
}
#endif

static int SelftestPolyphaseStride4Stereo(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int phase;

	failures = 0;
	seed = 0x57721566UL;
	for (i = 0; i < 500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (i < 8UL)
			pattern = 0;
		else if (i < 16UL)
			pattern = 3;
		else if (i < 24UL)
			pattern = 2;
		else if (i < 32UL)
			pattern = 4;
		else
			pattern = 1;
		for (phase = 0; phase < 4; phase++) {
			if (TestPolyphaseStride4StereoCase(i, seed, pattern, phase) != 0)
				failures++;
		}
	}

	SelftestPolyphaseStride4StereoKernelMapping();
	printf("Polyphase stride4 stereo selftest patterns: zero, impulse, alternating extremes, left/right asymmetric, deterministic random\n");
	printf("Polyphase stride4 stereo selftest cases: %lu\n", i * 4UL);
	printf("Polyphase stride4 stereo selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int TestPolyphaseStride2StereoCase(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[40];
	static short apcm[40];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		switch (pattern) {
		case 0: cvbuf[i] = 0; break;
		case 1: cvbuf[i] = ((int)seed) >> 9; break;
		case 2: cvbuf[i] = 0x03ffffff; break;
		case 3: cvbuf[i] = (int)0xfc000000UL; break;
		case 4: cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL; break;
		case 5: cvbuf[i] = (i & 32) ? 0 : (((int)seed) >> 9); break;
		case 6: cvbuf[i] = (i & 32) ? (((int)seed) >> 9) : 0; break;
		case 8: cvbuf[i] = (i & 32) ? 0 : (0x00100000 + ((i & 31) << 12)); break;
		case 9: cvbuf[i] = (i & 32) ? (0xffe00000 + ((i & 31) << 11)) : 0; break;
		default: cvbuf[i] = (i & 32) ? (((int)(seed ^ 0x55aa33ccUL)) >> 8) : (((int)seed) >> 10); break;
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < 40; i++) {
		cpcm[i] = (short)(0x7300 + i);
		apcm[i] = (short)(0x7300 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_C_REFERENCE(cpcm + 4, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_TEST_ACTIVE(apcm + 4, avbuf, AMIGA_POLY_COEF);

	if (ccount != 32 || acount != 32) {
		printf("PolyphaseStereoFast stride2 count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride2 vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < 40; i++) {
		if (apcm[i] != cpcm[i]) {
			int j;
			printf("PolyphaseStereoFast stride2 output/sentinel mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			printf("PolyphaseStereoFast stride2 diagnostic: expected L0=%ld actual L0=%ld expected R0=%ld actual R0=%ld\n",
				(long)cpcm[4], (long)apcm[4], (long)cpcm[5], (long)apcm[5]);
			printf("PolyphaseStereoFast stride2 diagnostic: left vbuf base=%p right vbuf base=%p phase=0 vindex=0\n",
				(void *)cvbuf, (void *)(cvbuf + 32));
			printf("PolyphaseStereoFast stride2 expected/actual packed LR samples:\n");
			for (j = 0; j < 32; j++)
				printf("  [%02d] expected=%ld actual=%ld%s\n", j,
					(long)cpcm[4 + j], (long)apcm[4 + j],
					(j & 1) ? " R" : " L");
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride2Stereo(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x66260700UL;
	if (TestPolyphaseStride2StereoCase(0, seed, 8) != 0)
		failures++;
	if (TestPolyphaseStride2StereoCase(1, seed ^ 0x13579bdfUL, 9) != 0)
		failures++;
	for (i = 0; i < 512UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 8UL) ? (int)i : 1;
		if (TestPolyphaseStride2StereoCase(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride2 stereo asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride2 stereo asm active: %s\n",
		AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride2 stereo selftest cases: %lu\n", i);
	printf("Polyphase stride2 stereo selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static double SqrtApprox(double x)
{
	double g;
	int i;

	if (x <= 0.0)
		return 0.0;
	g = x >= 1.0 ? x : 1.0;
	for (i = 0; i < 24; i++)
		g = 0.5 * (g + x / g);
	return g;
}

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_FDCT32_QUARTER)
static int Fdct32QuarterIsActiveIndex(const int *active, int nactive, int idx)
{
	int i;
	for (i = 0; i < nactive; i++) {
		if (active[i] == idx)
			return 1;
	}
	return 0;
}

#endif

static int SelftestFdct32Quarter(void)
{
#if !(defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_FDCT32_QUARTER))
	printf("FDCT32Quarter compile flag: no\n");
	printf("FDCT32Quarter selftest not run: quarter FDCT body is not compiled in this build\n");
	MP3SetExperimentalFDCT32Quarter(1);
	printf("FDCT32Quarter stride gate: stride 2 call=%s, stride 4 call=%s\n",
		(2 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no",
		(4 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no");
	MP3SetExperimentalFDCT32Quarter(0);
	printf("FDCT32Quarter selftest PASS (unavailable in this build)\n");
	return 0;
#else
	enum { CASES = 500, DEST_WORDS = 4096, ACTIVE = 9 };
	static int hbuf[32];
	static int qbuf[32];
	static int hdest[DEST_WORDS];
	static int qdest[DEST_WORDS];
	unsigned long seed;
	unsigned long i;
	unsigned long activeScatterMismatches;
	unsigned long staleMismatches;
	double squares;
	double samples;
	int j;

	seed = 0x4d504733UL;
	activeScatterMismatches = 0;
	staleMismatches = 0;
	squares = 0.0;
	samples = 0.0;

	for (i = 0; i < CASES; i++) {
		int offset;
		int oddBlock;
		int gb;
		int phase;
		int oddBase;
		int evenBase;
		int delayOff;
		int active[ACTIVE];

		offset = (int)(i & 7UL);
		oddBlock = (int)((i >> 3) & 1UL);
		gb = (int)((i >> 4) & 7UL);
		phase = (int)((i >> 7) & 3UL);
		for (j = 0; j < 32; j++) {
			seed = seed * 1664525UL + 1013904223UL;
			if (j < 8)
				hbuf[j] = ((int)seed) >> 9;
			else
				hbuf[j] = 0;
			qbuf[j] = hbuf[j];
		}
		for (j = 0; j < DEST_WORDS; j++) {
			hdest[j] = (int)(0x55aa0000UL ^ (unsigned long)j);
			qdest[j] = hdest[j];
		}

		oddBase = oddBlock ? AMIGA_POLYPHASE_VBUF_LENGTH : 0;
		evenBase = oddBlock ? 0 : AMIGA_POLYPHASE_VBUF_LENGTH;
		delayOff = (offset - oddBlock) & 7;
		active[0] = 64 * 16 + delayOff + evenBase;
		active[1] = offset + oddBase + 64 * 0;
		active[2] = offset + oddBase + 64 * 4;
		active[3] = offset + oddBase + 64 * 8;
		active[4] = offset + oddBase + 64 * 12;
		active[5] = 16 + delayOff + evenBase + 64 * 0;
		active[6] = 16 + delayOff + evenBase + 64 * 4;
		active[7] = 16 + delayOff + evenBase + 64 * 8;
		active[8] = 16 + delayOff + evenBase + 64 * 12;

		AMIGA_FDCT32_HALF(hbuf, hdest, offset, oddBlock, gb);
		AMIGA_FDCT32_QUARTER(qbuf, qdest, offset, oddBlock, gb, phase, 4);

		for (j = 0; j < ACTIVE; j++) {
			int idx = active[j];
			if (hdest[idx] == (int)(0x55aa0000UL ^ (unsigned long)idx) ||
				qdest[idx] == (int)(0x55aa0000UL ^ (unsigned long)idx) ||
				hdest[idx + 8] != hdest[idx] || qdest[idx + 8] != qdest[idx])
				activeScatterMismatches++;
			else {
				double d = (double)hdest[idx] - (double)qdest[idx];
				squares += d * d;
				samples += 1.0;
			}
		}
		for (j = 0; j < 16; j++) {
			int idx = offset + oddBase + 64 * j;
			if (!Fdct32QuarterIsActiveIndex(active, ACTIVE, idx) &&
				(qdest[idx] != 0 || qdest[idx + 8] != 0))
				staleMismatches++;
			idx = 16 + delayOff + evenBase + 64 * j;
			if (!Fdct32QuarterIsActiveIndex(active, ACTIVE, idx) &&
				(qdest[idx] != 0 || qdest[idx + 8] != 0))
				staleMismatches++;
		}
	}

	printf("FDCT32Quarter compile flag: %s\n",
#ifdef AMIGA_FAST_FDCT32_QUARTER
		"yes"
#else
		"no"
#endif
	);
	printf("FDCT32Quarter selftest cases: %lu\n", (unsigned long)CASES);
	printf("FDCT32Quarter active scatter positions: 9 (mismatches: %lu)\n",
		activeScatterMismatches);
	printf("FDCT32Quarter stale quarter-rate row clears: %lu mismatches\n",
		staleMismatches);
	printf("FDCT32Quarter RMS difference vs FDCT32Half active rows: %.2f counts\n",
		SqrtApprox(squares / (samples > 0.0 ? samples : 1.0)));
	MP3SetExperimentalFDCT32Quarter(1);
	printf("FDCT32Quarter stride gate: stride 2 call=%s, stride 4 call=%s\n",
		(2 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no",
		(4 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no");
	MP3SetExperimentalFDCT32Quarter(0);
	printf("FDCT32Quarter selftest PASS (lossy approximation)\n");
	return 0;
#endif
}


static int SelftestReducedTaps(void)
{
	enum { CASES = 500 };
	static int vbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short fullMono[AMIGA_POLYPHASE_NBANDS];
	static short reducedMono[AMIGA_POLYPHASE_NBANDS];
	static short fullStereo[AMIGA_POLYPHASE_NBANDS * 2];
	static short reducedStereo[AMIGA_POLYPHASE_NBANDS * 2];
	unsigned long seed;
	unsigned long i;
	unsigned long monoCountMismatches;
	unsigned long stereoCountMismatches;
	double monoSquares;
	double stereoSquares;
	double monoSamples;
	double stereoSamples;
	int j;

	seed = 0x8a7c4d11UL;
	monoCountMismatches = 0;
	stereoCountMismatches = 0;
	monoSquares = 0.0;
	stereoSquares = 0.0;
	monoSamples = 0.0;
	stereoSamples = 0.0;

	for (i = 0; i < CASES; i++) {
		int phase;
		int fullMonoCount;
		int reducedMonoCount;
		int fullStereoCount;
		int reducedStereoCount;

		phase = (int)(i & 3UL);
		for (j = 0; j < AMIGA_POLYPHASE_VBUF_LENGTH; j++) {
			seed = seed * 1664525UL + 1013904223UL;
			vbuf[j] = ((int)seed) >> 9;
		}
		for (j = 0; j < AMIGA_POLYPHASE_NBANDS; j++) {
			fullMono[j] = (short)(0x7300 + j);
			reducedMono[j] = (short)(0x7400 + j);
		}
		for (j = 0; j < AMIGA_POLYPHASE_NBANDS * 2; j++) {
			fullStereo[j] = (short)(0x7500 + j);
			reducedStereo[j] = (short)(0x7600 + j);
		}

		fullMonoCount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_C_REFERENCE(
			fullMono, vbuf, AMIGA_POLY_COEF);
		reducedMonoCount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_REDUCED_TEST_ACTIVE(
			reducedMono, vbuf, AMIGA_POLY_COEF, 0);
		fullStereoCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE(
			fullStereo, vbuf, AMIGA_POLY_COEF, phase);
		reducedStereoCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_TEST_ACTIVE(
			reducedStereo, vbuf, AMIGA_POLY_COEF, phase);

		if (fullMonoCount != 8 || reducedMonoCount != 8)
			monoCountMismatches++;
		if (fullStereoCount != 16 || reducedStereoCount != 16)
			stereoCountMismatches++;
		if (reducedMonoCount == 8) {
			for (j = 0; j < 8; j++) {
				double d = (double)((int)fullMono[j] - (int)reducedMono[j]);
				monoSquares += d * d;
				monoSamples += 1.0;
			}
		}
		if (reducedStereoCount == 16) {
			for (j = 0; j < 16; j++) {
				double d = (double)((int)fullStereo[j] - (int)reducedStereo[j]);
				stereoSquares += d * d;
				stereoSamples += 1.0;
			}
		}
	}

	printf("Reduced taps compile flag: %s\n",
#ifdef AMIGA_FAST_REDUCED_TAPS
		"yes"
#else
		"no"
#endif
	);
	printf("Reduced taps selftest cases: %lu\n", (unsigned long)CASES);
	printf("Reduced taps mono output samples per call: 8 (mismatches: %lu)\n",
		monoCountMismatches);
	printf("Reduced taps stereo output frames per call: 8 (16 shorts, mismatches: %lu)\n",
		stereoCountMismatches);
	printf("Reduced taps mono RMS difference: %.2f counts (target < 500)\n",
		SqrtApprox(monoSquares / (monoSamples > 0.0 ? monoSamples : 1.0)));
	printf("Reduced taps stereo RMS difference: %.2f counts (target < 500)\n",
		SqrtApprox(stereoSquares / (stereoSamples > 0.0 ? stereoSamples : 1.0)));
	printf("Reduced taps selftest PASS (lossy approximation)\n");
	return 0;
}

static int SelftestMonoFastLowrateStereo(void)
{
	enum {
		IN_CHANNELS = 2,
		OUT_CHANNELS = 1,
		TOTAL_FRAMES = 44100,
		CHUNK_FRAMES = 1152,
		STRIDE = 4,
		EXPECTED = TOTAL_FRAMES / STRIDE
	};
	short input[CHUNK_FRAMES * IN_CHANNELS];
	short lowrate[CHUNK_FRAMES * IN_CHANNELS];
	short mono[CHUNK_FRAMES];
	DecodeOptions opt;
	DecodeStats stats;
	int phase;
	int offset;
	int failures;

	memset(&opt, 0, sizeof(opt));
	memset(&stats, 0, sizeof(stats));
	opt.mono = 1;
	opt.fastLowrate = 1;
	opt.outputRate = 11025;
	stats.sampleRate = 44100;
	stats.outputSampleRate = 11025;
	stats.channels = IN_CHANNELS;
	stats.outputChannels = OUT_CHANNELS;
	phase = 0;
	failures = 0;

	for (offset = 0; offset < TOTAL_FRAMES; offset += CHUNK_FRAMES) {
		int frames;
		int i;
		int selected;
		int mixed;

		frames = TOTAL_FRAMES - offset;
		if (frames > CHUNK_FRAMES)
			frames = CHUNK_FRAMES;
		for (i = 0; i < frames; i++) {
			int frame = offset + i;
			input[2 * i] = (short)((frame % 251) * 101 - 12000);
			input[2 * i + 1] = (short)(12000 - (frame % 197) * 97);
		}
		selected = FastLowrateSelectFrame(&phase, input, lowrate,
			frames * IN_CHANNELS, STRIDE, IN_CHANNELS);
		mixed = MixFrame(lowrate, mono, selected, IN_CHANNELS, 1);
		stats.outputSamples += (unsigned long)mixed;
	}

	if (stats.outputSamples != EXPECTED) {
		fprintf(stderr,
			"mono fast-lowrate stereo selftest count mismatch: got=%lu expected=%d\n",
			stats.outputSamples, EXPECTED);
		failures++;
	}
	if (PerChannelEmittedSamples(&opt, &stats) != EXPECTED) {
		fprintf(stderr,
			"mono fast-lowrate stereo selftest per-channel mismatch: got=%lu expected=%d\n",
			PerChannelEmittedSamples(&opt, &stats), EXPECTED);
		failures++;
	}
	if (DecodedAudioSeconds(&opt, &stats) < 0.999 ||
		DecodedAudioSeconds(&opt, &stats) > 1.001) {
		fprintf(stderr,
			"mono fast-lowrate stereo selftest seconds mismatch: got=%.6f expected=1.000000\n",
			DecodedAudioSeconds(&opt, &stats));
		failures++;
	}
	if (!failures)
		printf("mono fast-lowrate stereo selftest passed: 44100 Hz stereo -> 11025 Hz mono emitted %lu samples\n",
			stats.outputSamples);
	return failures ? 1 : 0;
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

static int SelftestHuffman(const DecodeOptions *opt)
{
	enum { HUFFMAN_SELFTEST_CASES = 1000, HUFFMAN_PAIR_TABS = 32 };
	static unsigned char buf[128];
	static int cxy[MAX_NSAMP];
	static int axy[MAX_NSAMP];
	unsigned long seed;
	unsigned long failures;
	unsigned long i;
	int j;

	seed = 0x68756666UL;
	failures = 0;
	for (i = 0; i < HUFFMAN_SELFTEST_CASES; i++) {
		int nVals;
		int tabIdx;
		int bitsLeft;
		int bitOffset;
		int cret;
		int aret;

		for (j = 0; j < (int)sizeof(buf); j++) {
			if (j < (int)sizeof(buf) - 8)
				buf[j] = (unsigned char)(NextRand32(&seed) >> 24);
			else
				buf[j] = 0;
		}
		for (j = 0; j < MAX_NSAMP; j++) {
			cxy[j] = (int)(0x5a5a0000UL ^ (unsigned long)j);
			axy[j] = cxy[j];
		}

		tabIdx = (int)(NextRand32(&seed) % HUFFMAN_PAIR_TABS);
		nVals = (int)(NextRand32(&seed) % ((MAX_NSAMP / 2) + 1)) * 2;
		bitsLeft = (int)(NextRand32(&seed) % 769UL);
		bitOffset = (int)(NextRand32(&seed) & 7UL);

		cret = AMIGA_HUFFMAN_PAIRS_C_REFERENCE(cxy, nVals, tabIdx, bitsLeft, buf, bitOffset);
		aret = AMIGA_HUFFMAN_PAIRS_TEST_ACTIVE(axy, nVals, tabIdx, bitsLeft, buf, bitOffset);
		if (aret != cret) {
			printf("Huffman selftest bitsUsed mismatch %lu: tab=%d nVals=%d bitsLeft=%d bitOffset=%d first=%d second=%d\n",
				i, tabIdx, nVals, bitsLeft, bitOffset, cret, aret);
			failures++;
			continue;
		}
		for (j = 0; j < nVals; j++) {
			if (axy[j] != cxy[j]) {
				printf("Huffman selftest xy mismatch %lu[%d]: tab=%d nVals=%d bitsLeft=%d bitOffset=%d first=%ld second=%ld bitsUsed=%d\n",
					i, j, tabIdx, nVals, bitsLeft, bitOffset, (long)cxy[j], (long)axy[j], cret);
				failures++;
				break;
			}
		}
	}

	printf("Huffman asm compiled: %s\n", AMIGA_HUFFMAN_PAIRS_HAS_ASM() ? "yes" : "no");
	printf("Huffman runtime default: C\n");
	printf("Huffman selftest candidate: %s\n", AMIGA_HUFFMAN_PAIRS_HAS_ASM() ? "asm" : "C");
	printf("Huffman forced by --exp-huff: %s\n", (opt && opt->expHuff) ? "yes" : "no");
	printf("Huffman asm active: %s\n", AMIGA_HUFFMAN_PAIRS_ASM_NOTE());
	printf("Huffman selftest cases: %lu\n", i);
	printf("Huffman selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int SelftestDequant(void)
{
	enum { DEQUANT_X_MAX = 8206 };
	unsigned long cases;
	unsigned long failures;
	int scale;

	cases = 0;
	failures = 0;
	for (scale = -47; scale <= 0; scale++) {
		int x;
		for (x = 0; x <= DEQUANT_X_MAX; x++) {
			int signCase;
			for (signCase = 0; signCase < (x ? 2 : 1); signCase++) {
				int cin;
				int ain;
				int cout;
				int aout;
				int cmask;
				int amask;

				cin = signCase ? (int)(0x80000000UL | (unsigned long)x) : x;
				ain = cin;
				cout = (int)0x55aa55aaUL;
				aout = (int)0xaa55aa55UL;
				cmask = AMIGA_DEQUANT_BLOCK_C_REFERENCE(&cin, &cout, 1, scale);
				amask = AMIGA_DEQUANT_BLOCK_TEST_ACTIVE(&ain, &aout, 1, scale);
				cases++;
				if (cmask != amask || cout != aout || cin != ain) {
					printf("Dequant selftest mismatch scale=%d x=%d sign=%d C(out=%ld mask=%ld in=%ld) active(out=%ld mask=%ld in=%ld)\n",
						scale, x, signCase, (long)cout, (long)cmask, (long)cin,
						(long)aout, (long)amask, (long)ain);
					failures++;
					if (failures >= 16) {
						printf("Dequant selftest stopped after 16 failures\n");
						printf("Dequant selftest cases: %lu\n", cases);
						printf("Dequant selftest failures: %lu\n", failures);
						return 1;
					}
				}
			}
		}
	}

	printf("Dequant asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_DEQUANT
		"yes"
#else
		"no"
#endif
	);
	printf("Dequant asm active: %s\n", AMIGA_DEQUANT_BLOCK_HAS_ASM() ? "yes" : "no");
	printf("Dequant selftest cases: %lu\n", cases);
	printf("Dequant selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int SelftestCLZReference(int x)
{
	unsigned int ux;
	int numZeros;

	ux = (unsigned int)x;
	if (!ux)
		return 32;
	numZeros = 0;
	while (!(ux & 0x80000000UL)) {
		numZeros++;
		ux <<= 1;
	}
	return numZeros;
}

static int TestCLZValue(int x, unsigned long index)
{
	int c;
	int a;

	c = SelftestCLZReference(x);
	a = CLZ(x);
	if (a != c) {
		printf("CLZ mismatch %lu: x=0x%08lx first=%ld second=%ld\n",
			index, (unsigned long)(unsigned int)x, (long)c, (long)a);
		return -1;
	}
	return 0;
}

static int SelftestClz(void)
{
	static const int edges[] = {
		0,
		1,
		0x7fffffffL,
		(int)0xffffffffUL
	};
	unsigned long failures;
	unsigned long tested;
	unsigned long seed;
	unsigned long i;

	failures = 0;
	tested = 0;
	seed = 0x636c7a21UL;

	for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		if (TestCLZValue(edges[i], tested) != 0)
			failures++;
		tested++;
	}

	for (i = 0; i < 32UL; i++) {
		int x = (int)(1UL << i);
		if (TestCLZValue(x, tested) != 0)
			failures++;
		tested++;
	}

	for (i = 0; i < 10000UL; i++) {
		int x = (int)NextRand32(&seed);
		if (TestCLZValue(x, tested) != 0)
			failures++;
		tested++;
	}

	printf("CLZ bfffo asm available: %s\n",
#if defined(CLZ_HAS_AMIGA_M68K_ASM) && CLZ_HAS_AMIGA_M68K_ASM
		"yes"
#else
		"no (C reference path only in this build)"
#endif
	);
	printf("CLZ selftest cases: %lu\n", tested);
	printf("CLZ selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
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


typedef struct DecodeStream {
	InputSource *input;
	HMP3Decoder decoder;
	unsigned char readBuf[READBUF_SIZE];
	unsigned char *readPtr;
	short decodeBuf[OUTBUF_SAMPS];
	short writeBuf[OUTBUF_SAMPS];
	short rateBuf[OUTBUF_SAMPS];
	union {
		signed char interleaved[OUTBUF_SAMPS];
		signed char planar[2][OUTBUF_SAMPS / 2];
	} spill;
	int spillPos;
	int spillCount;
	int planarSpillPos;
	int planarSpillCount;
	int bytesLeft;
	int eofReached;
	int outOfData;
	int decodeError;
	int effectiveRate;
	DecodeStats *stats;
	TimingStats *timing;
	RateState rateState;
} DecodeStream;

static void DecodeStreamInit(DecodeStream *stream, InputSource *input,
	HMP3Decoder decoder, DecodeStats *stats, TimingStats *timing)
{
	memset(stream, 0, sizeof(*stream));
	stream->input = input;
	stream->decoder = decoder;
	stream->readPtr = stream->readBuf;
	stream->stats = stats;
	stream->timing = timing;
}

static int DecodeStreamCopySpill(DecodeStream *stream, signed char *dest,
	int maxBytes, int *outBytes)
{
	int n;

	if (stream->spillPos >= stream->spillCount) {
		stream->spillPos = 0;
		stream->spillCount = 0;
		return 0;
	}
	n = stream->spillCount - stream->spillPos;
	if (n > maxBytes)
		n = maxBytes;
	memcpy(dest + *outBytes, stream->spill.interleaved + stream->spillPos, n);
	stream->spillPos += n;
	*outBytes += n;
	if (stream->spillPos >= stream->spillCount) {
		stream->spillPos = 0;
		stream->spillCount = 0;
	}
	return n;
}

static int DecodeStreamFillS8(DecodeStream *stream, const DecodeOptions *opt,
	signed char *dest, int maxBytes)
{
	MP3FrameInfo info;
	int produced;

	produced = 0;
	DecodeStreamCopySpill(stream, dest, maxBytes, &produced);
	while (produced < maxBytes && !stream->outOfData) {
		int nRead;
		int offset;
		int err;
		unsigned char *frameStart;
		int frameBytes;

		if (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
			nRead = FillReadBuffer(stream->readBuf, stream->readPtr,
				READBUF_SIZE, stream->bytesLeft, stream->input);
			stream->bytesLeft += nRead;
			stream->readPtr = stream->readBuf;
			if (nRead == 0)
				stream->eofReached = 1;
		}

		offset = FindValidatedMpegSync(stream->readPtr, stream->bytesLeft);
		if (offset < 0) {
			if (stream->eofReached)
				break;
			if (stream->bytesLeft > 3) {
				stream->readPtr += stream->bytesLeft - 3;
				stream->bytesLeft = 3;
			}
			continue;
		}
		stream->readPtr += offset;
		stream->bytesLeft -= offset;
		frameStart = stream->readPtr;
		frameBytes = stream->bytesLeft;

		if (stream->timing) {
			clock_t t0 = clock();
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
			stream->timing->frameDecode += clock() - t0;
		} else {
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
		}
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW &&
				stream->stats->decodedFrames == 0 && frameBytes > 1) {
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
				stream->outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else if (stream->stats->decodedFrames == 0 && frameBytes > 1) {
				/* A false-positive first header must not make the whole file fail. */
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stream->stats->decodedFrames);
				stream->decodeError = 1;
				stream->outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(stream->decoder, &info);
		UpdateFirstFrameStats(stream->stats, &info);
		if (!stream->effectiveRate) {
			stream->effectiveRate = EffectiveOutputSampleRate(opt, info.samprate);
			stream->stats->outputSampleRate = stream->effectiveRate;
		}

		{
			int outSamps;
			int outChannels;
			int i;
			clock_t t0;

			if (stream->timing)
				t0 = clock();
			if (opt->stereo) {
				if (info.nChans == 1) {
					int frames = info.outputSamps;
					for (i = 0; i < frames; i++) {
						stream->writeBuf[2 * i] = stream->decodeBuf[i];
						stream->writeBuf[2 * i + 1] = stream->decodeBuf[i];
					}
					outSamps = frames * 2;
				} else {
					outSamps = MixFrame(stream->decodeBuf, stream->writeBuf,
						info.outputSamps, info.nChans, 0);
				}
				outChannels = 2;
			} else {
				outChannels = MP3GetOutputChannels(stream->decoder);
				if (info.nChans > 1 && outChannels == 1) {
					memmove(stream->writeBuf, stream->decodeBuf,
						info.outputSamps * sizeof(short));
					outSamps = info.outputSamps;
				} else {
					outSamps = MixFrame(stream->decodeBuf, stream->writeBuf,
						info.outputSamps, info.nChans, 1);
					outChannels = 1;
				}
			}
			if (!opt->fastLowrate && opt->outputRate &&
				info.samprate > opt->outputRate) {
				outSamps = DownsampleFrame(&stream->rateState,
					stream->writeBuf, stream->rateBuf, outSamps,
					info.samprate, opt->outputRate, outChannels);
				memmove(stream->writeBuf, stream->rateBuf,
					outSamps * sizeof(short));
			}
			if (opt->checksum)
				stream->stats->pcmChecksum = UpdatePcmChecksum(
					stream->stats->pcmChecksum, stream->writeBuf, outSamps);
			if (stream->timing)
				stream->timing->pcmConvert += clock() - t0;

			/*
			 * Playback usually has enough room for a whole decoded fast-lowrate
			 * frame.  Convert those samples straight into the caller's chip/work
			 * buffer instead of first filling spill[] and then memcpy()ing it out.
			 * Only the tail that does not fit is kept in spill[] for the next call.
			 */
			{
				int direct;
				int spill;

				direct = outSamps;
				if (direct > maxBytes - produced)
					direct = maxBytes - produced;
				i = 0;
				if (direct >= 4) {
					int direct4 = direct & ~3;

					for (; i < direct4; i += 4) {
						dest[produced + i] = Sample16ToS8(stream->writeBuf[i]);
						dest[produced + i + 1] = Sample16ToS8(stream->writeBuf[i + 1]);
						dest[produced + i + 2] = Sample16ToS8(stream->writeBuf[i + 2]);
						dest[produced + i + 3] = Sample16ToS8(stream->writeBuf[i + 3]);
					}
				}
				for (; i < direct; i++)
					dest[produced + i] = Sample16ToS8(stream->writeBuf[i]);
				produced += direct;

				spill = outSamps - direct;
				if (spill > 0) {
					stream->spillPos = 0;
					stream->spillCount = spill;
					for (i = 0; i < spill; i++)
						stream->spill.interleaved[i] =
							Sample16ToS8(stream->writeBuf[direct + i]);
				}
			}
			stream->stats->outputSamples += (unsigned long)outSamps;
			stream->stats->decodedFrames++;
		}
	}

	return produced;
}

static int DecodeStreamCopyPlanarSpill(DecodeStream *stream, signed char *left,
	signed char *right, int maxFrames, int *outFrames)
{
	int n;

	if (stream->planarSpillPos >= stream->planarSpillCount) {
		stream->planarSpillPos = 0;
		stream->planarSpillCount = 0;
		return 0;
	}
	n = stream->planarSpillCount - stream->planarSpillPos;
	if (n > maxFrames)
		n = maxFrames;
	memcpy(left + *outFrames, stream->spill.planar[0] + stream->planarSpillPos, n);
	memcpy(right + *outFrames, stream->spill.planar[1] + stream->planarSpillPos, n);
	stream->planarSpillPos += n;
	*outFrames += n;
	if (stream->planarSpillPos >= stream->planarSpillCount) {
		stream->planarSpillPos = 0;
		stream->planarSpillCount = 0;
	}
	return n;
}

/* Stereo streaming writes converted samples straight into Paula's planar buffers. */
static int DecodeStreamFillPlanarS8(DecodeStream *stream, const DecodeOptions *opt,
	signed char *left, signed char *right, int maxFrames)
{
	MP3FrameInfo info;
	int produced;

	produced = 0;
	DecodeStreamCopyPlanarSpill(stream, left, right, maxFrames, &produced);
	while (produced < maxFrames && !stream->outOfData) {
		const short *pcm;
		int frames;
		int channels;
		int nRead;
		int offset;
		int err;
		int i;
		int direct;
		unsigned char *frameStart;
		int frameBytes;
		clock_t t0;

		if (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
			nRead = FillReadBuffer(stream->readBuf, stream->readPtr,
				READBUF_SIZE, stream->bytesLeft, stream->input);
			stream->bytesLeft += nRead;
			stream->readPtr = stream->readBuf;
			if (nRead == 0)
				stream->eofReached = 1;
		}

		offset = FindValidatedMpegSync(stream->readPtr, stream->bytesLeft);
		if (offset < 0) {
			if (stream->eofReached)
				break;
			if (stream->bytesLeft > 3) {
				stream->readPtr += stream->bytesLeft - 3;
				stream->bytesLeft = 3;
			}
			continue;
		}
		stream->readPtr += offset;
		stream->bytesLeft -= offset;
		frameStart = stream->readPtr;
		frameBytes = stream->bytesLeft;

		if (stream->timing) {
			t0 = clock();
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
			stream->timing->frameDecode += clock() - t0;
		} else {
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
		}
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW &&
				stream->stats->decodedFrames == 0 && frameBytes > 1) {
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
				stream->outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else if (stream->stats->decodedFrames == 0 && frameBytes > 1) {
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stream->stats->decodedFrames);
				stream->decodeError = 1;
				stream->outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(stream->decoder, &info);
		UpdateFirstFrameStats(stream->stats, &info);
		if (!stream->effectiveRate) {
			stream->effectiveRate = EffectiveOutputSampleRate(opt, info.samprate);
			stream->stats->outputSampleRate = stream->effectiveRate;
		}
		if (stream->timing)
			t0 = clock();

		channels = info.nChans > 1 ? 2 : 1;
		pcm = stream->decodeBuf;
		frames = info.outputSamps / channels;
		if (!opt->fastLowrate && opt->outputRate && info.samprate > opt->outputRate) {
			if (channels == 1) {
				for (i = frames - 1; i >= 0; i--) {
					stream->writeBuf[2 * i] = stream->decodeBuf[i];
					stream->writeBuf[2 * i + 1] = stream->decodeBuf[i];
				}
				pcm = stream->writeBuf;
			} else {
				pcm = stream->decodeBuf;
			}
			frames = DownsampleFrame(&stream->rateState, pcm, stream->rateBuf,
				frames * 2, info.samprate, opt->outputRate, 2) / 2;
			pcm = stream->rateBuf;
			channels = 2;
		}

		if (stream->stats && opt->checksum) {
			if (channels == 2) {
				stream->stats->pcmChecksum = UpdatePcmChecksum(
					stream->stats->pcmChecksum, pcm, frames * 2);
			} else {
				for (i = 0; i < frames; i++) {
					short pair[2];
					pair[0] = pcm[i];
					pair[1] = pcm[i];
					stream->stats->pcmChecksum = UpdatePcmChecksum(
						stream->stats->pcmChecksum, pair, 2);
				}
			}
		}

		direct = frames;
		if (direct > maxFrames - produced)
			direct = maxFrames - produced;
		for (i = 0; i < direct; i++) {
			if (channels == 2) {
				left[produced + i] = Sample16ToS8(pcm[2 * i]);
				right[produced + i] = Sample16ToS8(pcm[2 * i + 1]);
			} else {
				left[produced + i] = Sample16ToS8(pcm[i]);
				right[produced + i] = left[produced + i];
			}
		}
		stream->planarSpillPos = 0;
		stream->planarSpillCount = frames - direct;
		for (i = direct; i < frames; i++) {
			int spill = i - direct;
			if (channels == 2) {
				stream->spill.planar[0][spill] = Sample16ToS8(pcm[2 * i]);
				stream->spill.planar[1][spill] = Sample16ToS8(pcm[2 * i + 1]);
			} else {
				stream->spill.planar[0][spill] = Sample16ToS8(pcm[i]);
				stream->spill.planar[1][spill] = stream->spill.planar[0][spill];
			}
		}
		produced += direct;
		stream->stats->outputSamples += (unsigned long)frames * 2UL;
		stream->stats->decodedFrames++;
		if (stream->timing)
			stream->timing->pcmConvert += clock() - t0;
	}
	return produced;
}

#ifdef AMIGA_M68K
/* Ctrl-C signal handling is unavailable in the libnix build for now. */
static volatile int gPlaybackInterrupted;
#else
static volatile sig_atomic_t gPlaybackInterrupted;

#endif

/* Shared status block written by the playback subprocess and read by the GUI
 * timer tick.  Both run in the same AmigaOS process address space so a plain
 * volatile struct is sufficient -- no Exec locking needed for this
 * loosely-consistent, one-writer/one-reader use. */
typedef struct GuiPlaybackStatus {
	volatile int           phase;            /* GUIPLAY_PHASE_* constants below */
	volatile long          spareMs;          /* last measured spare ms before buf end */
	volatile unsigned long underruns;        /* running total underrun count */
	volatile unsigned long decodedFrames;    /* MP3 frames decoded so far */
	volatile int           sampleRate;       /* effective output sample rate (Hz) */
	volatile unsigned long halfBufferMs;     /* selected playback half-buffer duration */
	volatile unsigned long runId;          /* playback generation that owns this status */
	volatile int           cleanupComplete;/* audio.device and buffers fully released */
	volatile int           cleanupStage;   /* GUIPLAY_CLEANUP_* diagnostic stage */
	volatile int           startupStage;   /* GUISTART_* diagnostic stage */
	volatile int           requestedRate;
	volatile int           effectiveRate;
	volatile unsigned int  paulaPeriod;
	volatile unsigned long requestedBytes;
	volatile unsigned long tryBytes;
	volatile int           lastError;
	volatile int           openDeviceResult;
} GuiPlaybackStatus;

GuiPlaybackStatus gGuiPlaybackStatus;

#define GUIPLAY_PHASE_IDLE      0   /* not playing */
#define GUIPLAY_PHASE_BUFFERING 1   /* filling initial buffers */
#define GUIPLAY_PHASE_PLAYING   2   /* steady-state streaming */
#define GUIPLAY_PHASE_UNDERRUN  3   /* underrun just occurred */
#define GUIPLAY_PHASE_DONE      4   /* playback finished normally */
#define GUIPLAY_PHASE_STOPPING  5   /* Stop/EOF cleanup is releasing audio */

#define GUIPLAY_CLEANUP_NONE          0
#define GUIPLAY_CLEANUP_ABORT_REAP    1
#define GUIPLAY_CLEANUP_DEVICE_CLOSED 2
#define GUIPLAY_CLEANUP_BUFFERS_FREED 3
#define GUIPLAY_CLEANUP_COMPLETE      4

#define GUISTART_NONE                  0
#define GUISTART_CHILD_ENTERED         10
#define GUISTART_ARGS_READY            20
#define GUISTART_INPUT_OPEN            30
#define GUISTART_INPUT_FOPEN_BEFORE    31
#define GUISTART_INPUT_FOPEN_AFTER     32
#define GUISTART_INPUT_PRELOAD_FASTMEM 35
#define GUISTART_INPUT_PREPARE         40
#define GUISTART_DECODER_ALLOC         50
#define GUISTART_DECODER_CONFIG        60
#define GUISTART_FASTLOWRATE_WARN_BEFORE 61
#define GUISTART_FASTLOWRATE_WARN_AFTER  62
#define GUISTART_PROBE_RATE            70
#define GUISTART_PROBE_RATE_DONE       80
#define GUISTART_STREAM_INIT           90
#define GUISTART_PREFILL               100
#define GUISTART_PREFILL_DONE          110
#define GUISTART_AUDIO_SETUP           120
#define GUISTART_CREATE_PORT           130
#define GUISTART_ALLOC_CHIP_BUFFERS    140
#define GUISTART_CREATE_IOREQUESTS     150
#define GUISTART_OPEN_DEVICE           160
#define GUISTART_OPEN_DEVICE_DONE      170
#define GUISTART_ALLOC_WORK_BUFFERS    180
#define GUISTART_AUDIO_SETUP_DONE      190
#define GUISTART_FILL_BUFFER_A         200
#define GUISTART_FILL_BUFFER_A_DONE    210
#define GUISTART_FILL_BUFFER_B         220
#define GUISTART_FILL_BUFFER_B_DONE    230
#define GUISTART_PREPARE_A             240
#define GUISTART_PREPARE_B             250
#define GUISTART_COMMIT_A              260
#define GUISTART_PLAYING               270
#define GUISTART_FAILED                900
#define GUISTART_CLEANUP               910

#ifdef MINIAMP3_DEBUG
#ifndef MINIAMP3_DEBUG_FMT_PTR
#if defined(AMIGA_M68K)
#define MINIAMP3_DEBUG_FMT_PTR(p) ((ULONG)(p))
#else
#define MINIAMP3_DEBUG_FMT_PTR(p) (p)
#endif
#endif

const char *GuiStartupStageName(int stage)
{
	switch (stage) {
	case GUISTART_NONE: return "none";
	case GUISTART_CHILD_ENTERED: return "child entered";
	case GUISTART_ARGS_READY: return "args ready";
	case GUISTART_INPUT_OPEN: return "input open";
	case GUISTART_INPUT_FOPEN_BEFORE: return "input fopen before";
	case GUISTART_INPUT_FOPEN_AFTER: return "input fopen after";
	case GUISTART_INPUT_PRELOAD_FASTMEM: return "copying input to Fast RAM";
	case GUISTART_INPUT_PREPARE: return "input prepare";
	case GUISTART_DECODER_ALLOC: return "decoder alloc";
	case GUISTART_DECODER_CONFIG: return "decoder config";
	case GUISTART_FASTLOWRATE_WARN_BEFORE: return "fast-lowrate warning gate before";
	case GUISTART_FASTLOWRATE_WARN_AFTER: return "fast-lowrate warning gate after";
	case GUISTART_PROBE_RATE: return "probing input rate";
	case GUISTART_PROBE_RATE_DONE: return "input rate probed";
	case GUISTART_STREAM_INIT: return "stream init";
	case GUISTART_PREFILL: return "prefill decode";
	case GUISTART_PREFILL_DONE: return "prefill done";
	case GUISTART_AUDIO_SETUP: return "audio setup";
	case GUISTART_CREATE_PORT: return "creating msg port";
	case GUISTART_ALLOC_CHIP_BUFFERS: return "allocating chip buffers";
	case GUISTART_CREATE_IOREQUESTS: return "creating IO requests";
	case GUISTART_OPEN_DEVICE: return "opening audio.device";
	case GUISTART_OPEN_DEVICE_DONE: return "audio.device opened";
	case GUISTART_ALLOC_WORK_BUFFERS: return "allocating work buffers";
	case GUISTART_AUDIO_SETUP_DONE: return "audio setup done";
	case GUISTART_FILL_BUFFER_A: return "filling playback buffer A";
	case GUISTART_FILL_BUFFER_A_DONE: return "buffer A filled";
	case GUISTART_FILL_BUFFER_B: return "filling playback buffer B";
	case GUISTART_FILL_BUFFER_B_DONE: return "buffer B filled";
	case GUISTART_PREPARE_A: return "preparing buffer A";
	case GUISTART_PREPARE_B: return "preparing buffer B";
	case GUISTART_COMMIT_A: return "submitting first buffer";
	case GUISTART_PLAYING: return "playing";
	case GUISTART_FAILED: return "failed";
	case GUISTART_CLEANUP: return "cleanup";
	default: return "unknown";
	}
}

static void GuiWriteDetailedStartupLog(int stage)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMIGA_AUDIO_DEVICE)
	{
		BPTR log;
		char line[320];
		int len;

		len = sprintf(line,
			"runId=%lu stage=%d name=%s requestedRate=%d effectiveRate=%d period=%u requestedBytes=%lu tryBytes=%lu openDeviceResult=%d interrupted=%d task=%p process=%p\n",
			gGuiPlaybackStatus.runId, stage,
			MINIAMP3_DEBUG_FMT_PTR(GuiStartupStageName(stage)),
			gGuiPlaybackStatus.requestedRate, gGuiPlaybackStatus.effectiveRate,
			gGuiPlaybackStatus.paulaPeriod, gGuiPlaybackStatus.requestedBytes,
			gGuiPlaybackStatus.tryBytes, gGuiPlaybackStatus.openDeviceResult,
			gPlaybackInterrupted,
			MINIAMP3_DEBUG_FMT_PTR((void *)FindTask(NULL)),
			MINIAMP3_DEBUG_FMT_PTR((void *)FindTask(NULL)));
		log = Open((STRPTR)"T:MiniAMP3-startup.log", MODE_READWRITE);
		if (log) {
			Seek(log, 0, OFFSET_END);
			Write(log, line, len);
			Close(log);
		}
	}
#else
	{
		FILE *log = fopen("MiniAMP3-startup.log", "a");
		if (log) {
			fprintf(log, "runId=%lu stage=%d name=%s\n",
				gGuiPlaybackStatus.runId, stage, GuiStartupStageName(stage));
			fclose(log);
		}
	}
#endif
}
#endif /* MINIAMP3_DEBUG */

static void GuiPublishStartupStage(int stage)
{
	gGuiPlaybackStatus.startupStage = stage;
#ifdef MINIAMP3_DEBUG
	GuiWriteDetailedStartupLog(stage);
#endif
}

static int AmigaPlaybackStopRequested(const DecodeOptions *opt, const char *where)
{
#if defined(AMIGA_M68K)
	ULONG pending = SetSignal(0, 0);
	if (pending & SIGBREAKF_CTRL_C)
		gPlaybackInterrupted = 1;
#endif
	if (gPlaybackInterrupted) {
#if defined(MINIAMP3_DEBUG)
		printf("miniamp3-debug: Stop observed %s\n", where);
#endif
		if (opt && opt->debugCleanup)
			printf("debug-cleanup: Stop observed %s\n", where);
		return 1;
	}
	return 0;
}

#if !defined(AMIGA_M68K)
static void PlaybackSignalHandler(int signum)
{
	(void)signum;
	gPlaybackInterrupted = 1;
}
#endif

int MP3ResetStatics(void)
{
	extern void AmigaResetPolyphaseStatics(void);

	/* MiniAMP3 calls the CLI main() repeatedly in one GUI process.  Clear
	 * frontend globals and decoder file-scope controls so each playback starts
	 * from the same state as a fresh command-line process.
	 *
	 * The current Helix synthesis and IMDCT overlap buffers are allocated inside
	 * MP3DecInfo by MP3InitDecoder(), so they are already fresh per decode.
	 */
	gPlaybackInterrupted = 0;
	memset((void *)&gGuiPlaybackStatus, 0, sizeof(gGuiPlaybackStatus));
	gTiming = NULL;
	MP3SetExperimentalHuffman(0);
	AmigaResetPolyphaseStatics();
	return 0;
}

typedef struct PlaybackCleanupStatus {
	unsigned long ioCompleted;
	unsigned long ioAborted;
	unsigned long ioRequestsDeleted;
	unsigned long messagePortsDeleted;
	unsigned long chipBuffersFreed;
	unsigned long workBuffersFreed;
	unsigned long canaryErrors;
	unsigned long devicesClosed;
} PlaybackCleanupStatus;

static void PlaybackCleanupStatusInit(PlaybackCleanupStatus *status)
{
	if (status)
		memset(status, 0, sizeof(*status));
}

static void PrintPlaybackCleanupStatus(const DecodeOptions *opt,
	const PlaybackCleanupStatus *status)
{
	if (!opt->debugCleanup || !status)
		return;
	printf("debug-cleanup: outstanding audio IOs completed/aborted: %lu/%lu\n",
		status->ioCompleted, status->ioAborted);
	printf("debug-cleanup: audio.device closed: %s (%lu)\n",
		status->devicesClosed ? "yes" : "not opened", status->devicesClosed);
	printf("debug-cleanup: IO requests deleted: %lu\n",
		status->ioRequestsDeleted);
	printf("debug-cleanup: message ports deleted: %lu\n",
		status->messagePortsDeleted);
	printf("debug-cleanup: chip buffers freed: %lu\n",
		status->chipBuffersFreed);
	printf("debug-cleanup: work buffers freed: %lu\n",
		status->workBuffersFreed);
	printf("debug-cleanup: playback buffer canaries: %s (%lu errors)\n",
		status->canaryErrors ? "CORRUPTED" : "ok", status->canaryErrors);
}

static unsigned int AmigaPalAudioPeriod(int outputRate)
{
	if (outputRate <= 0)
		return 0;
	return (unsigned int)((3546895UL + ((unsigned long)outputRate / 2UL)) /
		(unsigned long)outputRate);
}

#define AMIGA_MONO_AUDIO_SLOTS 3
#define AMIGA_STEREO_AUDIO_SLOTS 2
#define AMIGA_STEREO_DECODE_SLOTS 3
#define AMIGA_AUDIO_PLAYBACK_SLOTS AMIGA_MONO_AUDIO_SLOTS

static int AmigaAudioLiveSlots(int stereo)
{
	return stereo ? AMIGA_STEREO_AUDIO_SLOTS : AMIGA_MONO_AUDIO_SLOTS;
}

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#ifndef NDEBUG
#define PLAYBACK_GUARD_BYTES 16UL
#define PLAYBACK_GUARD_VALUE 0xa5
#else
#define PLAYBACK_GUARD_BYTES 0UL
#endif

typedef struct AmigaAudioPlayer {
	struct MsgPort *port;
	struct IOAudio *req[3][2];
	int deviceOpen[2];
	int sent[3][2];
	int prepared[3];
	int stereo;
	unsigned int period;
	signed char *splitBuf[3][2];
	void *splitBase[3][2];
	unsigned long splitBytes;
	signed char *splitWorkBuf[3][2];
	void *splitWorkBase[3][2];
	unsigned long splitWorkBytes;
	signed char *workBuf[3];
	void *workBase[3];
	unsigned long workBytes;
	int workChip;
	volatile int cleanupStarted;
	int debugCleanup;
	int stopping;
	int outputStride;
	int debugPlay;
	int startupVolumeDebugPrinted;
	unsigned long cleanupInvocationCount;
	UWORD requestVolume;
	unsigned short lastVolumePercent;
	unsigned long lastVolumeSequence;
} AmigaAudioPlayer;

static int PlaybackBufferCanaryOk(const void *base, unsigned long bytes)
{
#ifndef NDEBUG
	const unsigned char *p;
	unsigned long i;

	if (!base)
		return 1;
	p = (const unsigned char *)base;
	for (i = 0; i < PLAYBACK_GUARD_BYTES; i++) {
		if (p[i] != PLAYBACK_GUARD_VALUE ||
			p[PLAYBACK_GUARD_BYTES + bytes + i] != PLAYBACK_GUARD_VALUE)
			return 0;
	}
#else
	(void)base;
	(void)bytes;
#endif
	return 1;
}

static signed char *AmigaAllocGuarded(unsigned long bytes, int chip, void **baseOut)
{
	unsigned long total;
	unsigned char *base;

	total = bytes + 2UL * PLAYBACK_GUARD_BYTES;
	base = (unsigned char *)AllocMem(total,
		(chip ? MEMF_CHIP : MEMF_FAST) | MEMF_CLEAR);
	if (!base)
		return NULL;
#ifndef NDEBUG
	memset(base, PLAYBACK_GUARD_VALUE, PLAYBACK_GUARD_BYTES);
	memset(base + PLAYBACK_GUARD_BYTES + bytes, PLAYBACK_GUARD_VALUE,
		PLAYBACK_GUARD_BYTES);
#endif
	*baseOut = base;
	return (signed char *)(base + PLAYBACK_GUARD_BYTES);
}

static void AmigaFreeGuarded(void **basePtr, unsigned long bytes, int chip,
	PlaybackCleanupStatus *status)
{
	void *base;

	(void)chip;
	base = *basePtr;
	if (!base)
		return;
	if (!PlaybackBufferCanaryOk(base, bytes) && status)
		status->canaryErrors++;
	FreeMem(base, bytes + 2UL * PLAYBACK_GUARD_BYTES);
	*basePtr = NULL;
}

static void AmigaAudioCleanupTrace(const AmigaAudioPlayer *player, const char *msg)
{
	if (!player || !player->debugCleanup) {
		(void)msg;
		return;
	}
	printf("debug-cleanup: %s", msg);
}

static void AmigaAudioCleanupTrace4(const AmigaAudioPlayer *player,
	const char *fmt, unsigned long a, unsigned long b,
	unsigned long c, unsigned long d)
{
	if (!player || !player->debugCleanup) {
		(void)fmt;
		(void)a;
		(void)b;
		(void)c;
		(void)d;
		return;
	}
	printf("debug-cleanup: ");
	printf(fmt, a, b, c, d);
}

static void AmigaAudioClose(AmigaAudioPlayer *player,
	PlaybackCleanupStatus *status)
{
	int i;
	int ch;

	if (!player)
		return;
	if (player->cleanupStarted) {
		AmigaAudioCleanupTrace4(player, "cleanupComplete already set duplicate cleanup count=%lu\n",
			player->cleanupInvocationCount, 0, 0, 0);
		return;
	}
	player->cleanupStarted = 1;
	player->stopping = 1;
	player->cleanupInvocationCount++;
	AmigaAudioCleanupTrace4(player, "cleanup begin count=%lu outputRate=%ld stride=%ld\n",
		player->cleanupInvocationCount, (unsigned long)gGuiPlaybackStatus.sampleRate,
		(unsigned long)player->outputStride, 0);
	AmigaAudioCleanupTrace(player, "volume control request: none allocated (next-buffer ioa_Volume updates)\n");
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_ABORT_REAP;
	/* No request, device, port, or DMA buffer is destroyed until every write
	 * has either completed or has been aborted and reaped with WaitIO. */
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			if (player->req[i][ch] && player->sent[i][ch]) {
				int done = CheckIO((struct IORequest *)player->req[i][ch]) != 0;
				int aborted = 0;
				AmigaAudioCleanupTrace4(player, "request index=%ld channel=%ld submitted=%ld CheckIO=%ld\n",
					(unsigned long)i, (unsigned long)ch,
					(unsigned long)player->sent[i][ch], (unsigned long)done);
				if (!done) {
					AbortIO((struct IORequest *)player->req[i][ch]);
					aborted = 1;
					if (status)
						status->ioAborted++;
				} else if (status) {
					status->ioCompleted++;
				}
				AmigaAudioCleanupTrace4(player, "request index=%ld channel=%ld AbortIO issued=%ld\n",
					(unsigned long)i, (unsigned long)ch, (unsigned long)aborted, 0);
				WaitIO((struct IORequest *)player->req[i][ch]);
				AmigaAudioCleanupTrace4(player, "request index=%ld channel=%ld WaitIO completed=1\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				player->sent[i][ch] = 0;
			}
		}
	}
	if (player->port) {
		unsigned long drained = 0;
		while (GetMsg(player->port))
			drained++;
		AmigaAudioCleanupTrace4(player, "reply drained=%ld\n",
			drained, 0, 0, 0);
	}
	for (ch = 0; ch < 2; ch++) {
		if (player->deviceOpen[ch] && player->req[0][ch]) {
			CloseDevice((struct IORequest *)player->req[0][ch]);
			AmigaAudioCleanupTrace4(player, "channel=%ld device closed=1\n",
				(unsigned long)ch, 0, 0, 0);
			player->deviceOpen[ch] = 0;
			if (status)
				status->devicesClosed++;
		}
	}
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_DEVICE_CLOSED;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			if (player->splitBase[i][ch]) {
				AmigaFreeGuarded(&player->splitBase[i][ch], player->splitBytes, 1,
					status);
				player->splitBuf[i][ch] = NULL;
				AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld chip buffer freed\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				if (status)
					status->chipBuffersFreed++;
			}
			if (player->splitWorkBase[i][ch]) {
				AmigaFreeGuarded(&player->splitWorkBase[i][ch],
					player->splitWorkBytes, 0, status);
				player->splitWorkBuf[i][ch] = NULL;
				AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld work buffer freed\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				if (status)
					status->workBuffersFreed++;
			}
		}
		if (player->workBase[i]) {
			AmigaFreeGuarded(&player->workBase[i], player->workBytes,
				player->workChip, status);
			player->workBuf[i] = NULL;
			AmigaAudioCleanupTrace4(player, "slot=%ld work buffer freed\n",
				(unsigned long)i, 0, 0, 0);
			if (status) {
				if (player->workChip)
					status->chipBuffersFreed++;
				else
					status->workBuffersFreed++;
			}
		}
	}
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_BUFFERS_FREED;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			if (player->req[i][ch]) {
				DeleteIORequest((struct IORequest *)player->req[i][ch]);
				AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld IORequest deleted\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				player->req[i][ch] = NULL;
				if (status)
					status->ioRequestsDeleted++;
			}
		}
	}
	if (player->port) {
		DeleteMsgPort(player->port);
		AmigaAudioCleanupTrace(player, "message port deleted\n");
		player->port = NULL;
		if (status)
			status->messagePortsDeleted++;
	}
	player->stereo = 0;
	player->period = 0;
	player->splitBytes = 0;
	player->splitWorkBytes = 0;
	player->workBytes = 0;
	player->workChip = 0;
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
	gGuiPlaybackStatus.cleanupComplete = 1;
	AmigaAudioCleanupTrace(player, "cleanupComplete set=1\n");
}

static int AmigaAudioOpenOne(AmigaAudioPlayer *player, int ch,
	const UBYTE *channels, unsigned long channelCount)
{
	{
		int i;
		int liveSlots = AmigaAudioLiveSlots(player->stereo);

		for (i = 0; i < liveSlots; i++) {
			GuiPublishStartupStage(GUISTART_CREATE_IOREQUESTS);
			player->req[i][ch] = (struct IOAudio *)CreateIORequest(player->port,
				sizeof(struct IOAudio));
			if (!player->req[i][ch])
				return -1;
		}
	}
	player->req[0][ch]->ioa_Request.io_Message.mn_Node.ln_Pri = ADALLOC_MINPREC;
	player->req[0][ch]->ioa_Data = (UBYTE *)channels;
	player->req[0][ch]->ioa_Length = channelCount;
	{
		int openDeviceResult;

		GuiPublishStartupStage(GUISTART_OPEN_DEVICE);
		if (AmigaPlaybackStopRequested(NULL, "before opening audio.device"))
			return -1;
		openDeviceResult = OpenDevice(AUDIONAME, 0,
			(struct IORequest *)player->req[0][ch], 0);
#ifdef MINIAMP3_DEBUG
		gGuiPlaybackStatus.openDeviceResult = openDeviceResult;
#else
		if (!gMiniAmp3EmbeddedPlayback)
			gGuiPlaybackStatus.openDeviceResult = openDeviceResult;
#endif
		GuiPublishStartupStage(GUISTART_OPEN_DEVICE_DONE);
		if (openDeviceResult != 0)
			return -1;
	}
	player->deviceOpen[ch] = 1;
	{
		int i;
		for (i = 1; i < AmigaAudioLiveSlots(player->stereo); i++) {
			struct Message message;

			/* Preserve CreateIORequest's private message-node state.  Copying the
			 * opened request over that node can corrupt Exec message-port lists. */
			message = player->req[i][ch]->ioa_Request.io_Message;
			memcpy(player->req[i][ch], player->req[0][ch], sizeof(struct IOAudio));
			player->req[i][ch]->ioa_Request.io_Message = message;
			player->req[i][ch]->ioa_Request.io_Message.mn_ReplyPort = player->port;
		}
	}
	return 0;
}

static int AmigaAudioOpen(AmigaAudioPlayer *player, unsigned int period,
	int stereo, unsigned long maxBytes, int initialVolumePercent)
{
	UBYTE monoChannels[] = { 1, 2, 4, 8 };
	UBYTE leftChannels[] = { 1, 8 };
	UBYTE rightChannels[] = { 2, 4 };
	int i;
	int ch;

	memset(player, 0, sizeof(*player));
	player->period = period;
	player->stereo = stereo;
	if (initialVolumePercent < 0)
		initialVolumePercent = 0;
	if (initialVolumePercent > 100)
		initialVolumePercent = 100;
	player->lastVolumePercent = (unsigned short)initialVolumePercent;
	player->lastVolumeSequence = gMiniAmp3VolumeSequence;
	player->requestVolume = VolumePercentToAudioDevice(initialVolumePercent);
	GuiPublishStartupStage(GUISTART_CREATE_PORT);
	player->port = CreateMsgPort();
	if (!player->port)
		return -1;
	if (stereo) {
		player->splitBytes = maxBytes / 2UL;
		if (player->splitBytes == 0)
			player->splitBytes = 1;
		for (i = 0; i < AMIGA_STEREO_AUDIO_SLOTS; i++) {
			for (ch = 0; ch < 2; ch++) {
				GuiPublishStartupStage(GUISTART_ALLOC_CHIP_BUFFERS);
				player->splitBuf[i][ch] = AmigaAllocGuarded(player->splitBytes, 1,
					&player->splitBase[i][ch]);
				if (!player->splitBuf[i][ch]) {
					int wasInterrupted = gPlaybackInterrupted;
					AmigaAudioClose(player, NULL);
					if (!wasInterrupted)
						gPlaybackInterrupted = 0;
					return -1;
				}
			}
		}
		if (AmigaAudioOpenOne(player, 0, leftChannels, sizeof(leftChannels)) != 0 ||
			AmigaAudioOpenOne(player, 1, rightChannels, sizeof(rightChannels)) != 0) {
			int wasInterrupted = gPlaybackInterrupted;
			AmigaAudioClose(player, NULL);
			if (!wasInterrupted)
				gPlaybackInterrupted = 0;
			return -1;
		}
	} else {
		player->splitBytes = maxBytes;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			GuiPublishStartupStage(GUISTART_ALLOC_CHIP_BUFFERS);
			player->splitBuf[i][0] = AmigaAllocGuarded(player->splitBytes, 1,
				&player->splitBase[i][0]);
			if (!player->splitBuf[i][0]) {
				int wasInterrupted = gPlaybackInterrupted;
				AmigaAudioClose(player, NULL);
				if (!wasInterrupted)
					gPlaybackInterrupted = 0;
				return -1;
			}
		}
		if (AmigaAudioOpenOne(player, 0, monoChannels, sizeof(monoChannels)) != 0) {
			int wasInterrupted = gPlaybackInterrupted;
			AmigaAudioClose(player, NULL);
			if (!wasInterrupted)
				gPlaybackInterrupted = 0;
			return -1;
		}
	}
	return 0;
}

static void AmigaAudioPrepareOne(AmigaAudioPlayer *player, int index,
	int ch, signed char *buf, unsigned long len)
{
	struct IOAudio *req = player->req[index][ch];

	req->ioa_Request.io_Command = CMD_WRITE;
	req->ioa_Request.io_Flags = ADIOF_PERVOL;
	req->ioa_Data = (UBYTE *)buf;
	req->ioa_Length = len;
	req->ioa_Period = player->period;
	req->ioa_Volume = player->requestVolume;
	req->ioa_Cycles = 1;
}

static void AmigaAudioPrintStartupVolumeDebug(AmigaAudioPlayer *player,
	int index)
{
	if (!player || !player->debugPlay || player->startupVolumeDebugPrinted)
		return;
	player->startupVolumeDebugPrinted = 1;
	printf("debug-play: parsed --volume percent: %u\n",
		(unsigned int)player->lastVolumePercent);
	printf("debug-play: shared requested percent: %u\n",
		(unsigned int)(gMiniAmp3RequestedVolume > 100 ? 100 :
			gMiniAmp3RequestedVolume));
	printf("debug-play: mapped ioa_Volume: %u\n",
		(unsigned int)player->requestVolume);
	if (player->stereo) {
		printf("debug-play: request A channel 0 ioa_Volume: %u\n",
			(unsigned int)player->req[index][0]->ioa_Volume);
		printf("debug-play: request A channel 1 ioa_Volume: %u\n",
			(unsigned int)player->req[index][1]->ioa_Volume);
		printf("debug-play: request flags: ch0=0x%lx ch1=0x%lx\n",
			(unsigned long)player->req[index][0]->ioa_Request.io_Flags,
			(unsigned long)player->req[index][1]->ioa_Request.io_Flags);
	} else {
		printf("debug-play: request A channel 0 ioa_Volume: %u\n",
			(unsigned int)player->req[index][0]->ioa_Volume);
		printf("debug-play: request flags: 0x%lx\n",
			(unsigned long)player->req[index][0]->ioa_Request.io_Flags);
	}
}

static void AmigaAudioCommitOne(AmigaAudioPlayer *player, int index, int ch)
{
	if (gPlaybackInterrupted || player->stopping)
		return;
	player->req[index][ch]->ioa_Request.io_Command = CMD_WRITE;
	player->req[index][ch]->ioa_Request.io_Flags = ADIOF_PERVOL;
	player->req[index][ch]->ioa_Request.io_Error = 0;
	player->req[index][ch]->ioa_Volume = player->requestVolume;
	player->sent[index][ch] = 1;
	BeginIO((struct IORequest *)player->req[index][ch]);
}

static void AmigaPlaybackCopy(const signed char *src, signed char *dest,
	unsigned long bytes)
{
	CopyMem((APTR)src, (APTR)dest, bytes);
}

static unsigned long PlaybackMaxChunkBytes(int stereo);

static int AmigaAudioPrepare(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	if (gPlaybackInterrupted || player->stopping)
		return -1;
	if (len == 0 || len > PlaybackMaxChunkBytes(player->stereo) ||
		(player->stereo && (len & 1UL)))
		return -1;
	if (player->stereo) {
		unsigned long frames = len / 2UL;
		unsigned long i;

		if (frames > player->splitBytes)
			return -1;
		if (player->splitWorkBuf[index][0] && player->splitWorkBuf[index][1]) {
			if (frames > player->splitWorkBytes)
				return -1;
			AmigaPlaybackCopy(player->splitWorkBuf[index][0],
				player->splitBuf[index][0], frames);
			AmigaPlaybackCopy(player->splitWorkBuf[index][1],
				player->splitBuf[index][1], frames);
		} else if (buf) {
			for (i = 0; i < frames; i++) {
				player->splitBuf[index][0][i] = buf[2UL * i];
				player->splitBuf[index][1][i] = buf[2UL * i + 1UL];
			}
		} else if (!player->splitBuf[index][0] || !player->splitBuf[index][1]) {
			return -1;
		}
		AmigaAudioPrepareOne(player, index, 1, player->splitBuf[index][1], frames);
		AmigaAudioPrepareOne(player, index, 0, player->splitBuf[index][0], frames);
	} else {
		if (!buf || len > player->splitBytes)
			return -1;
		if (player->splitBuf[index][0] && buf != player->splitBuf[index][0]) {
			AmigaPlaybackCopy(buf, player->splitBuf[index][0], len);
			buf = player->splitBuf[index][0];
		}
		AmigaAudioPrepareOne(player, index, 0, buf, len);
	}
	player->prepared[index] = 1;
	return 0;
}

static void AmigaAudioRefreshRequestedVolume(AmigaAudioPlayer *player)
{
	unsigned long seq;
	unsigned short percent;

	if (!player || player->stopping || player->cleanupStarted)
		return;
	seq = gMiniAmp3VolumeSequence;
	percent = gMiniAmp3RequestedVolume;
	if (percent > 100)
		percent = 100;
	if (seq != player->lastVolumeSequence) {
		player->lastVolumeSequence = seq;
		player->lastVolumePercent = percent;
		player->requestVolume = VolumePercentToAudioDevice(percent);
	}
}

static void AmigaAudioApplyPreparedVolume(AmigaAudioPlayer *player, int index)
{
	if (!player || !player->prepared[index])
		return;
	if (player->req[index][0])
		player->req[index][0]->ioa_Volume = player->requestVolume;
	if (player->stereo && player->req[index][1])
		player->req[index][1]->ioa_Volume = player->requestVolume;
}

static int AmigaAudioCommit(AmigaAudioPlayer *player, int index)
{
	if (AmigaPlaybackStopRequested(NULL, "before first buffer submission"))
		return -1;
	if (gPlaybackInterrupted || player->stopping)
		return -1;
	if (!player->prepared[index])
		return -1;
	AmigaAudioRefreshRequestedVolume(player);
	AmigaAudioApplyPreparedVolume(player, index);
	AmigaAudioPrintStartupVolumeDebug(player, index);
	if (player->stereo) {
#if defined(AMIGA_M68K)
		Forbid();
#endif
		AmigaAudioCommitOne(player, index, 1);
		AmigaAudioCommitOne(player, index, 0);
#if defined(AMIGA_M68K)
		Permit();
#endif
	} else {
		AmigaAudioCommitOne(player, index, 0);
	}
	player->prepared[index] = 0;
	return 0;
}

static int AmigaAudioDone(AmigaAudioPlayer *player, int index)
{
	if (player->stereo) {
		if (!player->sent[index][0] || !player->sent[index][1])
			return 0;
		return CheckIO((struct IORequest *)player->req[index][0]) != 0 &&
			CheckIO((struct IORequest *)player->req[index][1]) != 0;
	}
	if (!player->sent[index][0])
		return 0;
	return CheckIO((struct IORequest *)player->req[index][0]) != 0;
}

static int AmigaAudioWaitOne(AmigaAudioPlayer *player, int index, int ch)
{
	struct IORequest *req;
	int err;

	req = (struct IORequest *)player->req[index][ch];
#if defined(AMIGA_M68K)
	while (!CheckIO(req)) {
		ULONG sigs = (1UL << player->port->mp_SigBit) | SIGBREAKF_CTRL_C;
		ULONG got = Wait(sigs);
		if (got & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			AbortIO(req);
			break;
		}
	}
#endif
	err = WaitIO(req);
	if (!err)
		err = player->req[index][ch]->ioa_Request.io_Error;
	player->sent[index][ch] = 0;
	return err;
}

static int AmigaAudioAbortSent(AmigaAudioPlayer *player, int index, int ch)
{
	struct IORequest *req;
	int err;

	req = (struct IORequest *)player->req[index][ch];
	if (!req || !player->sent[index][ch])
		return 0;
	if (!CheckIO(req))
		AbortIO(req);
	err = WaitIO(req);
	if (!err)
		err = player->req[index][ch]->ioa_Request.io_Error;
	player->sent[index][ch] = 0;
	return err;
}

static int AmigaAudioAbortOutstanding(AmigaAudioPlayer *player)
{
	int i;
	int ch;
	int err;

	err = 0;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			int err2 = AmigaAudioAbortSent(player, i, ch);
			if (!err)
				err = err2;
		}
	}
	return err;
}

static int AmigaAudioWait(AmigaAudioPlayer *player, int index)
{
	int err;

	/* Stop can arrive while high-rate stereo has multiple Paula writes queued.
	 * Abort and reap the whole ring before returning so cleanup never closes an
	 * audio.device unit or frees a chip buffer that its paired channel may still
	 * be DMA-reading. */
	if (gPlaybackInterrupted)
		return AmigaAudioAbortOutstanding(player);
	err = 0;
	if (player->sent[index][0])
		err = AmigaAudioWaitOne(player, index, 0);
	if (gPlaybackInterrupted) {
		int err2 = AmigaAudioAbortOutstanding(player);
		if (!err)
			err = err2;
		return err;
	}
	if (player->stereo && player->sent[index][1]) {
		int err2 = AmigaAudioWaitOne(player, index, 1);
		if (!err)
			err = err2;
	}
	return err;
}

static int AmigaAudioAllocWorkBuffers(AmigaAudioPlayer *player, int stereo,
	unsigned long bytes)
{
	int i;

	if (stereo) {
		player->splitWorkBytes = bytes / 2UL;
		if (player->splitWorkBytes == 0)
			player->splitWorkBytes = 1;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			int ch;
			for (ch = 0; ch < 2; ch++) {
				player->splitWorkBuf[i][ch] =
					AmigaAllocGuarded(player->splitWorkBytes, 0,
						&player->splitWorkBase[i][ch]);
				if (!player->splitWorkBuf[i][ch])
					return -1;
			}
		}
	} else {
		player->workBytes = bytes;
		player->workChip = 0;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			player->workBuf[i] = AmigaAllocGuarded(bytes, player->workChip,
				&player->workBase[i]);
			if (!player->workBuf[i])
				return -1;
		}
	}
	return 0;
}
#else
typedef struct AmigaAudioPlayer {
	int stereo;
	int sent[3][2];
	int prepared[3];
	signed char *splitBuf[3][2];
	unsigned long splitBytes;
	signed char *splitWorkBuf[3][2];
	unsigned long splitWorkBytes;
	signed char *workBuf[3];
	unsigned long workBytes;
	int debugCleanup;
	int stopping;
	int outputStride;
	int debugPlay;
	int startupVolumeDebugPrinted;
	UWORD requestVolume;
	unsigned short lastVolumePercent;
	unsigned long lastVolumeSequence;
} AmigaAudioPlayer;
static void AmigaAudioClose(AmigaAudioPlayer *player,
	PlaybackCleanupStatus *status)
{
	int i;
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_ABORT_REAP;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		if (player->workBuf[i]) {
			free(player->workBuf[i]);
			player->workBuf[i] = NULL;
			if (status)
				status->workBuffersFreed++;
		}
	}
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
	gGuiPlaybackStatus.cleanupComplete = 1;
}
static int AmigaAudioOpen(AmigaAudioPlayer *player, unsigned int period,
	int stereo, unsigned long maxBytes, int initialVolumePercent)
{
	(void)player;
	(void)period;
	(void)stereo;
	(void)maxBytes;
	(void)initialVolumePercent;
	fprintf(stderr, "--play requires an AmigaOS audio.device build\n");
	return -1;
}
static int AmigaAudioPrepare(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	(void)buf;
	if (len == 0 || (player->stereo && (len & 1UL)))
		return -1;
	player->prepared[index] = 1;
	return 0;
}
static void AmigaAudioPrintStartupVolumeDebug(AmigaAudioPlayer *player,
	int index)
{ (void)player; (void)index; }
static int AmigaAudioCommit(AmigaAudioPlayer *player, int index)
{
	if (!player->prepared[index])
		return -1;
	player->sent[index][0] = 1;
	if (player->stereo)
		player->sent[index][1] = 1;
	player->prepared[index] = 0;
	return 0;
}
static int AmigaAudioDone(AmigaAudioPlayer *player, int index)
{ (void)player; (void)index; return 1; }
static int AmigaAudioWait(AmigaAudioPlayer *player, int index)
{ player->sent[index][0] = 0; player->sent[index][1] = 0; return 0; }
static int AmigaAudioAllocWorkBuffers(AmigaAudioPlayer *player, int stereo,
	unsigned long bytes)
{
	int i;
	(void)stereo;
	player->workBytes = bytes;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		player->workBuf[i] = (signed char *)malloc(bytes);
		if (!player->workBuf[i])
			return -1;
	}
	return 0;
}
#endif

static unsigned long PlaybackMaxChunkBytes(int stereo)
{
	return AMIGA_AUDIO_MAX_CHANNEL_BYTES * (stereo ? 2UL : 1UL);
}

static unsigned long AlignPlaybackChunkBytes(unsigned long bytes, int stereo)
{
	unsigned long maxBytes;

	/*
	 * Both streaming playback and --decode-then-play eventually submit these
	 * chunks through audio.device.  Keep every per-channel CMD_WRITE length below
	 * Paula's 16-bit DMA length boundary; otherwise a 22050 Hz multi-second
	 * buffer can play only its wrapped/truncated beginning while the decoder has
	 * already advanced to the next much-later chunk.
	 */
	maxBytes = PlaybackMaxChunkBytes(stereo);
	if (bytes > maxBytes)
		bytes = maxBytes;
	if (stereo && (bytes & 1UL))
		bytes--;
	if (bytes == 0)
		bytes = stereo ? 2UL : 1UL;
	return bytes;
}

static unsigned long PlaybackRequestedChunkBytes(const DecodeOptions *opt,
	int playbackRate)
{
	if (playbackRate <= 0)
		playbackRate = opt->outputRate > 0 ? opt->outputRate : 8287;
	return (unsigned long)playbackRate *
		(unsigned long)opt->bufferSeconds * (opt->stereo ? 2UL : 1UL);
}

static unsigned long PlaybackMaxHalfBufferMilliseconds(const DecodeOptions *opt,
	int playbackRate)
{
	unsigned long channels;
	unsigned long maxBytes;

	if (playbackRate <= 0)
		return 0;
	channels = opt->stereo ? 2UL : 1UL;
	maxBytes = PlaybackMaxChunkBytes(opt->stereo);
	return ((maxBytes / channels) * 1000UL) / (unsigned long)playbackRate;
}

static int PlaybackHalfBufferSamples(const DecodeOptions *opt,
	unsigned long chunkBytes)
{
	unsigned long channels;

	channels = opt->stereo ? 2UL : 1UL;
	if (chunkBytes == 0)
		return 0;
	return (int)(chunkBytes / channels);
}

static unsigned long PlaybackBufferDurationMilliseconds(const DecodeOptions *opt,
	unsigned long bytes, int playbackRate)
{
	unsigned long channels;
	unsigned long samples;

	channels = opt->stereo ? 2UL : 1UL;
	if (playbackRate <= 0 || bytes == 0)
		return 0;
	samples = bytes / channels;
	return (samples * 1000UL) / (unsigned long)playbackRate;
}

/* NOTE: clock() on AmigaOS/libnix uses CLOCKS_PER_SEC = 50 (VBL-based),
 * giving ~20ms resolution.  Reported spare times are therefore quantised
 * in 20ms steps; true underruns may appear as spareMs == 0 even when a
 * few ms of genuine headroom existed.  False-positive underrun counts at
 * low spare values (< 20ms) are expected and not indicative of audible
 * glitches. */
static unsigned long PlaybackElapsedMilliseconds(clock_t start, clock_t end)
{
	if (CLOCKS_PER_SEC <= 0 || end <= start)
		return 0;
	return (unsigned long)(((double)(end - start) * 1000.0) /
		(double)CLOCKS_PER_SEC);
}

static const char *PlaybackBufferName(int index)
{
	return index == 0 ? "A" : (index == 1 ? "B" : "C");
}

static void PrintPlaybackFillDebug(const DecodeOptions *opt, int index,
	unsigned long bytes)
{
	unsigned long channels;

	if (!opt->debugPlay)
		return;
	channels = opt->stereo ? 2UL : 1UL;
	printf("debug-play: buffer %s fill samples/bytes: %lu/%lu\n",
		PlaybackBufferName(index), bytes / channels, bytes);
}

static int PlaybackBufferPeakS8(const signed char *buf, unsigned long len)
{
	int peak;
	unsigned long i;

	peak = 0;
	for (i = 0; i < len; i++) {
		int v = buf[i];
		if (v < 0)
			v = -v;
		if (v > peak)
			peak = v;
	}
	return peak;
}

static unsigned long DecodeStreamFillPlaybackBuffer(DecodeStream *stream,
	const DecodeOptions *opt, AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long maxBytes)
{
	if (gPlaybackInterrupted)
		return 0;
	if (opt->stereo) {
		signed char *left = player->splitWorkBuf[index][0] ?
			player->splitWorkBuf[index][0] : player->splitBuf[index][0];
		signed char *right = player->splitWorkBuf[index][1] ?
			player->splitWorkBuf[index][1] : player->splitBuf[index][1];
		int frames;

		if (!left || !right)
			return 0;
		frames = DecodeStreamFillPlanarS8(stream, opt, left, right,
			(int)(maxBytes / 2UL));
		if (gPlaybackInterrupted)
			return 0;
		return (unsigned long)frames * 2UL;
	}
	{
		int bytes = DecodeStreamFillS8(stream, opt, buf, (int)maxBytes);
		if (gPlaybackInterrupted || bytes < 0)
			return 0;
		return (unsigned long)bytes;
	}
}


static int AmigaAudioCopyStereoDecodeAheadToSlot(AmigaAudioPlayer *player,
	int dest, int src, unsigned long len)
{
	unsigned long frames = len / 2UL;

	if (!player->stereo || (len & 1UL) || frames > player->splitWorkBytes ||
		!player->splitWorkBuf[src][0] || !player->splitWorkBuf[src][1] ||
		!player->splitWorkBuf[dest][0] || !player->splitWorkBuf[dest][1])
		return -1;
	memcpy(player->splitWorkBuf[dest][0], player->splitWorkBuf[src][0],
		(size_t)frames);
	memcpy(player->splitWorkBuf[dest][1], player->splitWorkBuf[src][1],
		(size_t)frames);
	return 0;
}

static int AmigaAudioPreparePlaybackBuffer(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	return AmigaAudioPrepare(player, index, buf, len);
}

static int AmigaAudioCommitPlaybackBuffer(AmigaAudioPlayer *player, int index)
{
	return AmigaAudioCommit(player, index);
}

static int PlaybackBufferPeak(const DecodeOptions *opt,
	const AmigaAudioPlayer *player, int index, const signed char *buf,
	unsigned long len)
{
	if (opt->stereo) {
		unsigned long frames = len / 2UL;
		const signed char *left = player->splitWorkBuf[index][0] ?
			player->splitWorkBuf[index][0] : player->splitBuf[index][0];
		const signed char *right = player->splitWorkBuf[index][1] ?
			player->splitWorkBuf[index][1] : player->splitBuf[index][1];
		int leftPeak = PlaybackBufferPeakS8(left, frames);
		int rightPeak = PlaybackBufferPeakS8(right, frames);
		return leftPeak > rightPeak ? leftPeak : rightPeak;
	}
	return PlaybackBufferPeakS8(buf, len);
}

static unsigned long DecodeStreamFillPlaybackPrefill(DecodeStream *stream,
	const DecodeOptions *opt, signed char *dest, unsigned long maxBytes,
	unsigned long minSamples)
{
	unsigned long channels;
	unsigned long produced;
	int attempts;

	channels = opt->stereo ? 2UL : 1UL;
	if (channels == 0)
		channels = 1UL;
	if (minSamples == 0 || minSamples * channels > maxBytes)
		minSamples = maxBytes / channels;
	produced = 0;
	attempts = 0;
	while (produced < maxBytes && produced / channels < minSamples &&
		attempts < 8 && !stream->outOfData && !gPlaybackInterrupted) {
		int n;

		if (gPlaybackInterrupted)
			break;
		n = DecodeStreamFillS8(stream, opt, dest + produced,
			(int)(maxBytes - produced));
		if (gPlaybackInterrupted)
			break;
		if (n < 0)
			break;
		if (n == 0) {
			attempts++;
			if (stream->eofReached || stream->outOfData)
				break;
		} else {
			produced += (unsigned long)n;
			attempts = 0;
		}
	}
	return produced;
}

static int ProbeInputSampleRate(InputSource *input, HMP3Decoder decoder,
	DecodeStats *stats)
{
	unsigned char probe[READBUF_SIZE];
	HMP3Decoder probeDecoder;
	unsigned long pos;
	size_t nRead;
	int offset;
	int err;
	MP3FrameInfo info;

	(void)decoder;
	pos = InputSourceTell(input);
	nRead = InputSourceRead(input, probe, sizeof(probe));
	InputSourceSeek(input, pos);
	if (nRead == 0)
		return 0;
	offset = FindValidatedMpegSync(probe, (int)nRead);
	if (offset < 0)
		return 0;
	probeDecoder = MP3InitDecoder();
	if (!probeDecoder)
		return 0;
	err = MP3GetNextFrameInfo(probeDecoder, &info, probe + offset);
	MP3FreeDecoder(probeDecoder);
	if (err != ERR_MP3_NONE)
		return 0;
	UpdateFirstFrameStats(stats, &info);
	return info.samprate;
}

static void PrintPlaybackDebugStartup(const DecodeOptions *opt,
	int playbackRate, unsigned int period, unsigned long requestedBytes,
	unsigned long chunkBytes, const AmigaAudioPlayer *player,
	signed char *buf[3])
{
	if (!opt->debugPlay)
		return;
	printf("debug-play: actual output rate: %d Hz\n", playbackRate);
	printf("debug-play: PAL period: %u\n", period);
	printf("debug-play: requested buffer seconds: %d\n", opt->bufferSeconds);
	printf("debug-play: requested volume percent: %d\n", opt->volumePercent);
	printf("debug-play: mapped audio.device volume: %u (range 0-%u)\n", (unsigned int)VolumePercentToAudioDevice(opt->volumePercent), (unsigned int)AMIGA_AUDIO_DEVICE_MAX_VOLUME);
	printf("debug-play: initial request volume: %u\n", (unsigned int)player->requestVolume);
	printf("debug-play: live volume update method: next CMD_WRITE buffer ioa_Volume; no active writes aborted\n");
	printf("debug-play: volume update sequence count: %lu\n", gMiniAmp3VolumeSequence);
	printf("debug-play: requested half-buffer bytes: %lu\n", requestedBytes);
	printf("debug-play: selected half-buffer samples: %d\n",
		PlaybackHalfBufferSamples(opt, chunkBytes));
	printf("debug-play: selected half-buffer bytes: %lu\n", chunkBytes);
	if (opt->stereo) {
		printf("debug-play: chip buffer A left/right: %p/%p size %lu\n",
			(void *)player->splitBuf[0][0], (void *)player->splitBuf[0][1],
			player->splitBytes);
		printf("debug-play: chip buffer B left/right: %p/%p size %lu\n",
			(void *)player->splitBuf[1][0], (void *)player->splitBuf[1][1],
			player->splitBytes);
		printf("debug-play: fast planar work A left/right: %p/%p size %lu\n",
			(void *)player->splitWorkBuf[0][0],
			(void *)player->splitWorkBuf[0][1], player->splitWorkBytes);
		printf("debug-play: fast planar work B left/right: %p/%p size %lu\n",
			(void *)player->splitWorkBuf[1][0],
			(void *)player->splitWorkBuf[1][1], player->splitWorkBytes);
		printf("debug-play: fast planar work C left/right: %p/%p size %lu\n",
			(void *)player->splitWorkBuf[2][0],
			(void *)player->splitWorkBuf[2][1], player->splitWorkBytes);
	} else {
		printf("debug-play: chip submit buffer A: %p size %lu\n",
			(void *)player->splitBuf[0][0], player->splitBytes);
		printf("debug-play: chip submit buffer B: %p size %lu\n",
			(void *)player->splitBuf[1][0], player->splitBytes);
		printf("debug-play: fast conversion buffer A/B/C: %p/%p/%p size %lu\n",
			(void *)buf[0], (void *)buf[1], (void *)buf[2], chunkBytes);
	}
}

static int AmigaSetupPlaybackBuffers(AmigaAudioPlayer *player,
	const DecodeOptions *opt, unsigned int period, unsigned long requestedBytes,
	unsigned long minBytes, int directPlanar, signed char *buf[3],
	unsigned long *chunkBytes, PlaybackCleanupStatus *status)
{
	unsigned long tryBytes;

	buf[0] = NULL;
	buf[1] = NULL;
	buf[2] = NULL;
	tryBytes = AlignPlaybackChunkBytes(requestedBytes, opt->stereo);
	minBytes = AlignPlaybackChunkBytes(minBytes, opt->stereo);
	if (minBytes == 0)
		minBytes = opt->stereo ? 2UL : 1UL;
	if (tryBytes < minBytes)
		tryBytes = minBytes;

	while (tryBytes >= minBytes) {
		gGuiPlaybackStatus.tryBytes = tryBytes;
		GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
		if (AmigaAudioOpen(player, period, opt->stereo, tryBytes, opt->volumePercent) == 0) {
			int workReady;

			player->debugCleanup = opt->debugCleanup;
			player->debugPlay = opt->debugPlay;
			player->outputStride = opt->fastLowrate ?
				FastLowrateStrideForOutputRate(opt->outputRate) : 1;
			workReady = 0;
			if (!directPlanar)
				GuiPublishStartupStage(GUISTART_ALLOC_WORK_BUFFERS);
			if (!directPlanar &&
				AmigaAudioAllocWorkBuffers(player, opt->stereo, tryBytes) == 0) {
				if (opt->stereo) {
					workReady =
						player->splitWorkBuf[0][0] && player->splitWorkBuf[0][1] &&
						player->splitWorkBuf[1][0] && player->splitWorkBuf[1][1] &&
						player->splitWorkBuf[2][0] && player->splitWorkBuf[2][1];
				} else {
					buf[0] = player->workBuf[0];
					buf[1] = player->workBuf[1];
					buf[2] = player->workBuf[2];
					workReady = buf[0] && buf[1] && buf[2];
				}
			}
			if (directPlanar || workReady) {
				GuiPublishStartupStage(GUISTART_AUDIO_SETUP_DONE);
				*chunkBytes = tryBytes;
				if (tryBytes != requestedBytes)
					printf("play buffer reduced to %lu bytes per half-buffer\n",
						tryBytes);
				return 0;
			}
			{
				int wasInterrupted = gPlaybackInterrupted;
				AmigaAudioClose(player, status);
				if (!wasInterrupted)
					gPlaybackInterrupted = 0;
			}
			buf[0] = NULL;
			buf[1] = NULL;
			buf[2] = NULL;
		}

		if (tryBytes <= minBytes)
			break;
		tryBytes = AlignPlaybackChunkBytes(tryBytes / 2UL, opt->stereo);
		if (tryBytes < minBytes)
			tryBytes = minBytes;
	}

	fprintf(stderr, "cannot allocate audio buffers (requested %lu bytes per half-buffer)\n",
		requestedBytes);
	return -1;
}

static int AmigaAudioOpenSilentSelftest(const DecodeOptions *opt)
{
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	signed char *buf[3];
	unsigned long chunkBytes;
	unsigned int period;
	int err;

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	buf[0] = NULL;
	buf[1] = NULL;
	buf[2] = NULL;
	gGuiPlaybackStatus.requestedRate = opt->outputRate;
	gGuiPlaybackStatus.effectiveRate = opt->outputRate;
	period = AmigaPalAudioPeriod(opt->outputRate);
	gGuiPlaybackStatus.paulaPeriod = period;
	gGuiPlaybackStatus.requestedBytes = 256UL;
	err = AmigaSetupPlaybackBuffers(&player, opt, period, 256UL, 1UL, 0,
		buf, &chunkBytes, &cleanupStatus);
	if (err == 0) {
		memset(buf[0], 0, (size_t)chunkBytes);
		if (AmigaAudioPreparePlaybackBuffer(&player, 0, buf[0], chunkBytes) != 0 ||
			AmigaAudioCommitPlaybackBuffer(&player, 0) != 0 ||
			AmigaAudioWait(&player, 0) != 0)
			err = -1;
	}
	printf("audio-open-silent-test: rate=%d period=%u bytes=%lu result=%d openDevice=%d\n",
		opt->outputRate, period, chunkBytes, err, gGuiPlaybackStatus.openDeviceResult);
	AmigaAudioClose(&player, &cleanupStatus);
	return err;
}

static void AmigaPlaybackCopyInterleavedToWork(AmigaAudioPlayer *player,
	int index, const signed char *pcm, unsigned long len)
{
	if (player->stereo) {
		unsigned long frames = len / 2UL;
		unsigned long i;

		for (i = 0; i < frames; i++) {
			player->splitWorkBuf[index][0][i] = pcm[2UL * i];
			player->splitWorkBuf[index][1][i] = pcm[2UL * i + 1UL];
		}
	} else {
		memcpy(player->workBuf[index], pcm, len);
	}
}

static int AmigaPlayWholeBuffer(const signed char *pcm, unsigned long totalBytes,
	const DecodeOptions *opt, DecodeStats *stats)
{
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	unsigned int period;
	unsigned long pos;
	unsigned long chunkBytes;
	signed char *buf[3];
	unsigned long len[3];
	int cur;
	int pending;
	int first;
	int err;

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	err = -1;
	if (totalBytes == 0) {
		fprintf(stderr, "no decoded samples; audio.device playback not started\n");
		goto cleanup;
	}
	{
		int playbackRate = PlaybackOutputSampleRate(opt, stats);
		period = AmigaPalAudioPeriod(playbackRate);
		PrintFastLowrateOutputRateDifference(opt, playbackRate);
		printf("play output rate: %d Hz\n", playbackRate);
	}
	printf("PAL audio period: %u\n", period);
	chunkBytes = PlaybackRequestedChunkBytes(opt, PlaybackOutputSampleRate(opt, stats));
	if (chunkBytes > PlaybackMaxChunkBytes(opt->stereo))
		printf("requested %d second half-buffer exceeds audio.device per-write limit; maximum at this rate is %lu ms\n",
			opt->bufferSeconds, PlaybackMaxHalfBufferMilliseconds(opt,
				PlaybackOutputSampleRate(opt, stats)));
	if (AmigaSetupPlaybackBuffers(&player, opt, period, chunkBytes, 1UL, 0,
		buf, &chunkBytes, &cleanupStatus) != 0) {
		goto cleanup;
	}
	printf("playback half-buffer: %lu ms, %lu bytes\n",
		PlaybackBufferDurationMilliseconds(opt, chunkBytes,
			PlaybackOutputSampleRate(opt, stats)), chunkBytes);
	pos = 0;
	for (cur = 0; cur < 2; cur++) {
		len[cur] = totalBytes - pos;
		if (len[cur] > chunkBytes)
			len[cur] = chunkBytes;
		if (opt->stereo && (len[cur] & 1UL))
			len[cur]--;
		if (len[cur] > 0)
			AmigaPlaybackCopyInterleavedToWork(&player, cur, pcm + pos,
				len[cur]);
		pos += len[cur];
	}
	cur = 0;
	pending = 0;
	first = 1;
	while (!gPlaybackInterrupted && len[cur] > 0) {
		if (AmigaAudioPreparePlaybackBuffer(&player, cur, opt->stereo ? NULL : buf[cur],
			len[cur]) != 0 || AmigaAudioCommitPlaybackBuffer(&player, cur) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(cur));
			goto cleanup;
		}
		pending = 1;
		if (!first) {
			if (AmigaAudioWait(&player, 1 - cur) != 0) {
				fprintf(stderr, "audio.device write failed\n");
				goto cleanup;
			}
			len[1 - cur] = totalBytes - pos;
			if (len[1 - cur] > chunkBytes)
				len[1 - cur] = chunkBytes;
			if (opt->stereo && (len[1 - cur] & 1UL))
				len[1 - cur]--;
			if (len[1 - cur] > 0) {
				AmigaPlaybackCopyInterleavedToWork(&player, 1 - cur,
					pcm + pos, len[1 - cur]);
				pos += len[1 - cur];
			}
		} else {
			first = 0;
		}
		cur = 1 - cur;
	}
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		goto cleanup;
	}
	if (pending) {
		if (AmigaAudioWait(&player, 1 - cur) != 0) {
			fprintf(stderr, "audio.device write failed\n");
			goto cleanup;
		}
	}
	err = 0;
cleanup:
	GuiPublishStartupStage(err == 0 ? GUISTART_CLEANUP : GUISTART_FAILED);
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_STOPPING;
	gGuiPlaybackStatus.cleanupComplete = 0;
	AmigaAudioClose(&player, &cleanupStatus);
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	if (cleanupStatus.canaryErrors)
		err = -1;
	PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	(void)stats;
	return err;
}

static int AmigaPlayDecodeThenPlay(InputSource *input, HMP3Decoder decoder,
	const DecodeOptions *opt, DecodeStats *stats, TimingStats *timing)
{
	DecodeStream stream;
	signed char temp[4096];
	signed char *all;
	unsigned long used;
	unsigned long cap;
	int n;
	int err;

	DecodeStreamInit(&stream, input, decoder, stats, timing);
	all = NULL;
	used = 0;
	cap = 0;
	err = -1;
	while (!gPlaybackInterrupted &&
		(n = DecodeStreamFillS8(&stream, opt, temp, sizeof(temp))) > 0) {
		if (used + (unsigned long)n > cap) {
			unsigned long newCap = cap ? cap * 2UL : 65536UL;
			signed char *newAll;
			while (newCap < used + (unsigned long)n)
				newCap *= 2UL;
			newAll = (signed char *)realloc(all, newCap);
			if (!newAll) {
				fprintf(stderr, "cannot allocate decode-then-play RAM\n");
				goto cleanup;
			}
			all = newAll;
			cap = newCap;
		}
		memcpy(all + used, temp, n);
		used += (unsigned long)n;
	}
	if (stream.decodeError)
		goto cleanup;
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		goto cleanup;
	}
	printf("decode-then-play bytes: %lu\n", used);
	err = AmigaPlayWholeBuffer(all, used, opt, stats);
cleanup:
	free(all);
	all = NULL;
	if (!gGuiPlaybackStatus.cleanupComplete) {
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_STOPPING;
		gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
		gGuiPlaybackStatus.cleanupComplete = 1;
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	}
	return err;
}

static int AmigaPlayStreaming(InputSource *input, HMP3Decoder decoder,
	const DecodeOptions *opt, DecodeStats *stats, TimingStats *timing)
{
	DecodeStream stream;
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	unsigned int period;
	unsigned long bufBytes;
	unsigned long requestedBytes;
	signed char *buf[3];
	signed char startupBuf[OUTBUF_SAMPS];
	unsigned long startupLen;
	unsigned long len[3];
	unsigned long playbackChannels;
	unsigned long halfMilliseconds;
	int playbackRate;
	int inputSampleRate;
	int active;
	int decodeAhead;
	int initialDecodeSlots;
	int liveSlots;
	int refill;
	int err;

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	/* Publish an immediate child-side state before any probing or
	 * audio.device setup can block.  This keeps the GUI from sitting on
	 * its optimistic launch message and proves the new playback process
	 * accepted the start request. */
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_BUFFERING;
	buf[0] = NULL;
	buf[1] = NULL;
	buf[2] = NULL;
	len[0] = 0;
	len[1] = 0;
	len[2] = 0;
	err = -1;
	GuiPublishStartupStage(GUISTART_PROBE_RATE);
	if (AmigaPlaybackStopRequested(opt, "before input rate probe"))
		goto cleanup;
	inputSampleRate = ProbeInputSampleRate(input, decoder, stats);
	GuiPublishStartupStage(GUISTART_PROBE_RATE_DONE);
	if (AmigaPlaybackStopRequested(opt, "after input rate probe"))
		goto cleanup;
	playbackRate = EffectiveOutputSampleRate(opt, inputSampleRate);
	if (playbackRate <= 0)
		playbackRate = opt->outputRate > 0 ? opt->outputRate : 8287;
	stats->outputSampleRate = playbackRate;
	gGuiPlaybackStatus.sampleRate = playbackRate;
	gGuiPlaybackStatus.effectiveRate = playbackRate;
	GuiPublishStartupStage(GUISTART_STREAM_INIT);
	if (AmigaPlaybackStopRequested(opt, "before stream init"))
		goto cleanup;
	DecodeStreamInit(&stream, input, decoder, stats, timing);
	if (AmigaPlaybackStopRequested(opt, "after stream init"))
		goto cleanup;
	period = AmigaPalAudioPeriod(playbackRate);
	gGuiPlaybackStatus.paulaPeriod = period;
	PrintFastLowrateOutputRateDifference(opt, playbackRate);
	printf("play output rate: %d Hz\n", playbackRate);
	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	if (requestedBytes > PlaybackMaxChunkBytes(opt->stereo))
		printf("requested %d second half-buffer exceeds audio.device per-write limit; maximum at this rate is %lu ms\n",
			opt->bufferSeconds, PlaybackMaxHalfBufferMilliseconds(opt, playbackRate));
	printf("PAL audio period: %u\n", period);
	/* Mono validates a decoded frame before allocating playback buffers. */
	startupLen = 0;
	if (!opt->stereo) {
		GuiPublishStartupStage(GUISTART_PREFILL);
		if (AmigaPlaybackStopRequested(opt, "before prefill"))
			goto cleanup;
		startupLen = DecodeStreamFillPlaybackPrefill(&stream, opt, startupBuf,
			OUTBUF_SAMPS, 1UL);
		GuiPublishStartupStage(GUISTART_PREFILL_DONE);
		if (AmigaPlaybackStopRequested(opt, "after prefill"))
			goto cleanup;
		if (stream.decodeError || startupLen == 0) {
			fprintf(stderr, "no decoded samples; audio.device playback not started\n");
			goto cleanup;
		}
	}
	GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
	if (AmigaPlaybackStopRequested(opt, "before audio setup"))
		goto cleanup;
	gGuiPlaybackStatus.requestedBytes = requestedBytes;
	if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
		opt->stereo ? 2UL : startupLen, 0, buf, &bufBytes,
		&cleanupStatus) != 0) {
		goto cleanup;
	}
	halfMilliseconds = PlaybackBufferDurationMilliseconds(opt, bufBytes,
		playbackRate);
	gGuiPlaybackStatus.halfBufferMs = halfMilliseconds;
	if (AmigaPlaybackStopRequested(opt, "after audio setup"))
		goto cleanup;
	printf("playback half-buffer: %lu ms, %lu bytes\n", halfMilliseconds,
		bufBytes);
	PrintPlaybackDebugStartup(opt, playbackRate, period, requestedBytes,
		bufBytes, &player, buf);

	/* Fill decode buffers before the first CMD_WRITE starts playback.  Mono
	 * remains a true three-request audio.device ring.  Stereo queues only two
	 * live DMA pairs (A/B) and keeps C as a Fast RAM decode-ahead buffer; C is
	 * copied into whichever A/B chip pair has been WaitIO-reaped. */
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_BUFFERING;
	playbackChannels = opt->stereo ? 2UL : 1UL;
	liveSlots = AmigaAudioLiveSlots(opt->stereo);
	decodeAhead = opt->stereo ? 2 : -1;
	initialDecodeSlots = opt->stereo ? AMIGA_STEREO_DECODE_SLOTS : liveSlots;
	for (active = 0; active < initialDecodeSlots; active++) {
		GuiPublishStartupStage(active == 0 ? GUISTART_FILL_BUFFER_A :
			(active == 1 ? GUISTART_FILL_BUFFER_B : GUISTART_FILL_BUFFER_B));
		if (gPlaybackInterrupted)
			goto cleanup;
		if (active == 0 && !opt->stereo) {
			memcpy(buf[0], startupBuf, (size_t)startupLen);
			len[0] = startupLen;
			if (gPlaybackInterrupted)
				goto cleanup;
			if (len[0] < bufBytes)
				len[0] += (unsigned long)DecodeStreamFillS8(&stream, opt,
					buf[0] + len[0], (int)(bufBytes - len[0]));
		} else {
			len[active] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				active, buf[active], bufBytes);
		}
		if (gPlaybackInterrupted)
			goto cleanup;
		GuiPublishStartupStage(active == 0 ? GUISTART_FILL_BUFFER_A_DONE :
			(active == 1 ? GUISTART_FILL_BUFFER_B_DONE : GUISTART_FILL_BUFFER_B_DONE));
		PrintPlaybackFillDebug(opt, active, len[active]);
		if (stream.decodeError)
			goto cleanup;
		if (active == 0 && len[0] > 0 && opt->debugPlay &&
			PlaybackBufferPeak(opt, &player, 0, buf[0], len[0]) == 0)
			printf("first playback buffer is silent/near-silent\n");
		if (active == 0 && (len[0] == 0 || len[0] / playbackChannels == 0)) {
			fprintf(stderr, "first playback buffer fill produced zero CMD_WRITE bytes\n");
			goto cleanup;
		}
		if (len[active] == 0)
			break;
		if (active < liveSlots) {
			GuiPublishStartupStage(active == 0 ? GUISTART_PREPARE_A :
				(active == 1 ? GUISTART_PREPARE_B : GUISTART_PREPARE_B));
			if (gPlaybackInterrupted)
				goto cleanup;
			if (AmigaAudioPreparePlaybackBuffer(&player, active, buf[active],
				len[active]) != 0) {
				fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
					PlaybackBufferName(active));
				goto cleanup;
			}
		}
	}

	if (active == 0)
		goto cleanup;
	for (refill = 0; refill < active && refill < liveSlots; refill++) {
		if (gPlaybackInterrupted)
			goto cleanup;
		if (refill == 0)
			GuiPublishStartupStage(GUISTART_COMMIT_A);
		if (AmigaAudioCommitPlaybackBuffer(&player, refill) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(refill));
			goto cleanup;
		}
	}
	GuiPublishStartupStage(GUISTART_PLAYING);
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_PLAYING;
	if (opt->debugPlay) {
		printf("debug-play: CMD_WRITE queued initial ring depth %d\n",
			active < liveSlots ? active : liveSlots);
		if (opt->stereo)
			printf("debug-play: stereo decode-ahead buffer C prepared: %lu bytes\n",
				len[decodeAhead]);
	}
	err = 0;

	active = 0;
	while (err == 0 && !gPlaybackInterrupted &&
		player.sent[active][0]) {
		clock_t waitStartedAt;
		clock_t refillFinishedAt;
		unsigned long elapsedMilliseconds;
		unsigned long activeMilliseconds;
		long spareMilliseconds;
		int justFreed;
		int underrun;
		int late;

#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			break;
		}
#endif

		/* Wait for the oldest queued live slot before reusing any buffers.
		 * Mono reuses the completed slot in its three-request ring.  Stereo first
		 * WaitIO-reaps both channels in the completed A/B pair, then copies the
		 * prepared Fast RAM C decode-ahead block into that chip pair before
		 * resubmitting it and decoding the next block into C. */
		waitStartedAt = clock();
		if (gPlaybackInterrupted)
			break;
		underrun = AmigaAudioDone(&player, active);
		if (AmigaAudioWait(&player, active) != 0) {
			fprintf(stderr, "audio.device write failed\n");
			err = -1;
			break;
		}
#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			break;
		}
#endif
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE completed %s\n",
				PlaybackBufferName(active));

		justFreed = active;
		if (opt->stereo) {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[decodeAhead], playbackRate);
			if (len[decodeAhead] == 0) {
				active = (active + 1) % liveSlots;
				break;
			}
			if (AmigaAudioCopyStereoDecodeAheadToSlot(&player, justFreed,
				decodeAhead, len[decodeAhead]) != 0) {
				fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
					PlaybackBufferName(justFreed));
				err = -1;
				break;
			}
			len[justFreed] = len[decodeAhead];
		} else {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[justFreed], playbackRate);
			if (gPlaybackInterrupted)
				break;
			len[justFreed] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				justFreed, buf[justFreed], bufBytes);
			PrintPlaybackFillDebug(opt, justFreed, len[justFreed]);
			if (stream.decodeError) {
				err = -1;
				break;
			}
			if (len[justFreed] == 0) {
				active = (active + 1) % liveSlots;
				break;
			}
		}

		if (gPlaybackInterrupted)
			break;
		if (AmigaAudioPreparePlaybackBuffer(&player, justFreed, buf[justFreed],
			len[justFreed]) != 0 ||
			AmigaAudioCommitPlaybackBuffer(&player, justFreed) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(justFreed));
			err = -1;
			break;
		}
		if (opt->stereo) {
			if (gPlaybackInterrupted)
				break;
			len[decodeAhead] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				decodeAhead, buf[decodeAhead], bufBytes);
			PrintPlaybackFillDebug(opt, decodeAhead, len[decodeAhead]);
			if (stream.decodeError) {
				err = -1;
				break;
			}
		}
		refillFinishedAt = clock();
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE resubmitted %s: %lu bytes\n",
				PlaybackBufferName(justFreed), len[justFreed]);

		active = (active + 1) % liveSlots;
		elapsedMilliseconds = PlaybackElapsedMilliseconds(waitStartedAt,
			refillFinishedAt);
		spareMilliseconds = (long)activeMilliseconds - (long)elapsedMilliseconds;
		late = (spareMilliseconds < 0) || underrun;
		if (!stats->spareTimeMeasured || spareMilliseconds < stats->minimumSpareMilliseconds) {
			stats->minimumSpareMilliseconds = spareMilliseconds;
			stats->spareTimeMeasured = 1;
		}
		if (late)
			stats->lateBuffers++;
		if (underrun) {
			stats->underruns++;
			stats->underrunBuffers[justFreed]++;
			if (opt->debugPlay)
				printf("debug-play: underrun detected before buffer %s refill wait\n",
					PlaybackBufferName(justFreed));
		}
		gGuiPlaybackStatus.spareMs = spareMilliseconds;
		gGuiPlaybackStatus.underruns = stats->underruns;
		gGuiPlaybackStatus.decodedFrames = stats->decodedFrames;
		if (stream.effectiveRate)
			gGuiPlaybackStatus.sampleRate = stream.effectiveRate;
		if (underrun)
			gGuiPlaybackStatus.phase = GUIPLAY_PHASE_UNDERRUN;
		else if (gGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN)
			gGuiPlaybackStatus.phase = GUIPLAY_PHASE_PLAYING;
	}

	if (err == 0 && !gPlaybackInterrupted) {
		int drain;
		for (drain = 0; drain < liveSlots; drain++) {
			if (player.sent[drain][0]) {
				if (AmigaAudioWait(&player, drain) != 0) {
					fprintf(stderr, "audio.device write failed\n");
					err = -1;
					break;
				} else if (opt->debugPlay) {
					printf("debug-play: CMD_WRITE completed %s\n",
						PlaybackBufferName(drain));
				}
			}
		}
	}

	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		err = -1;
	}
cleanup:
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_STOPPING;
	gGuiPlaybackStatus.cleanupComplete = 0;
	AmigaAudioClose(&player, &cleanupStatus);
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	if (cleanupStatus.canaryErrors)
		err = -1;
	PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	return err;
}

static int AmigaPlayLifecycleTest(const DecodeOptions *opt)
{
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	unsigned int period;
	unsigned long requestedBytes;
	unsigned long chunkBytes;
	signed char *buf[3];
	int playbackRate;
	int pass;
	int err;

	playbackRate = opt->outputRate > 0 ? opt->outputRate : (opt->stereo ? 8820 : 8287);
	period = AmigaPalAudioPeriod(playbackRate);
	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	err = 0;
	for (pass = 0; pass < 5 && err == 0 && !gPlaybackInterrupted; pass++) {
		unsigned long len;

		memset(&player, 0, sizeof(player));
		PlaybackCleanupStatusInit(&cleanupStatus);
		buf[0] = NULL;
		buf[1] = NULL;
		printf("play cleanup self-test pass %d/5\n", pass + 1);
		GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
	gGuiPlaybackStatus.requestedBytes = requestedBytes;
	if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
			opt->stereo ? 2UL : 1UL, 0, buf, &chunkBytes, &cleanupStatus) != 0) {
			PrintPlaybackCleanupStatus(opt, &cleanupStatus);
			err = -1;
			break;
		}
		len = (unsigned long)playbackRate / 20UL;
		if (len < 1UL)
			len = 1UL;
		if (opt->stereo)
			len *= 2UL;
		if (len > chunkBytes)
			len = chunkBytes;
		if (opt->stereo) {
			if (!player.splitWorkBuf[0][0] || !player.splitWorkBuf[0][1]) {
				fprintf(stderr, "play lifecycle test work buffer missing\n");
				err = -1;
			} else {
				memset(player.splitWorkBuf[0][0], 0, len / 2UL);
				memset(player.splitWorkBuf[0][1], 0, len / 2UL);
			}
		} else {
			memset(buf[0], 0, len);
		}
		if (err != 0) {
			AmigaAudioClose(&player, &cleanupStatus);
			PrintPlaybackCleanupStatus(opt, &cleanupStatus);
			break;
		}
		if (AmigaAudioPreparePlaybackBuffer(&player, 0, opt->stereo ? NULL : buf[0],
			len) != 0 || AmigaAudioCommitPlaybackBuffer(&player, 0) != 0) {
			fprintf(stderr, "play lifecycle test CMD_WRITE byte length is invalid\n");
			err = -1;
		}
		AmigaAudioClose(&player, &cleanupStatus);
		if (cleanupStatus.canaryErrors)
			err = -1;
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	}
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		err = -1;
	}
	return err;
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
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	BPTR amigaInputFile;
#endif
	InputSource input;
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
	int verifyError;
	clock_t startClock;
	clock_t endClock;
	NormalizedArgs normalized;
	int debugArgv;
	int effectiveRate;
	char *resolvedOutName;

	resolvedOutName = NULL;
	infile = NULL;
	outfile = NULL;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	amigaInputFile = (BPTR)0;
#endif

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
	if (opt.selftestClz) {
		int selftestErr = SelftestClz();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32) {
		int selftestErr;
		gSelftestVerbose = opt.selftestVerbose;
		selftestErr = SelftestFdct32();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32Half) {
		int selftestErr;
		gSelftestVerbose = opt.selftestVerbose || opt.selftestFdct32HalfDebug;
		gSelftestFdct32HalfDebug = opt.selftestFdct32HalfDebug;
		selftestErr = SelftestFdct32Half();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdct) {
		int selftestErr = SelftestImdct();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdctThin) {
		int selftestErr = AMIGA_IMDCT_THIN_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestSubbandCap) {
		int selftestErr = AMIGA_IMDCT_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestAntialias) {
		int selftestErr = SelftestAntialias();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphase) {
		int selftestErr = SelftestPolyphase();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2) {
		int selftestErr = SelftestPolyphaseStride2();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4) {
		int selftestErr = SelftestPolyphaseStride4();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4Stereo) {
		int selftestErr = SelftestPolyphaseStride4Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2Stereo) {
		int selftestErr = SelftestPolyphaseStride2Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFastLowrate) {
		int selftestErr = SelftestFastLowrate();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestReducedTaps) {
		int selftestErr = SelftestReducedTaps();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32Quarter) {
		int selftestErr = SelftestFdct32Quarter();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestHuffman) {
		int selftestErr = SelftestHuffman(&opt);
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestDequant) {
		int selftestErr = SelftestDequant();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestBitstream) {
		int selftestErr = AMIGA_BITSTREAM_REFILL_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestMonoFastLowrateStereo) {
		int selftestErr = SelftestMonoFastLowrateStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestQuality) {
		int selftestErr = SelftestQuality();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.startupVolumeSelftest) {
		int selftestErr = SelftestStartupVolume();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr == 0 ? 0 : 1;
	}
	if (opt.audioOpenSilentTest) {
		int audioTestErr = AmigaAudioOpenSilentSelftest(&opt);
		AmigaFreeNormalizedArgs(&normalized);
		return audioTestErr == 0 ? 0 : 1;
	}
	if (opt.playLifecycleTest) {
		int playTestErr;
		/* Preserve any GUI stop request that may have arrived before the
		 * lifecycle test reaches its playback loop. */
#ifndef AMIGA_M68K
		signal(SIGINT, PlaybackSignalHandler);
#endif
		playTestErr = AmigaPlayLifecycleTest(&opt);
#ifndef AMIGA_M68K
		signal(SIGINT, SIG_DFL);
#endif
		AmigaFreeNormalizedArgs(&normalized);
		return playTestErr == 0 ? 0 : 1;
	}

	GuiPublishStartupStage(GUISTART_ARGS_READY);
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

	GuiPublishStartupStage(GUISTART_INPUT_OPEN);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before input open")) {
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	gGuiPlaybackStatus.requestedRate = opt.outputRate;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (opt.play) {
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
		amigaInputFile = Open((STRPTR)opt.inName, MODE_OLDFILE);
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
		if (!amigaInputFile) {
			fprintf(stderr, "cannot open input: %s\n", opt.inName);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		InputSourceInitAmigaDos(&input, amigaInputFile);
		amigaInputFile = (BPTR)0;
	} else
#endif
	{
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
		infile = fopen(opt.inName, "rb");
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
		if (!infile) {
			fprintf(stderr, "cannot open input: %s\n", opt.inName);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		InputSourceInit(&input, infile);
	}
	if (opt.info && infile) {
		PrintMp3Info(infile, opt.inName);
		if (!opt.play && !opt.outName) {
			CloseInputFile(&infile, opt.debugCleanup);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 0;
		}
	}
	if (opt.play)
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_BUFFERING;
	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input open")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (opt.fastMem)
		GuiPublishStartupStage(GUISTART_INPUT_PRELOAD_FASTMEM);
	if (opt.fastMem && InputSourcePreloadFastMemory(&input) != 0) {
		fprintf(stderr, "cannot preload input into Fast RAM: %s\n", opt.inName);
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input preload")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	GuiPublishStartupStage(GUISTART_INPUT_PREPARE);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before input prepare")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (InputSourcePrepareMp3(&input) != 0) {
		fprintf(stderr, "cannot inspect MP3 input: %s\n", opt.inName);
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	outfile = NULL;
	if (!opt.noOutput) {
		outfile = fopen(opt.outName, opt.outFormat == OUT_8SVX ? "wb+" : "wb");
		if (!outfile) {
			fprintf(stderr, "cannot open output: %s\n", opt.outName);
			InputSourceClose(&input);
			CloseInputFile(&infile, opt.debugCleanup);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
	}

	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input prepare")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	GuiPublishStartupStage(GUISTART_DECODER_ALLOC);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before decoder alloc")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	decoder = MP3InitDecoder();
	if (!decoder) {
		fprintf(stderr, "MP3InitDecoder failed\n");
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		if (outfile)
			fclose(outfile);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	if (opt.stereo)
		fprintf(stderr, "Stereo playback needs significantly more CPU and may underrun on 030.\n");
	GuiPublishStartupStage(GUISTART_DECODER_CONFIG);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before decoder config")) {
		MP3FreeDecoder(decoder);
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		if (outfile)
			fclose(outfile);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	MP3SetOutputMono(decoder, opt.mono && !opt.stereo && !opt.noMonoMSSideSkip);
	MP3SetMonoMSSideSkip(decoder, !opt.noMonoMSSideSkip);
	if (opt.expPoly) {
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
		fprintf(stderr, "warning: --exp-poly enables experimental 68030 asm "
			"mono polyphase when real/amiga_m68k_polyphase.S is linked; "
			"otherwise it falls back to the existing fast path\n");
#else
		fprintf(stderr, "warning: --exp-poly requested, but this build has no 68030 asm polyphase; using existing polyphase\n");
#endif
	}
	MP3SetExperimentalPolyphase(opt.expPoly);
	MP3SetForceStereoStride2PolyphaseC(opt.forceCPolyphaseStride2Stereo);
	MP3ResetStereoStride2PolyphaseCounters();
	MP3SetExperimentalHuffman(opt.expHuff);
	MP3SetExperimentalIMDCTThin(decoder, opt.expImdctThin);
	MP3SetExperimentalReducedTaps(opt.expReducedTaps);
	MP3SetExperimentalFDCT32Quarter(opt.expFdct32Quarter ||
		(opt.superfastLowrate && opt.outputRate == 11025));
	if (opt.fastLowrate) {
		int stride = FastLowrateStrideForOutputRate(opt.outputRate);
		if (opt.expReducedTaps && stride != 4)
			fprintf(stderr, "warning: --exp-reduced-taps only affects 11025 Hz stride-4 fast-lowrate output\n");
		if (opt.expFdct32Quarter && stride != 4)
			fprintf(stderr, "warning: --exp-fdct32-quarter only affects 11025 Hz stride-4 fast-lowrate output\n");
		MP3SetFastLowrate(decoder, stride);
		if (opt.superfastLowrate)
			MP3SetSuperfastLowrate(decoder, 1);
		GuiPublishStartupStage(GUISTART_FASTLOWRATE_WARN_BEFORE);
		if (!gMiniAmp3EmbeddedPlayback && opt.outputRate == 22050)
			fprintf(stderr,
				"22050 requires significantly more CPU and may underrun on 030 systems.\n");
		GuiPublishStartupStage(GUISTART_FASTLOWRATE_WARN_AFTER);
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
		if (opt.expReducedTaps) {
#if defined(AMIGA_FAST_REDUCED_TAPS)
			fprintf(stderr, "warning: --exp-reduced-taps enables lossy 8-segment stride-4 dewindowing\n");
#else
			fprintf(stderr, "warning: --exp-reduced-taps requested, but this build lacks AMIGA_FAST_REDUCED_TAPS\n");
#endif
		}
		if (opt.expFdct32Quarter) {
#if defined(AMIGA_FAST_FDCT32_QUARTER)
			fprintf(stderr, "warning: --exp-fdct32-quarter enables lossy stride-4 quarter-rate FDCT32 synthesis\n");
#else
			fprintf(stderr, "warning: --exp-fdct32-quarter requested, but this build lacks AMIGA_FAST_FDCT32_QUARTER\n");
#endif
		}
		if (opt.expImdctThin) {
#if defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
			fprintf(stderr, "warning: --exp-imdct-thin is disabled because stride-4 playback needs every IMDCT subband for full FDCT32 synthesis\n");
#else
			fprintf(stderr, "warning: --exp-imdct-thin requested, but this build lacks AMIGA_M68K_IMDCT_THIN_OUTPUT\n");
#endif
		}
		fprintf(stderr, "warning: --fast-lowrate is experimental, lower quality, "
			"and only skips polyphase output samples%s\n",
			opt.expFdct32Quarter ? "; FDCT32 uses the requested lossy quarter-rate path" :
			"; IMDCT/DCT32 still run full-rate");
#else
		fprintf(stderr, "warning: --fast-lowrate is experimental and lower quality; "
			"this build still generates full polyphase output before decimation\n");
#endif
	}
	if (opt.play && opt.outputRate == 28600)
		fprintf(stderr,
			"28600 PAL-top playback uses normal post-decode decimation and may underrun on 030 systems.\n");

	if (opt.play) {
		int playErr;
		TimingStats *playTiming;

		GuiPublishStartupStage(GUISTART_STREAM_INIT);
		if (AmigaPlaybackStopRequested(&opt, "immediately before playback")) {
			MP3FreeDecoder(decoder);
			InputSourceClose(&input);
			CloseInputFile(&infile, opt.debugCleanup);
			if (outfile)
				fclose(outfile);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		playTiming = opt.bench ? &timing : NULL;
		/* Do not clear gPlaybackInterrupted here.  The MiniAMP3 GUI can
		 * signal Stop after PlaybackEntry() resets decoder statics but before
		 * this play block is reached; clearing the flag at this late point
		 * loses that stop request and leaves the old child holding audio.device
		 * while the GUI waits to start the next song/rate.  Fresh CLI starts and
		 * GUI launches already reset the flag before entering main(). */
#ifndef AMIGA_M68K
		signal(SIGINT, PlaybackSignalHandler);
#endif
		gMiniAmp3RequestedVolume = (unsigned short)opt.volumePercent;
		gMiniAmp3VolumeSequence++;
		gTiming = playTiming;
		MP3SetDecodeCoreProfileEnabled(opt.bench);
		if (opt.bench) {
			MP3ResetDecodeCoreProfile();
			startClock = clock();
		}
		if (opt.decodeThenPlay)
			playErr = AmigaPlayDecodeThenPlay(&input, decoder, &opt, &stats, playTiming);
		else
			playErr = AmigaPlayStreaming(&input, decoder, &opt, &stats, playTiming);
		if (opt.bench)
			endClock = clock();
		if (!stats.outputSampleRate)
			stats.outputSampleRate = PlaybackOutputSampleRate(&opt, &stats);
		printf("input sample rate: %d Hz\n", stats.sampleRate);
		PrintFastLowrateOutputRateDifference(&opt, stats.outputSampleRate);
		printf("output sample rate: %d Hz\n", stats.outputSampleRate);
		printf("channels: %d (%s output)\n", stats.channels,
			opt.stereo ? "stereo" : "mono");
		printf("bitrate: %d bps\n", stats.bitrate);
		printf("decoded frames: %lu\n", stats.decodedFrames);
		printf("output samples: %lu\n", stats.outputSamples);
		PrintOutputStats(&opt, &stats);
		if (opt.checksum)
			printf("playback PCM checksum: %08lx\n", stats.pcmChecksum);
		printf("playback underruns: %lu\n", stats.underruns);
		printf("playback underruns buffer 0: %lu\n", stats.underrunBuffers[0]);
		printf("playback underruns buffer 1: %lu\n", stats.underrunBuffers[1]);
		printf("playback underruns buffer 2: %lu\n", stats.underrunBuffers[2]);
		printf("playback late buffers: %lu\n", stats.lateBuffers);
		if (stats.spareTimeMeasured)
			printf("playback minimum spare before buffer end: %ld ms\n",
				stats.minimumSpareMilliseconds);
		else
			printf("playback minimum spare before buffer end: n/a\n");
		printf("fast-lowrate stride: %d (experimental; IMDCT/DCT32 still full-rate)\n",
			MP3GetFastLowrateStride(decoder));
		if (opt.bench) {
			double elapsed = 0.0;
			double audioSeconds;
			if (CLOCKS_PER_SEC > 0)
				elapsed = (double)(endClock - startClock) / (double)CLOCKS_PER_SEC;
			audioSeconds = DecodedAudioSeconds(&opt, &stats);
			printf("elapsed seconds: %.3f\n", elapsed);
			if (elapsed > 0.0 && audioSeconds > 0.0)
				printf("decode speed: %.2fx realtime\n", audioSeconds / elapsed);
			printf("playback underruns: %lu\n", stats.underruns);
			printf("playback underruns buffer 0: %lu\n", stats.underrunBuffers[0]);
			printf("playback underruns buffer 1: %lu\n", stats.underrunBuffers[1]);
			printf("playback underruns buffer 2: %lu\n", stats.underrunBuffers[2]);
			printf("playback late buffers: %lu\n", stats.lateBuffers);
			if (stats.spareTimeMeasured)
				printf("playback minimum spare before buffer end: %ld ms\n",
					stats.minimumSpareMilliseconds);
			else
				printf("playback minimum spare before buffer end: n/a\n");
			printf("timing frame decode: %.3f s\n", ClocksToSeconds(timing.frameDecode));
			printf("timing PCM conversion: %.3f s\n", ClocksToSeconds(timing.pcmConvert));
		}
#ifndef AMIGA_M68K
		signal(SIGINT, SIG_DFL);
#endif
		MP3FreeDecoder(decoder);
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		gTiming = NULL;
		MP3SetDecodeCoreProfileEnabled(0);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return playErr == 0 ? 0 : 1;
	}

	bytesLeft = 0;
	eofReached = 0;
	outOfData = 0;
	svxOpen = 0;
	verifyError = 0;
	readPtr = readBuf;
	gTiming = opt.bench ? &timing : NULL;
	MP3SetDecodeCoreProfileEnabled(opt.bench);
	if (opt.bench)
		MP3ResetDecodeCoreProfile();
	effectiveRate = 0;
	if (opt.bench)
		startClock = clock();

	while (!outOfData) {
		int nRead;
		int offset;
		int err;
		unsigned char *frameStart;
		int frameBytes;

		if (bytesLeft < 2 * MAINBUF_SIZE && !eofReached) {
			nRead = FillReadBuffer(readBuf, readPtr, READBUF_SIZE,
				bytesLeft, &input);
			bytesLeft += nRead;
			readPtr = readBuf;
			if (nRead == 0)
				eofReached = 1;
		}

		offset = FindValidatedMpegSync(readPtr, bytesLeft);
		if (offset < 0) {
			if (eofReached)
				break;
			if (bytesLeft > 3) {
				readPtr += bytesLeft - 3;
				bytesLeft = 3;
			}
			continue;
		}

		readPtr += offset;
		bytesLeft -= offset;
		frameStart = readPtr;
		frameBytes = bytesLeft;

		if (opt.bench) {
			clock_t t0 = clock();
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
			timing.frameDecode += clock() - t0;
		} else {
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
		}
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW &&
				stats.decodedFrames == 0 && frameBytes > 1) {
				readPtr = frameStart + 1;
				bytesLeft = frameBytes - 1;
			} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
				outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else if (stats.decodedFrames == 0 && frameBytes > 1) {
				/* Rescan after a bad first candidate before giving up. */
				readPtr = frameStart + 1;
				bytesLeft = frameBytes - 1;
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stats.decodedFrames);
				outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(decoder, &info);
		if (opt.debugFastLowrate) {
			MP3FastLowrateGranuleDebug fastDbg[MAX_NGRAN];
			int dbgCount = MP3GetFastLowrateDebug(decoder, fastDbg, MAX_NGRAN);
			int dbgIndex;
			for (dbgIndex = 0; dbgIndex < dbgCount && dbgIndex < MAX_NGRAN; dbgIndex++) {
				fprintf(stderr,
					"fast-lowrate frame=%lu granule=%d stride=%d "
					"phase=%d..%d full-rate-samps=%d lowrate-samps=%d "
					"cumulative-lowrate-samps=%d dest-offset=%d..%d\n",
					stats.decodedFrames, fastDbg[dbgIndex].granule,
					fastDbg[dbgIndex].stride, fastDbg[dbgIndex].phaseStart,
					fastDbg[dbgIndex].phaseEnd,
					fastDbg[dbgIndex].fullRateSamps,
					fastDbg[dbgIndex].lowrateSamps,
					fastDbg[dbgIndex].cumulativeLowrateSamps,
					fastDbg[dbgIndex].destOffsetStart,
					fastDbg[dbgIndex].destOffsetEnd);
			}
		}
		UpdateFirstFrameStats(&stats, &info);
		if (opt.checksum && !opt.fastLowrate)
			stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, decodeBuf,
				info.outputSamps);
		if (!effectiveRate) {
			effectiveRate = EffectiveOutputSampleRate(&opt, info.samprate);
			stats.outputSampleRate = effectiveRate;
		}
		if (!stats.outputChannels)
			stats.outputChannels = (opt.mono || info.nChans <= 1) ? 1 : info.nChans;

		if (!opt.decodeOnly && opt.outFormat == OUT_8SVX && !svxOpen) {
			if (!info.samprate) {
				fprintf(stderr, "cannot write 8SVX before sample rate is known\n");
				outOfData = 1;
				break;
			}
			if (opt.noOutput) {
				InitNoOutputSvx(&svx, opt.compression);
			} else {
				int beginErr;
				if (opt.bench) {
					clock_t t0 = clock();
					beginErr = SvxBegin(&svx, outfile, effectiveRate, opt.compression);
					timing.svxWrite += clock() - t0;
				} else {
					beginErr = SvxBegin(&svx, outfile, effectiveRate, opt.compression);
				}
				if (beginErr != 0) {
					fprintf(stderr, "cannot write 8SVX header\n");
					outOfData = 1;
					break;
				}
			}
			svxOpen = 1;
		}

		if (opt.decodeOnly) {
			const short *accountBuf;
			int accountSamps;
			int decoderOutputChannels;

			accountBuf = decodeBuf;
			accountSamps = info.outputSamps;
			decoderOutputChannels = MP3GetOutputChannels(decoder);
			if (opt.mono && info.nChans > 1 && decoderOutputChannels != 1) {
				accountSamps = MixFrame(decodeBuf, writeBuf, info.outputSamps,
					info.nChans, 1);
				accountBuf = writeBuf;
			}
			if (opt.checksum && opt.fastLowrate)
				stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, accountBuf,
					accountSamps);
			stats.outputSamples += (unsigned long)accountSamps;
		} else {
			int outSamps;
			int outChannels;
			int writeErr;
			clock_t t0;

			if (opt.bench)
				t0 = clock();
			outChannels = MP3GetOutputChannels(decoder);
			if (opt.mono && info.nChans > 1 && outChannels == 1) {
				if (writeBuf != decodeBuf)
					memmove(writeBuf, decodeBuf, info.outputSamps * sizeof(short));
				outSamps = info.outputSamps;
			} else {
				outSamps = MixFrame(decodeBuf, writeBuf, info.outputSamps,
					info.nChans, opt.mono);
				outChannels = (opt.mono || info.nChans <= 1) ? 1 : info.nChans;
			}
			stats.outputChannels = outChannels;
			if (!opt.fastLowrate && opt.outputRate && info.samprate > opt.outputRate) {
				outSamps = DownsampleFrame(&rateState, writeBuf, rateBuf, outSamps,
					info.samprate, opt.outputRate, outChannels);
				memmove(writeBuf, rateBuf, outSamps * sizeof(short));
			}
			if (opt.checksum && opt.fastLowrate)
				stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, writeBuf,
					outSamps);
			if (opt.bench)
				timing.pcmConvert += clock() - t0;

			if (opt.outFormat == OUT_8SVX) {
				if (opt.bench) {
					t0 = clock();
					writeErr = SvxWriteSamples(&svx, writeBuf, outSamps);
					timing.svxWrite += clock() - t0;
				} else {
					writeErr = SvxWriteSamples(&svx, writeBuf, outSamps);
				}
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
		clock_t t0;
		if (opt.bench)
			t0 = clock();
		if (SvxEnd(&svx) != 0) {
			fprintf(stderr, "error finalizing 8SVX file\n");
			verifyError = 1;
		}
		if (svx.sourceSamples != stats.outputSamples) {
			fprintf(stderr,
				"8SVX VHDR sample count mismatch: vhdr=%lu output=%lu\n",
				svx.sourceSamples, stats.outputSamples);
			verifyError = 1;
		}
		if (svx.compression == SVX_COMP_NONE && svx.bodyBytes != svx.sourceSamples) {
			fprintf(stderr,
				"8SVX BODY/sample count mismatch: body=%lu samples=%lu\n",
				svx.bodyBytes, svx.sourceSamples);
			verifyError = 1;
		}
		if (opt.bench)
			timing.svxWrite += clock() - t0;
	}

	if (opt.bench)
		endClock = clock();

	if (!stats.outputSampleRate)
		stats.outputSampleRate = effectiveRate ? effectiveRate : stats.sampleRate;
	printf("input sample rate: %d Hz\n", stats.sampleRate);
	PrintFastLowrateOutputRateDifference(&opt, stats.outputSampleRate);
	if (stats.outputSampleRate && stats.outputSampleRate != stats.sampleRate)
		printf("output sample rate: %d Hz\n", stats.outputSampleRate);
	printf("channels: %d%s\n", stats.channels, opt.mono ? " (mono output)" : "");
	printf("bitrate: %d bps\n", stats.bitrate);
	printf("decoded frames: %lu\n", stats.decodedFrames);
	printf("output samples: %lu\n", stats.outputSamples);
	PrintOutputStats(&opt, &stats);
	if (opt.checksum)
		printf("%s PCM checksum: %08lx\n",
			opt.fastLowrate ? "fast-lowrate output" : "decoded",
			stats.pcmChecksum);
	if (opt.fastLowrate)
		printf("fast-lowrate stride: %d (experimental; IMDCT/DCT32 still full-rate)\n",
			MP3GetFastLowrateStride(decoder));

	if (opt.bench) {
		double elapsed = 0.0;
		double audioSeconds = 0.0;
		if (CLOCKS_PER_SEC > 0)
			elapsed = (double)(endClock - startClock) / (double)CLOCKS_PER_SEC;
		audioSeconds = DecodedAudioSeconds(&opt, &stats);
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
				{
					unsigned long s2Asm = 0, s2C = 0;
					MP3GetStereoStride2PolyphaseCounters(&s2Asm, &s2C);
					printf("stereo stride-2 polyphase: %s\n",
						s2Asm ? "asm" : "C");
					printf("stereo stride-2 polyphase calls: asm=%lu C=%lu\n",
						s2Asm, s2C);
				}
				printf("core IMDCT subbands: executed=%lu skipped=%lu\n",
					coreProfile.imdctSubbandsExecuted, coreProfile.imdctSubbandsSkipped);
				printf("mono M/S side-channel skip: eligible=%lu huffman=%lu dequant=%lu imdct=%lu synthesis=%lu\n",
					coreProfile.monoMSSideSkipEligible,
					coreProfile.monoMSSideHuffmanSkipped,
					coreProfile.monoMSSideDequantSkipped,
					coreProfile.monoMSSideIMDCTSkipped,
					coreProfile.monoMSSideSynthesisSkipped);
				printf("mono M/S fallback: not-stereo-source=%lu output-stereo=%lu not-joint=%lu no-ms=%lu intensity=%lu disabled=%lu malformed=%lu\n",
					coreProfile.monoMSSideFallbackNotStereoSource,
					coreProfile.monoMSSideFallbackOutputStereo,
					coreProfile.monoMSSideFallbackNotJointStereo,
					coreProfile.monoMSSideFallbackNoMS,
					coreProfile.monoMSSideFallbackIntensity,
					coreProfile.monoMSSideFallbackDisabled,
					coreProfile.monoMSSideFallbackMalformed);
				if (opt.fastLowrate)
					printf("sparse low-rate: stride=%d active-subbands=%d fdct=%s\n",
						FastLowrateStrideForOutputRate(opt.outputRate),
						FastLowrateStrideForOutputRate(opt.outputRate) == 2 ? 16 :
						(FastLowrateStrideForOutputRate(opt.outputRate) == 4 ? 8 : 32),
						FastLowrateStrideForOutputRate(opt.outputRate) == 4 && opt.superfastLowrate ?
						"FDCT32Quarter" : (FastLowrateStrideForOutputRate(opt.outputRate) == 2 ?
						"FDCT32Half" : "FDCT32"));
			}
		}
		printf("timing frame decode: %.3f s\n", ClocksToSeconds(timing.frameDecode));
		printf("timing PCM conversion: %.3f s\n", ClocksToSeconds(timing.pcmConvert));
		printf("timing 8SVX write: %.3f s\n", ClocksToSeconds(timing.svxWrite));
		printf("timing Fibonacci compression: %.3f s\n", ClocksToSeconds(timing.fibCompress));
		printf("timing file writing: %.3f s\n", ClocksToSeconds(timing.fileWrite));
	}

	MP3FreeDecoder(decoder);
	InputSourceClose(&input);
	CloseInputFile(&infile, opt.debugCleanup);
	if (outfile)
		fclose(outfile);
	gTiming = NULL;
	MP3SetDecodeCoreProfileEnabled(0);
	free(resolvedOutName);
	AmigaFreeNormalizedArgs(&normalized);

	return verifyError ? 1 : 0;
}
