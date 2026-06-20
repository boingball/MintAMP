/* ***** BEGIN LICENSE BLOCK ***** 
 * Version: RCSL 1.0/RPSL 1.0 
 *  
 * Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved. 
 *      
 * The contents of this file, and the files included with this file, are 
 * subject to the current version of the RealNetworks Public Source License 
 * Version 1.0 (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the RealNetworks Community Source License Version 1.0 
 * (the "RCSL") available at http://www.helixcommunity.org/content/rcsl, 
 * in which case the RCSL will apply. You may also obtain the license terms 
 * directly from RealNetworks.  You may not use this file except in 
 * compliance with the RPSL or, if you have a valid RCSL with RealNetworks 
 * applicable to this file, the RCSL.  Please see the applicable RPSL or 
 * RCSL for the rights, obligations and limitations governing use of the 
 * contents of the file.  
 *  
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the portions 
 * it created. 
 *  
 * This file, and the files included with this file, is distributed and made 
 * available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
 * EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. 
 * 
 * Technology Compatibility Kit Test Suite(s) Location: 
 *    http://www.helixcommunity.org/content/tck 
 * 
 * Contributor(s): 
 *  
 * ***** END LICENSE BLOCK ***** */ 

/**************************************************************************************
 * Fixed-point MP3 decoder
 * Jon Recker (jrecker@real.com), Ken Cooke (kenc@real.com)
 * June 2003
 *
 * mp3dec.h - public C API for MP3 decoder
 **************************************************************************************/

#ifndef _MP3DEC_H
#define _MP3DEC_H

#include <time.h>

#if defined(_WIN32) && !defined(_WIN32_WCE)
#
#elif defined(_WIN32) && defined(_WIN32_WCE) && defined(ARM)
#
#elif defined(_WIN32) && defined(WINCE_EMULATOR)
#
#elif defined(ARM_ADS)
#
#elif defined(_SYMBIAN) && defined(__WINS__)	/* Symbian emulator for Ix86 */
#
#elif defined(__GNUC__) && defined(ARM)
#
#elif defined(AMIGA_M68K) || \
	(defined(__GNUC__) && (defined(__mc68000__) || defined(__mc68020__) || defined(mc68000)))
#
#elif defined(__GNUC__) && defined(__i386__)
#
#elif defined(_OPENWAVE_SIMULATOR) || defined(_OPENWAVE_ARMULATOR)
#
#else
//#error No platform defined. See valid options in mp3dec.h
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* determining MAINBUF_SIZE:
 *   max mainDataBegin = (2^9 - 1) bytes (since 9-bit offset) = 511
 *   max nSlots (concatenated with mainDataBegin bytes from before) = 1440 - 9 - 4 + 1 = 1428
 *   511 + 1428 = 1939, round up to 1940 (4-byte align)
 */
#define MAINBUF_SIZE	1940

#define MAX_NGRAN		2		/* max granules */
#define MAX_NCHAN		2		/* max channels */
#define MAX_NSAMP		576		/* max samples per channel, per granule */

/* map to 0,1,2 to make table indexing easier */
typedef enum {
	MPEG1 =  0,
	MPEG2 =  1,
	MPEG25 = 2
} MPEGVersion;

typedef void *HMP3Decoder;

enum {
	ERR_MP3_NONE =                  0,
	ERR_MP3_INDATA_UNDERFLOW =     -1,
	ERR_MP3_MAINDATA_UNDERFLOW =   -2,
	ERR_MP3_FREE_BITRATE_SYNC =    -3,
	ERR_MP3_OUT_OF_MEMORY =	       -4,
	ERR_MP3_NULL_POINTER =         -5,
	ERR_MP3_INVALID_FRAMEHEADER =  -6,
	ERR_MP3_INVALID_SIDEINFO =     -7,
	ERR_MP3_INVALID_SCALEFACT =    -8,
	ERR_MP3_INVALID_HUFFCODES =    -9,
	ERR_MP3_INVALID_DEQUANTIZE =   -10,
	ERR_MP3_INVALID_IMDCT =        -11,
	ERR_MP3_INVALID_SUBBAND =      -12,

	ERR_UNKNOWN =                  -9999
};

typedef struct _MP3FrameInfo {
	int bitrate;
	int nChans;
	int samprate;
	int bitsPerSample;
	int outputSamps;
	int layer;
	int version;
} MP3FrameInfo;

typedef struct _MP3DecodeCoreProfile {
	clock_t bitstreamFrameParsing;
	clock_t huffman;
	clock_t dequant;
	clock_t stereoPost;
	clock_t imdct;
	clock_t subbandDct32;
	clock_t polyphase;
	unsigned long imdctSubbandsExecuted;
	unsigned long imdctSubbandsSkipped;
	unsigned long monoMSSideSkipEligible;
	unsigned long monoMSSideHuffmanSkipped;
	unsigned long monoMSSideDequantSkipped;
	unsigned long monoMSSideIMDCTSkipped;
	unsigned long monoMSSideSynthesisSkipped;
	unsigned long monoMSSideFallbackNotStereoSource;
	unsigned long monoMSSideFallbackOutputStereo;
	unsigned long monoMSSideFallbackNotJointStereo;
	unsigned long monoMSSideFallbackNoMS;
	unsigned long monoMSSideFallbackIntensity;
	unsigned long monoMSSideFallbackDisabled;
	unsigned long monoMSSideFallbackMalformed;
} MP3DecodeCoreProfile;

typedef struct _MP3FastLowrateGranuleDebug {
	int granule;
	int stride;
	int phaseStart;
	int phaseEnd;
	int fullRateSamps;
	int lowrateSamps;
	int cumulativeLowrateSamps;
	int destOffsetStart;
	int destOffsetEnd;
} MP3FastLowrateGranuleDebug;

enum {
	MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING = 0,
	MP3_DECODE_CORE_PROFILE_HUFFMAN,
	MP3_DECODE_CORE_PROFILE_DEQUANT,
	MP3_DECODE_CORE_PROFILE_STEREO_POST,
	MP3_DECODE_CORE_PROFILE_IMDCT,
	MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32,
	MP3_DECODE_CORE_PROFILE_POLYPHASE
};

/* public API */
HMP3Decoder MP3InitDecoder(void);
void MP3FreeDecoder(HMP3Decoder hMP3Decoder);
int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, int *bytesLeft, short *outbuf, int useSize);

void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo);
int MP3GetNextFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo, unsigned char *buf);
int MP3FindSyncWord(unsigned char *buf, int nBytes);
void MP3SetFastLowrate(HMP3Decoder hMP3Decoder, int stride);
int MP3GetFastLowrateStride(HMP3Decoder hMP3Decoder);
int MP3GetFastLowrateActiveSubbands(HMP3Decoder hMP3Decoder);
void MP3SetSuperfastLowrate(HMP3Decoder hMP3Decoder, int enabled);
int MP3SuperfastLowrateEnabled(HMP3Decoder hMP3Decoder);
void MP3SetExperimentalIMDCTThin(HMP3Decoder hMP3Decoder, int enabled);
int MP3ExperimentalIMDCTThinActive(HMP3Decoder hMP3Decoder);
void MP3SetExperimentalPolyphase(int enabled);
int MP3ExperimentalPolyphaseEnabled(void);
void MP3SetExperimentalReducedTaps(int enabled);
int MP3ExperimentalReducedTapsEnabled(void);
void MP3SetExperimentalFDCT32Quarter(int enabled);
int MP3ExperimentalFDCT32QuarterEnabled(void);
void MP3SetExperimentalFDCT32Mulsw(int enabled);
int MP3ExperimentalFDCT32MulswEnabled(void);
void MP3SetExperimentalIMDCTMulsw(int enabled);
int MP3ExperimentalIMDCTMulswEnabled(void);
void MP3SetExperimentalHuffman(int enabled);
int MP3ExperimentalHuffmanEnabled(void);
void MP3SetOutputMono(HMP3Decoder hMP3Decoder, int enabled);
void MP3SetMonoMSSideSkip(HMP3Decoder hMP3Decoder, int enabled);
int MP3GetOutputChannels(HMP3Decoder hMP3Decoder);
int MP3GetFastLowrateDebug(HMP3Decoder hMP3Decoder,
	MP3FastLowrateGranuleDebug *debug, int maxDebug);

void MP3SetDecodeCoreProfileEnabled(int enabled);
void MP3SetForceStereoStride2PolyphaseC(int enabled);
int MP3ForceStereoStride2PolyphaseC(void);
void MP3GetStereoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride2PolyphaseCounters(void);
void MP3ResetDecodeCoreProfile(void);
void MP3GetDecodeCoreProfile(MP3DecodeCoreProfile *profile);
int MP3DecodeCoreProfileIsEnabled(void);
void MP3AddDecodeCoreProfile(int bucket, clock_t elapsed);
void MP3AddDecodeCoreIMDCTSubbands(unsigned long executed, unsigned long skipped);
void MP3AddDecodeCoreMonoMSSideSkip(int bucket);

#ifdef __cplusplus
}
#endif

#endif	/* _MP3DEC_H */
