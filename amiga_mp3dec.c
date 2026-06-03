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
	int help;
} DecodeOptions;

typedef struct DecodeStats {
	unsigned long decodedFrames;
	unsigned long outputSamples;
	int sampleRate;
	int channels;
	int bitrate;
} DecodeStats;

typedef struct SvxWriter {
	FILE *fp;
	long formSizePos;
	long oneShotPos;
	long bodySizePos;
	unsigned long sourceSamples;
	unsigned long bodyBytes;
	int compression;
	int fibStarted;
	signed char fibPrev;
	int fibHaveHighNibble;
	unsigned char fibPending;
} SvxWriter;

static void PrintUsage(const char *prog)
{
	printf("usage: %s [options] infile.mp3 outfile\n", prog);
	printf("options:\n");
	printf("  --mono       mix stereo to mono before writing\n");
	printf("  --s8         write raw signed 8-bit PCM instead of signed 16-bit PCM\n");
	printf("  --8svx       write Amiga IFF-8SVX signed 8-bit output (implies mono)\n");
	printf("  --fibdelta   use 8SVX Fibonacci Delta compression (implies --8svx)\n");
	printf("  --bench      print elapsed decode/write time and realtime ratio\n");
	printf("\n");
	printf("default output is raw signed 16-bit big-endian PCM.\n");
}

static int ParseOptions(int argc, char **argv, DecodeOptions *opt)
{
	int i;

	memset(opt, 0, sizeof(*opt));
	opt->outFormat = OUT_PCM16;
	opt->compression = SVX_COMP_NONE;

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
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			opt->help = 1;
			return 0;
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

	if (!opt->inName || !opt->outName)
		return -1;

	return 0;
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

static void WriteU16BE(FILE *fp, unsigned int v)
{
	fputc((int)((v >> 8) & 0xff), fp);
	fputc((int)(v & 0xff), fp);
}

static void WriteU32BE(FILE *fp, unsigned long v)
{
	fputc((int)((v >> 24) & 0xff), fp);
	fputc((int)((v >> 16) & 0xff), fp);
	fputc((int)((v >> 8) & 0xff), fp);
	fputc((int)(v & 0xff), fp);
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
			fputc((int)(unsigned char)Sample16ToS8(pcm[i]), fp);
	} else {
		for (i = 0; i < nSamps; i++)
			WriteU16BE(fp, (unsigned int)(unsigned short)pcm[i]);
	}

	return ferror(fp) ? -1 : 0;
}

static int SvxBegin(SvxWriter *svx, FILE *fp, int sampleRate, int compression)
{
	memset(svx, 0, sizeof(*svx));
	svx->fp = fp;
	svx->compression = compression;

	fwrite("FORM", 1, 4, fp);
	svx->formSizePos = ftell(fp);
	WriteU32BE(fp, 0);
	fwrite("8SVX", 1, 4, fp);

	fwrite("VHDR", 1, 4, fp);
	WriteU32BE(fp, 20);
	svx->oneShotPos = ftell(fp);
	WriteU32BE(fp, 0);              /* oneShotHiSamples */
	WriteU32BE(fp, 0);              /* repeatHiSamples */
	WriteU32BE(fp, 0);              /* samplesPerHiCycle */
	WriteU16BE(fp, (unsigned int)sampleRate);
	fputc(1, fp);                   /* ctOctave */
	fputc(compression, fp);         /* sCompression */
	WriteU32BE(fp, 0x00010000UL);   /* volume */

	fwrite("BODY", 1, 4, fp);
	svx->bodySizePos = ftell(fp);
	WriteU32BE(fp, 0);

	return ferror(fp) ? -1 : 0;
}

static void SvxWriteByte(SvxWriter *svx, unsigned char b)
{
	fputc((int)b, svx->fp);
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
	int nibble;

	if (!svx->fibStarted)
		SvxStartFibDelta(svx, sample);

	nibble = FibDeltaNibble(svx->fibPrev, sample);
	svx->fibPrev = FibDeltaApply(svx->fibPrev, nibble);
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

	return ferror(svx->fp) ? -1 : 0;
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
		fputc(0, svx->fp);

	endPos = ftell(svx->fp);
	formSize = (unsigned long)(endPos - 8);
	PatchU32BE(svx->fp, svx->oneShotPos, svx->sourceSamples);
	PatchU32BE(svx->fp, svx->bodySizePos, svx->bodyBytes);
	PatchU32BE(svx->fp, svx->formSizePos, formSize);

	return ferror(svx->fp) ? -1 : 0;
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

int main(int argc, char **argv)
{
	DecodeOptions opt;
	DecodeStats stats;
	unsigned char readBuf[READBUF_SIZE];
	unsigned char *readPtr;
	short decodeBuf[OUTBUF_SAMPS];
	short writeBuf[OUTBUF_SAMPS];
	FILE *infile;
	FILE *outfile;
	HMP3Decoder decoder;
	MP3FrameInfo info;
	SvxWriter svx;
	int bytesLeft;
	int eofReached;
	int outOfData;
	int svxOpen;
	clock_t startClock;
	clock_t endClock;

	if (ParseOptions(argc, argv, &opt) != 0) {
		PrintUsage(argv[0]);
		return 1;
	}
	if (opt.help) {
		PrintUsage(argv[0]);
		return 0;
	}

	memset(&stats, 0, sizeof(stats));
	memset(&info, 0, sizeof(info));

	infile = fopen(opt.inName, "rb");
	if (!infile) {
		fprintf(stderr, "cannot open input: %s\n", opt.inName);
		return 1;
	}

	outfile = fopen(opt.outName, "wb+");
	if (!outfile) {
		fprintf(stderr, "cannot open output: %s\n", opt.outName);
		fclose(infile);
		return 1;
	}

	decoder = MP3InitDecoder();
	if (!decoder) {
		fprintf(stderr, "MP3InitDecoder failed\n");
		fclose(infile);
		fclose(outfile);
		return 1;
	}

	bytesLeft = 0;
	eofReached = 0;
	outOfData = 0;
	svxOpen = 0;
	readPtr = readBuf;
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

		err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
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

		if (opt.outFormat == OUT_8SVX && !svxOpen) {
			if (!info.samprate) {
				fprintf(stderr, "cannot write 8SVX before sample rate is known\n");
				outOfData = 1;
				break;
			}
			if (SvxBegin(&svx, outfile, info.samprate, opt.compression) != 0) {
				fprintf(stderr, "cannot write 8SVX header\n");
				outOfData = 1;
				break;
			}
			svxOpen = 1;
		}

		{
			int outSamps = MixFrame(decodeBuf, writeBuf, info.outputSamps,
				info.nChans, opt.mono);
			int writeErr;

			if (opt.outFormat == OUT_8SVX)
				writeErr = SvxWriteSamples(&svx, writeBuf, outSamps);
			else
				writeErr = WriteRawSamples(outfile, writeBuf, outSamps,
					opt.outFormat);

			if (writeErr != 0) {
				fprintf(stderr, "output write error\n");
				outOfData = 1;
				break;
			}
			stats.outputSamples += (unsigned long)outSamps;
		}

		stats.decodedFrames++;
	}

	if (svxOpen && SvxEnd(&svx) != 0)
		fprintf(stderr, "error finalizing 8SVX file\n");

	endClock = clock();

	printf("sample rate: %d Hz\n", stats.sampleRate);
	printf("channels: %d%s\n", stats.channels, opt.mono ? " (mono output)" : "");
	printf("bitrate: %d bps\n", stats.bitrate);
	printf("decoded frames: %lu\n", stats.decodedFrames);
	printf("output samples: %lu\n", stats.outputSamples);

	if (opt.bench) {
		double elapsed = 0.0;
		double audioSeconds = 0.0;
		if (CLOCKS_PER_SEC > 0)
			elapsed = (double)(endClock - startClock) / (double)CLOCKS_PER_SEC;
		if (stats.sampleRate > 0) {
			int outputChannels = opt.mono ? 1 : stats.channels;
			if (outputChannels <= 0)
				outputChannels = 1;
			audioSeconds = (double)stats.outputSamples /
				((double)stats.sampleRate * (double)outputChannels);
		}
		printf("elapsed seconds: %.3f\n", elapsed);
		if (elapsed > 0.0 && audioSeconds > 0.0)
			printf("decode speed: %.2fx realtime\n", audioSeconds / elapsed);
	}

	MP3FreeDecoder(decoder);
	fclose(infile);
	fclose(outfile);

	return 0;
}
