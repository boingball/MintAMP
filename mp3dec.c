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
 * mp3dec.c - platform-independent top level MP3 decoder API
 **************************************************************************************/

#include "string.h"
//#include "hlxclib/string.h"		/* for memmove, memcpy (can replace with different implementations if desired) */
#include "mp3common.h"	/* includes mp3dec.h (public API) and internal, platform-independent API */
#include "real/coder.h"
#include "amiga_profile_decode.h"


//#define PROFILE
#ifdef PROFILE
#include "systime.h"
#endif


static MP3DecodeCoreProfile gDecodeCoreProfile;
static int gDecodeCoreProfileEnabled;

void MP3ResetDecodeCoreProfile(void)
{
	memset(&gDecodeCoreProfile, 0, sizeof(gDecodeCoreProfile));
}

void MP3GetDecodeCoreProfile(MP3DecodeCoreProfile *profile)
{
	if (profile)
		memcpy(profile, &gDecodeCoreProfile, sizeof(*profile));
}

void MP3SetDecodeCoreProfileEnabled(int enabled)
{
#ifdef AMIGA_PROFILE_DECODE
	gDecodeCoreProfileEnabled = enabled ? 1 : 0;
#else
	(void)enabled;
	gDecodeCoreProfileEnabled = 0;
#endif
}

int MP3DecodeCoreProfileIsEnabled(void)
{
#ifdef AMIGA_PROFILE_DECODE
	return gDecodeCoreProfileEnabled;
#else
	return 0;
#endif
}

void MP3AddDecodeCoreProfile(int bucket, clock_t elapsed)
{
#ifdef AMIGA_PROFILE_DECODE
	switch (bucket) {
	case MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING:
		gDecodeCoreProfile.bitstreamFrameParsing += elapsed;
		break;
	case MP3_DECODE_CORE_PROFILE_HUFFMAN:
		gDecodeCoreProfile.huffman += elapsed;
		break;
	case MP3_DECODE_CORE_PROFILE_DEQUANT:
		gDecodeCoreProfile.dequant += elapsed;
		break;
	case MP3_DECODE_CORE_PROFILE_STEREO_POST:
		gDecodeCoreProfile.stereoPost += elapsed;
		break;
	case MP3_DECODE_CORE_PROFILE_IMDCT:
		gDecodeCoreProfile.imdct += elapsed;
		break;
	case MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32:
		gDecodeCoreProfile.subbandDct32 += elapsed;
		break;
	case MP3_DECODE_CORE_PROFILE_POLYPHASE:
		gDecodeCoreProfile.polyphase += elapsed;
		break;
	default:
		break;
	}
#else
	(void)bucket;
	(void)elapsed;
#endif
}

void MP3AddDecodeCoreIMDCTSubbands(unsigned long executed, unsigned long skipped)
{
#ifdef AMIGA_PROFILE_DECODE
	gDecodeCoreProfile.imdctSubbandsExecuted += executed;
	gDecodeCoreProfile.imdctSubbandsSkipped += skipped;
#else
	(void)executed;
	(void)skipped;
#endif
}

void MP3AddDecodeCoreMonoMSSideSkip(int bucket)
{
#ifdef AMIGA_PROFILE_DECODE
	unsigned long *counter = 0;

	switch (bucket) {
	case 0: counter = &gDecodeCoreProfile.monoMSSideSkipEligible; break;
	case 1: counter = &gDecodeCoreProfile.monoMSSideHuffmanSkipped; break;
	case 2: counter = &gDecodeCoreProfile.monoMSSideDequantSkipped; break;
	case 3: counter = &gDecodeCoreProfile.monoMSSideIMDCTSkipped; break;
	case 4: counter = &gDecodeCoreProfile.monoMSSideSynthesisSkipped; break;
	case 5: counter = &gDecodeCoreProfile.monoMSSideFallbackNotStereoSource; break;
	case 6: counter = &gDecodeCoreProfile.monoMSSideFallbackOutputStereo; break;
	case 7: counter = &gDecodeCoreProfile.monoMSSideFallbackNotJointStereo; break;
	case 8: counter = &gDecodeCoreProfile.monoMSSideFallbackNoMS; break;
	case 9: counter = &gDecodeCoreProfile.monoMSSideFallbackIntensity; break;
	case 10: counter = &gDecodeCoreProfile.monoMSSideFallbackDisabled; break;
	case 11: counter = &gDecodeCoreProfile.monoMSSideFallbackMalformed; break;
	default: break;
	}
	if (counter)
		(*counter)++;
#else
	(void)bucket;
#endif
}

/**************************************************************************************
 * Function:    MP3InitDecoder
 *
 * Description: allocate memory for platform-specific data
 *              clear all the user-accessible fields
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      handle to mp3 decoder instance, 0 if malloc fails
 **************************************************************************************/
HMP3Decoder MP3InitDecoder(void)
{
	MP3DecInfo *mp3DecInfo;

	mp3DecInfo = AllocateBuffers();
	if (mp3DecInfo)
		mp3DecInfo->monoMSSideSkipEnabled = 1;

	return (HMP3Decoder)mp3DecInfo;
}

/**************************************************************************************
 * Function:    MP3FreeDecoder
 *
 * Description: free platform-specific data allocated by InitMP3Decoder
 *              zero out the contents of MP3DecInfo struct
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *
 * Outputs:     none
 *
 * Return:      none
 **************************************************************************************/
void MP3FreeDecoder(HMP3Decoder hMP3Decoder)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return;

	FreeBuffers(mp3DecInfo);
}

/**************************************************************************************
 * Function:    MP3FindSyncWord
 *
 * Description: locate the next byte-alinged sync word in the raw mp3 stream
 *
 * Inputs:      buffer to search for sync word
 *              max number of bytes to search in buffer
 *
 * Outputs:     none
 *
 * Return:      offset to first sync word (bytes from start of buf)
 *              -1 if sync not found after searching nBytes
 **************************************************************************************/
int MP3FindSyncWord(unsigned char *buf, int nBytes)
{
	int i;

	/* A successful decode advances the input pointer to the expected start of
	 * the next frame.  Keep that clean-stream path out of the byte scan, but
	 * retain the scan for tags, corruption, and callers starting mid-stream. */
	if (nBytes >= 2 && (buf[0] & SYNCWORDH) == SYNCWORDH &&
		(buf[1] & SYNCWORDL) == SYNCWORDL)
		return 0;

	/* find byte-aligned syncword - need 12 (MPEG 1,2) or 11 (MPEG 2.5) matching bits */
	for (i = 1; i < nBytes - 1; i++) {
		if ( (buf[i+0] & SYNCWORDH) == SYNCWORDH && (buf[i+1] & SYNCWORDL) == SYNCWORDL )
			return i;
	}
	
	return -1;
}

static int MP3FastLowrateActiveSubbandsForStride(int stride)
{
	if (stride >= 5)
		return 6;	/* 8820/8287 Hz: output Nyquist ~4.4 kHz -> 6 subbands */
	if (stride >= 4)
		return 8;
	if (stride == 2)
		return 16;
	return 32;
}

void MP3SetFastLowrate(HMP3Decoder hMP3Decoder, int stride)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return;
	if (stride < 2)
		stride = 1;
	mp3DecInfo->fastLowrateStride = stride;
	mp3DecInfo->fastLowrateActiveSubbands = MP3FastLowrateActiveSubbandsForStride(stride);
	mp3DecInfo->fastLowratePhase = 0;
	mp3DecInfo->fastLowrateOutputSamps = 0;
	mp3DecInfo->fastLowrateDebugCount = 0;
}

void MP3SetSuperfastLowrate(HMP3Decoder hMP3Decoder, int enabled)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return;
	mp3DecInfo->superfastLowrate = enabled ? 1 : 0;
	if (mp3DecInfo->superfastLowrate)
		mp3DecInfo->fastLowrateActiveSubbands =
			MP3FastLowrateActiveSubbandsForStride(mp3DecInfo->fastLowrateStride);
}

int MP3SuperfastLowrateEnabled(HMP3Decoder hMP3Decoder)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;
	return mp3DecInfo ? mp3DecInfo->superfastLowrate : 0;
}


void MP3SetExperimentalIMDCTThin(HMP3Decoder hMP3Decoder, int enabled)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return;
	mp3DecInfo->expImdctThin = enabled ? 1 : 0;
	mp3DecInfo->imdctThinActive = 0;
}

int MP3ExperimentalIMDCTThinActive(HMP3Decoder hMP3Decoder)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return 0;
	return mp3DecInfo->imdctThinActive;
}

int MP3GetFastLowrateStride(HMP3Decoder hMP3Decoder)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return 1;
	return mp3DecInfo->fastLowrateStride > 1 ?
		mp3DecInfo->fastLowrateStride : 1;
}

int MP3GetFastLowrateDebug(HMP3Decoder hMP3Decoder,
	MP3FastLowrateGranuleDebug *debug, int maxDebug)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;
	int count;
	int copyCount;

	if (!mp3DecInfo)
		return 0;
	count = mp3DecInfo->fastLowrateDebugCount;
	if (debug && maxDebug > 0) {
		copyCount = count < maxDebug ? count : maxDebug;
		memcpy(debug, mp3DecInfo->fastLowrateDebug,
			copyCount * sizeof(debug[0]));
	}
	return count;
}


void MP3SetOutputMono(HMP3Decoder hMP3Decoder, int enabled)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return;
	mp3DecInfo->outputMono = enabled ? 1 : 0;
}

void MP3SetMonoMSSideSkip(HMP3Decoder hMP3Decoder, int enabled)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return;
	mp3DecInfo->monoMSSideSkipEnabled = enabled ? 1 : 0;
}

static int MP3MonoMSSideSkipEligible(MP3DecInfo *mp3DecInfo, int gr, int mainBits)
{
	FrameHeader *fh;

	if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS)
		return 11;
	fh = (FrameHeader *)mp3DecInfo->FrameHeaderPS;
	if (!mp3DecInfo->monoMSSideSkipEnabled)
		return 10;
	if (mp3DecInfo->nChans != 2)
		return 5;
	if (!mp3DecInfo->outputMono)
		return 6;
	if (fh->sMode != Joint)
		return 7;
	if (!(fh->modeExt & 0x02))
		return 8;
	if (fh->modeExt & 0x01)
		return 9;
	if (gr < 0 || gr >= mp3DecInfo->nGrans ||
		mp3DecInfo->part23Length[gr][1] < 0 ||
		mainBits < mp3DecInfo->part23Length[gr][1])
		return 11;
	return 0;
}

static int MP3SkipMainDataBits(unsigned char **mainPtr, int *bitOffset, int *mainBits, int nBits)
{
	int totalBits;
	int offset;

	if (!mainPtr || !*mainPtr || !bitOffset || !mainBits || nBits < 0 || *mainBits < nBits)
		return -1;
	totalBits = *bitOffset + nBits;
	offset = totalBits >> 3;
	*bitOffset = totalBits & 7;
	*mainPtr += offset;
	*mainBits -= nBits;
	return 0;
}

int MP3GetOutputChannels(HMP3Decoder hMP3Decoder)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return 0;
	if (mp3DecInfo->outputMono && mp3DecInfo->nChans > 1)
		return 1;
	return mp3DecInfo->nChans;
}

/**************************************************************************************
 * Function:    MP3FindFreeSync
 *
 * Description: figure out number of bytes between adjacent sync words in "free" mode
 *
 * Inputs:      buffer to search for next sync word
 *              the 4-byte frame header starting at the current sync word
 *              max number of bytes to search in buffer
 *
 * Outputs:     none
 *
 * Return:      offset to next sync word, minus any pad byte (i.e. nSlots)
 *              -1 if sync not found after searching nBytes
 *
 * Notes:       this checks that the first 22 bits of the next frame header are the
 *                same as the current frame header, but it's still not foolproof
 *                (could accidentally find a sequence in the bitstream which 
 *                 appears to match but is not actually the next frame header)
 *              this could be made more error-resilient by checking several frames
 *                in a row and verifying that nSlots is the same in each case
 *              since free mode requires CBR (see spec) we generally only call
 *                this function once (first frame) then store the result (nSlots)
 *                and just use it from then on
 **************************************************************************************/
static int MP3FindFreeSync(unsigned char *buf, unsigned char firstFH[4], int nBytes)
{
	int offset = 0;
	unsigned char *bufPtr = buf;

	/* loop until we either: 
	 *  - run out of nBytes (FindMP3SyncWord() returns -1)
	 *  - find the next valid frame header (sync word, version, layer, CRC flag, bitrate, and sample rate
	 *      in next header must match current header)
	 */
	while (1) {
		offset = MP3FindSyncWord(bufPtr, nBytes);
		bufPtr += offset;
		if (offset < 0) {
			return -1;
		} else if ( (bufPtr[0] == firstFH[0]) && (bufPtr[1] == firstFH[1]) && ((bufPtr[2] & 0xfc) == (firstFH[2] & 0xfc)) ) {
			/* want to return number of bytes per frame, NOT counting the padding byte, so subtract one if padFlag == 1 */
			if ((firstFH[2] >> 1) & 0x01)
				bufPtr--;
			return bufPtr - buf;
		}
		bufPtr += 3;
		nBytes -= (offset + 3);
	};

	return -1;
}

/**************************************************************************************
 * Function:    MP3GetLastFrameInfo
 *
 * Description: get info about last MP3 frame decoded (number of sampled decoded, 
 *                sample rate, bitrate, etc.)
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *              pointer to MP3FrameInfo struct
 *
 * Outputs:     filled-in MP3FrameInfo struct
 *
 * Return:      none
 *
 * Notes:       call this right after calling MP3Decode
 **************************************************************************************/
void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;
	int outputChans;

	if (!mp3DecInfo || mp3DecInfo->layer != 3) {
		mp3FrameInfo->bitrate = 0;
		mp3FrameInfo->nChans = 0;
		mp3FrameInfo->samprate = 0;
		mp3FrameInfo->bitsPerSample = 0;
		mp3FrameInfo->outputSamps = 0;
		mp3FrameInfo->layer = 0;
		mp3FrameInfo->version = 0;
	} else {
		mp3FrameInfo->bitrate = mp3DecInfo->bitrate;
		mp3FrameInfo->nChans = mp3DecInfo->nChans;
		mp3FrameInfo->samprate = mp3DecInfo->samprate;
		mp3FrameInfo->bitsPerSample = 16;
		if (mp3DecInfo->fastLowrateStride > 1 && mp3DecInfo->fastLowrateOutputSamps > 0) {
			mp3FrameInfo->outputSamps = mp3DecInfo->fastLowrateOutputSamps;
		} else {
			outputChans = (mp3DecInfo->outputMono && mp3DecInfo->nChans > 1) ? 1 : mp3DecInfo->nChans;
			mp3FrameInfo->outputSamps = outputChans * (int)samplesPerFrameTab[mp3DecInfo->version][mp3DecInfo->layer - 1];
		}
		mp3FrameInfo->layer = mp3DecInfo->layer;
		mp3FrameInfo->version = mp3DecInfo->version;
	}
}

/**************************************************************************************
 * Function:    MP3GetNextFrameInfo
 *
 * Description: parse MP3 frame header
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *              pointer to MP3FrameInfo struct
 *              pointer to buffer containing valid MP3 frame header (located using 
 *                MP3FindSyncWord(), above)
 *
 * Outputs:     filled-in MP3FrameInfo struct
 *
 * Return:      error code, defined in mp3dec.h (0 means no error, < 0 means error)
 **************************************************************************************/
int MP3GetNextFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo, unsigned char *buf)
{
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;

	if (!mp3DecInfo)
		return ERR_MP3_NULL_POINTER;

	if (UnpackFrameHeader(mp3DecInfo, buf) == -1 || mp3DecInfo->layer != 3)
		return ERR_MP3_INVALID_FRAMEHEADER;

	MP3GetLastFrameInfo(mp3DecInfo, mp3FrameInfo);

	return ERR_MP3_NONE;
}

/**************************************************************************************
 * Function:    MP3ClearBadFrame
 *
 * Description: zero out pcm buffer if error decoding MP3 frame
 *
 * Inputs:      mp3DecInfo struct with correct frame size parameters filled in
 *              pointer pcm output buffer
 *
 * Outputs:     zeroed out pcm buffer
 *
 * Return:      none
 **************************************************************************************/
static void MP3ClearBadFrame(MP3DecInfo *mp3DecInfo, short *outbuf)
{
	int i;

	if (!mp3DecInfo)
		return;

	for (i = 0; i < mp3DecInfo->nGrans * mp3DecInfo->nGranSamps * mp3DecInfo->nChans; i++)
		outbuf[i] = 0;
}


static int FastLowrateExpectedGranuleSamps(const MP3DecInfo *mp3DecInfo)
{
	int outputChans;
	int stride;

	stride = mp3DecInfo->fastLowrateStride;
	if (stride != 2 && stride != 4)
		return 0;
	outputChans = (mp3DecInfo->outputMono && mp3DecInfo->nChans > 1) ? 1 : mp3DecInfo->nChans;
	return (mp3DecInfo->nGranSamps / stride) * outputChans;
}

/**************************************************************************************
 * Function:    MP3Decode
 *
 * Description: decode one frame of MP3 data
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *              double pointer to buffer of MP3 data (containing headers + mainData)
 *              number of valid bytes remaining in inbuf
 *              pointer to outbuf, big enough to hold one frame of decoded PCM samples
 *              flag indicating whether MP3 data is normal MPEG format (useSize = 0)
 *                or reformatted as "self-contained" frames (useSize = 1)
 *
 * Outputs:     PCM data in outbuf, interleaved LRLRLR... if stereo
 *                number of output samples = nGrans * nGranSamps * output channels
 *              updated inbuf pointer, updated bytesLeft
 *
 * Return:      error code, defined in mp3dec.h (0 means no error, < 0 means error)
 *
 * Notes:       switching useSize on and off between frames in the same stream 
 *                is not supported (bit reservoir is not maintained if useSize on)
 **************************************************************************************/
int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, int *bytesLeft, short *outbuf, int useSize)
{
	int offset, bitOffset, mainBits, gr, ch, fhBytes, siBytes, freeFrameBytes;
	int prevBitOffset, sfBlockBits, huffBlockBits;
	unsigned char *mainPtr;
	clock_t amigaProfileStart;
	MP3DecInfo *mp3DecInfo = (MP3DecInfo *)hMP3Decoder;
	
	#ifdef PROFILE
	long time;
	#endif

	if (!mp3DecInfo)
		return ERR_MP3_NULL_POINTER;

	/* unpack frame header */
	AMIGA_PROFILE_START(amigaProfileStart);
	fhBytes = UnpackFrameHeader(mp3DecInfo, *inbuf);
	AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING, amigaProfileStart);
	if (fhBytes < 0)	
		return ERR_MP3_INVALID_FRAMEHEADER;		/* don't clear outbuf since we don't know size (failed to parse header) */
	*inbuf += fhBytes;
	
#ifdef PROFILE
	time = systime_get();
#endif
	/* unpack side info */
	AMIGA_PROFILE_START(amigaProfileStart);
	siBytes = UnpackSideInfo(mp3DecInfo, *inbuf);
	AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING, amigaProfileStart);
	if (siBytes < 0) {
		MP3ClearBadFrame(mp3DecInfo, outbuf);
		return ERR_MP3_INVALID_SIDEINFO;
	}
	*inbuf += siBytes;
	*bytesLeft -= (fhBytes + siBytes);
#ifdef PROFILE
	time = systime_get() - time;
	printf("UnpackSideInfo: %i ms\n", time);
#endif
	
	
	/* if free mode, need to calculate bitrate and nSlots manually, based on frame size */
	if (mp3DecInfo->bitrate == 0 || mp3DecInfo->freeBitrateFlag) {
		if (!mp3DecInfo->freeBitrateFlag) {
			/* first time through, need to scan for next sync word and figure out frame size */
			mp3DecInfo->freeBitrateFlag = 1;
			AMIGA_PROFILE_START(amigaProfileStart);
			mp3DecInfo->freeBitrateSlots = MP3FindFreeSync(*inbuf, *inbuf - fhBytes - siBytes, *bytesLeft);
			AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING, amigaProfileStart);
			if (mp3DecInfo->freeBitrateSlots < 0) {
				MP3ClearBadFrame(mp3DecInfo, outbuf);
				return ERR_MP3_FREE_BITRATE_SYNC;
			}
			freeFrameBytes = mp3DecInfo->freeBitrateSlots + fhBytes + siBytes;
			mp3DecInfo->bitrate = (freeFrameBytes * mp3DecInfo->samprate * 8) / (mp3DecInfo->nGrans * mp3DecInfo->nGranSamps);
		}
		mp3DecInfo->nSlots = mp3DecInfo->freeBitrateSlots + CheckPadBit(mp3DecInfo);	/* add pad byte, if required */
	}

	/* useSize != 0 means we're getting reformatted (RTP) packets (see RFC 3119)
	 *  - calling function assembles "self-contained" MP3 frames by shifting any main_data 
	 *      from the bit reservoir (in previous frames) to AFTER the sync word and side info
	 *  - calling function should set mainDataBegin to 0, and tell us exactly how large this
	 *      frame is (in bytesLeft)
	 */
	if (useSize) {
		mp3DecInfo->nSlots = *bytesLeft;
		if (mp3DecInfo->mainDataBegin != 0 || mp3DecInfo->nSlots <= 0) {
			/* error - non self-contained frame, or missing frame (size <= 0), could do loss concealment here */
			MP3ClearBadFrame(mp3DecInfo, outbuf);
			return ERR_MP3_INVALID_FRAMEHEADER;
		}

		/* can operate in-place on reformatted frames */
		mp3DecInfo->mainDataBytes = mp3DecInfo->nSlots;
		mainPtr = *inbuf;
		*inbuf += mp3DecInfo->nSlots;
		*bytesLeft -= (mp3DecInfo->nSlots);
	} else {
		/* out of data - assume last or truncated frame */
		if (mp3DecInfo->nSlots > *bytesLeft) {
			MP3ClearBadFrame(mp3DecInfo, outbuf);
			return ERR_MP3_INDATA_UNDERFLOW;	
		}

#ifdef PROFILE
	time = systime_get();
#endif
		/* fill main data buffer with enough new data for this frame */
		AMIGA_PROFILE_START(amigaProfileStart);
		if (mp3DecInfo->mainDataBytes >= mp3DecInfo->mainDataBegin) {
			/* adequate "old" main data available (i.e. bit reservoir) */
			memmove(mp3DecInfo->mainBuf, mp3DecInfo->mainBuf + mp3DecInfo->mainDataBytes - mp3DecInfo->mainDataBegin, mp3DecInfo->mainDataBegin);
			memcpy(mp3DecInfo->mainBuf + mp3DecInfo->mainDataBegin, *inbuf, mp3DecInfo->nSlots);

			mp3DecInfo->mainDataBytes = mp3DecInfo->mainDataBegin + mp3DecInfo->nSlots;
			*inbuf += mp3DecInfo->nSlots;
			*bytesLeft -= (mp3DecInfo->nSlots);
			mainPtr = mp3DecInfo->mainBuf;
		} else {
			/* not enough data in bit reservoir from previous frames (perhaps starting in middle of file) */
			memcpy(mp3DecInfo->mainBuf + mp3DecInfo->mainDataBytes, *inbuf, mp3DecInfo->nSlots);
			mp3DecInfo->mainDataBytes += mp3DecInfo->nSlots;
			*inbuf += mp3DecInfo->nSlots;
			*bytesLeft -= (mp3DecInfo->nSlots);
			MP3ClearBadFrame(mp3DecInfo, outbuf);
			AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING, amigaProfileStart);
			return ERR_MP3_MAINDATA_UNDERFLOW;
		}
		AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING, amigaProfileStart);
#ifdef PROFILE
	time = systime_get() - time;
	printf("data buffer filling: %i ms\n", time);
#endif

	}
	bitOffset = 0;
	mainBits = mp3DecInfo->mainDataBytes * 8;
	mp3DecInfo->fastLowrateOutputSamps = 0;
	mp3DecInfo->fastLowrateDebugCount = 0;

	/* decode one complete frame */
	for (gr = 0; gr < mp3DecInfo->nGrans; gr++) {
		int msSideSkipReason = MP3MonoMSSideSkipEligible(mp3DecInfo, gr, mainBits);
		mp3DecInfo->monoMSSideSkipGranule[gr] = (msSideSkipReason == 0);
		if (msSideSkipReason == 0)
			MP3AddDecodeCoreMonoMSSideSkip(0);
		else
			MP3AddDecodeCoreMonoMSSideSkip(msSideSkipReason);
		for (ch = 0; ch < mp3DecInfo->nChans; ch++) {
			if (ch == 1 && mp3DecInfo->monoMSSideSkipGranule[gr]) {
				if (MP3SkipMainDataBits(&mainPtr, &bitOffset, &mainBits,
					mp3DecInfo->part23Length[gr][ch]) < 0) {
					MP3AddDecodeCoreMonoMSSideSkip(11);
					MP3ClearBadFrame(mp3DecInfo, outbuf);
					return ERR_MP3_INVALID_SCALEFACT;
				}
				MP3AddDecodeCoreMonoMSSideSkip(1);
				continue;
			}
			
			#ifdef PROFILE
				time = systime_get();
			#endif
			/* unpack scale factors and compute size of scale factor block */
			prevBitOffset = bitOffset;
			AMIGA_PROFILE_START(amigaProfileStart);
			offset = UnpackScaleFactors(mp3DecInfo, mainPtr, &bitOffset, mainBits, gr, ch);
			AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_BITSTREAM_FRAME_PARSING, amigaProfileStart);
			#ifdef PROFILE
				time = systime_get() - time;
				printf("UnpackScaleFactors: %i ms\n", time);
			#endif

			sfBlockBits = 8*offset - prevBitOffset + bitOffset;
			huffBlockBits = mp3DecInfo->part23Length[gr][ch] - sfBlockBits;
			mainPtr += offset;
			mainBits -= sfBlockBits;

			if (offset < 0 || huffBlockBits < 0 || mainBits < huffBlockBits) {
				MP3ClearBadFrame(mp3DecInfo, outbuf);
				return ERR_MP3_INVALID_SCALEFACT;
			}

			#ifdef PROFILE
				time = systime_get();
			#endif
			/* decode Huffman code words */
			prevBitOffset = bitOffset;
			AMIGA_PROFILE_START(amigaProfileStart);
			offset = DecodeHuffman(mp3DecInfo, mainPtr, &bitOffset, huffBlockBits, gr, ch);
			AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_HUFFMAN, amigaProfileStart);
			if (offset < 0) {
				MP3ClearBadFrame(mp3DecInfo, outbuf);
				return ERR_MP3_INVALID_HUFFCODES;
			}
			#ifdef PROFILE
				time = systime_get() - time;
				printf("Huffman: %i ms\n", time);
			#endif

			mainPtr += offset;
			mainBits -= (8*offset - prevBitOffset + bitOffset);
		}
		
		#ifdef PROFILE
			time = systime_get();
		#endif
		/* dequantize coefficients, decode stereo, reorder short blocks */
		if (Dequantize(mp3DecInfo, gr) < 0) {
			MP3ClearBadFrame(mp3DecInfo, outbuf);
			return ERR_MP3_INVALID_DEQUANTIZE;			
		}
		#ifdef PROFILE
			time = systime_get() - time;
			printf("Dequantize: %i ms\n", time);
		#endif

		/* alias reduction, inverse MDCT, overlap-add, frequency inversion */
		{
			int synthChans;

			synthChans = (mp3DecInfo->outputMono && mp3DecInfo->nChans > 1) ? 1 : mp3DecInfo->nChans;
			if (synthChans == 1 && mp3DecInfo->nChans > 1 &&
				mp3DecInfo->monoMSSideSkipGranule[gr])
				MP3AddDecodeCoreMonoMSSideSkip(3);
			for (ch = 0; ch < synthChans; ch++)
			{
		#ifdef PROFILE
			time = systime_get();
		#endif
			AMIGA_PROFILE_START(amigaProfileStart);
			if (IMDCT(mp3DecInfo, gr, ch) < 0) {
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_IMDCT, amigaProfileStart);
				MP3ClearBadFrame(mp3DecInfo, outbuf);
				return ERR_MP3_INVALID_IMDCT;			
			}
			AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_IMDCT, amigaProfileStart);
		#ifdef PROFILE
			time = systime_get() - time;
			printf("IMDCT: %i ms\n", time);
		#endif
			}
		}
		
		#ifdef PROFILE
			time = systime_get();
		#endif
		/* subband transform - if stereo, interleaves pcm LRLRLR */
		{
			int dstStart;
			int dstEnd;
			int phaseStart;

			phaseStart = mp3DecInfo->fastLowratePhase;

			if (mp3DecInfo->fastLowrateStride > 1)
				dstStart = mp3DecInfo->fastLowrateOutputSamps;
			else
				dstStart = gr * mp3DecInfo->nGranSamps *
					((mp3DecInfo->outputMono && mp3DecInfo->nChans > 1) ? 1 : mp3DecInfo->nChans);

			if (Subband(mp3DecInfo, outbuf + dstStart) < 0) {
				MP3ClearBadFrame(mp3DecInfo, outbuf);
				return ERR_MP3_INVALID_SUBBAND;
			}
			if (mp3DecInfo->monoMSSideSkipGranule[gr])
				MP3AddDecodeCoreMonoMSSideSkip(4);

			if (mp3DecInfo->fastLowrateStride > 1) {
				int expectedLowrateSamps;

				dstEnd = mp3DecInfo->fastLowrateOutputSamps;
				expectedLowrateSamps = FastLowrateExpectedGranuleSamps(mp3DecInfo);
				if (expectedLowrateSamps > 0 &&
					dstEnd - dstStart > expectedLowrateSamps) {
					dstEnd = dstStart + expectedLowrateSamps;
					mp3DecInfo->fastLowrateOutputSamps = dstEnd;
				}
			}

			if (mp3DecInfo->fastLowrateStride > 1 &&
				mp3DecInfo->fastLowrateDebugCount < MAX_NGRAN) {
				MP3FastLowrateGranuleDebug *dbg =
					&mp3DecInfo->fastLowrateDebug[mp3DecInfo->fastLowrateDebugCount++];
				dstEnd = mp3DecInfo->fastLowrateOutputSamps;
				dbg->granule = gr;
				dbg->stride = mp3DecInfo->fastLowrateStride;
				dbg->phaseStart = phaseStart;
				dbg->phaseEnd = mp3DecInfo->fastLowratePhase;
				dbg->fullRateSamps = mp3DecInfo->nGranSamps *
					((mp3DecInfo->outputMono && mp3DecInfo->nChans > 1) ? 1 : mp3DecInfo->nChans);
				dbg->lowrateSamps = dstEnd - dstStart;
				dbg->cumulativeLowrateSamps = dstEnd;
				dbg->destOffsetStart = dstStart;
				dbg->destOffsetEnd = dstEnd;
			}
		}
		#ifdef PROFILE
			time = systime_get() - time;
			printf("Subband: %i ms\n", time);
		#endif
		
	}
	return ERR_MP3_NONE;
}
