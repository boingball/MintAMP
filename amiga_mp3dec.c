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
#include <proto/exec.h>
#ifndef AUDIONAME
#define AUDIONAME "audio.device"
#endif
#endif

#include "mp3dec.h"
#include "assembly.h"
#include "statname.h"

void STATNAME(FDCT32)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32_C_REFERENCE)(int *x, int *d, int offset, int oddBlock, int gb);
int STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(IMDCT36_C_REFERENCE)(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int STATNAME(IMDCT36_TEST_ACTIVE)(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
#define AMIGA_FDCT32 STATNAME(FDCT32)
#define AMIGA_FDCT32_C_REFERENCE STATNAME(FDCT32_C_REFERENCE)
#define AMIGA_FDCT32_HAS_ASM STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_IMDCT36_C_REFERENCE STATNAME(IMDCT36_C_REFERENCE)
#define AMIGA_IMDCT36_TEST_ACTIVE STATNAME(IMDCT36_TEST_ACTIVE)
#define AMIGA_IMDCT36_HAS_ASM STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)

#define READBUF_SIZE (1024 * 16)
#define OUTBUF_SAMPS (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)
#define AMIGA_IMDCT_BLOCK_SIZE 18
#define AMIGA_IMDCT_NBANDS 32

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
	int selftestFdct32;
	int selftestImdct;
	int selftestFastLowrate;
	int selftestMonoFastLowrateStereo;
	int checksum;
	int outputRate;
	int fastLowrate;
	int help;
	int debugArgv;
	int debugFastLowrate;
	int debugPlay;
	int play;
	int stereo;
	int decodeThenPlay;
	int playLifecycleTest;
	int bufferSeconds;
	int fastMem;
	int info;
} DecodeOptions;

typedef struct InputSource {
	FILE *file;
	unsigned char *memory;
	unsigned long memorySize;
	unsigned long memoryPos;
} InputSource;

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
	unsigned long underrunBuffers[2];
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
	printf("               stereo rates: 8820, 11025, or experimental high-CPU 22050 Hz\n");
	printf("               mono rates: 8287 default, 8820, 11025, or experimental 22050 Hz\n");
	printf("  --play-fast-path accepted alias; --play already uses reduced-overhead playback\n");
	printf("  --decode-then-play decode whole MP3 to RAM, then play (debug for --play)\n");
	printf("  --play-lifecycle-test open/submit/cleanup audio.device three times\n");
	printf("  --buffer-seconds N playback seconds per half-buffer (default 4, clamped 1-10)\n");
	printf("  --fast-mem   preload the compressed MP3 into Fast RAM before decoding/playback\n");
	printf("  --decode-only decode frames only; skip PCM conversion and output\n");
	printf("  --no-output  run conversion/compression paths but discard output bytes\n");
	printf("  --rate HZ    output/downsample rate: 22050, 11025, 8820, or 8287 Hz\n");
	printf("               22050 playback is experimental/high CPU and may underrun\n");
	printf("  --fast-lowrate experimental lower-quality Amiga conversion; requires --rate\n");
	printf("                 22050, 11025, 8820, or 8287 and can skip discarded synthesis samples\n");
	printf("  --selftest-mulshift compare C and optional asm MULSHIFT32 helpers\n");
	printf("  --selftest-fdct32 compare C reference and optional m68k asm FDCT32 path\n");
	printf("  --selftest-imdct compare C reference and optional m68k asm long IMDCT path\n");
	printf("  --selftest-fastlowrate compare synthetic stride decimation paths\n");
	printf("  --selftest-mono-fastlowrate-stereo verify stereo-to-mono low-rate accounting\n");
	printf("  --checksum  print a 32-bit checksum of decoded PCM samples\n");
	printf("  --debug-fastlowrate print per-frame/granule fast-lowrate placement\n");
	printf("  --debug-play print audio.device playback startup diagnostics\n");
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

static int ParseOptions(int argc, char **argv, DecodeOptions *opt)
{
	int i;

	memset(opt, 0, sizeof(*opt));
	opt->outFormat = OUT_PCM16;
	opt->compression = SVX_COMP_NONE;
	opt->outputRate = 0;
	opt->bufferSeconds = 4;

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
		} else if (!strcmp(argv[i], "--play-lifecycle-test")) {
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
		} else if (!strcmp(argv[i], "--fast-mem")) {
			opt->fastMem = 1;
		} else if (!strcmp(argv[i], "--decode-only")) {
			opt->decodeOnly = 1;
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--no-output")) {
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--selftest-mulshift")) {
			opt->selftestMulshift = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32")) {
			opt->selftestFdct32 = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct")) {
			opt->selftestImdct = 1;
		} else if (!strcmp(argv[i], "--selftest-fastlowrate")) {
			opt->selftestFastLowrate = 1;
		} else if (!strcmp(argv[i], "--selftest-mono-fastlowrate-stereo")) {
			opt->selftestMonoFastLowrateStereo = 1;
		} else if (!strcmp(argv[i], "--checksum")) {
			opt->checksum = 1;
		} else if (!strcmp(argv[i], "--fast-lowrate")) {
			opt->fastLowrate = 1;
		} else if (!strcmp(argv[i], "--rate")) {
			if (++i >= argc)
				return -1;
			opt->outputRate = atoi(argv[i]);
			if (opt->outputRate != 22050 && opt->outputRate != 11025 &&
				opt->outputRate != 8820 && opt->outputRate != 8287)
				return -1;
		} else if (!strcmp(argv[i], "--debug-fastlowrate")) {
			opt->debugFastLowrate = 1;
		} else if (!strcmp(argv[i], "--debug-play")) {
			opt->debugPlay = 1;
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

	if (opt->selftestMulshift || opt->selftestFdct32 || opt->selftestImdct ||
		opt->selftestFastLowrate || opt->selftestMonoFastLowrateStereo)
		return 0;

	if (opt->stereo && !opt->play) {
		fprintf(stderr, "--stereo is only supported with --play\n");
		return -1;
	}

	if (opt->play && !opt->outputRate)
		opt->outputRate = opt->stereo ? 8820 : 8287;

	if (opt->play && opt->outputRate != 8287 && opt->outputRate != 8820 &&
		opt->outputRate != 11025 && opt->outputRate != 22050) {
		fprintf(stderr, "--play supports --rate 8287, 8820, 11025, or 22050 only\n");
		return -1;
	}
	if (opt->stereo && opt->outputRate == 8287) {
		fprintf(stderr, "--stereo supports --rate 8820, 11025, or experimental 22050 only\n");
		return -1;
	}
	if (opt->play) {
		opt->mono = opt->stereo ? 0 : 1;
		opt->outFormat = OUT_S8;
		opt->fastLowrate = 1;
		opt->noOutput = 1;
	}

	if (opt->fastLowrate && (opt->outputRate != 22050 &&
		opt->outputRate != 11025 && opt->outputRate != 8820 &&
		opt->outputRate != 8287)) {
		fprintf(stderr, "--fast-lowrate requires --rate 22050, 11025, 8820, or 8287\n");
		return -1;
	}

	if (opt->playLifecycleTest)
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

static int PrintFirstFrameInfo(FILE *fp, unsigned long audioOffset)
{
	unsigned char probe[READBUF_SIZE];
	HMP3Decoder decoder;
	MP3FrameInfo info;
	size_t nRead;
	int offset;
	int err;
	const char *version;

	if (fseek(fp, (long)audioOffset, SEEK_SET) != 0)
		return 0;
	nRead = fread(probe, 1, sizeof(probe), fp);
	offset = MP3FindSyncWord(probe, (int)nRead);
	if (offset < 0)
		return 0;
	decoder = MP3InitDecoder();
	if (!decoder)
		return 0;
	err = MP3GetNextFrameInfo(decoder, &info, probe + offset);
	MP3FreeDecoder(decoder);
	if (err != ERR_MP3_NONE)
		return 0;
	version = info.version == MPEG1 ? "1" : (info.version == MPEG2 ? "2" : "2.5");
	printf("MPEG audio: version %s, layer %d\n", version, info.layer);
	printf("sample rate: %d Hz\n", info.samprate);
	printf("channels: %d\n", info.nChans);
	printf("bitrate: %d bps\n", info.bitrate);
	return 1;
}

static void PrintMp3Info(FILE *fp, const char *name)
{
	long fileSize;
	unsigned long audioOffset;

	fileSize = -1;
	if (fseek(fp, 0, SEEK_END) == 0)
		fileSize = ftell(fp);
	printf("file: %s\n", name);
	if (fileSize >= 0)
		printf("file size: %lu bytes\n", (unsigned long)fileSize);
	audioOffset = PrintId3v2(fp);
	if (!PrintFirstFrameInfo(fp, audioOffset))
		printf("MPEG audio: no frame found in first %d bytes after tags\n", READBUF_SIZE);
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

static void InputSourceClose(InputSource *input)
{
	FreeFastInputMemory(input->memory, input->memorySize);
	input->memory = NULL;
	input->memorySize = 0;
	input->memoryPos = 0;
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
	return fread(dest, 1, bytes, input->file);
}

static unsigned long InputSourceTell(const InputSource *input)
{
	if (input->memory)
		return input->memoryPos;
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
		fseek(input->file, (long)pos, SEEK_SET);
	}
}

static int InputSourcePreloadFastMemory(InputSource *input)
{
	long fileSize;
	unsigned char *memory;
	size_t nRead;

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
	printf("fast-mem input preload: %lu bytes\n", input->memorySize);
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



static int TestFdct32Case(unsigned long index, unsigned long seed, int offset,
	int oddBlock, int gb)
{
	static int cbuf[32];
	static int abuf[32];
	static int cdest[4096];
	static int adest[4096];
	int i;

	for (i = 0; i < 32; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		cbuf[i] = ((int)seed) >> 8;
		abuf[i] = cbuf[i];
	}
	for (i = 0; i < 4096; i++) {
		cdest[i] = (int)(0x55aa0000UL ^ (unsigned long)i);
		adest[i] = cdest[i];
	}

	AMIGA_FDCT32_C_REFERENCE(cbuf, cdest, offset, oddBlock, gb);
	AMIGA_FDCT32(abuf, adest, offset, oddBlock, gb);

	for (i = 0; i < 32; i++) {
		if (abuf[i] != cbuf[i]) {
			printf("FDCT32 buffer mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)cbuf[i], (long)abuf[i], offset, oddBlock, gb);
			return -1;
		}
	}
	for (i = 0; i < 4096; i++) {
		if (adest[i] != cdest[i]) {
			printf("FDCT32 dest mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)cdest[i], (long)adest[i], offset, oddBlock, gb);
			return -1;
		}
	}
	return 0;
}

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
		printf("IMDCT36 mOut mismatch %lu: C=%ld active=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
			index, (long)cm, (long)am, btCurr, btPrev, blockIdx, gb);
		return -1;
	}
	for (i = 0; i < 18; i++) {
		if (ax[i] != cx[i]) {
			printf("IMDCT36 input mismatch %lu[%d]: C=%ld active=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cx[i], (long)ax[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	for (i = 0; i < 9; i++) {
		if (ap[i] != cp[i]) {
			printf("IMDCT36 overlap mismatch %lu[%d]: C=%ld active=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cp[i], (long)ap[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS; i++) {
		if (ay[i] != cy[i]) {
			printf("IMDCT36 output mismatch %lu[%d]: C=%ld active=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cy[i], (long)ay[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	return 0;
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
	signed char spillBuf[OUTBUF_SAMPS];
	int spillPos;
	int spillCount;
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
	memcpy(dest + *outBytes, stream->spillBuf + stream->spillPos, n);
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

		if (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
			nRead = FillReadBuffer(stream->readBuf, stream->readPtr,
				READBUF_SIZE, stream->bytesLeft, stream->input);
			stream->bytesLeft += nRead;
			stream->readPtr = stream->readBuf;
			if (nRead == 0)
				stream->eofReached = 1;
		}

		offset = MP3FindSyncWord(stream->readPtr, stream->bytesLeft);
		if (offset < 0)
			break;
		stream->readPtr += offset;
		stream->bytesLeft -= offset;

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
			if (err == ERR_MP3_INDATA_UNDERFLOW) {
				stream->outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stream->stats->decodedFrames);
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

			for (i = 0; i < outSamps; i++)
				stream->spillBuf[i] = Sample16ToS8(stream->writeBuf[i]);
			stream->spillPos = 0;
			stream->spillCount = outSamps;
			stream->stats->outputSamples += (unsigned long)outSamps;
			stream->stats->decodedFrames++;
			DecodeStreamCopySpill(stream, dest, maxBytes, &produced);
		}
	}

	return produced;
}

#ifdef AMIGA_M68K
/* Ctrl-C signal handling is unavailable in the libnix build for now. */
static volatile int gPlaybackInterrupted;
#else
static volatile sig_atomic_t gPlaybackInterrupted;

static void PlaybackSignalHandler(int signum)
{
	(void)signum;
	gPlaybackInterrupted = 1;
}
#endif

typedef struct PlaybackCleanupStatus {
	unsigned long ioCompleted;
	unsigned long ioAborted;
	unsigned long buffersFreed;
	int deviceClosed;
} PlaybackCleanupStatus;

static void PlaybackCleanupStatusInit(PlaybackCleanupStatus *status)
{
	if (status)
		memset(status, 0, sizeof(*status));
}

static void PrintPlaybackCleanupStatus(const DecodeOptions *opt,
	const PlaybackCleanupStatus *status)
{
	if (!opt->debugPlay || !status)
		return;
	printf("debug-play: cleanup IO completed/aborted: %lu/%lu\n",
		status->ioCompleted, status->ioAborted);
	printf("debug-play: cleanup buffers freed: %lu\n", status->buffersFreed);
	printf("debug-play: cleanup device closed: %s\n",
		status->deviceClosed ? "yes" : "no");
}

static unsigned int AmigaPalAudioPeriod(int outputRate)
{
	if (outputRate <= 0)
		return 0;
	return (unsigned int)((3546895UL + ((unsigned long)outputRate / 2UL)) /
		(unsigned long)outputRate);
}

#ifdef HAVE_AMIGA_AUDIO_DEVICE
typedef struct AmigaAudioPlayer {
	struct MsgPort *port;
	struct IOAudio *req[2][2];
	int deviceOpen[2];
	int sent[2][2];
	int stereo;
	unsigned int period;
	signed char *splitBuf[2][2];
	unsigned long splitBytes;
} AmigaAudioPlayer;

static void AmigaAudioClose(AmigaAudioPlayer *player,
	PlaybackCleanupStatus *status)
{
	int i;
	int ch;

	if (!player)
		return;
	for (i = 0; i < 2; i++) {
		for (ch = 0; ch < 2; ch++) {
			if (player->req[i][ch] && player->sent[i][ch]) {
				if (!CheckIO((struct IORequest *)player->req[i][ch])) {
					AbortIO((struct IORequest *)player->req[i][ch]);
					WaitIO((struct IORequest *)player->req[i][ch]);
					if (status)
						status->ioAborted++;
				} else {
					WaitIO((struct IORequest *)player->req[i][ch]);
					if (status)
						status->ioCompleted++;
				}
				player->sent[i][ch] = 0;
			}
		}
	}
	for (ch = 0; ch < 2; ch++) {
		if (player->deviceOpen[ch] && player->req[0][ch]) {
			CloseDevice((struct IORequest *)player->req[0][ch]);
			player->deviceOpen[ch] = 0;
			if (status)
				status->deviceClosed = 1;
		}
	}
	if (player->port) {
		while (GetMsg(player->port))
			;
	}
	for (i = 0; i < 2; i++) {
		for (ch = 0; ch < 2; ch++) {
			if (player->req[i][ch]) {
				DeleteIORequest((struct IORequest *)player->req[i][ch]);
				player->req[i][ch] = NULL;
			}
			if (player->splitBuf[i][ch]) {
				FreeMem(player->splitBuf[i][ch], player->splitBytes);
				player->splitBuf[i][ch] = NULL;
				if (status)
					status->buffersFreed++;
			}
		}
	}
	if (player->port) {
		DeleteMsgPort(player->port);
		player->port = NULL;
	}
	player->stereo = 0;
	player->period = 0;
	player->splitBytes = 0;
}

static int AmigaAudioOpenOne(AmigaAudioPlayer *player, int ch,
	const UBYTE *channels, unsigned long channelCount)
{
	player->req[0][ch] = (struct IOAudio *)CreateIORequest(player->port,
		sizeof(struct IOAudio));
	player->req[1][ch] = (struct IOAudio *)CreateIORequest(player->port,
		sizeof(struct IOAudio));
	if (!player->req[0][ch] || !player->req[1][ch])
		return -1;
	player->req[0][ch]->ioa_Request.io_Message.mn_Node.ln_Pri = ADALLOC_MINPREC;
	player->req[0][ch]->ioa_Data = (UBYTE *)channels;
	player->req[0][ch]->ioa_Length = channelCount;
	if (OpenDevice(AUDIONAME, 0, (struct IORequest *)player->req[0][ch], 0) != 0)
		return -1;
	player->deviceOpen[ch] = 1;
	memcpy(player->req[1][ch], player->req[0][ch], sizeof(struct IOAudio));
	player->req[1][ch]->ioa_Request.io_Message.mn_ReplyPort = player->port;
	return 0;
}

static int AmigaAudioOpen(AmigaAudioPlayer *player, unsigned int period,
	int stereo, unsigned long maxBytes)
{
	UBYTE monoChannels[] = { 1, 2, 4, 8 };
	UBYTE leftChannels[] = { 1, 8 };
	UBYTE rightChannels[] = { 2, 4 };
	int i;
	int ch;

	memset(player, 0, sizeof(*player));
	player->period = period;
	player->stereo = stereo;
	player->port = CreateMsgPort();
	if (!player->port)
		return -1;
	if (stereo) {
		player->splitBytes = maxBytes / 2UL;
		if (player->splitBytes == 0)
			player->splitBytes = 1;
		for (i = 0; i < 2; i++) {
			for (ch = 0; ch < 2; ch++) {
				player->splitBuf[i][ch] = (signed char *)AllocMem(player->splitBytes,
					MEMF_CHIP | MEMF_CLEAR);
				if (!player->splitBuf[i][ch]) {
					AmigaAudioClose(player, NULL);
					return -1;
				}
			}
		}
		if (AmigaAudioOpenOne(player, 0, leftChannels, sizeof(leftChannels)) != 0 ||
			AmigaAudioOpenOne(player, 1, rightChannels, sizeof(rightChannels)) != 0) {
			AmigaAudioClose(player, NULL);
			return -1;
		}
	} else {
		if (AmigaAudioOpenOne(player, 0, monoChannels, sizeof(monoChannels)) != 0) {
			AmigaAudioClose(player, NULL);
			return -1;
		}
	}
	return 0;
}

static void AmigaAudioSubmitOne(AmigaAudioPlayer *player, int index,
	int ch, signed char *buf, unsigned long len)
{
	struct IOAudio *req = player->req[index][ch];

	req->ioa_Request.io_Command = CMD_WRITE;
	req->ioa_Request.io_Flags = ADIOF_PERVOL;
	req->ioa_Data = (UBYTE *)buf;
	req->ioa_Length = len;
	req->ioa_Period = player->period;
	req->ioa_Volume = 64;
	req->ioa_Cycles = 1;
	BeginIO((struct IORequest *)req);
	player->sent[index][ch] = 1;
}

static int AmigaAudioSubmit(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	if (!buf || len == 0 || (player->stereo && (len & 1UL)))
		return -1;
	if (player->stereo) {
		unsigned long frames = len / 2UL;
		unsigned long i;
		for (i = 0; i < frames; i++) {
			player->splitBuf[index][0][i] = buf[2UL * i];
			player->splitBuf[index][1][i] = buf[2UL * i + 1UL];
		}
		AmigaAudioSubmitOne(player, index, 1, player->splitBuf[index][1], frames);
		AmigaAudioSubmitOne(player, index, 0, player->splitBuf[index][0], frames);
	} else {
		AmigaAudioSubmitOne(player, index, 0, buf, len);
	}
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

static int AmigaAudioWait(AmigaAudioPlayer *player, int index)
{
	int err;

	err = 0;
	if (player->sent[index][0]) {
		err = WaitIO((struct IORequest *)player->req[index][0]);
		if (!err)
			err = player->req[index][0]->ioa_Request.io_Error;
		player->sent[index][0] = 0;
	}
	if (player->stereo && player->sent[index][1]) {
		int err2;

		err2 = WaitIO((struct IORequest *)player->req[index][1]);
		if (!err2)
			err2 = player->req[index][1]->ioa_Request.io_Error;
		if (!err)
			err = err2;
		player->sent[index][1] = 0;
	}
	return err;
}

static signed char *AmigaAllocAudioBuffer(unsigned long bytes)
{
	return (signed char *)AllocMem(bytes, MEMF_CHIP | MEMF_CLEAR);
}

static void AmigaFreeAudioBuffer(signed char *buf, unsigned long bytes)
{
	if (buf)
		FreeMem(buf, bytes);
}

static signed char *AmigaAllocPlaybackWorkBuffer(int stereo, unsigned long bytes)
{
	if (stereo)
		return (signed char *)malloc(bytes);
	return AmigaAllocAudioBuffer(bytes);
}

static void AmigaFreePlaybackWorkBuffer(int stereo, signed char *buf,
	unsigned long bytes)
{
	if (stereo)
		free(buf);
	else
		AmigaFreeAudioBuffer(buf, bytes);
}
#else
typedef struct AmigaAudioPlayer {
	int stereo;
	int sent[2][2];
	signed char *splitBuf[2][2];
	unsigned long splitBytes;
} AmigaAudioPlayer;
static void AmigaAudioClose(AmigaAudioPlayer *player,
	PlaybackCleanupStatus *status) { (void)player; (void)status; }
static int AmigaAudioOpen(AmigaAudioPlayer *player, unsigned int period,
	int stereo, unsigned long maxBytes)
{
	(void)player;
	(void)period;
	(void)stereo;
	(void)maxBytes;
	fprintf(stderr, "--play requires an AmigaOS audio.device build\n");
	return -1;
}
static int AmigaAudioSubmit(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	(void)buf;
	if (len == 0)
		return -1;
	player->sent[index][0] = 1;
	return 0;
}
static int AmigaAudioDone(AmigaAudioPlayer *player, int index)
{ (void)player; (void)index; return 1; }
static int AmigaAudioWait(AmigaAudioPlayer *player, int index)
{ player->sent[index][0] = 0; return 0; }
static signed char *AmigaAllocPlaybackWorkBuffer(int stereo, unsigned long bytes)
{ (void)stereo; return (signed char *)malloc(bytes); }
static void AmigaFreePlaybackWorkBuffer(int stereo, signed char *buf,
	unsigned long bytes)
{ (void)stereo; (void)bytes; free(buf); }
#endif

static unsigned long AlignPlaybackChunkBytes(unsigned long bytes, int stereo)
{
	if (stereo && (bytes & 1UL))
		bytes--;
	if (bytes == 0)
		bytes = stereo ? 2UL : 1UL;
	return bytes;
}

static unsigned long PlaybackRequestedChunkBytes(const DecodeOptions *opt,
	int playbackRate)
{
	unsigned long bytes;

	if (playbackRate <= 0)
		playbackRate = opt->outputRate > 0 ? opt->outputRate : 8287;
	bytes = (unsigned long)playbackRate *
		(unsigned long)opt->bufferSeconds * (opt->stereo ? 2UL : 1UL);
	return AlignPlaybackChunkBytes(bytes, opt->stereo);
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

static unsigned long PlaybackElapsedMilliseconds(clock_t start, clock_t end)
{
	if (CLOCKS_PER_SEC <= 0 || end <= start)
		return 0;
	return (unsigned long)(((double)(end - start) * 1000.0) /
		(double)CLOCKS_PER_SEC);
}

static const char *PlaybackBufferName(int index)
{
	return index == 0 ? "A" : "B";
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

static unsigned long PlaybackStartupPrefillSamples(const DecodeOptions *opt,
	unsigned long chunkBytes)
{
	unsigned long samples;

	samples = (unsigned long)PlaybackHalfBufferSamples(opt, chunkBytes);
	if (samples == 0)
		return 0;
	return samples;
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
		attempts < 8 && !stream->outOfData) {
		int n;

		n = DecodeStreamFillS8(stream, opt, dest + produced,
			(int)(maxBytes - produced));
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
	offset = MP3FindSyncWord(probe, (int)nRead);
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
	signed char *buf[2])
{
	if (!opt->debugPlay)
		return;
	printf("debug-play: actual output rate: %d Hz\n", playbackRate);
	printf("debug-play: PAL period: %u\n", period);
	printf("debug-play: requested buffer seconds: %d\n", opt->bufferSeconds);
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
		printf("debug-play: work buffer A/B: %p/%p size %lu\n",
			(void *)buf[0], (void *)buf[1], chunkBytes);
	} else {
		printf("debug-play: chip buffer A: %p size %lu\n",
			(void *)buf[0], chunkBytes);
		printf("debug-play: chip buffer B: %p size %lu\n",
			(void *)buf[1], chunkBytes);
		printf("debug-play: work buffer: none (mono decodes directly to chip buffers)\n");
	}
}

static int AmigaSetupPlaybackBuffers(AmigaAudioPlayer *player,
	const DecodeOptions *opt, unsigned int period, unsigned long requestedBytes,
	unsigned long minBytes, signed char *buf[2], unsigned long *chunkBytes,
	PlaybackCleanupStatus *status)
{
	unsigned long tryBytes;

	buf[0] = NULL;
	buf[1] = NULL;
	tryBytes = AlignPlaybackChunkBytes(requestedBytes, opt->stereo);
	minBytes = AlignPlaybackChunkBytes(minBytes, opt->stereo);
	if (minBytes == 0)
		minBytes = opt->stereo ? 2UL : 1UL;
	if (tryBytes < minBytes)
		tryBytes = minBytes;

	while (tryBytes >= minBytes) {
		if (AmigaAudioOpen(player, period, opt->stereo, tryBytes) == 0) {
			buf[0] = AmigaAllocPlaybackWorkBuffer(opt->stereo, tryBytes);
			buf[1] = AmigaAllocPlaybackWorkBuffer(opt->stereo, tryBytes);
			if (buf[0] && buf[1]) {
				*chunkBytes = tryBytes;
				if (tryBytes != requestedBytes)
					printf("play buffer reduced to %lu bytes per half-buffer\n",
						tryBytes);
				return 0;
			}
			if (buf[0]) {
				AmigaFreePlaybackWorkBuffer(opt->stereo, buf[0], tryBytes);
				buf[0] = NULL;
				if (status)
					status->buffersFreed++;
			}
			if (buf[1]) {
				AmigaFreePlaybackWorkBuffer(opt->stereo, buf[1], tryBytes);
				buf[1] = NULL;
				if (status)
					status->buffersFreed++;
			}
			AmigaAudioClose(player, status);
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

static void AmigaFreePlaybackBuffers(const DecodeOptions *opt, signed char *buf[2],
	unsigned long chunkBytes, PlaybackCleanupStatus *status)
{
	if (buf[0]) {
		AmigaFreePlaybackWorkBuffer(opt->stereo, buf[0], chunkBytes);
		buf[0] = NULL;
		if (status)
			status->buffersFreed++;
	}
	if (buf[1]) {
		AmigaFreePlaybackWorkBuffer(opt->stereo, buf[1], chunkBytes);
		buf[1] = NULL;
		if (status)
			status->buffersFreed++;
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
	signed char *buf[2];
	unsigned long len[2];
	int cur;
	int pending;
	int first;

	PlaybackCleanupStatusInit(&cleanupStatus);
	{
		int playbackRate = PlaybackOutputSampleRate(opt, stats);
		period = AmigaPalAudioPeriod(playbackRate);
		PrintFastLowrateOutputRateDifference(opt, playbackRate);
		printf("play output rate: %d Hz\n", playbackRate);
	}
	printf("PAL audio period: %u\n", period);
	chunkBytes = PlaybackRequestedChunkBytes(opt, PlaybackOutputSampleRate(opt, stats));
	if (AmigaSetupPlaybackBuffers(&player, opt, period, chunkBytes, 1UL,
		buf, &chunkBytes, &cleanupStatus) != 0) {
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
		return -1;
	}
	printf("playback half-buffer: %lu ms, %lu bytes\n",
		PlaybackBufferDurationMilliseconds(opt, chunkBytes,
			PlaybackOutputSampleRate(opt, stats)), chunkBytes);
	pos = 0;
	for (cur = 0; cur < 2; cur++) {
		len[cur] = totalBytes - pos;
		if (len[cur] > chunkBytes)
			len[cur] = chunkBytes;
		memcpy(buf[cur], pcm + pos, len[cur]);
		pos += len[cur];
	}
	cur = 0;
	pending = 0;
	first = 1;
	while (!gPlaybackInterrupted && len[cur] > 0) {
		if (AmigaAudioSubmit(&player, cur, buf[cur], len[cur]) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(cur));
			AmigaAudioClose(&player, &cleanupStatus);
			AmigaFreePlaybackBuffers(opt, buf, chunkBytes, &cleanupStatus);
			PrintPlaybackCleanupStatus(opt, &cleanupStatus);
			return -1;
		}
		pending = 1;
		if (!first) {
			if (AmigaAudioWait(&player, 1 - cur) != 0) {
				fprintf(stderr, "audio.device write failed\n");
				AmigaAudioClose(&player, &cleanupStatus);
				AmigaFreePlaybackBuffers(opt, buf, chunkBytes, &cleanupStatus);
				PrintPlaybackCleanupStatus(opt, &cleanupStatus);
				return -1;
			}
			len[1 - cur] = totalBytes - pos;
			if (len[1 - cur] > chunkBytes)
				len[1 - cur] = chunkBytes;
			if (len[1 - cur] > 0) {
				memcpy(buf[1 - cur], pcm + pos, len[1 - cur]);
				pos += len[1 - cur];
			}
		} else {
			first = 0;
		}
		cur = 1 - cur;
	}
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		AmigaAudioClose(&player, &cleanupStatus);
		AmigaFreePlaybackBuffers(opt, buf, chunkBytes, &cleanupStatus);
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
		return -1;
	}
	if (pending) {
		if (AmigaAudioWait(&player, 1 - cur) != 0) {
			fprintf(stderr, "audio.device write failed\n");
			AmigaAudioClose(&player, &cleanupStatus);
			AmigaFreePlaybackBuffers(opt, buf, chunkBytes, &cleanupStatus);
			PrintPlaybackCleanupStatus(opt, &cleanupStatus);
			return -1;
		}
	}
	AmigaAudioClose(&player, &cleanupStatus);
	AmigaFreePlaybackBuffers(opt, buf, chunkBytes, &cleanupStatus);
	PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	(void)stats;
	return 0;
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

	DecodeStreamInit(&stream, input, decoder, stats, timing);
	all = NULL;
	used = 0;
	cap = 0;
	while (!gPlaybackInterrupted &&
		(n = DecodeStreamFillS8(&stream, opt, temp, sizeof(temp))) > 0) {
		if (used + (unsigned long)n > cap) {
			unsigned long newCap = cap ? cap * 2UL : 65536UL;
			signed char *newAll;
			while (newCap < used + (unsigned long)n)
				newCap *= 2UL;
			newAll = (signed char *)realloc(all, newCap);
			if (!newAll) {
				free(all);
				all = NULL;
				fprintf(stderr, "cannot allocate decode-then-play RAM\n");
				return -1;
			}
			all = newAll;
			cap = newCap;
		}
		memcpy(all + used, temp, n);
		used += (unsigned long)n;
	}
	if (stream.decodeError) {
		free(all);
		all = NULL;
		return -1;
	}
	if (gPlaybackInterrupted) {
		free(all);
		all = NULL;
		fprintf(stderr, "playback interrupted\n");
		return -1;
	}
	printf("decode-then-play bytes: %lu\n", used);
	n = AmigaPlayWholeBuffer(all, used, opt, stats);
	free(all);
	all = NULL;
	return n;
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
	signed char *buf[2];
	unsigned long len[2];
	unsigned long playbackChannels;
	unsigned long halfMilliseconds;
	int playbackRate;
	int inputSampleRate;
	int active;
	int refill;
	int err;

	PlaybackCleanupStatusInit(&cleanupStatus);
	inputSampleRate = ProbeInputSampleRate(input, decoder, stats);
	playbackRate = EffectiveOutputSampleRate(opt, inputSampleRate);
	if (playbackRate <= 0)
		playbackRate = opt->outputRate > 0 ? opt->outputRate : 8287;
	stats->outputSampleRate = playbackRate;
	DecodeStreamInit(&stream, input, decoder, stats, timing);
	period = AmigaPalAudioPeriod(playbackRate);
	PrintFastLowrateOutputRateDifference(opt, playbackRate);
	printf("play output rate: %d Hz\n", playbackRate);
	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	printf("PAL audio period: %u\n", period);
	if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
		opt->stereo ? 2UL : 1UL, buf, &bufBytes, &cleanupStatus) != 0) {
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
		return -1;
	}
	halfMilliseconds = PlaybackBufferDurationMilliseconds(opt, bufBytes,
		playbackRate);
	printf("playback half-buffer: %lu ms, %lu bytes\n", halfMilliseconds,
		bufBytes);
	PrintPlaybackDebugStartup(opt, playbackRate, period, requestedBytes,
		bufBytes, &player, buf);

	/* Fill both halves before the first CMD_WRITE starts playback. */
	playbackChannels = opt->stereo ? 2UL : 1UL;
	len[0] = DecodeStreamFillPlaybackPrefill(&stream, opt, buf[0],
		bufBytes, PlaybackStartupPrefillSamples(opt, bufBytes));
	PrintPlaybackFillDebug(opt, 0, len[0]);
	if (stream.decodeError) {
		AmigaAudioClose(&player, &cleanupStatus);
		AmigaFreePlaybackBuffers(opt, buf, bufBytes, &cleanupStatus);
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
		return -1;
	}
	if (len[0] > 0 && opt->debugPlay && PlaybackBufferPeakS8(buf[0], len[0]) == 0)
		printf("first playback buffer is silent/near-silent\n");
	if (len[0] == 0 || len[0] / playbackChannels == 0) {
		fprintf(stderr, "first playback buffer fill produced zero CMD_WRITE bytes\n");
		AmigaAudioClose(&player, &cleanupStatus);
		AmigaFreePlaybackBuffers(opt, buf, bufBytes, &cleanupStatus);
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
		return -1;
	}
	len[1] = (unsigned long)DecodeStreamFillS8(&stream, opt, buf[1],
		(int)bufBytes);
	PrintPlaybackFillDebug(opt, 1, len[1]);
	if (stream.decodeError) {
		AmigaAudioClose(&player, &cleanupStatus);
		AmigaFreePlaybackBuffers(opt, buf, bufBytes, &cleanupStatus);
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
		return -1;
	}

	/* Submit both prefilled requests so audio.device can switch without a gap. */
	if (AmigaAudioSubmit(&player, 0, buf[0], len[0]) != 0) {
		fprintf(stderr, "playback buffer A CMD_WRITE byte length is invalid\n");
		err = -1;
	} else {
		err = 0;
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE submitted A: %lu bytes\n", len[0]);
	}
	if (err == 0 && len[1] > 0) {
		if (AmigaAudioSubmit(&player, 1, buf[1], len[1]) != 0) {
			fprintf(stderr, "playback buffer B CMD_WRITE byte length is invalid\n");
			err = -1;
		} else if (opt->debugPlay) {
			printf("debug-play: CMD_WRITE submitted B: %lu bytes\n", len[1]);
		}
	}

	active = 0;
	while (err == 0 && !gPlaybackInterrupted && player.sent[active][0]) {
		clock_t activeStarted;
		clock_t submittedAt;
		unsigned long elapsedMilliseconds;
		unsigned long activeMilliseconds;
		long spareMilliseconds;
		int underrun;
		int late;

		if (AmigaAudioWait(&player, active) != 0) {
			fprintf(stderr, "audio.device write failed\n");
			err = -1;
			break;
		}
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE completed %s\n",
				PlaybackBufferName(active));

		/* The queued request starts now; refill the completed half while it plays. */
		activeStarted = clock();
		refill = active;
		active = 1 - active;
		len[refill] = (unsigned long)DecodeStreamFillS8(&stream, opt,
			buf[refill], (int)bufBytes);
		PrintPlaybackFillDebug(opt, refill, len[refill]);
		if (stream.decodeError) {
			err = -1;
			break;
		}
		if (len[refill] == 0)
			continue;
		if (!player.sent[active][0]) {
			stats->underruns++;
			stats->underrunBuffers[refill]++;
			stats->lateBuffers++;
			if (opt->debugPlay)
				printf("debug-play: underrun detected before buffer %s submit (no queued buffer)\n",
					PlaybackBufferName(refill));
			if (AmigaAudioSubmit(&player, refill, buf[refill], len[refill]) != 0) {
				fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
					PlaybackBufferName(refill));
				err = -1;
				break;
			}
			if (opt->debugPlay)
				printf("debug-play: CMD_WRITE submitted %s: %lu bytes\n",
					PlaybackBufferName(refill), len[refill]);
			active = refill;
			continue;
		}

		underrun = AmigaAudioDone(&player, active);
		submittedAt = clock();
		elapsedMilliseconds = PlaybackElapsedMilliseconds(activeStarted, submittedAt);
		activeMilliseconds = PlaybackBufferDurationMilliseconds(opt, len[active],
			playbackRate);
		spareMilliseconds = (long)activeMilliseconds - (long)elapsedMilliseconds;
		late = spareMilliseconds <= 0 || underrun;
		if (!stats->spareTimeMeasured || spareMilliseconds < stats->minimumSpareMilliseconds) {
			stats->minimumSpareMilliseconds = spareMilliseconds;
			stats->spareTimeMeasured = 1;
		}
		if (late)
			stats->lateBuffers++;
		if (underrun) {
			stats->underruns++;
			stats->underrunBuffers[active]++;
			if (opt->debugPlay)
				printf("debug-play: underrun detected before buffer %s refill submit\n",
					PlaybackBufferName(refill));
		}
		if (AmigaAudioSubmit(&player, refill, buf[refill], len[refill]) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(refill));
			err = -1;
			break;
		}
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE submitted %s: %lu bytes\n",
				PlaybackBufferName(refill), len[refill]);
	}

	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		err = -1;
	}
	AmigaAudioClose(&player, &cleanupStatus);
	AmigaFreePlaybackBuffers(opt, buf, bufBytes, &cleanupStatus);
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
	signed char *buf[2];
	int playbackRate;
	int pass;
	int err;

	playbackRate = opt->outputRate > 0 ? opt->outputRate : (opt->stereo ? 8820 : 8287);
	period = AmigaPalAudioPeriod(playbackRate);
	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	err = 0;
	for (pass = 0; pass < 3 && err == 0 && !gPlaybackInterrupted; pass++) {
		unsigned long len;

		PlaybackCleanupStatusInit(&cleanupStatus);
		buf[0] = NULL;
		buf[1] = NULL;
		printf("play lifecycle test pass %d/3\n", pass + 1);
		if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
			opt->stereo ? 2UL : 1UL, buf, &chunkBytes, &cleanupStatus) != 0) {
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
		memset(buf[0], 0, len);
		if (AmigaAudioSubmit(&player, 0, buf[0], len) != 0) {
			fprintf(stderr, "play lifecycle test CMD_WRITE byte length is invalid\n");
			err = -1;
		}
		AmigaAudioClose(&player, &cleanupStatus);
		AmigaFreePlaybackBuffers(opt, buf, chunkBytes, &cleanupStatus);
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
	if (opt.selftestFdct32) {
		int selftestErr = SelftestFdct32();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdct) {
		int selftestErr = SelftestImdct();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFastLowrate) {
		int selftestErr = SelftestFastLowrate();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestMonoFastLowrateStereo) {
		int selftestErr = SelftestMonoFastLowrateStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.playLifecycleTest) {
		int playTestErr;
		gPlaybackInterrupted = 0;
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
	InputSourceInit(&input, infile);
	if (opt.info) {
		PrintMp3Info(infile, opt.inName);
		if (!opt.play && !opt.outName) {
			fclose(infile);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 0;
		}
	}
	if (opt.fastMem && InputSourcePreloadFastMemory(&input) != 0) {
		fprintf(stderr, "cannot preload input into Fast RAM: %s\n", opt.inName);
		fclose(infile);
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
			fclose(infile);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
	}

	decoder = MP3InitDecoder();
	if (!decoder) {
		fprintf(stderr, "MP3InitDecoder failed\n");
		InputSourceClose(&input);
		fclose(infile);
		if (outfile)
			fclose(outfile);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	if (opt.stereo)
		fprintf(stderr, "Stereo playback needs significantly more CPU and may underrun on 030.\n");

	MP3SetOutputMono(decoder, opt.mono && !opt.stereo);

	if (opt.fastLowrate) {
		int stride = FastLowrateStrideForOutputRate(opt.outputRate);
		MP3SetFastLowrate(decoder, stride);
		if (opt.outputRate == 22050)
			fprintf(stderr,
				"22050 requires significantly more CPU and may underrun on 030 systems.\n");
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
		fprintf(stderr, "warning: --fast-lowrate is experimental, lower quality, "
			"and only skips polyphase output samples; IMDCT/DCT32 still run full-rate\n");
#else
		fprintf(stderr, "warning: --fast-lowrate is experimental and lower quality; "
			"this build still generates full polyphase output before decimation\n");
#endif
	}

	if (opt.play) {
		int playErr;
		TimingStats *playTiming;
		playTiming = opt.bench ? &timing : NULL;
		gPlaybackInterrupted = 0;
#ifndef AMIGA_M68K
		signal(SIGINT, PlaybackSignalHandler);
#endif
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
		fclose(infile);
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

		if (bytesLeft < 2 * MAINBUF_SIZE && !eofReached) {
			nRead = FillReadBuffer(readBuf, readPtr, READBUF_SIZE,
				bytesLeft, &input);
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

		if (opt.bench) {
			clock_t t0 = clock();
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
			timing.frameDecode += clock() - t0;
		} else {
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
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
		if (opt.debugFastLowrate) {
			MP3FastLowrateGranuleDebug fastDbg[MAX_NGRAN];
			int dbgCount = MP3GetFastLowrateDebug(decoder, fastDbg, MAX_NGRAN);
			int dbgIndex;
			for (dbgIndex = 0; dbgIndex < dbgCount && dbgIndex < MAX_NGRAN; dbgIndex++) {
				fprintf(stderr,
					"fast-lowrate frame=%lu granule=%d full-rate-samps=%d "
					"lowrate-samps=%d cumulative-lowrate-samps=%d "
					"dest-offset=%d..%d\n",
					stats.decodedFrames, fastDbg[dbgIndex].granule,
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
	fclose(infile);
	if (outfile)
		fclose(outfile);
	gTiming = NULL;
	MP3SetDecodeCoreProfileEnabled(0);
	free(resolvedOutName);
	AmigaFreeNormalizedArgs(&normalized);

	return verifyError ? 1 : 0;
}
