Warning: truncated output (original token count: 106263)
Total output lines: 12545

/* Minimal AmigaOS/m68k-friendly command-line MP3 decoder.
 *
 * Builds the public decoder (mp3dec.c, mp3tabs.c) plus the portable real C files and writes raw
 * PCM or Amiga IFF-8SVX audio.  The code intentionally uses plain C library
 * calls only so it can be compiled by m68k-amigaos-gcc for 68020 systems.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniamp_memguard.h"
#include "radio_stream.h"
#include <time.h>
#include <stdarg.h>
#ifndef AMIGA_M68K
#include <signal.h>
#include <unistd.h>
#endif

#if defined(AMIGA_M68K)
#include <exec/semaphores.h>
#include <proto/exec.h>
/* Storage for the cross-task stdout lock declared extern by radio_debug.h/
 * radio_stream.h (RADIO_DBG/RADIO_STOP_DEBUG_PRINTF) and by this file's own
 * MiniAmp3Printf-family wrappers below. minimp3r.c's real main() -- not
 * amiga_mp3dec.c's own main(), which becomes HelixAmp3CliMain, the
 * per-playback-child entry point, when included into minimp3r.c -- calls
 * InitSemaphore() on this exactly once, before any worker task or playback
 * child that could race on it is ever spawned. */
struct SignalSemaphore radio_console_lock;
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

#define MINTAMP_CLI_VERSION "1.3.0"
#if defined(AMIGA_M68K) && !defined(MINTAMP_EMBEDDED_FRONTEND)
/* Version metadata for the standalone amiga_mp3dec.fastexp edition.  GUI
 * translation units define MINTAMP_EMBEDDED_FRONTEND before including this
 * source so their binaries expose only the frontend-specific Version tag. */
static const char gMintAmpCliVersionTag[] __attribute__((used)) =
	"\0$VER: amiga_mp3dec.fastexp " MINTAMP_CLI_VERSION " (05.09.2026)";
#endif

/*
 * AmigaOS priority for the playback child process (minimp3r.c's radio
 * player and amiga_mp3gui.c's file player both create theirs with this).
 * 0 (default/Workbench priority) keeps CPU-bound decoding from starving
 * the GUI's own event loop -- otherwise Stop becomes hard to press during
 * heavy decode, the same tradeoff that makes Songplayer feel like it
 * nearly locks up the GUI while it plays.
 *
 * Tried bumping this to 1 to let playback preempt the GUI process on
 * contention; real-hardware testing showed the app doing "weird things"
 * with the bump active, so this is back to 0 (the original, tested
 * behavior) rather than trading a hiccup-reduction guess for a worse,
 * less predictable failure mode. Revisit only with a specific, reproduced
 * symptom in hand, not as a blind A/B.
 */
#ifndef AMIGA_PLAYBACK_TASK_PRIORITY
#define AMIGA_PLAYBACK_TASK_PRIORITY 0
#endif

volatile int gMiniAmp3EmbeddedPlayback;
static int gMiniAmp3DebugPlayRequested;

#if defined(AMIGA_M68K)
/* Shared GUI/decoder stop latch. */
static volatile int gPlaybackInterrupted;
#else
static volatile sig_atomic_t gPlaybackInterrupted;
#endif

static int MiniAmp3ConsoleSuppressed(void)
{
	return gMiniAmp3EmbeddedPlayback != 0;
}

#if defined(AMIGA_M68K)
/* Every printf/fprintf/fputs/puts/putchar/fwrite call in this file (and,
 * since minimp3r.c #includes it as one translation unit, in minimp3r.c's own
 * GUI code too) funnels through these Mini* wrappers via the #define printf
 * MiniAmp3Printf-style redirect below. The net worker task, every playback
 * child, and the GUI task each call these concurrently against the same
 * shared stdout -- confirmed directly in a captured field log, where two
 * tasks' printf() output was physically spliced together mid-line. Lock
 * around the actual I/O call in each wrapper (radio_console_lock, shared
 * with radio_stream.c's RADIO_DBG macros via radio_debug.h/radio_stream.h,
 * initialized once at true program startup in minimp3r.c's real main()) so
 * concurrent debug/status output can no longer corrupt the C library's
 * shared stdio buffer state. radio_console_lock itself is defined near the
 * top of this file. */
#define MINIAMP3_IO_LOCK() ObtainSemaphore(&radio_console_lock)
#define MINIAMP3_IO_UNLOCK() ReleaseSemaphore(&radio_console_lock)
#else
#define MINIAMP3_IO_LOCK() ((void)0)
#define MINIAMP3_IO_UNLOCK() ((void)0)
#endif

static int MiniAmp3Printf(const char *fmt, ...)
{
	int r;
	va_list ap;
	if (MiniAmp3ConsoleSuppressed())
		return 0;
	MINIAMP3_IO_LOCK();
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Fprintf(FILE *stream, const char *fmt, ...)
{
	int r;
	va_list ap;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return 0;
	MINIAMP3_IO_LOCK();
	va_start(ap, fmt);
	r = vfprintf(stream, fmt, ap);
	va_end(ap);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Fputs(const char *s, FILE *stream)
{
	int r;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return 0;
	MINIAMP3_IO_LOCK();
	r = fputs(s, stream);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Puts(const char *s)
{
	int r;
	if (MiniAmp3ConsoleSuppressed())
		return 0;
	MINIAMP3_IO_LOCK();
	r = puts(s);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Putchar(int c)
{
	int r;
	if (MiniAmp3ConsoleSuppressed())
		return c;
	MINIAMP3_IO_LOCK();
	r = putchar(c);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Fflush(FILE *stream)
{
	int r;
	if (MiniAmp3ConsoleSuppressed() && (!stream || stream == stdout || stream == stderr))
		return 0;
	MINIAMP3_IO_LOCK();
	r = fflush(stream);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static size_t MiniAmp3Fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t r;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return nmemb;
	MINIAMP3_IO_LOCK();
	r = fwrite(ptr, size, nmemb, stream);
	MINIAMP3_IO_UNLOCK();
	return r;
}

#ifdef RADIO_DEBUG
/* Bypasses MiniAmp3ConsoleSuppressed() entirely -- defined before the
 * printf -> MiniAmp3Printf redirect below, so its own vprintf() call is the
 * real C library one, not the suppressible wrapper.  Every other printf()
 * in this file goes silent for the whole radio playback child (Embedded
 * Playback sets gMiniAmp3EmbeddedPlayback for its entire run), which is why
 * PrintPlaybackCleanupStatus()'s playback buffer canary result -- the one
 * diagnostic that reports on AllocMem-based buffers the malloc-based
 * MiniMem guard cannot see -- never showed up in any radio session log. */
static void RadioDebugUnsuppressedPrintf(const char *fmt, ...)
{
	va_list ap;
	MINIAMP3_IO_LOCK();
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	MINIAMP3_IO_UNLOCK();
}
#endif

#ifdef printf
#undef printf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef fputs
#undef fputs
#endif
#ifdef puts
#undef puts
#endif
#ifdef putchar
#undef putchar
#endif
#ifdef fflush
#undef fflush
#endif
#define printf MiniAmp3Printf
#define fprintf MiniAmp3Fprintf
#define fputs MiniAmp3Fputs
#define puts MiniAmp3Puts
#define putchar MiniAmp3Putchar
#define fflush MiniAmp3Fflush
#define fwrite MiniAmp3Fwrite
#ifdef RADIO_DEBUG
#define RADIO_INPUT_DIAG_PRINTF RadioDebugUnsuppressedPrintf
#else
#define RADIO_INPUT_DIAG_PRINTF MiniAmp3Printf
#endif

#if defined(AMIGA_M68K)
/* Tell AmigaOS to provide at least 256 KB of stack for this executable. */
static const char amigaStackCookie[] __attribute__((used, aligned(4))) = "$STACK:262144";
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
int STATNAME(AntiAliasSubbandCapSelftest)(void);
int STATNAME(IMDCT36AsmGeneralPathSelftest)(void);
int STATNAME(DequantSubbandCapSelftest)(void);
int STATNAME(CollapseStereoToMonoSelftest)(void);
int STATNAME(MidSideProcSubbandCapSelftest)(void);
int STATNAME(CollapseStereoToMonoSubbandCapSelftest)(void);
int STATNAME(IntensityProcSubbandCapSelftest)(void);
int STATNAME(FDCT32HalfSparse16Selftest)(void);
void STATNAME(PolyphaseMonoFast_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
void STATNAME(PolyphaseMonoFast_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
void STATNAME(PolyphaseStereo)(short *pcm, int *vbuf, const int *coefBase);
void MP3EnableFdct32HalfSparse16PreconditionCheck(int enabled);
extern volatile unsigned long gFdct32HalfSparse16PreconditionViolations;
extern volatile unsigned long gFdct32HalfSparse16PreconditionChecks;
int PolyphaseMonoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase);
int PolyphaseStereoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase);
int STATNAME(MonoFastPolyphaseStride3_Amiga_m68k_IsActive)(void);
int STATNAME(StereoFastPolyphaseStride3_Amiga_m68k_IsActive)(void);
int STATNAME(MonoFastPolyphaseStride5_Amiga_m68k_IsActive)(void);
int STATNAME(MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive)(void);
#define AMIGA_POLYPHASE_STEREO_FULL STATNAME(PolyphaseStereo)
#define AMIGA_POLYPHASE_MONO_STRIDE3_HAS_ASM STATNAME(MonoFastPolyphaseStride3_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_STEREO_STRIDE3_HAS_ASM STATNAME(StereoFastPolyphaseStride3_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_MONO_STRIDE5_HAS_ASM STATNAME(MonoFastPolyphaseStride5_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_MONO_STRIDE4_ALLPHASES_HAS_ASM STATNAME(MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive)
int STATNAME(PolyphaseMonoFastLowrateStride2_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseMonoFastLowrateStride4_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride4_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseStereoFastLowrateStride2_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
void MP3GetStereoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride2PolyphaseCounters(void);
void MP3GetStereoStride4PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride4PolyphaseCounters(void);
void MP3GetMonoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetMonoStride2PolyphaseCounters(void);
int STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride5_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride5_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(StereoFastPolyphaseStride5_Amiga_m68k_IsActive)(void);
int STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_POLYPHASE)
extern void StereoFastPolyphaseStride4Half_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase) __asm__("StereoFastPolyphaseStride4Half_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4HalfReduced_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase) __asm__("StereoFastPolyphaseStride4HalfReduced_Amiga_m68k")
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
int STATNAME(AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive)(void);
int STATNAME(PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
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
#define AMIGA_ANTIALIAS_SUBBAND_CAP_SELFTEST STATNAME(AntiAliasSubbandCapSelftest)
#define AMIGA_IMDCT36_ASM_GENERAL_PATH_SELFTEST STATNAME(IMDCT36AsmGeneralPathSelftest)
#define AMIGA_DEQUANT_SUBBAND_CAP_SELFTEST STATNAME(DequantSubbandCapSelftest)
#define AMIGA_COLLAPSE_STEREO_TO_MONO_SELFTEST STATNAME(CollapseStereoToMonoSelftest)
#define AMIGA_MIDSIDE_SUBBAND_CAP_SELFTEST STATNAME(MidSideProcSubbandCapSelftest)
#define AMIGA_COLLAPSE_STEREO_TO_MONO_SUBBAND_CAP_SELFTEST STATNAME(CollapseStereoToMonoSubbandCapSelftest)
#define AMIGA_INTENSITY_SUBBAND_CAP_SELFTEST STATNAME(IntensityProcSubbandCapSelftest)
#define AMIGA_FDCT32_HALF_SPARSE16_SELFTEST STATNAME(FDCT32HalfSparse16Selftest)
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
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_HAS_ASM STATNAME(PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
int STATNAME(PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_HAS_ASM STATNAME(PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_HAS_ASM STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_HAS_ASM STATNAME(StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride5_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride5_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_IS_ACTIVE STATNAME(StereoFastPolyphaseStride5_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_REDUCED_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_HAS_ASM STATNAME(PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_M68K_POLYPHASE_MONO_FAST_IS_ACTIVE STATNAME(AmigaM68KPolyphaseMonoFast_IsActive)
#define AMIGA_M68K_POLYPHASE_MONO_FAST_STRIDE2_IS_ACTIVE STATNAME(AmigaM68KPolyphaseMonoFastStride2_IsActive)
#define AMIGA_HUFFMAN_PAIRS_C_REFERENCE STATNAME(DecodeHuffmanPairs_C_REFERENCE)
#define AMIGA_HUFFMAN_PAIRS_TEST_ACTIVE STATNAME(DecodeHuffmanPairs_TEST_ACTIVE)
#define AMIGA_HUFFMAN_PAIRS_HAS_ASM STATNAME(DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_HUFFMAN_PAIRS_ASM_NOTE STATNAME(DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE)
#define AMIGA_DEQUANT_BLOCK_C_REFERENCE STATNAME(DequantBlock_C_REFERENCE)
#define AMIGA_DEQUANT_BLOCK_TEST_ACTIVE STATNAME(DequantBlock_TEST_ACTIVE)
#define AMIGA_DEQUANT_BLOCK_HAS_ASM STATNAME(DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_INTENSITY_SCALE_RUN_C_REFERENCE STATNAME(IntensityScaleRun_C_REFERENCE)
#define AMIGA_INTENSITY_SCALE_RUN1_TEST_ACTIVE STATNAME(IntensityScaleRun1_TEST_ACTIVE)
#define AMIGA_INTENSITY_SCALE_RUN3_TEST_ACTIVE STATNAME(IntensityScaleRun3_TEST_ACTIVE)
#define AMIGA_INTENSITY_SCALE_RUN_HAS_ASM STATNAME(IntensityScaleRun_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_BITSTREAM_REFILL_SELFTEST STATNAME(BitstreamRefillSelftest)
#define AMIGA_POLY_COEF STATNAME(polyCoef)

#define READBUF_SIZE (1024 * 16)
#define OUTBUF_SAMPS (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#include "decoders/decoder_module.h"
#endif

/* Fake-stereo (pseudo-stereo) widener parameters; see FakeStereo below. */
#define FAKE_STEREO_MAX_DELAY 256
#define FAKE_STEREO_DELAY_MASK (FAKE_STEREO_MAX_DELAY - 1)
#define FAKE_STEREO_DEFAULT_DELAY 96
#define FAKE_STEREO_DEFAULT_SHIFT 2
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
	int selftestAntialiasSubbandCap;
	int selftestImdct36AsmGeneralPath;
	int selftestDequantSubbandCap;
	int selftestCollapseStereoToMono;
	int selftestMidSideSubbandCap;
	int selftestCollapseStereoToMonoSubbandCap;
	int selftestIntensitySubbandCap;
	int selftestFdct32HalfSparse16;
	int selftestAntialias;
	int selftestPolyphase;
	int selftestPolyphaseStride2;
	int selftestPolyphaseStride2Reduced;
	int selftestPolyphaseStride4;
	int selftestPolyphaseStride4Stereo;
	int selftestPolyphaseStride4StereoReduced;
	int selftestPlanarS8TrueStereo;
	int selftestPolyphaseStride2Stereo;
	int selftestPolyphaseStride2StereoReduced;
	int selftestPolyphaseStride5Stereo;
	int selftestPolyphaseStride3;
	int selftestPolyphaseStride3Stereo;
	int selftestPolyphaseStride5;
	int selftestPolyphaseStride4AllPhases;
	int forceCPolyphaseStride2Stereo;
	int selftestFastLowrate;
	int selftestReducedTaps;
	int selftestFdct32Quarter;
	int selftestFdct32QuarterStereo;
	int selftestHuffman;
	int selftestDequant;
	int selftestIntensity;
	int selftestBitstream;
	int selftestMonoFastLowrateStereo;
	int selftestQuality;
	int selftestFakeStereo;
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
	int subbandCap;
	int expFdct32Quarter;
	int help;
	int debugArgv;
	int debugFastLowrate;
	int debugSubbandPrecondition;
	int debugPlay;
	int debugTone;
	int debugCleanup;
	int debugDecoder;
	int testAac;
	int play;
	int stereo;
	int fakeStereo;
	int fakeStereoDelay;
	int fakeStereoShift;
	int decodeThenPlay;
	int playLifecycleTest;
	int audioOpenSilentTest;
	int startupVolumeSelftest;
	int bufferSeconds;
	int volumePercent;
	int fastMem;
	int info;
	int noMonoMSSideSkip;
	int radioStream;
	int haveRadioHostAddr;
	const char *radioCodecHint;
	unsigned long radioHostAddrBe;
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
	unsigned char prefix[4096];
	unsigned long prefixSize;
	unsigned long prefixPos;
	Mp3InputInfo info;
	RadioStream *radio;
} InputSource;

static void InputSourceInit(InputSource *input, FILE *file);
#ifdef HAVE_AMIGA_AUDIO_DEVICE
static void InputSourceInitAmigaDos(InputSource *input, BPTR amigaFile);
#endif
static int InputSourcePrepareMp3(InputSource *input);
static void GuiPublishRadioMetadata(RadioStream *radio);
static void GuiMarkRadioStopped(void);

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
	printf("  --stereo     opt-in stereo output for --play or --decode-only benchmarking\n");
	printf("               stereo rates: 8820, 11025, 14700, 22050, or PAL-top 28600 Hz\n");
	printf("               mono rates: 8287 default, 8820, 11025, 14700, 22050, or PAL-top 28600 Hz\n");
	printf("  --fake-stereo  --play pseudo-stereo width from the mono decode (mono CPU cost)\n");
	printf("               energy-symmetric cross-delay; mutually exclusive with --stereo\n");
	printf("  --fake-stereo-delay N  fake-stereo delay in samples (1-%d, default %d)\n",
		FAKE_STEREO_MAX_DELAY, FAKE_STEREO_DEFAULT_DELAY);
	printf("  --fake-stereo-shift K  fake-stereo cross-bleed >>K (0-8, default %d; higher=wider, 0=mono)\n",
		FAKE_STEREO_DEFAULT_SHIFT);
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
	printf("  --rate HZ    output/downsample rate: 28600, 22050, 14700, 11025, 8820, or 8287 Hz\n");
	printf("               28600/22050 playback is experimental/high CPU and may underrun\n");
	printf("  --fast-lowrate lower-quality Amiga conversion; requires --rate\n");
	printf("  --superfast-lowrate sparse low-rate mode; use --rate 8287, 8820, 11025, 14700, or 22050\n");
	printf("                 defaults to 11025 if no --rate is specified\n");
	printf("  --ultrafast  cap IMDCT to 26 subbands (~18 kHz) at full 44.1 kHz rate;\n");
	printf("                 saves ~18%% IMDCT work with negligible audible impact\n");
	printf("  --subband-cap N limit IMDCT to N active subbands 1-32 (use after --rate/--fast-lowrate)\n");
	printf("  --quality N set quality/speed level (0 fastest, 1 fast, 2 balanced, 3 accurate)\n");
	printf("               default: 1 for --fast-lowrate --rate 11025, 14700, or 22050, otherwise 3\n");
	printf("               0 enables Superfast FDCT32 quarter + Huffman asm; 1 adds reduced taps; 3 is original behavior\n");
	printf("               individual --exp-* flags may still be enabled independently\n");
	printf("  --exp-poly  use experimental 68030 asm mono polyphase when compiled in\n");
	printf("  --exp-huff  use experimental 68030 inline-asm Huffman pair refill when compiled in\n");
	printf("  --exp-imdct-thin request experimental fast-lowrate IMDCT output thinning\n");
	printf("  --exp-reduced-taps use experimental reduced-tap fast-lowrate dewindow\n");
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
	printf("  --selftest-antialias-subband-cap verify capping antialias butterflies to the subband cap leaves kept bands bit-exact\n");
	printf("  --selftest-imdct36-asm-general-path verify m68k asm IMDCT36 matches C reference for transition/mixed-window (btCurr/btPrev != 0) blocks, not just the fast long-window path\n");
	printf("  --selftest-dequant-subband-cap verify capping dequant long-block work to the subband cap leaves kept samples, cbEndL, and guard-bit count bit-exact\n");
	printf("  --selftest-collapse-stereo-mono verify the stereo-to-mono collapse (plain LR stereo source, mono output) asm matches the C reference bit-exact\n");
	printf("  --selftest-midside-subband-cap verify capping MidSideProc's scan to the fast-lowrate subband cap is safe and equivalent to a zeroed discarded region\n");
	printf("  --selftest-collapse-stereo-mono-subband-cap verify capping stereo-to-mono collapse to the dequant subband cap matches an uncapped collapse with the discarded region zeroed, including gb[0] and nonZeroBound[0]\n");
	printf("  --selftest-intensity-subband-cap verify capping IntensityProcMPEG1/2's scan to the fast-lowrate subband cap matches an uncapped call with the discarded region zeroed, for MPEG1/MPEG2 and long/short/mixed blocks\n");
	printf("  --selftest-fdct32half-sparse16 verify prototype sparse-input FDCT32Half (NOT wired into playback) matches the reference when subbands 16-31 are zero\n");
	printf("  --selftest-antialias compare C reference and optional m68k asm antialias path\n");
	printf("  --selftest-polyphase compare C fast mono polyphase and optional m68k asm path\n");
	printf("  --selftest-polyphase-stride2 compare C and optional asm stride-2 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride2-reduced compare C and optional asm reduced stride-2 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride4 compare C and optional asm stride-4 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride4-stereo compare stereo stride-4 compact polyphase output\n");
	printf("  --selftest-polyphase-stride4-stereo-reduced compare C and optional asm reduced stride-4 stereo polyphase paths\n");
	printf("  --selftest-planar-s8-stereo compare C and optional asm true-stereo interleaved s16 -> planar s8 conversion\n");
	printf("  --selftest-polyphase-stride2-stereo compare stereo stride-2 compact polyphase output\n");
	printf("  --selftest-polyphase-stride2-stereo-reduced compare stereo reduced stride-2 compact polyphase output\n");
	printf("  --selftest-polyphase-stride5-stereo compare stereo stride-5 compact polyphase output\n");
	printf("  --selftest-polyphase-stride3 verify stride-3 (14700 Hz) mono hand-unrolled polyphase output against full-band synthesis\n");
	printf("  --selftest-polyphase-stride3-stereo verify stride-3 (14700 Hz) stereo hand-unrolled polyphase output against full-band synthesis\n");
	printf("  --selftest-polyphase-stride5 verify stride-5 (8820/8287 Hz) mono polyphase output against full-band synthesis\n");
	printf("  --selftest-polyphase-stride4-allphases verify stride-4 (11025 Hz) mono polyphase output at all 4 phases against full-band synthesis\n");
	printf("  --force-c-polyphase-stride2-stereo benchmark stereo stride-2 C fallback in this binary\n");
	printf("  --selftest-fastlowrate compare synthetic stride decimation paths\n");
	printf("  --selftest-reduced-taps compare full and reduced stride-4 dewindow paths\n");
	printf("  --selftest-fdct32-quarter inspect lossy stride-4 quarter-rate FDCT32 scatter\n");
	printf("  --selftest-fdct32-quarter-stereo verify independent stereo stride-4 quarter FDCT32 dispatch\n");
	printf("  --selftest-huffman compare C and active Huffman pair decode paths\n");
	printf("  --selftest-dequant compare C and optional m68k asm dequant block paths\n");
	printf("  --selftest-intensity compare C and optional m68k asm intensity scale paths\n");
	printf("  --selftest-bitstream compare C and optional m68k move.l bitstream refill paths\n");
	printf("  --selftest-mono-fastlowrate-stereo verify stereo-to-mono low-rate accounting\n");
	printf("  --selftest-quality verify --quality flag mapping and auto-default selection\n");
	printf("  --selftest-fake-stereo verify pseudo-stereo mono-compatibility and delay line\n");
	printf("  --checksum  print a 32-bit checksum of decoded PCM samples\n");
	printf("  --no-ms-mono-skip force full two-channel M/S decode before mono regression checks\n");
	printf("  --debug-fastlowrate print per-frame/granule fast-lowrate placement\n");
	printf("  --debug-subband-precondition check FDCT32HalfSparse16's buf[16..31]==0 precondition on every real 22050 Hz block and print violation counts at exit\n");
	printf("  --debug-play print audio.device playback startup diagnostics\n");
	printf("  --debug-tone submit a generated signed-8 Paula test tone through --play audio path\n");
	printf("  --debug-cleanup print playback resource cleanup diagnostics\n");
	printf("  --debug-decoder print generic decoder module/rate diagnostics\n");
	printf("  --test-aac FILE smoke-test ADTS AAC module loading and one-frame decode\n");
	printf("  --radio-codec-hint CODEC  Radio Browser codec hint (MP3, AAC, AAC+)\n");
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
		(opt->fastLowrate && (opt->outputRate == 11025 || opt->outputRate == 14700 ||
			opt->outputRate == 22050) ? 1 : 3);
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
	opt->fakeStereoDelay = FAKE_STEREO_DEFAULT_DELAY;
	opt->fakeStereoShift = FAKE_STEREO_DEFAULT_SHIFT;
#if defined(DEBUG_DECODER) && DEBUG_DECODER
	opt->debugDecoder = 1;
#endif

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
		} else if (!strcmp(argv[i], "--fake-stereo")) {
			opt->fakeStereo = 1;
		} else if (!strcmp(argv[i], "--fake-stereo-delay")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "--fake-stereo-delay requires a value (1-%d)\n",
					FAKE_STEREO_MAX_DELAY);
				return -1;
			}
			i++;
			opt->fakeStereoDelay = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--fake-stereo-shift")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "--fake-stereo-shift requires a value (0-8)\n");
				return -1;
			}
			i++;
			opt->fakeStereoShift = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--play-fast-path")) {
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--radio-stream")) {
#if ENABLE_RADIO
			opt->radioStream = 1;
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
#else
			fprintf(stderr, "--radio-stream requested, but radio support not built; rebuild with RADIO=1 or HAVE_BSDSOCKET=1\n");
			return -1;
#endif
		} else if (!strcmp(argv[i], "--radio-host-addr-be")) {
			if (++i >= argc)
				return -1;
			opt->radioHostAddrBe = strtoul(argv[i], NULL, 0);
			opt->haveRadioHostAddr = 1;
		} else if (!strcmp(argv[i], "--radio-codec-hint")) {
			if (++i >= argc)
				return -1;
			opt->radioCodecHint = argv[i];
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
		} else if (!strcmp(argv[i], "--selftest-antialias-subband-cap")) {
			opt->selftestAntialiasSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct36-asm-general-path")) {
			opt->selftestImdct36AsmGeneralPath = 1;
		} else if (!strcmp(argv[i], "--selftest-dequant-subband-cap")) {
			opt->selftestDequantSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-collapse-stereo-mono")) {
			opt->selftestCollapseStereoToMono = 1;
		} else if (!strcmp(argv[i], "--selftest-midside-subband-cap")) {
			opt->selftestMidSideSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-collapse-stereo-mono-subband-cap")) {
			opt->selftestCollapseStereoToMonoSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-intensity-subband-cap")) {
			opt->selftestIntensitySubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32half-sparse16")) {
			opt->selftestFdct32HalfSparse16 = 1;
		} else if (!strcmp(argv[i], "--selftest-antialias")) {
			opt->selftestAntialias = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase")) {
			opt->selftestPolyphase = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2")) {
			opt->selftestPolyphaseStride2 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-reduced")) {
			opt->selftestPolyphaseStride2Reduced = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4")) {
			opt->selftestPolyphaseStride4 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-stereo")) {
			opt->selftestPolyphaseStride4Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-stereo-reduced")) {
			opt->selftestPolyphaseStride4StereoReduced = 1;
		} else if (!strcmp(argv[i], "--selftest-planar-s8-stereo")) {
			opt->selftestPlanarS8TrueStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-stereo")) {
			opt->selftestPolyphaseStride2Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-stereo-reduced")) {
			opt->selftestPolyphaseStride2StereoReduced = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride5-stereo")) {
			opt->selftestPolyphaseStride5Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride5")) {
			opt->selftestPolyphaseStride5 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-allphases")) {
			opt->selftestPolyphaseStride4AllPhases = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride3")) {
			opt->selftestPolyphaseStride3 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride3-stereo")) {
			opt->selftestPolyphaseStride3Stereo = 1;
		} else if (!strcmp(argv[i], "--force-c-polyphase-stride2-stereo")) {
			opt->forceCPolyphaseStride2Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-fastlowrate")) {
			opt->selftestFastLowrate = 1;
		} else if (!strcmp(argv[i], "--selftest-reduced-taps")) {
			opt->selftestReducedTaps = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32-quarter")) {
			opt->selftestFdct32Quarter = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32-quarter-stereo")) {
			opt->selftestFdct32QuarterStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-huffman")) {
			opt->selftestHuffman = 1;
		} else if (!strcmp(argv[i], "--selftest-dequant")) {
			opt->selftestDequant = 1;
		} else if (!strcmp(argv[i], "--selftest-intensity") ||
			!strcmp(argv[i], "--Selftest-Intensity")) {
			opt->selftestIntensity = 1;
		} else if (!strcmp(argv[i], "--selftest-bitstream")) {
			opt->selftestBitstream = 1;
		} else if (!strcmp(argv[i], "--selftest-mono-fastlowrate-stereo")) {
			opt->selftestMonoFastLowrateStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-quality")) {
			opt->selftestQuality = 1;
		} else if (!strcmp(argv[i], "--selftest-fake-stereo")) {
			opt->selftestFakeStereo = 1;
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
		} else if (!strcmp(argv[i], "--ultrafast")) {
			opt->subbandCap = 26;
		} else if (!strcmp(argv[i], "--subband-cap")) {
			if (++i >= argc)
				return -1;
			opt->subbandCap = atoi(argv[i]);
			if (opt->subbandCap < 1 || opt->subbandCap > 32) {
				fprintf(stderr, "error: --subband-cap N must be 1-32\n");
				return -1;
			}
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
				opt->outputRate != 14700 && opt->outputRate != 11025 &&
				opt->outputRate != 8820 && opt->outputRate != 8287)
				return -1;
		} else if (!strcmp(argv[i], "--debug-fastlowrate")) {
			opt->debugFastLowrate = 1;
		} else if (!strcmp(argv[i], "--debug-subband-precondition")) {
			opt->debugSubbandPrecondition = 1;
		} else if (!strcmp(argv[i], "--debug-play")) {
			opt->debugPlay = 1;
		} else if (!strcmp(argv[i], "--debug-tone")) {
			opt->debugTone = 1;
			opt->debugPlay = 1;
			opt->play = 1;
			opt->outFormat = OUT_S8;
		} else if (!strcmp(argv[i], "--debug-cleanup")) {
			opt->debugCleanup = 1;
		} else if (!strcmp(argv[i], "--debug-decoder")) {
			opt->debugDecoder = 1;
		} else if (!strcmp(argv[i], "--test-aac")) {
			opt->testAac = 1;
			opt->debugDecoder = 1;
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
    opt->selftestAntialiasSubbandCap ||
    opt->selftestImdct36AsmGeneralPath ||
    opt->selftestDequantSubbandCap ||
    opt->selftestCollapseStereoToMono ||
    opt->selftestMidSideSubbandCap ||
    opt->selftestCollapseStereoToMonoSubbandCap ||
    opt->selftestIntensitySubbandCap ||
    opt->selftestFdct32HalfSparse16 ||
    opt->selftestAntialias ||
    opt->selftestPolyphase ||
    opt->selftestPolyphaseStride2 ||
    opt->selftestPolyphaseStride2Reduced ||
    opt->selftestPolyphaseStride4 ||
    opt->selftestPolyphaseStride4Stereo ||
    opt->selftestPolyphaseStride4StereoReduced ||
    opt->selftestPlanarS8TrueStereo ||
    opt->selftestPolyphaseStride2Stereo ||
    opt->selftestPolyphaseStride2StereoReduced ||
    opt->selftestPolyphaseStride5Stereo ||
    opt->selftestPolyphaseStride3 ||
    opt->selftestPolyphaseStride3Stereo ||
    opt->selftestPolyphaseStride5 ||
    opt->selftestPolyphaseStride4AllPhases ||
    opt->selftestFastLowrate ||
    opt->selftestReducedTaps ||
    opt->selftestFdct32Quarter ||
    opt->selftestFdct32QuarterStereo ||
    opt->selftestHuffman ||
    opt->selftestDequant ||
    opt->selftestBitstream ||
    opt->selftestMonoFastLowrateStereo ||
    opt->selftestQuality ||
    opt->selftestFakeStereo)
		return 0;

	if (opt->stereo && !opt->play && !opt->decodeOnly) {
		fprintf(stderr, "--stereo is only supported with --play or --decode-only\n");
		return -1;
	}
	if (opt->fakeStereo && !opt->play) {
		fprintf(stderr, "--fake-stereo is only supported with --play\n");
		return -1;
	}
	if (opt->fakeStereo && opt->stereo) {
		fprintf(stderr, "--fake-stereo and --stereo are mutually exclusive\n");
		return -1;
	}
	if (opt->fakeStereo &&
		(opt->fakeStereoDelay < 1 || opt->fakeStereoDelay > FAKE_STEREO_MAX_DELAY)) {
		fprintf(stderr, "--fake-stereo-delay must be 1-%d samples\n",
			FAKE_STEREO_MAX_DELAY);
		return -1;
	}
	if (opt->fakeStereo &&
		(opt->fakeStereoShift < 0 || opt->fakeStereoShift > 8)) {
		fprintf(stderr, "--fake-stereo-shift must be 0-8\n");
		return -1;
	}

	if (opt->superfastLowrate && !opt->outputRate)
		opt->outputRate = 11025;

	if (opt->play && !opt->outputRate)
		opt->outputRate = opt->stereo ? 8820 : 8287;

	if (opt->play && opt->outputRate != 8287 && opt->outputRate != 8820 &&
		opt->outputRate != 11025 && opt->outputRate != 14700 &&
		opt->outputRate != 22050 && opt->outputRate != 28600) {
		fprintf(stderr, "--play supports --rate 8287, 8820, 11025, 14700, 22050, or 28600 only\n");
		return -1;
	}
	if (opt->play && opt->stereo && opt->outputRate == 8287) {
		fprintf(stderr, "--stereo playback supports --rate 8820, 11025, 14700, 22050, or PAL-top 28600 only\n");
		return -1;
	}
	if (opt->play) {
		opt->mono = opt->stereo ? 0 : 1;
		opt->outFormat = OUT_S8;
		if (opt->outputRate != 28600)
			opt->fastLowrate = 1;
		opt->noOutput = 1;
	}

	if (opt->superfastLowrate && opt->outputRate != 11025 && opt->outputRate != 14700 &&
		opt->outputRate != 22050 && opt->outputRate != 8820 && opt->outputRate != 8287) {
		fprintf(stderr, "--superfast-lowrate supports only --rate 8287, 8820, 11025, 14700, or 22050\n");
		return -1;
	}
	if (opt->fastLowrate && (opt->outputRate != 22050 && opt->outputRate != 14700 &&
		opt->outputRate != 11025 && opt->outputRate != 8820 &&
		opt->outputRate != 8287)) {
		fprintf(stderr, "--fast-lowrate requires --rate 22050, 14700, 11025, 8820, or 8287\n");
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
	if (!strcmp(id, "TCOP") || !strcmp(id, "TCR")) return "copyright…76263 tokens truncated…\n");
	if (!LoadDecoderModuleForExt("aac", &mod, 1)) {
		fprintf(stderr, "AAC test: no aac.decoder module found\n");
		goto done_input;
	}

	printf("AAC test: module entry\n");
	printf("AAC test: validate ops\n");
	if (!ValidateDecoderModuleOps(mod.ops, mod.path, 1))
		goto done_module;

	printf("AAC test: open/init\n");
	printf("AAC: before open\n");
	handle = mod.ops->open(DecModReadCb, DecModSeekCb, &input, &sinfo);
	printf("AAC: after open handle=%p\n", (void *)handle);
	if (!handle) {
		fprintf(stderr, "AAC test: decoder open/init failed\n");
		goto done_module;
	}
	printf("AAC test: stream %lu Hz %u ch %u-bit\n",
		sinfo.sampleRate, sinfo.channels, sinfo.bitsPerSample);

	printf("AAC test: decode one frame\n");
	pcm = (short *)AllocMem(4096UL * sizeof(short), MEMF_FAST);
	if (!pcm)
		pcm = (short *)AllocMem(4096UL * sizeof(short), MEMF_PUBLIC);
	if (!pcm) {
		fprintf(stderr, "AAC test: cannot allocate PCM smoke-test buffer\n");
		goto done_handle;
	}
	printf("AAC: before first decode\n");
	nDecoded = mod.ops->decode(handle, pcm, 1024UL);
	printf("AAC: after first decode rc=%ld\n", (long)nDecoded);
	if (nDecoded <= 0) {
		fprintf(stderr, "AAC test: decode one frame failed rc=%ld\n", (long)nDecoded);
		goto done_handle;
	}
	printf("AAC test: decoded %ld sample frames (%ld total int16 samples, %ld bytes)\n",
		(long)nDecoded, (long)nDecoded * (long)sinfo.channels,
		(long)nDecoded * (long)sinfo.channels * (long)sizeof(short));
	ret = 0;

done_handle:
	if (pcm)
		FreeMem(pcm, 4096UL * sizeof(short));
	printf("AAC test: close\n");
	mod.ops->close(handle);
done_module:
	UnloadDecoderModule(&mod);
done_input:
	InputSourceClose(&input);
	printf("AAC test: done\n");
	return ret;
}

#endif /* HAVE_AMIGA_AUDIO_DEVICE */
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
	GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
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
	/* Publish the SOURCE (decoder input) rate for the progress clock, not the
	 * downsampled Paula output rate -- see note in the generic path above.
	 * effectiveRate keeps the actual output rate. */
	gGuiPlaybackStatus.sampleRate = inputSampleRate > 0 ? inputSampleRate : playbackRate;
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
	GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
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
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, active, len[active], "mp3 startup fill");
			#endif
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
		if (refill == 0)
			RADIO_DBG(printf("radio-audio: first audio.device CMD_WRITE queued slot=%s bytes=%lu live_slots=%d stereo=%d\n",
				PlaybackBufferName(refill), len[refill], liveSlots, opt->stereo ? 1 : 0);)
	}
	GuiPublishStartupStage(GUISTART_PLAYING);
	GuiSetPlaybackPhase(GUIPLAY_PHASE_PLAYING);
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
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, justFreed, len[decodeAhead], "mp3 decode-ahead copy");
			#endif
			len[justFreed] = len[decodeAhead];
		} else {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[justFreed], playbackRate);
			if (gPlaybackInterrupted)
				break;
			len[justFreed] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				justFreed, buf[justFreed], bufBytes);
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, justFreed, len[justFreed], "mp3 refill justFreed");
			#endif
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
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, decodeAhead, len[decodeAhead], "mp3 refill decodeAhead");
			#endif
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
		/* Progress clock wants the source rate (frames*1152 are source
		 * samples); fall back to the effective output rate until the first
		 * frame reports the stream's real rate. */
		if (stream.stats && stream.stats->sampleRate)
			gGuiPlaybackStatus.sampleRate = stream.stats->sampleRate;
		else if (stream.effectiveRate)
			gGuiPlaybackStatus.sampleRate = stream.effectiveRate;
		if (underrun)
			GuiSetPlaybackPhase(GUIPLAY_PHASE_UNDERRUN);
		else if (gGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN)
			GuiSetPlaybackPhase(GUIPLAY_PHASE_PLAYING);
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
	RADIO_DBG(printf("radio-audio: AmigaAudioClose returned session=%lu\n",
		(input && input->radio) ? Radio_GetSessionId(input->radio) : 0UL));
	RADIO_DBG(printf("radio-audio: entering post-audio cleanup session=%lu\n",
		(input && input->radio) ? Radio_GetSessionId(input->radio) : 0UL));
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	if (cleanupStatus.canaryErrors)
		err = -1;
	PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	RADIO_DBG(printf("radio-audio: leaving post-audio cleanup session=%lu\n",
		(input && input->radio) ? Radio_GetSessionId(input->radio) : 0UL));
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

#if defined(AMIGA_M68K) && !defined(RADIO_CONSOLE_LOCK_INIT_ELSEWHERE)
	/* Standalone build only (fast030/amiga_mp3dec targets): this main() is
	 * the true program entry point, so it is safe to InitSemaphore() here,
	 * once, before anything else runs. When this file is #included into
	 * minimp3r.c instead, main() is renamed to HelixAmp3CliMain and becomes
	 * the per-playback-child entry point -- RADIO_CONSOLE_LOCK_INIT_ELSEWHERE
	 * is defined there so this block is skipped; minimp3r.c's own real
	 * main() does the one-time init instead, since re-running InitSemaphore()
	 * on every child spawn would race any other task already using the
	 * semaphore. */
	InitSemaphore(&radio_console_lock);
#endif
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
	if (opt.selftestAntialiasSubbandCap) {
		int selftestErr = AMIGA_ANTIALIAS_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdct36AsmGeneralPath) {
		int selftestErr = AMIGA_IMDCT36_ASM_GENERAL_PATH_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestDequantSubbandCap) {
		int selftestErr = AMIGA_DEQUANT_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestCollapseStereoToMono) {
		int selftestErr = AMIGA_COLLAPSE_STEREO_TO_MONO_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestMidSideSubbandCap) {
		int selftestErr = AMIGA_MIDSIDE_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestCollapseStereoToMonoSubbandCap) {
		int selftestErr = AMIGA_COLLAPSE_STEREO_TO_MONO_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestIntensitySubbandCap) {
		int selftestErr = AMIGA_INTENSITY_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32HalfSparse16) {
		int selftestErr = AMIGA_FDCT32_HALF_SPARSE16_SELFTEST();
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
	if (opt.selftestPolyphaseStride2Reduced) {
		int selftestErr = SelftestPolyphaseStride2Reduced();
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
	if (opt.selftestPolyphaseStride4StereoReduced) {
		int selftestErr = SelftestPolyphaseStride4StereoReduced();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPlanarS8TrueStereo) {
		int selftestErr = SelftestPlanarS8TrueStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2Stereo) {
		int selftestErr = SelftestPolyphaseStride2Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2StereoReduced) {
		int selftestErr = SelftestPolyphaseStride2StereoReduced();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride5Stereo) {
		int selftestErr = SelftestPolyphaseStride5Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride3) {
		int selftestErr = SelftestPolyphaseStride3();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride3Stereo) {
		int selftestErr = SelftestPolyphaseStride3Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride5) {
		int selftestErr = SelftestPolyphaseStride5();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4AllPhases) {
		int selftestErr = SelftestPolyphaseStride4AllPhases();
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
	if (opt.selftestFdct32QuarterStereo) {
		int selftestErr = SelftestFdct32QuarterStereo();
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
	if (opt.selftestIntensity) {
		int selftestErr = SelftestIntensity();
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
	if (opt.selftestFakeStereo) {
		int selftestErr = SelftestFakeStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.startupVolumeSelftest) {
		int selftestErr = SelftestStartupVolume();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr == 0 ? 0 : 1;
	}
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (opt.testAac) {
		int aacTestErr;
		if (!opt.inName) {
			fprintf(stderr, "--test-aac requires an input .aac file\n");
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		aacTestErr = AmigaAacSmokeTest(opt.inName, &opt);
		AmigaFreeNormalizedArgs(&normalized);
		return aacTestErr == 0 ? 0 : 1;
	}
#else
	if (opt.testAac) {
		if (!opt.inName)
			fprintf(stderr, "--test-aac requires an input .aac file\n");
		fprintf(stderr, "--test-aac requires an AmigaOS LoadSeg decoder-module build\n");
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
#endif
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
	if (opt.inName && !strncmp(opt.inName, "http://", 7))
		opt.radioStream = 1;
#if defined(HAVE_AMISSL)
	if (opt.inName && !strncmp(opt.inName, "https://", 8))
		opt.radioStream = 1;
#endif
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
	if (opt.play && opt.radioStream) {
		RadioStream *radio;
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
		radio = Radio_OpenWithHostAddr(opt.inName, opt.haveRadioHostAddr, opt.radioHostAddrBe);
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
		if (!radio || Radio_GetStatus(radio) == RADIO_STATUS_ERROR) {
			fprintf(stderr, "cannot open radio stream: %s\n", radio ? Radio_GetError(radio) : "out of memory");
			GuiMarkRadioErrorText(radio ? Radio_GetError(radio) : "out of memory");
			if (radio) Radio_Close(radio);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		InputSourceInitRadio(&input, radio);
		{
			int probePump;
			for (probePump = 0; probePump < 200 && !Radio_GetContentType(radio)[0] &&
				Radio_GetStatus(radio) != RADIO_STATUS_ERROR; probePump++)
				Radio_Pump(radio);
		}
		GuiPublishRadioMetadata(radio);
		{
			const char *radioExt;
			fprintf(stderr, "radio-codec: Radio Browser codec=%s URL codec hint=%s HTTP content-type=%s\n",
				opt.radioCodecHint ? opt.radioCodecHint : "(none)",
				RadioUrlHasMp3Hint(opt.inName) ? "MP3" : "none", Radio_GetContentType(radio));
			if (RadioStreamLooksLikeOpus(opt.inName, Radio_GetContentType(radio), opt.radioCodecHint)) {
				fprintf(stderr, "cannot open radio stream: Opus-in-Ogg is not supported by this decoder\n");
				GuiMarkRadioErrorText("Unsupported stream codec: OPUS");
				Radio_Close(radio);
				free(resolvedOutName);
				AmigaFreeNormalizedArgs(&normalized);
				return 1;
			}
			radioExt = RadioDecoderExtFromUrlOrTypeHint(opt.inName, Radio_GetContentType(radio), opt.radioCodecHint);
			fprintf(stderr, "radio-codec: final selected decoder=%s\n", radioExt ? radioExt : "mp3");
			if (radioExt && StrCaseCmp(radioExt, "mp3") != 0) {
				int gret = AmigaGenericInputPlay(opt.inName, &input, radioExt, &opt, &stats, 1);
				printf("radio-teardown: generic(AAC/FLAC) play returned, freeing resolvedOutName=%p\n", (void *)resolvedOutName);
				free(resolvedOutName);
				printf("radio-teardown: resolvedOutName freed, freeing normalized args\n");
				AmigaFreeNormalizedArgs(&normalized);
				printf("radio-teardown: normalized args freed, returning gret=%d from main\n", gret);
				return gret;
			}
		}
	} else if (opt.play) {
		/* If the file extension is not .mp3, try a generic decoder module. */
		{
			const char *ext = GetFileExtension(opt.inName);
			if (ext && StrCaseCmp(ext, "mp3") != 0) {
				int gret = AmigaGenericFormatPlay(opt.inName, ext, &opt, &stats);
				free(resolvedOutName);
				AmigaFreeNormalizedArgs(&normalized);
				return gret;
			}
		}
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
		if (opt.radioStream) {
			RadioStream *radio;
			GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
			radio = Radio_OpenWithHostAddr(opt.inName, opt.haveRadioHostAddr, opt.radioHostAddrBe);
			GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
			if (!radio || Radio_GetStatus(radio) == RADIO_STATUS_ERROR) {
				fprintf(stderr, "cannot open radio stream: %s\n", radio ? Radio_GetError(radio) : "out of memory");
				GuiMarkRadioErrorText(radio ? Radio_GetError(radio) : "out of memory");
				if (radio) Radio_Close(radio);
				free(resolvedOutName);
				AmigaFreeNormalizedArgs(&normalized);
				return 1;
			}
			InputSourceInitRadio(&input, radio);
			GuiPublishRadioMetadata(radio);
		} else {
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
		GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input open")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (opt.fastMem) {
		int preloadResult;

		GuiPublishStartupStage(GUISTART_INPUT_PRELOAD_FASTMEM);
		preloadResult = InputSourcePreloadFastMemory(&input);
		if (preloadResult != 0) {
			if (preloadResult < 0)
				fprintf(stderr, "cannot preload input into Fast RAM: %s\n", opt.inName);
			InputSourceClose(&input);
			CloseInputFile(&infile, opt.debugCleanup);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		gGuiPlaybackStatus.fastInputBytes = input.memorySize;
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
	Radio_CheckMiniMem("after decoder init");

	if (opt.play && opt.stereo)
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
	MP3ResetStereoStride4PolyphaseCounters();
	MP3ResetMonoStride2PolyphaseCounters();
	MP3SetExperimentalHuffman(opt.expHuff);
	MP3SetExperimentalIMDCTThin(decoder, opt.expImdctThin);
	MP3SetExperimentalReducedTaps(opt.expReducedTaps);
	MP3SetExperimentalFDCT32Quarter(opt.expFdct32Quarter ||
		(opt.superfastLowrate && opt.outputRate == 11025));
	if (opt.fastLowrate) {
		int stride = FastLowrateStrideForOutputRate(opt.outputRate);
		if (opt.expReducedTaps && stride != 2 && stride != 4)
			fprintf(stderr, "warning: --exp-reduced-taps only affects 22050/11025 Hz stride-2/stride-4 fast-lowrate output\n");
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
			fprintf(stderr, "warning: --exp-reduced-taps enables lossy reduced-tap fast-lowrate dewindowing\n");
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
	if (opt.subbandCap > 0)
		MP3SetSubbandCap(decoder, opt.subbandCap);
	if (opt.debugSubbandPrecondition)
		MP3EnableFdct32HalfSparse16PreconditionCheck(1);
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
		/* Do not clear gPlaybackInterrupted here.  The MintAMP GUI can
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
		if (opt.debugSubbandPrecondition)
			printf("FDCT32HalfSparse16 precondition checks: %lu, violations: %lu\n",
				gFdct32HalfSparse16PreconditionChecks,
				gFdct32HalfSparse16PreconditionViolations);
		if (stats.spareTimeMeasured)
			printf("playback minimum spare before buffer end: %ld ms\n",
				stats.minimumSpareMilliseconds);
		else
			printf("playback minimum spare before buffer end: n/a\n");
		if (MP3SuperfastLowrateEnabled(decoder))
			printf("fast-lowrate stride: %d (superfast: IMDCT/overlap capped to %d of %d subbands; %s)\n",
				MP3GetFastLowrateStride(decoder),
				MP3GetFastLowrateActiveSubbands(decoder), 32,
				(MP3GetFastLowrateStride(decoder) == 4 &&
				 MP3ExperimentalFDCT32QuarterEnabled()) ?
					"FDCT32Quarter" : "FDCT32 full-rate");
		else
			printf("fast-lowrate stride: %d (fast-lowrate: IMDCT/DCT32 full-rate)\n",
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
		{
			unsigned long teardownSessionId = (input.radio) ? Radio_GetSessionId(input.radio) : 0UL;
			int teardownFatal = (input.radio) ? Radio_IsSessionFatal(input.radio) : 0;

			RADIO_DBG(printf("radio-teardown: post-audio cleanup begin session=%lu fatal=%d\n", teardownSessionId, teardownFatal));
			if (AmigaPostAudioLeakTestEnabled()) {
				/* Test mode (MP3_POST_AUDIO_LEAK_TEST=1): deliberately skip
				 * MP3FreeDecoder()/InputSourceClose() (which frees the
				 * decoder, the radio ring buffer, and the RadioStream
				 * struct/socket/SSL context via Radio_Close()) and leak them
				 * instead.  If a reboot that used to happen right here stops
				 * happening with this set, the corruptor is a use-after-free
				 * or double-free in that free/close path, not in
				 * AmigaAudioClose() (already fixed) or upstream of it. */
				RADIO_DBG(printf("radio-teardown: MP3_POST_AUDIO_LEAK_TEST active -- leaking decoder/ring/stream session=%lu\n",
					teardownSessionId));
			} else {
				Radio_DebugCheckExecMem("before decoder free/skip");
				if (teardownFatal) {
					/* Fatal TLS teardown quarantine (see
					 * Radio_IsSessionFatal()): the decoder context is part of
					 * the same child-owned object graph the fatal fault may
					 * have damaged by corrupting exec memory around it --
					 * leak it instead of risking MP3FreeDecoder() writing
					 * into/through already-damaged heap state. */
					RADIO_DBG(printf("radio-teardown: decoder free skipped (fatal TLS quarantine) session=%lu -- leaking\n",
						teardownSessionId));
				} else {
					RADIO_DBG(printf("radio-teardown: before decoder free session=%lu\n", teardownSessionId));
					MP3FreeDecoder(decoder);
					RADIO_DBG(printf("radio-teardown: after decoder free session=%lu\n", teardownSessionId));
				}
				Radio_DebugCheckExecMem("after decoder free/skip");
				Radio_CheckMiniMem("after decoder cleanup");
				RADIO_DBG(printf("radio-teardown: before Radio_Close second stop phase (InputSourceClose) session=%lu\n",
					teardownSessionId));
				InputSourceClose(&input);
				RADIO_DBG(printf("radio-teardown: after InputSourceClose/Radio_Close session=%lu\n", teardownSessionId));
				CloseInputFile(&infile, opt.debugCleanup);
			}
			gTiming = NULL;
			MP3SetDecodeCoreProfileEnabled(0);
			RADIO_DBG(printf("radio-teardown: freeing resolvedOutName=%p session=%lu\n",
				(void *)resolvedOutName, teardownSessionId));
			free(resolvedOutName);
			RADIO_DBG(printf("radio-teardown: freeing normalized args session=%lu\n", teardownSessionId));
			AmigaFreeNormalizedArgs(&normalized);
			RADIO_DBG(printf("radio-teardown: post-audio cleanup end, returning from main playErr=%d session=%lu\n",
				playErr, teardownSessionId));
		}
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
		InputSourceAlignDecodePointer(readBuf, &readPtr, &bytesLeft);
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
	if (opt.fastLowrate) {
		if (MP3SuperfastLowrateEnabled(decoder))
			printf("fast-lowrate stride: %d (superfast: IMDCT/overlap capped to %d of %d subbands; %s)\n",
				MP3GetFastLowrateStride(decoder),
				MP3GetFastLowrateActiveSubbands(decoder), 32,
				(MP3GetFastLowrateStride(decoder) == 4 &&
				 MP3ExperimentalFDCT32QuarterEnabled()) ?
					"FDCT32Quarter" : "FDCT32 full-rate");
		else
			printf("fast-lowrate stride: %d (fast-lowrate: IMDCT/DCT32 full-rate)\n",
				MP3GetFastLowrateStride(decoder));
	}

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
					unsigned long m2Asm = 0, m2C = 0, m2Reduced = 0;
					MP3GetMonoStride2PolyphaseCounters(&m2Asm, &m2C, &m2Reduced);
					printf("mono stride-2 polyphase: %s\n",
						m2Reduced ? "reduced" : (m2Asm ? "asm" : "C"));
					printf("mono stride-2 polyphase calls: asm=%lu C=%lu reduced=%lu\n",
						m2Asm, m2C, m2Reduced);
				}
				{
					unsigned long s2Asm = 0, s2C = 0, s2Reduced = 0;
					MP3GetStereoStride2PolyphaseCounters(&s2Asm, &s2C, &s2Reduced);
					printf("stereo stride-2 polyphase: %s\n",
						s2Reduced ? "reduced" : (s2Asm ? "asm" : "C"));
					printf("stereo stride-2 polyphase calls: asm=%lu C=%lu reduced=%lu\n",
						s2Asm, s2C, s2Reduced);
				}
				{
					unsigned long s4Asm = 0, s4C = 0, s4Reduced = 0;
					MP3GetStereoStride4PolyphaseCounters(&s4Asm, &s4C, &s4Reduced);
					printf("stereo stride-4 polyphase: %s\n",
						s4Reduced ? "reduced" : (s4Asm ? "asm" : "C"));
					printf("stereo stride-4 polyphase calls: asm=%lu C=%lu reduced=%lu\n",
						s4Asm, s4C, s4Reduced);
				}
				printf("core IMDCT subbands: executed=%lu skipped=%lu\n",
					coreProfile.imdctSubbandsExecuted, coreProfile.imdctSubbandsSkipped);
				{
					unsigned long imdctTotalBlocks = coreProfile.imdct36BlockCount + coreProfile.imdct12x3BlockCount;
					double shortPct = imdctTotalBlocks ? (100.0 * (double)coreProfile.imdct12x3BlockCount / (double)imdctTotalBlocks) : 0.0;
					printf("imdct block kind: IMDCT36(long)=%lu IMDCT12x3(short)=%lu short-block%%=%.1f\n",
						coreProfile.imdct36BlockCount, coreProfile.imdct12x3BlockCount, shortPct);
				}
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
				printf("intensity stereo usage: MPEG1=%lu MPEG2=%lu with-M/S=%lu intensity-only=%lu\n",
					coreProfile.intensityMPEG1Count,
					coreProfile.intensityMPEG2Count,
					coreProfile.intensityWithMidSideCount,
					coreProfile.intensityOnlyCount);
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
	Radio_CheckMiniMem("after decoder cleanup");
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
