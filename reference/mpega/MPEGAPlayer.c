/*
* This file is part of MPEGAPlayer.
* Copyright (C) 1998 Stephane Tavenard
* 
* MPEGAPlayer is free software: you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* MPEGAPlayer is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with MPEGAPlayer.  If not, see <http://www.gnu.org/licenses/>.
*
*/
/*------------------------------------------------------------------------------

    File    :   MPEGAPlayer.c

    Author  :   Stéphane TAVENARD

    $VER:   MPEGAPlayer.c  2.55  (17/03/1998)

    (C) Copyright 1995-1997 Stéphane TAVENARD
        All Rights Reserved

    #Rev|   Date   |                      Comment
    ----|----------|--------------------------------------------------------
    0   |06/06/1995| Initial revision                                     ST
    1   |11/06/1995| First release                                        ST
    2   |18/06/1995| Use of fast cosine transform                         ST
    3   |09/07/1995| Config window don't stop decoding now                ST
    3   |09/07/1995| Screen is unlocked when no window on it              ST
    4   |10/09/1995| Added mixing frequency option                        ST
    5   |01/07/1996| Corrected bug in Chk (thanks to Peter Kunath)        ST
    5   |01/07/1996| Corrected old bug that kill my prog !!!!!!!!         ST
    6   |02/12/1996| Corrected old bug (config r/w)                       ST
    7   |26/12/1996| Scopes & Volume support, set player PRI to 19        ST
    7   |26/12/1996| Slower & Faster / Prev & Next pattern                ST
    8   |16/02/1997| Added Buffer Time config & High priority config      ST
    9   |26/02/1997| Added Boost Volume                                   ST
    10  |29/04/1997| 'C' Version & new MPEG routines ! (and layer III)    ST
    11  |16/05/1997| Use new AudioMan (V0.2)                              ST
    12  |18/05/1997| Use new MPEGDEC & AUDIOMAN                           ST
    13  |22/05/1997| Fixed naughty bug that crashes player somtimes       ST
    14  |26/05/1997| Fixed audioport pause func pb with soft int          ST
    15  |02/06/1997| Changed status window, added save pos & open         ST
    16  |07/06/1997| Use ASYNC I/O for bitstream reading                  ST
    17  |07/06/1997| Time count down in status window, status process     ST
    18  |30/06/1997| Added Slider in Status window                        ST
    19  |16/07/1997| Added Layer I&II / Layer III options                 ST
    20  |16/08/1997| Fixed some bugs                                      ST
    21  |15/11/1997| Use of mpega.library now & status boost & ram load   ST
    22  |15/12/1997| Added pattern matching for MPEG-Audio check          ST
    23  |17/03/1998| Added duration & format & tag infos                  ST

    ------------------------------------------------------------------------

    An MPEG Audio player for delitracker !

------------------------------------------------------------------------------*/

/**
Chargement fichier mpeg:

   InitPlayer    -> AllocAudio + Start (CONFIG)
   VolBalance
   InitSound     -> Stop + FreeAudio (RESET)
   StartInt      -> Start (PLAY)

Pause/Resume:

   StopInt / StartInt

Stop:

   (StopInt) (si play)
   EndSound
   VolBalance
   InitSound

Eject:
   EndSound
   EndPlayer

**/

#define ASYNC_IO

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <libraries/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <devices/audio.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/gadtools.h>
#include <intuition/intuition.h>
#include <string.h>
#include <stdarg.h>
#include <clib/timer_protos.h>
#include <pragmas/timer_pragmas.h>
#include "DeliPlayer.h"


#include "DEFS.H"
#include <proto/mpega.h> // #21
//#include "MPEGDEC.H" // #21
#include "AUDIOMAN.H"

#include "MPEGAPlayer_conv.h"
#include "MPEGA_cfg_window.h"

#ifdef ASYNC_IO
#include <dos/dos.h>
#include <proto/dos.h>
#include <pragmas/dos_pragmas.h>
#if 1
#include "asyncio.h"
#else
#define ASIO_REGARGS
#include <libraries/asyncio.h>
#define _ARGS
#include <clib/asyncio_protos.h>
#endif
#endif

// #define DEBUG

//#define DEBUG_STATES

//#define SCOPES_SYNC  // If scopes needs to be synchronous to song by direct sample start func call.

#ifdef DEBUG
#define DEBUGP( msg ) Message( msg )
#else
#define DEBUGP( msg )
#endif

#ifdef DEBUG_STATES
void SetDebugState( char *msg );
#define DEBUG_STATE( msg ) SetDebugState( msg )
#else
#define DEBUG_STATE( msg )
#endif

/* Version String */

#define PLAYER_VERSION  2
#define PLAYER_REVISION 55

UBYTE Version[] = "$VER:MPEGAPlayer 2.55 "__AMIGADATE__" Audio player for Delitracker (C)1995-1997 Stéphane TAVENARD";

/* Externals */

extern struct DosLibrary *DOSBase;
extern struct IntuitionBase *IntuitionBase;
extern struct Library *GadToolsBase;

/* DeliTracker's stuff */

struct DeliTrackerGlobals *DeliBase;
struct MsgPort *DeliPort;

/* Copyright and info */

UBYTE AboutString[] = "© 1995-1997 Stéphane TAVENARD\n"
                      "Enjoy Faaast MPEG Audio ;^) \n"
                      "stephane.tavenard@wanadoo.fr";

// #23 Begin
typedef struct {
   char title[31] ;
   char artist[31] ;
   char album[31] ;
   char year[5] ;
   char comment[31] ;
   char genre[31] ;
   unsigned int gennum;
} MPEGTAG;

static const char *mpeg_audio_modes[ 4 ] = { " stereo ", "j-stereo", "  dual  ", "  mono  " };

char sta_type[ 10 ];
char sta_mode[ 10 ];
char sta_rate[ 10 ];
char sta_freq[ 10 ];
MPEGTAG mpeg_tag;

char *Info_ModuleNamePtr = NULL;
char *Info_AuthorNamePtr = NULL;
char *Info_FormatNamePtr = NULL;
// #23 End


struct Library *MPEGABase = NULL; // #21

void __asm __saveds DeliProcess(void);
ULONG __asm __saveds Check(void);
ULONG __asm __saveds InitPlayer(void);
ULONG __asm __saveds EndPlayer(void);
ULONG __asm __saveds InitSound(void);
ULONG __asm __saveds EndSound(void);
ULONG __asm __saveds StartInt(void);
ULONG __asm __saveds StopInt(void);
ULONG __asm __saveds Faster(void);
ULONG __asm __saveds Slower(void);
ULONG __asm __saveds PrevPatt(void);
ULONG __asm __saveds NextPatt(void);
ULONG __asm __saveds VolBalance(void);

ULONG __asm __saveds Config( void );
ULONG __asm __saveds OpenConfigWindow( void );
ULONG __asm __saveds OpenStatusWindow( void );
ULONG __asm __saveds CloseStatusWindow( void );
ULONG __asm __saveds DurationCalc( void ); // #23
ULONG __asm __saveds MiscText( void ); // #23

void UpdateStatusWindow( void ); // #17

static void process_scopes( void );

void CloseWindows( void );
BOOL ConfigWindowHandler( void );
static void SetVolume( INT32 boost_volume );

void __saveds AudioProcess( void );
void SetPerVol(void);
void __stdargs Message(UBYTE *Msg,...);

struct NoteStruct *NotePlay;

static UBYTE PlayerName[] = "MPEG Audio";
static UBYTE AboutText[] =  "      MPEG Audio Player\n"
                         " © 1995-97 by Stéphane TAVENARD\n"
                         "          ANGERS / France\n"
                         " Release date: "__DATE__"\n"
                         "\n"
                         "This player can decode MPEG Audio\n"
                         "        norms 1, 2 or 2.5\n"
                         "       layers I, II & III.\n"
                         "\n"
                         "This decoder is highly optimized\n"
                         "but it's strongly recommended\n"
                         "to have at least a 68030/40Mhz\n"
                         "    to reach the real time...\n"
                         "\n"
#if 0 // #21
#ifdef _M68060
#ifdef _M68881
                         "CPU version: 68040/60+FPU\n"
#else
                         "CPU version: 68040/60\n"
#endif
#else
#ifdef _M68020
#ifdef _M68881
                         "CPU version: 68020/30+FPU\n"
#else
                         "CPU version: 68020/30\n"
#endif
#endif
#endif
                         "\n"
#endif // #21
                         "14-bit routine © Christian Buchner\n"
                         "AHI © Martin Blom\n"
                         "\n"
                         "Enjoy Amiga & MPEG audio ;o)\n";

/* Tag list for DeliTracker */

struct TagItem PlayerTagArray[] = {
   DTP_RequestDTVersion, DELIVERSION,
   DTP_PlayerVersion,    (PLAYER_VERSION<<16)+PLAYER_REVISION,
   DTP_PlayerName,       (ULONG)PlayerName,
   DTP_Creator,          (ULONG)AboutString,
   DTP_Description,      (ULONG)"a player for MPEG Audio samples",
   DTP_Flags,            PLYF_SONGEND,
   DTP_DeliBase,         (ULONG)&DeliBase,
   DTP_Check1,           (ULONG)&Check,
   DTP_Process,          (ULONG)&DeliProcess,
   DTP_Priority,         0,
   DTP_StackSize,        8192,
   DTP_MsgPort,          (ULONG)&DeliPort,
   DTP_NoteStruct,       (ULONG)&NotePlay,
   DTP_InitPlayer,       (ULONG)&InitPlayer,
   DTP_EndPlayer,        (ULONG)&EndPlayer,
   DTP_InitSound,        (ULONG)&InitSound,
   DTP_EndSound,         (ULONG)&EndSound,
   DTP_StartInt,         (ULONG)&StartInt,
   DTP_StopInt,          (ULONG)&StopInt,
   DTP_Volume,           (ULONG)&VolBalance,
   DTP_Balance,          (ULONG)&VolBalance,
   DTP_Faster,           (ULONG)&Faster,
   DTP_Slower,           (ULONG)&Slower,
   DTP_PrevPatt,         (ULONG)&PrevPatt,
   DTP_NextPatt,         (ULONG)&NextPatt,

   DTP_Config,           (ULONG)&Config,
   DTP_UserConfig,       (ULONG)&OpenConfigWindow,
   DTP_Appear,           (ULONG)&OpenStatusWindow,
   DTP_Disappear,        (ULONG)&CloseStatusWindow,

   DTP_ModuleName,       (ULONG)&Info_ModuleNamePtr,  // #23
   DTP_AuthorName,       (ULONG)&Info_AuthorNamePtr,  // #23
   DTP_FormatName,       (ULONG)&Info_FormatNamePtr,  // #23
   DTP_Duration,         (ULONG)&DurationCalc,    // #23
//   DTP_MiscText,         (ULONG)&MiscText, // #23

   TAG_DONE
};


MPEGA_STREAM *mps = NULL; // #21
BOOL Playing = FALSE;

struct Process *ConfigProcess = NULL;
struct SignalSemaphore ConfigSemaphore;
BOOL ConfigWindowOpened = FALSE;

struct Process *StatusProcess = NULL; // #17
struct SignalSemaphore StatusSemaphore; // #17
BOOL StatusWindowOpened = FALSE;

#define PLAYER_CONFIG_ID 0x444D5041 // "DMPA"

typedef struct {
   INT32 magic_id;
   INT32 l12_mono_quality;
   INT32 l12_mono_freq_max;
   INT32 l12_stereo_quality;
   INT32 l12_stereo_freq_max;
   INT32 l12_mono_forced;
   INT32 mixing_enabled;
   INT32 mixing_freq;
   INT32 buffer_time;      // Time in sec
   INT32 player_priority;
   INT32 scopes_enabled;
   INT32 boost_volume;
   AUM_MODE audio_mode;
   INT32 sta_auto;      // #15
   INT32 sta_win_x;     // #15
   INT32 sta_win_y;     // #15
   INT32 l3_mono_quality;    // #19
   INT32 l3_mono_freq_max;   // #19
   INT32 l3_stereo_quality;  // #19
   INT32 l3_stereo_freq_max; // #19
   INT32 l3_mono_forced;     // #19
   INT32 sta_tag; // #23
   INT32 load_ram; // #21
   char  pattern[ 256 ]; // #22

} PLAYER_CONFIG;

static PLAYER_CONFIG player_config = {
   PLAYER_CONFIG_ID,
   2, 48000,
   2, 48000,
   0,
   0, 28000,
   2, 0, 1,
   100,
   { AUM_DEVICE_PAULA, 0, "" },
   FALSE, 0, 0,          // #15
   2, 48000, // #19
   2, 48000, // #19
   0,        // #19
   0,  // sta_tag #23
   0,  // load_ram  #21
   "~(mod.#?)" // pattern #22
};

struct Task *PlayerTask;
struct SignalSemaphore LoadQuitSemaphore;

BOOL ProcActive = TRUE;

struct Process *AudioProc = NULL;

INT16 *pcm[ MPEGA_MAX_CHANNELS ]; // #21
INT32 prev_pcm_count = 0;
UWORD VolumeR = 64;
UWORD VolumeL = 64;

BOOL ChanInit;

ULONG Bits;
ULONG Frequency;

BOOL Loading=FALSE;
BOOL LoadingStopped=TRUE;

BOOL NewConfig = FALSE; // #21

/*** NotePlayer interface ***/

BOOL NoteInit=FALSE;
UBYTE *NotePosition=NULL;
ULONG NoteLength=0;
UWORD NotePeriod=0;

struct NoteChannel NoteChannels[ MPEGA_MAX_CHANNELS ] = {  // #21
   &NoteChannels[ 1 ], /* NextChannel */
   0,                  /* for use by NotePlayer */
   0,                  /* Reserved0 */
   0,                  /* Private */
   0,                  /* Changed */
   NCHD_FarLeft,0,     /* StereoPos, Stereo */
   0,0,                /* SampleStart, SampleLength */
   0,0,                /* RepeatStart, RepeatLength */
   0,                  /* Frequency */
   0,                  /* Volume */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* Reserved1 */

   NULL,               /* NextChannel */
   0,                  /* for use by NotePlayer */
   0,                  /* Reserved0 */
   0,                  /* Private */
   0,                  /* Changed */
   NCHD_FarRight,1,    /* StereoPos, Stereo */
   0,0,                /* SampleStart, SampleLength */
   0,0,                /* RepeatStart, RepeatLength */
   0,                  /* Frequency */
   0,                  /* Volume */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0  /* Reserved1 */
};

struct NoteStruct NoteStruct= {
   &NoteChannels[ 0 ],                           /* Channels */
   NSTF_Dummy|
   NSTF_Signed|NSTF_8Bit,/* Flags */
   48000,                                        /* Max Frequency */
   64,                                           /* Max Volume */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0           /* reserved */
};

struct NoteStruct *NotePlay = &NoteStruct;

BYTE AudioSignal = 0; // *** Should be -1
AUM_SAMPLE current_sample;
BYTE *note_sample[ MPEGA_MAX_CHANNELS ] = { NULL, NULL }; // #21
INT32 note_max_size = 0;

/****************************************************************************/

void __stdargs _XCEXIT( long code )
{
   return;
}

static void UpdatePlayerPriority( void )
{
   if( !PlayerTask ) return;
   SetTaskPri( PlayerTask, player_config.player_priority );
}

void __asm __saveds DeliProcess(void)
{
   ULONG Signals;
   struct DeliMessage *DeliMessage;
   INT16 i;
   INT32 pcm_count;
   INT32 frame = 0;

   DOSBase = (struct DosLibrary*)DeliBase->DOSBase;
   IntuitionBase = (struct IntuitionBase*)DeliBase->IntuitionBase;
   GadToolsBase = (struct Library*)DeliBase->GadToolsBase;

   // #21 Begin
   MPEGABase = OpenLibrary( "mpega.library", 0L );
   if( !MPEGABase ) {
      Message( "mpega.library not found !" );
      return;
   }
   // #21 End

   Info_ModuleNamePtr = ""; // #23
   Info_AuthorNamePtr = ""; // #23
   Info_FormatNamePtr = ""; // #23

   AudioSignal = AllocSignal( -1 );

   for( i=0; i<MPEGA_MAX_CHANNELS; i++ ) { // #21
      pcm[ i ] = AllocVec( (ULONG)(MPEGA_PCM_SIZE * sizeof( INT16 )), MEMF_PUBLIC ); // #21
      if( !pcm[ i ] ) ProcActive = FALSE;
   }

   note_max_size = 4*AUM_BUFFER_SIZE;

   for( i=0; i<MPEGA_MAX_CHANNELS; i++ ) { // #21
      note_sample[ i ] = AllocVec( note_max_size, MEMF_PUBLIC );
   }

   InitSemaphore( &ConfigSemaphore );
   InitSemaphore( &StatusSemaphore ); // #17

   PlayerTask = (struct Task*)FindTask( NULL );

   Playing = FALSE;

   while( ProcActive ) {

      ULONG SigMask = SIGBREAKF_CTRL_C | (1L<<DeliPort->mp_SigBit);

      if( AudioSignal != -1 ) SigMask |= (1L<<AudioSignal);

      if( Playing ) Signals = SetSignal( 0L, SigMask );
      else Signals = Wait( SigMask );

      if( Signals & SIGBREAKF_CTRL_C ) {
         ProcActive = FALSE;
      }

      if( Signals & (1L<<DeliPort->mp_SigBit) ) {
         if( DeliMessage = (struct DeliMessage*)GetMsg( DeliPort ) ) {
            DEBUG_STATE( "DELIMSG" );
            DeliMessage->Result = (*DeliMessage->Function)();
            ReplyMsg( (struct Message *)DeliMessage );
         }
      }

      if( (Playing) && (mps) ) {
         DEBUG_STATE( "DECODE" );
         pcm_count = MPEGA_decode_frame( mps, pcm ); // #21
         if( pcm_count != MPEGA_ERR_EOF ) { // #21
            AUM_SAMPLE aum_sample;

            if( pcm_count < 0 ) { // MPEG decoding error -> repeat last frame
               pcm_count = prev_pcm_count;
            }
            else {
               prev_pcm_count = pcm_count;
            }
            if( pcm_count > 0 ) {
               aum_sample.size = pcm_count;
               aum_sample.sample[ 0 ] = pcm[ 0 ];
               aum_sample.sample[ 1 ] = pcm[ 1 ];
               DEBUG_STATE( "WR.AUDIO" );
               (void)AUM_write( &aum_sample );
            }
            UpdateStatusWindow(); // #17
            if( (frame & 63) == 0 ) {
               UpdatePlayerPriority();
            }
            frame++;
         }
         else { // Wait for end of song
            INT32 state;

            AUM_control( AUM_CTRL_STATE, (ULONG)&state );
            if( state != AUM_STA_PLAY ) {
               VolumeR = VolumeL = 0;
               process_scopes();
               dt_SongEnd();
               Playing = FALSE;
            }
            else {
               Delay( 1 );
            }
            UpdateStatusWindow(); // #17
         }
      }

      if( Signals & (1L<<AudioSignal) ) { // A new sample has started
         process_scopes();
      }

      if( NewConfig ) { // #21
         CloseStatusWindow();
         if( player_config.sta_auto ) { // #15
            OpenStatusWindow();
         }
         NewConfig = FALSE;
      }

   }

   EndPlayer();
   CloseWindows();

   for( i=0; i<MPEGA_MAX_CHANNELS; i++ ) { // #21
      if( pcm[ i ] ) {
         FreeVec( pcm[ i ] );
         pcm[ i ] = NULL;
      }
   }

   for( i=0; i<MPEGA_MAX_CHANNELS; i++ ) { // #21
      if( note_sample[ i ] ) {
         FreeVec( note_sample[ i ] );
         note_sample[ i ] = NULL;
      }
   }

   if( AudioSignal != -1 ) FreeSignal( AudioSignal );
   AudioSignal = -1;
   // #21 Begin
   if( MPEGABase ) {
      CloseLibrary( MPEGABase );
      MPEGABase = NULL;
   }
   // #21 End
}

static void process_scopes( void )
{
   INT16 ch;
   INT8 *note_pcm[ 2 ];
   INT32 note_size;
//   register INT32 size;
//   register INT8 *src;
   register INT8 *dest;

   if( (mps) && (player_config.scopes_enabled) ) {
      note_size = current_sample.size;
      if( note_size > note_max_size ) note_size = note_max_size;
      note_size &= ~3; // Multiple of 4 to optimize loops
      for( ch=0; ch<mps->dec_channels; ch++ ) {
         dest = note_sample[ ch ];
#if 1
         if( (dest) && (note_size) ) {
            conv_16_to_8( current_sample.sample[ ch ], dest, note_size );
         }
#else
         size = note_size>>2;
         if( (dest) && (size) ) {
            src = (INT8 *)current_sample.sample[ ch ];
            do {
               *dest++ =  *src;
               src += 2;
               *dest++ =  *src;
               src += 2;
               *dest++ =  *src;
               src += 2;
               *dest++ =  *src;
               src += 2;
            } while( size-- );
         }
#endif
      }
      note_pcm[ 0 ] = note_sample[ 0 ];
      if( mps->dec_channels == 1 ) note_pcm[ 1 ] = note_pcm[ 0 ];
      else note_pcm[ 1 ] = note_sample[ 1 ];

      for( ch=0; ch<2; ch++ ) {
         NoteChannels[ ch ].nch_SampleStart =
         NoteChannels[ ch ].nch_RepeatStart = note_pcm[ ch ];
         NoteChannels[ ch ].nch_SampleLength =
         NoteChannels[ ch ].nch_RepeatLength = note_size;
         NoteChannels[ ch ].nch_Frequency = Frequency;
         NoteChannels[ ch ].nch_Volume = (ch == 0)?VolumeL:VolumeR;
         NoteChannels[ ch ].nch_Changed=NCHF_Sample|NCHF_Repeat|NCHF_Frequency|NCHF_Volume;
      }
      dt_NotePlayer();
   }
}

// #16 Begin

#ifdef ASYNC_IO

#if 1 // #21

typedef struct {
   struct AsyncFile *file;
   INT8 *stream_buffer;
   UINT32 stream_pos;
   UINT32 stream_size;
} STREAM_HANDLE;

static ULONG __saveds __asm bs_access_func( register __a0 struct Hook  *hook,
                                            register __a2 APTR          handle,
                                            register __a1 MPEGA_ACCESS *access ) {
/*----------------------------------------------------------------------------
*/

   switch( access->func ) {

      case MPEGA_BSFUNC_OPEN: {
         STREAM_HANDLE *stream_handle;
         BPTR lock;
         __aligned struct FileInfoBlock fib;

         lock = Lock( access->data.open.stream_name, ACCESS_READ );
         if( !lock ) return NULL;

         if( Examine( lock, &fib ) ) {
            access->data.open.stream_size = (long)fib.fib_Size;
         }

         UnLock( lock );

         stream_handle = (STREAM_HANDLE *)AllocVec( sizeof(STREAM_HANDLE), MEMF_CLEAR | MEMF_PUBLIC );
         if( !stream_handle ) return NULL;
         stream_handle->stream_size = fib.fib_Size;

         if( player_config.load_ram ) {
            stream_handle->stream_buffer = AllocVec( stream_handle->stream_size, MEMF_PUBLIC );
         }
         if( stream_handle->stream_buffer ) {
            BPTR fh = Open( access->data.open.stream_name, MODE_OLDFILE );
            if( fh ) {
               stream_handle->stream_size = Read( fh, stream_handle->stream_buffer,
                                                  stream_handle->stream_size );
               Close( fh );
            }
            else {
               FreeVec( stream_handle );
               stream_handle = NULL;
            }
         }
         else {
            stream_handle->file = OpenAsync( access->data.open.stream_name, MODE_READ, access->data.open.buffer_size*2 );
            if( !stream_handle->file ) {
               FreeVec( stream_handle );
               stream_handle = NULL;
            }
         }
         return (ULONG)stream_handle;
      }

      case MPEGA_BSFUNC_CLOSE:

         if( handle ) {
            STREAM_HANDLE *stream_handle = handle;

            if( stream_handle->file ) CloseAsync( stream_handle->file );
            if( stream_handle->stream_buffer ) FreeVec( stream_handle->stream_buffer );
            FreeVec( stream_handle );
         }
         break;

      case MPEGA_BSFUNC_READ: {
         LONG read_size = 0;

         if( handle ) {
            STREAM_HANDLE *stream_handle = handle;

            if( stream_handle->file ) {
               read_size = ReadAsync( stream_handle->file,
                                      access->data.read.buffer, access->data.read.num_bytes );
            }
            else {
               LONG size = stream_handle->stream_size - stream_handle->stream_pos;
               if( access->data.read.num_bytes < size ) {
                  size = access->data.read.num_bytes;
               }
               if( size > 0 ) {
                  CopyMem( stream_handle->stream_buffer + stream_handle->stream_pos,
                           access->data.read.buffer, size );
                  stream_handle->stream_pos += size;
                  read_size = size;
               }
            }
         }

         return (ULONG)read_size;
      }
      case MPEGA_BSFUNC_SEEK: {

         int err = 0;

         if( handle ) {
            STREAM_HANDLE *stream_handle = handle;

            if( stream_handle->file ) {
               err = SeekAsync( stream_handle->file, access->data.seek.abs_byte_seek_pos, MODE_START );
               if( err > 0 ) err = 0;
            }
            else {
               if( access->data.seek.abs_byte_seek_pos < stream_handle->stream_size ) {
                  stream_handle->stream_pos = access->data.seek.abs_byte_seek_pos;
               }
               else err = 1;
            }
         }

         return (ULONG)err;
      }
   }
   return 0;
}

#else

long bs_open( char *stream_name, long buffer_size, long *stream_size )
/*-------------------------------------------------------------------
*/
{
   struct AsyncFile *file_ptr;
   BPTR lock;
   struct FileInfoBlock fib;

   lock = Lock( stream_name, ACCESS_READ );
   if( lock ) {
      if( Examine( lock, &fib ) ) {
         *stream_size = (long)fib.fib_Size;
      }
      UnLock( lock );
   }

   file_ptr = OpenAsync( stream_name, MODE_READ, buffer_size*2 );

   if( file_ptr && (!lock) ) {
      *stream_size = SeekAsync( file_ptr, 0, MODE_END );
      (void)SeekAsync( file_ptr, 0, MODE_START );
   }

   return (long)file_ptr;
}

void bs_close( long handle )
/*-------------------------
*/
{
   if( handle ) CloseAsync( (struct AsyncFile *)handle );
}

long bs_read( long handle, void *buffer, long num_bytes )
/*------------------------------------------------------
*/
{
   long read_size = -1;

   if( handle ) {
      read_size = ReadAsync( (struct AsyncFile *)handle,
                             buffer, num_bytes );
   }

   return read_size;
}

int bs_seek( long handle, long abs_byte_seek_pos )
/*-----------------------------------------------
*/
{
   int err = 0;

   if( handle ) {
      err = SeekAsync( (struct AsyncFile *)handle, abs_byte_seek_pos, MODE_START );
      if( err > 0 ) err = 0;
   }

   return err;
}

#endif // #21

#endif

// #16 End



static void sample_started( AUM_SAMPLE *aus )
/*---------------------------------------------
   Called each time a new sample is started
*/
{
   current_sample = *aus;
#ifdef SCOPES_SYNC
   process_scopes();
#else
   if( AudioSignal != -1 ) {
      Signal( PlayerTask, 1<<AudioSignal );
   }
#endif
}

/****************************************************************************/


BOOL LockScreen( BOOL on )
{
   static BOOL screen_locked = FALSE;

   if( on ) { // Lock
      if( !screen_locked ) {
         Scr = dt_LockScreen();
         if( Scr ) {
            VisualInfo = GetVisualInfo( Scr, TAG_DONE );
            if( VisualInfo ) screen_locked = TRUE;
         }
      }
   }
   else { // Unlock
      if( (screen_locked) && (!StatusWindowOpened) && (!ConfigWindowOpened) ) {
         if( VisualInfo ) FreeVisualInfo( VisualInfo );
         VisualInfo = NULL;

         dt_UnlockScreen();
         screen_locked = FALSE;
      }
   }
   return screen_locked;
}

static UINT32 status_sec_time = 0; // #17
static BOOL update_status_slider = TRUE; // #18
static BOOL status_slider_seek = FALSE; // #18

static void TimeToString( long secs, char *str_time ) // #18
{
   long mins;

   mins = secs / 60;
   secs -= mins * 60;
   sprintf( str_time, "%02d:%02d", mins, secs );
}

static void SetStatusWindowTime( void ) // #17
{
   static char sta_time[ 10 ];

   TimeToString( status_sec_time, sta_time );
   GT_SetGadgetAttrs( MPEGA_STAGadgets[ GD_STA_CUR ], MPEGA_STAWnd, NULL,
                      GTTX_Text, sta_time, TAG_DONE );
   if( update_status_slider ) { // #18
      GT_SetGadgetAttrs( MPEGA_STAGadgets[ GD_STA_SLIDER ], MPEGA_STAWnd, NULL,
                         GTSL_Level, status_sec_time, TAG_DONE );
   }
}

void UpdateStatusWindow( void ) // #17
{
   static long old_secs = -1;

   if( !StatusWindowOpened ) return;
   if( mps && update_status_slider ) {
      UINT32 ms_time;
      ULONG remain;
      UINT32 ms_audio;

      if( status_slider_seek ) { // #18
         status_slider_seek = FALSE;
         AUM_control( AUM_CTRL_STOP, (ULONG)TRUE );
         if( MPEGA_seek( mps, status_sec_time * 1000 ) == 0 ) { // #21
            prev_pcm_count = 0;
         }
         else {
            dt_SongEnd();
            Playing = FALSE;
         }
      }
      // Get decoded time position
      MPEGA_time( mps, &ms_time ); // #21

      // Get audio buffer remaining
      AUM_control( AUM_CTRL_REMAIN, (ULONG)&remain );
      ms_audio = (remain * 1000) / Frequency;

      // Substract time pos to audio remain time to get real time pos
      if( ms_audio <= ms_time ) ms_time -= ms_audio;
      else ms_time = 0;

      Forbid();
//      // Substract from total size to get count down
//      if( ms_time <= mps->ms_duration ) status_sec_time = (mps->ms_duration - ms_time) / 1000;
//      else status_sec_time = 0;

      status_sec_time = ms_time / 1000; // #18

      if( status_sec_time != old_secs ) {
         old_secs = status_sec_time;
         // Signal update to status window
         if( StatusProcess ) Signal( &StatusProcess->pr_Task, SIGBREAKF_CTRL_D );
      }
      Permit();
   }
}

#define SET_STA_GADGET( index, gtype, value ) GT_SetGadgetAttrs( MPEGA_STAGadgets[ index ],\
                                                                 MPEGA_STAWnd, NULL,\
                                                                 gtype, value, TAG_DONE )
void  SetStatusWindow( void )
{
   static char sta_time[ 10 ]; // #18
   long secs; // #18

   if( !StatusWindowOpened ) return; // #13: Added !!!

   if( mps ) {
      secs = mps->ms_duration / 1000;

      TimeToString( secs, sta_time ); // #18
      SET_STA_GADGET( GD_STA_TIME, GTTX_Text, sta_time ); // #18

      SET_STA_GADGET( GD_STA_TYPE, GTTX_Text, sta_type );
      SET_STA_GADGET( GD_STA_MODE, GTTX_Text, sta_mode );
      SET_STA_GADGET( GD_STA_RATE, GTTX_Text, sta_rate );
      SET_STA_GADGET( GD_STA_FREQ, GTTX_Text, sta_freq );

      status_sec_time = 0; // #18
      SetStatusWindowTime(); // #18

      SET_STA_GADGET( GD_STA_SLIDER, GTSL_Min, 0 ); // #18
      SET_STA_GADGET( GD_STA_SLIDER, GTSL_Max, secs ); // #18
      SET_STA_GADGET( GD_STA_SLIDER, GTSL_Level, 0 ); // #18

      /* #23 Begin */
      SET_STA_GADGET( GD_STA_ALBUM, GTTX_Text, mpeg_tag.album );
      SET_STA_GADGET( GD_STA_ARTIST, GTTX_Text, mpeg_tag.artist );
      SET_STA_GADGET( GD_STA_GENRE, GTTX_Text, mpeg_tag.genre );
      SET_STA_GADGET( GD_STA_YEAR, GTTX_Text, mpeg_tag.year );
      SET_STA_GADGET( GD_STA_COMMENT, GTTX_Text, mpeg_tag.comment );
      /* #23 End */

   }
   SET_STA_GADGET( GD_STA_BOOST, GTSL_Level, player_config.boost_volume ); // #21

}

#ifdef DEBUG_STATES
void SetDebugState( char *msg )
{
   static char state[ 16 ];

   if( !StatusWindowOpened ) return;
   strncpy( state, msg, 16 );
   GT_SetGadgetAttrs( MPEGA_STAGadgets[ GD_STA_MODE ], MPEGA_STAWnd, NULL,
                      GTTX_Text, state, TAG_DONE );

}
#endif

// #17 Begin

ULONG __asm __saveds CloseStatusWindow( void )
{
   if( !StatusProcess ) return 0;

   Signal( &StatusProcess->pr_Task, SIGBREAKF_CTRL_C );

   ObtainSemaphore( &StatusSemaphore );
   ReleaseSemaphore( &StatusSemaphore ); // #20

   return 0;
}

static SAVEDS void StatusEntry( void );

ULONG __asm __saveds OpenStatusWindow( void )
{
   if( StatusProcess ) return 0;

   StatusProcess = CreateNewProcTags(
      NP_Entry, (void(*)())(StatusEntry),
      NP_Name, "MPEGAPlayer_Status",
      NP_Priority, 0,
      NP_StackSize, 4096,
      TAG_END );

   if( !StatusProcess ) return 1;

   Wait( SIGBREAKF_CTRL_D );

   return 0;
}

static SAVEDS void StatusEntry( void )
{
   ULONG window_sigmask;
   ULONG signals;
   BOOL active = TRUE;
   struct IntuiMessage *imsg; // #18
   ULONG class; // #18
   APTR iaddress; // #18

   ObtainSemaphore( &StatusSemaphore );
   Signal( PlayerTask, SIGBREAKF_CTRL_D );

   if( LockScreen( TRUE ) ) {

      MPEGA_STALeft = player_config.sta_win_x;
      MPEGA_STATop = player_config.sta_win_y;
      MPEGA_STAHeight = (player_config.sta_tag) ? 116 : 48; // #23
//#23      MPEGA_STAHeight = (player_config.sta_boost) ? 50 : 34; // #21
      OpenMPEGA_STAWindow();

      StatusWindowOpened = TRUE;
      SetStatusWindow();

      window_sigmask = 1<<(MPEGA_STAWnd->UserPort->mp_SigBit);

      update_status_slider = TRUE; // #18

      do {
         signals = Wait( window_sigmask | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D );

         if( signals & window_sigmask ) { // #18
            while( imsg = GT_GetIMsg( MPEGA_STAWnd->UserPort ) ) {
               class = imsg->Class;
               iaddress = imsg->IAddress;
               GT_ReplyIMsg( imsg );

               switch( class ) {
                  case IDCMP_REFRESHWINDOW:
//                     GT_BeginRefresh( MPEGA_STAWnd );
//                     MPEGA_STARender();
//                     GT_EndRefresh( MPEGA_STAWnd, TRUE );
                     break;
                  case IDCMP_CLOSEWINDOW:
                     active = FALSE;
                     break;
                  case IDCMP_GADGETDOWN:
                     if( iaddress == MPEGA_STAGadgets[ GD_STA_SLIDER ] ) {
                        update_status_slider = FALSE;
                     }
                     break;
                  case IDCMP_MOUSEMOVE:
                     if( iaddress == MPEGA_STAGadgets[ GD_STA_SLIDER ] ) {
                        update_status_slider = FALSE;
                        GT_GetGadgetAttrs( MPEGA_STAGadgets[ GD_STA_SLIDER ], MPEGA_STAWnd, NULL,
                                           GTSL_Level, &status_sec_time, TAG_DONE );
                        SetStatusWindowTime();
                     }
                     // #21 Begin
                     if( iaddress == MPEGA_STAGadgets[ GD_STA_BOOST ] ) {

                        GT_GetGadgetAttrs( MPEGA_STAGadgets[ GD_STA_BOOST ], MPEGA_STAWnd, NULL,
                                           GTSL_Level, &player_config.boost_volume, TAG_DONE );
                        VolBalance();
                     }
                     // #21 End
                     break;
                  case IDCMP_GADGETUP:
                     if( iaddress == MPEGA_STAGadgets[ GD_STA_SLIDER ] ) {
                        status_slider_seek = TRUE;
                        update_status_slider = TRUE;
                     }
                     break;
                  default:
                     break;
               }
            }
         }
         if( signals & SIGBREAKF_CTRL_C ) { // Kill
            active = FALSE;
         }
         if( signals & SIGBREAKF_CTRL_D ) { // Update window
            if( update_status_slider ) SetStatusWindowTime();
         }
      } while( active );

      // #20 Begin
      player_config.sta_win_x = MPEGA_STAWnd->LeftEdge;
      player_config.sta_win_y = MPEGA_STAWnd->TopEdge;
      // #20 End

      // Clear message port...
      while( GT_GetIMsg( MPEGA_STAWnd->UserPort ) ) ;
      CloseMPEGA_STAWindow();
      StatusWindowOpened = FALSE;
      LockScreen( FALSE );
   }

   Forbid();
   ReleaseSemaphore( &StatusSemaphore );
   StatusProcess = NULL;
}

// #17 End

char *ConfigPathName( void )
{
#define PATH_SIZE 256
   static char path[ PATH_SIZE ];
   LONG size;

   size = GetVar( "DeliConfig", path, PATH_SIZE, 0 );
   if( size < 0 ) { // Env var not foud
      path[ 0 ] = '\0';
      if( !AddPart( path, "PROGDIR:DeliConfig/", PATH_SIZE ) ) return NULL;
   }
   if( !AddPart( path, "MPEGAudio.prefs", PATH_SIZE ) ) return NULL;
   return path;
}

void CheckConfigValues( void )
{
#define CHECK_VALUE( v, low, high ) if( v < low ) v = low; else if( v > high ) v = high

   CHECK_VALUE( player_config.buffer_time, 1, 10 );
   CHECK_VALUE( player_config.boost_volume, 100, 400 );
   CHECK_VALUE( player_config.mixing_freq, 4000, 48000 );
   CHECK_VALUE( player_config.player_priority, -9, 19 );
   // #19 Begin
   CHECK_VALUE( player_config.l12_mono_quality, 0, 2 );
   CHECK_VALUE( player_config.l12_mono_freq_max, 4000, 48000 );
   CHECK_VALUE( player_config.l12_stereo_quality, 0, 2 );
   CHECK_VALUE( player_config.l12_stereo_freq_max, 4000, 48000 );
   CHECK_VALUE( player_config.l3_mono_quality, 0, 2 );
   CHECK_VALUE( player_config.l3_mono_freq_max, 4000, 48000 );
   CHECK_VALUE( player_config.l3_stereo_quality, 0, 2 );
   CHECK_VALUE( player_config.l3_stereo_freq_max, 4000, 48000 );
   // #19 End

#undef CHECK_VALUE
}

void ReadConfigFile( void )
{
   char *filename;
   FILE *file;
   PLAYER_CONFIG cfg;
   long size;

   filename = ConfigPathName();
   if( !filename ) return;
   file = fopen( filename, "rb" );
   if( !file ) return;
   size = fread( &cfg, 1, sizeof( cfg ), file );
   if( size >= 4 ) {
      if( cfg.magic_id == PLAYER_CONFIG_ID ) { // Valid ID
         memcpy( &player_config, &cfg, size ); // Copy read buffer
      }
   }
   fclose( file );
   CheckConfigValues();
   UpdatePlayerPriority();
//Message( "Read config %ld", player_config.player_priority );
}

void WriteConfigFile( void )
{
   char *filename;
   FILE *file;

   filename = ConfigPathName();
   if( !filename ) return;
   file = fopen( filename, "wb" );
   if( !file ) return;
   fwrite( &player_config, sizeof( player_config ), 1, file );
   fclose( file );
}

ULONG __asm __saveds Config( void )
{
   ReadConfigFile();
   return 0;
}

#define SET_CFG_GADGET( index, gtype, value ) GT_SetGadgetAttrs( MPEGA_CFGGadgets[ index ],\
                                                                 MPEGA_CFGWnd, NULL,\
                                                                 gtype, value, TAG_DONE )
#define GET_CFG_GADGET( index, gtype, value ) GT_GetGadgetAttrs( MPEGA_CFGGadgets[ index ],\
                                                                 MPEGA_CFGWnd, NULL,\
                                                                 gtype, value, TAG_DONE )
void SetConfigWindow( void )
{
   // #19 Begin
   SET_CFG_GADGET( GD_CFG_L12_MONO_QUALITY, GTCY_Active, player_config.l12_mono_quality );
   SET_CFG_GADGET( GD_CFG_L12_MONO_FREQ_MAX, GTIN_Number, player_config.l12_mono_freq_max );
   SET_CFG_GADGET( GD_CFG_L12_STEREO_QUALITY, GTCY_Active, player_config.l12_stereo_quality );
   SET_CFG_GADGET( GD_CFG_L12_STEREO_FREQ_MAX, GTIN_Number, player_config.l12_stereo_freq_max );
   SET_CFG_GADGET( GD_CFG_L12_FORCE_MONO, GTCB_Checked, player_config.l12_mono_forced );

   SET_CFG_GADGET( GD_CFG_L3_MONO_QUALITY, GTCY_Active, player_config.l3_mono_quality );
   SET_CFG_GADGET( GD_CFG_L3_MONO_FREQ_MAX, GTIN_Number, player_config.l3_mono_freq_max );
   SET_CFG_GADGET( GD_CFG_L3_STEREO_QUALITY, GTCY_Active, player_config.l3_stereo_quality );
   SET_CFG_GADGET( GD_CFG_L3_STEREO_FREQ_MAX, GTIN_Number, player_config.l3_stereo_freq_max );
   SET_CFG_GADGET( GD_CFG_L3_FORCE_MONO, GTCB_Checked, player_config.l3_mono_forced );
   // #19 End

   SET_CFG_GADGET( GD_CFG_MIXING_ENABLED, GTCB_Checked, player_config.mixing_enabled );
   SET_CFG_GADGET( GD_CFG_MIXING_FREQUENCY, GTIN_Number, player_config.mixing_freq );

   SET_CFG_GADGET( GD_CFG_STATUS, GTCB_Checked, player_config.sta_auto ); // #15
   SET_CFG_GADGET( GD_CFG_STATAG, GTCB_Checked, player_config.sta_tag ); // #23

   SET_CFG_GADGET( GD_CFG_LOADRAM, GTCB_Checked, player_config.load_ram ); // #21

   SET_CFG_GADGET( GD_CFG_PATTERN, GTST_String, player_config.pattern ); // #22

   SET_CFG_GADGET( GD_CFG_SCOPES_ENABLED, GTCB_Checked, player_config.scopes_enabled );
   SET_CFG_GADGET( GD_CFG_PLAYER_PRIORITY, GTSL_Level, player_config.player_priority );
   SET_CFG_GADGET( GD_CFG_BUFFER_TIME, GTSL_Level, player_config.buffer_time );
   SET_CFG_GADGET( GD_CFG_BOOST, GTSL_Level, player_config.boost_volume );

   SET_CFG_GADGET( GD_CFG_AHI_ENABLED, GTCB_Checked,
                  (player_config.audio_mode.device == AUM_DEVICE_AHI)?TRUE:FALSE );
   SET_CFG_GADGET( GD_CFG_AHI_MODE_ID, GTNM_Format, "%lx" );
   SET_CFG_GADGET( GD_CFG_AHI_MODE_ID, GTNM_Number, player_config.audio_mode.id );

   SET_CFG_GADGET( GD_CFG_AHI_MODE_NAME, GTTX_Text, player_config.audio_mode.name );

}

void GetConfigWindow( void )
{
   ULONG value;
   char *ptr; // #22

   // #19 Begin
   GET_CFG_GADGET( GD_CFG_L12_MONO_QUALITY, GTCY_Active, &player_config.l12_mono_quality );
   GET_CFG_GADGET( GD_CFG_L12_MONO_FREQ_MAX, GTIN_Number, &player_config.l12_mono_freq_max );
   GET_CFG_GADGET( GD_CFG_L12_STEREO_QUALITY, GTCY_Active, &player_config.l12_stereo_quality );
   GET_CFG_GADGET( GD_CFG_L12_STEREO_FREQ_MAX, GTIN_Number, &player_config.l12_stereo_freq_max );
   GET_CFG_GADGET( GD_CFG_L12_FORCE_MONO, GTCB_Checked, &player_config.l12_mono_forced );

   GET_CFG_GADGET( GD_CFG_L3_MONO_QUALITY, GTCY_Active, &player_config.l3_mono_quality );
   GET_CFG_GADGET( GD_CFG_L3_MONO_FREQ_MAX, GTIN_Number, &player_config.l3_mono_freq_max );
   GET_CFG_GADGET( GD_CFG_L3_STEREO_QUALITY, GTCY_Active, &player_config.l3_stereo_quality );
   GET_CFG_GADGET( GD_CFG_L3_STEREO_FREQ_MAX, GTIN_Number, &player_config.l3_stereo_freq_max );
   GET_CFG_GADGET( GD_CFG_L3_FORCE_MONO, GTCB_Checked, &player_config.l3_mono_forced );
   // #19 End

   GET_CFG_GADGET( GD_CFG_MIXING_ENABLED, GTCB_Checked, &player_config.mixing_enabled );
   GET_CFG_GADGET( GD_CFG_MIXING_FREQUENCY, GTIN_Number, &player_config.mixing_freq );

   GET_CFG_GADGET( GD_CFG_STATUS, GTCB_Checked, &player_config.sta_auto ); // #15
   GET_CFG_GADGET( GD_CFG_STATAG, GTCB_Checked, &player_config.sta_tag ); // #23

   GET_CFG_GADGET( GD_CFG_LOADRAM, GTCB_Checked, &player_config.load_ram ); // #21

   GET_CFG_GADGET( GD_CFG_PATTERN, GTST_String, &ptr ); // #22
   strncpy( player_config.pattern, ptr, 256 ); // #22

   GET_CFG_GADGET( GD_CFG_SCOPES_ENABLED, GTCB_Checked, &player_config.scopes_enabled );
   GET_CFG_GADGET( GD_CFG_PLAYER_PRIORITY, GTSL_Level, &player_config.player_priority );
   GET_CFG_GADGET( GD_CFG_BUFFER_TIME, GTSL_Level, &player_config.buffer_time );
   GET_CFG_GADGET( GD_CFG_BOOST, GTSL_Level, &player_config.boost_volume );

   SET_STA_GADGET( GD_STA_BOOST, GTSL_Level, player_config.boost_volume ); // #21

   GET_CFG_GADGET( GD_CFG_AHI_ENABLED, GTCB_Checked, &value );
   player_config.audio_mode.device = (value)?AUM_DEVICE_AHI:AUM_DEVICE_PAULA;

   GET_CFG_GADGET( GD_CFG_AHI_MODE_ID, GTNM_Number, &player_config.audio_mode.id );

   GET_CFG_GADGET( GD_CFG_AHI_MODE_NAME, GTTX_Text, &value );
   strncpy( player_config.audio_mode.name, (char *)value, 256 );

   CheckConfigValues();

   /* #15 Begin */
   Forbid();
   if( StatusWindowOpened ) {
      if( MPEGA_STAWnd ) {
         player_config.sta_win_x = MPEGA_STAWnd->LeftEdge;
         player_config.sta_win_y = MPEGA_STAWnd->TopEdge;
      }
   }
   Permit();
   NewConfig = TRUE;
   /* #15 End */
}

void CloseConfigWindow( void )
{
   if( !ConfigProcess ) return;

   Signal( &ConfigProcess->pr_Task, SIGBREAKF_CTRL_C );

   ObtainSemaphore( &ConfigSemaphore );
   ReleaseSemaphore( &ConfigSemaphore ); // #20
}

static SAVEDS void ConfigEntry( void );

ULONG __asm __saveds OpenConfigWindow( void )
{
   if( ConfigProcess ) return 0;

   ConfigProcess = CreateNewProcTags(
      NP_Entry, (void(*)())(ConfigEntry),
      NP_Name, "MPEGAPlayer_Config",
      NP_Priority, 0,
      NP_StackSize, 4096,
      TAG_END );

   if( !ConfigProcess ) return 1;

   Wait( SIGBREAKF_CTRL_D );

   return 0;
}

static SAVEDS void ConfigEntry( void )
{
   ULONG window_sigmask;
   ULONG signals;
   BOOL active = TRUE;


   ObtainSemaphore( &ConfigSemaphore );
   Signal( PlayerTask, SIGBREAKF_CTRL_D );

   if( LockScreen( TRUE ) ) {

      OpenMPEGA_CFGWindow();
      ConfigWindowOpened = TRUE;
      SetConfigWindow();

      window_sigmask = 1<<(MPEGA_CFGWnd->UserPort->mp_SigBit);

      do {
         signals = Wait( window_sigmask | SIGBREAKF_CTRL_C );

         if( signals & window_sigmask ) {
            active = ConfigWindowHandler();
         }
         if( signals & SIGBREAKF_CTRL_C ) {
            active = FALSE;
         }
      } while( active );

      // Clear message port...
      while( GT_GetIMsg( MPEGA_CFGWnd->UserPort ) ) ;
      CloseMPEGA_CFGWindow();
      ConfigWindowOpened = FALSE;
      LockScreen( FALSE );

      VolBalance();
      UpdatePlayerPriority();
   }

   Forbid();
   ReleaseSemaphore( &ConfigSemaphore );
   ConfigProcess = NULL;
}

BOOL ConfigWindowHandler( void )
/*------------------------------
   Return TRUE if window active
          FALSE if window to close
*/
{
   struct IntuiMessage *imsg;
   ULONG class;
   APTR iaddress;
   BOOL active = TRUE;
   static AUM_MODE am;

   if( !ConfigWindowOpened ) return FALSE;
   while( imsg = GT_GetIMsg( MPEGA_CFGWnd->UserPort ) ) {
      class = imsg->Class;
      iaddress = imsg->IAddress;
      GT_ReplyIMsg( imsg );

      switch( class ) {
         case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh( MPEGA_CFGWnd );
            MPEGA_CFGRender();
            GT_EndRefresh( MPEGA_CFGWnd, TRUE );
            break;
         case IDCMP_CLOSEWINDOW:
            active = FALSE;
            break;
         case IDCMP_MOUSEMOVE:
            if( iaddress == MPEGA_CFGGadgets[ GD_CFG_BOOST ] ) {
               INT32 boost_volume;

               GET_CFG_GADGET( GD_CFG_BOOST, GTSL_Level, &boost_volume );
               SetVolume( boost_volume );
            }
            break;
         case IDCMP_GADGETUP:
            if( iaddress == MPEGA_CFGGadgets[ GD_CFG_AHI_QUERY ] ) {

               am.device = AUM_DEVICE_AHI;
               if( !AUM_query( &am ) ) {

                  GT_SetGadgetAttrs( MPEGA_CFGGadgets[ GD_CFG_AHI_MODE_ID ],
                                     MPEGA_CFGWnd, NULL,
                                     GTNM_Number, am.id, TAG_DONE );
                  GT_SetGadgetAttrs( MPEGA_CFGGadgets[ GD_CFG_AHI_MODE_NAME ],
                                     MPEGA_CFGWnd, NULL,
                                     GTTX_Text, am.name, TAG_DONE );
               }
            }
            if( iaddress == MPEGA_CFGGadgets[ GD_CFG_SAVE ] ) {
               GetConfigWindow();
               WriteConfigFile();
               active = FALSE;
            }
            if( iaddress == MPEGA_CFGGadgets[ GD_CFG_USE ] ) {
               GetConfigWindow();
               active = FALSE;
            }
            if( iaddress == MPEGA_CFGGadgets[ GD_CFG_CANCEL ] ) {
               active = FALSE;
            }
            if( iaddress == MPEGA_CFGGadgets[ GD_CFG_ABOUT ] ) {
               static struct EasyStruct easy = {
                  sizeof (struct EasyStruct),
                  0,
                  PlayerName,
                  AboutText,
                  "  Ok  ",
               };
               EasyRequestArgs( MPEGA_CFGWnd, &easy, NULL, NULL );
            }
            break;
         default:
            break;
      }
   }
   return active;
}

void CloseWindows( void )
{
   CloseConfigWindow();
   (void)CloseStatusWindow();
}

/****************************************************************************/

/* Module check routine */

ULONG __asm __saveds Check( void ) // #21
{
   MPEGA_STREAM *mps;
   MPEGA_CTRL ctrl;
   BOOL ok = TRUE;

//   LONG sync_pos = MPEGA_find_sync( DeliBase->ChkData, 1024 );
//   if( sync_pos < 0 ) return (ULONG)1; // Bad sync

   // #22 Begin
   {
      char check_pattern[ 256 ];
      if( ParsePatternNoCase( player_config.pattern, check_pattern, 256 ) >= 0 ) {
         if( !MatchPatternNoCase( check_pattern, DeliBase->FileArrayPtr ) ) {
            return (ULONG)1; // Not an MPEG-Audio filename
         }
      }
   }
   // #22 End

   // Set settings to minimum (just for cheking MPEG Audio)
   ctrl.bs_access = NULL;
   ctrl.layer_1_2.force_mono = TRUE;
   ctrl.layer_1_2.mono.freq_div = ctrl.layer_1_2.stereo.freq_div = 4;
   ctrl.layer_1_2.mono.quality = ctrl.layer_1_2.stereo.quality = 0;
   ctrl.layer_1_2.mono.freq_max = ctrl.layer_1_2.stereo.freq_max = 0;
   ctrl.layer_3.force_mono = TRUE;
   ctrl.layer_3.mono.freq_div = ctrl.layer_3.stereo.freq_div = 4;
   ctrl.layer_3.mono.quality = ctrl.layer_3.stereo.quality = 0;
   ctrl.layer_3.mono.freq_max = ctrl.layer_3.stereo.freq_max = 0;
   ctrl.check_mpeg = 0;
   ctrl.stream_buffer_size = 0;

   mps = MPEGA_open( DeliBase->PathArrayPtr, &ctrl );
   if( mps ) { // Try to decode one frame to see if this is an mpeg audio stream
      int i;
      INT16 *pcm[ MPEGA_MAX_CHANNELS ];

      for( i=0; i<MPEGA_MAX_CHANNELS; i++ ) {
         pcm[ i ] = AllocVec( (ULONG)(MPEGA_PCM_SIZE * sizeof( INT16 )), MEMF_PUBLIC );
         if( !pcm[ i ] ) ok = FALSE;
      }
      if( ok ) {
         INT32 samples;
         int tries = 3;

         while( tries-- ) {
            samples = MPEGA_decode_frame( mps, pcm );
            if( samples < 0 ) {
               ok = FALSE;
               break;
            }
         }
      }
      for( i=0; i<MPEGA_MAX_CHANNELS; i++ ) {
         if( pcm[ i ] ) FreeVec( pcm[ i ] );
      }
   }
   else ok = FALSE;

   if( mps ) MPEGA_close( mps );

   if( ok ) return 0;

   return (ULONG)1; // not an MPEG Audio stream
}


/****************************************************************************/
/* #23 Begin */
#define GENRE_LARGEST 44
#define GENRE_MAXLEN  30

typedef struct {
   char tag[3] ;
   char title[30] ;
   char artist[30] ;
   char album[30] ;
   char year[4] ;
   char comment[30] ;
   unsigned char genre ;
} MPEGTAG_ID3;

static const char *genres[]={
  "Blues",
  "Classic Rock",
  "Country",
  "Dance",
  "Disco",
  "Funk",
  "Grunge",
  "Hip-Hop",
  "Jazz",
  "Metal",
  "New Age",
  "Oldies",
  "Other",
  "Pop",
  "R&B",
  "Rap",
  "Reggae",
  "Rock",
  "Techno",
  "Industrial",
  "Alternative",
  "Ska",
  "Death Metal",
  "Pranks",
  "Soundtrack",
  "Euro-Techno",
  "Ambient",
  "Trip-Hop",
  "Vocal",
  "Jazz+Funk",
  "Fusion",
  "Trance",
  "Classical",
  "Instrumental",
  "Acid",
  "House",
  "Game",
  "Sound Clip",
  "Gospel",
  "Noise",
  "AlternRock",
  "Bass",
  "Soul",
  "Punk",
  ""
} ;

static void safecopy(char *to,char *from, int maxlen) {
   int where;
   strncpy(to,from,maxlen);
   to[maxlen]=0;
   for ( where=maxlen-1 ; ((where>=0) && (to[where]==' ')) ; where-- ) {
      to[where]=0 ;
   }
}

void get_mpeg_tag( char *filename )
/*---------------------------------
   Get mpeg tag infos
*/
{
   MPEGTAG_ID3 song;
   FILE *file;
   char *filepart;

   filepart = FilePart( filename );
   safecopy( mpeg_tag.title, filepart, 30 );
   mpeg_tag.artist[ 0 ] = mpeg_tag.album[ 0 ] = mpeg_tag.year[ 0 ] =
      mpeg_tag.comment[ 0 ] = mpeg_tag.genre[ 0 ] = '\0';

   file = fopen( filename, "rb" );
   if( !file ) return;

   fseek( file, -128, SEEK_END );
   (void)fread( &song, 128, 1, file );
   fclose( file );

   if( !strncmp(song.tag,"TAG",3) ) {
      safecopy( mpeg_tag.title, song.title, 30 );
      safecopy( mpeg_tag.artist, song.artist, 30 );
      safecopy( mpeg_tag.album, song.album, 30 );
      safecopy( mpeg_tag.year, song.year, 4 );
      safecopy( mpeg_tag.comment, song.comment, 30 );
      mpeg_tag.gennum = song.genre & 0xFF;
      if( mpeg_tag.gennum >= GENRE_LARGEST ) {
         mpeg_tag.gennum = GENRE_LARGEST;
      }
      strcpy( mpeg_tag.genre, genres[ mpeg_tag.gennum ] );
   }
   Info_ModuleNamePtr = mpeg_tag.title;
}
/* #23 End */

/* Init player (alloc channels, etc...) */

ULONG __asm __saveds InitPlayer( void )
{
   static char format_info[ 80 ]; // #23

#ifdef ASYNC_IO
//   static MPEGDEC_ACCESS bs_access = { bs_open, bs_close, bs_read, bs_seek }; // #21
   static struct Hook bs_access = { { NULL, NULL }, bs_access_func, NULL, NULL }; // #21
#endif

DEBUGP( "InitPlayer" );
//Message( "InitPlayer" );

   get_mpeg_tag( DeliBase->PathArrayPtr ); // #23

   if( AUM_open() ) return 1; // can't open audio

   if( !mps ) {
      MPEGA_CTRL ctrl; // #21
      AUM_PARAMS aup;

#ifdef ASYNC_IO
      ctrl.bs_access = &bs_access;
#else
      ctrl.bs_access = NULL;
#endif
      // #19 Begin
      ctrl.layer_1_2.force_mono = (player_config.l12_mono_forced)?TRUE:FALSE;
      ctrl.layer_1_2.mono.freq_div = 0;
      ctrl.layer_1_2.mono.quality = player_config.l12_mono_quality;
      ctrl.layer_1_2.mono.freq_max = player_config.l12_mono_freq_max;
      ctrl.layer_1_2.stereo.freq_div = 0;
      ctrl.layer_1_2.stereo.quality = player_config.l12_stereo_quality;
      ctrl.layer_1_2.stereo.freq_max = player_config.l12_stereo_freq_max;
      ctrl.layer_3.force_mono = (player_config.l3_mono_forced)?TRUE:FALSE;
      ctrl.layer_3.mono.freq_div = 0;
      ctrl.layer_3.mono.quality = player_config.l3_mono_quality;
      ctrl.layer_3.mono.freq_max = player_config.l3_mono_freq_max;
      ctrl.layer_3.stereo.freq_div = 0;
      ctrl.layer_3.stereo.quality = player_config.l3_stereo_quality;
      ctrl.layer_3.stereo.freq_max = player_config.l3_stereo_freq_max;
      // #19 End
      ctrl.check_mpeg = 0;
      ctrl.stream_buffer_size = 0;

      mps = MPEGA_open( DeliBase->PathArrayPtr, &ctrl ); // #21
      prev_pcm_count = 0;
      Frequency = mps->dec_frequency;

      if( !mps ) {
         AUM_close();
         return 1;
      }
      // #23 Begin
      sprintf( sta_type, "MPEG%d-%s", mps->norm, (mps->layer == 1)?"I":(mps->layer == 2)?"II":"III" );
      sprintf( sta_mode, "%8s", mpeg_audio_modes[ mps->mode ] );
      sprintf( sta_rate, "%3dkbps", mps->bitrate );
      sprintf( sta_freq, "%5dHz", mps->frequency );
      sprintf( format_info, "%s %s %s", sta_type, sta_mode, sta_rate );
      Info_FormatNamePtr = format_info;
      // #23 End

//      SetStatusWindow();

      if( player_config.scopes_enabled ) {
         aup.sample_started = sample_started;
      }
      else {
         aup.sample_started = NULL;
      }
      aup.mode = player_config.audio_mode;
      aup.audio_size = player_config.buffer_time * mps->dec_frequency;
      if( aup.audio_size < (AUM_BUFFER_SIZE*4) ) aup.audio_size = (AUM_BUFFER_SIZE*4);
      aup.channels = mps->dec_channels;
      aup.filter = AUM_FILTER_KEEP; // Don't change filter
      aup.frequency = mps->dec_frequency,
      aup.mixing = (player_config.mixing_enabled)?player_config.mixing_freq:0;
      aup.volume[ 0 ] = VolumeL;
      aup.volume[ 1 ] = VolumeR;
      aup.effects = 0;

      if( AUM_config( &aup ) ) { // Can't configure audio
         AUM_close();
         MPEGA_close( mps ); // #21
         mps = NULL;
         return 1;
      }

      if( player_config.sta_auto ) { // #15
         OpenStatusWindow();
      }

   }

   return 0;
}


/****************************************************************************/

/* Clean up the Player (deallocate, etc..) */

ULONG __asm __saveds EndPlayer( void )
{
DEBUGP( "EndPlayer" );

   if( mps ) {
      (void)AUM_close();
      MPEGA_close( mps ); // #21
      mps = NULL;
   }

   return 0;
}

#if 0
static void skip_frames( void )
/*-----------------------------
   Skip some frames to avoid
   gliches in layer III
*/
{
   int i, j;
   INT32 pcm_count;

   for( i=0; i<10; i++ ) {
      // Skip frames in error
      pcm_count = MPEGA_decode_frame( mps, pcm ); // #21
      if( pcm_count == MPEGA_ERR_EOF ) break; // #21
      // Decode some frames to prevent gliches (for Layer III)
      if( pcm_count > 0 ) {
         for( j=0; j<8; j++ ) MPEGA_decode_frame( mps, pcm ); // #21
         break;
      }
   }
}
#endif


/****************************************************************************/

/* Initialize the "Module" */

ULONG __asm __saveds InitSound(void)
{
DEBUGP( "InitSound" );

   if( mps ) {
      prev_pcm_count = 0;
      MPEGA_seek( mps, 0 ); // #21
//      AUM_control( AUM_CTRL_RESET, (ULONG)TRUE ); // #20 Suppressed
      SetStatusWindow(); // #17
//      skip_frames(); // #17
   }

   return 0;
}


/****************************************************************************/

/* End sound */

ULONG __asm __saveds EndSound(void)
{
DEBUGP( "EndSound" );
//Message( "EndSound" );

   AUM_control( AUM_CTRL_RESET, (ULONG)TRUE );
   prev_pcm_count = 0;
   MPEGA_seek( mps, 0 ); // #21
   Playing = FALSE;

   return 0;
}


/****************************************************************************/

/* Start sound */

ULONG __asm __saveds StartInt(void)
{
DEBUGP( "StartInt" );

   AUM_control( AUM_CTRL_PLAY, (ULONG)TRUE );
   Playing = TRUE;

   return 0;
}


/****************************************************************************/

/* Stop sound */

ULONG __asm __saveds StopInt(void)
{
DEBUGP( "StopInt" );

   AUM_control( AUM_CTRL_PLAY, (ULONG)FALSE );
   Playing = FALSE;

   return 0;
}


/****************************************************************************/

/* Play Faster */

ULONG __asm __saveds Faster(void)
{
DEBUGP( "Faster" );

   if( Frequency <= (48000-100) ) {
      Frequency += 100;
      AUM_control( AUM_CTRL_FREQ, (ULONG)Frequency );
   }
   return 0;
}


/****************************************************************************/

/* Slower */

ULONG __asm __saveds Slower(void)
{
DEBUGP( "Slower" );

   if( Frequency >= (4000+100) ) {
      Frequency -= 100;
      AUM_control( AUM_CTRL_FREQ, (ULONG)Frequency );
   }
   return 0;
}

/****************************************************************************/

ULONG __asm __saveds PrevPatt(void)
{
DEBUGP( "PrevPatt" );

   if( (mps) && (Playing) ) {
      UINT32 ms_time_pos;
      UINT32 offset;
      int err;

      err = MPEGA_time( mps, &ms_time_pos ); // #21
      if( !err ) {
         AUM_control( AUM_CTRL_STOP, (ULONG)TRUE );
         offset = 10000 + (1000 * player_config.buffer_time);
         if( offset < ms_time_pos ) ms_time_pos -= offset;
         else ms_time_pos = 0;
         if( ms_time_pos < 0 ) ms_time_pos = 0;
         MPEGA_seek( mps, ms_time_pos ); // #21
//         skip_frames();
         prev_pcm_count = 0;
      }
   }
   return 0;
}

/****************************************************************************/

ULONG __asm __saveds NextPatt(void)
{
DEBUGP( "NextPatt" );

   if( (mps) && (Playing) ) {
      UINT32 ms_time_pos;
      int err;

      err = MPEGA_time( mps, &ms_time_pos ); // #21
      if( !err ) {
         AUM_control( AUM_CTRL_STOP, (ULONG)TRUE );
         ms_time_pos += 10000 - (1000 * player_config.buffer_time);
         MPEGA_seek( mps, ms_time_pos ); // #21
//         skip_frames();
         prev_pcm_count = 0;
      }
   }
   return 0;
}

/****************************************************************************/

/* Volume and Balance */

static void SetVolume( INT32 boost_volume )
{
   INT32 volume[ 2 ];

   volume[ 0 ] = (VolumeL * boost_volume) / 100;
   volume[ 1 ] = (VolumeR * boost_volume) / 100;

   AUM_control( AUM_CTRL_LVOL, (ULONG)volume[ 0 ] );
   AUM_control( AUM_CTRL_RVOL, (ULONG)volume[ 1 ] );
}

ULONG __asm __saveds VolBalance( void )
{

DEBUGP( "VolBalance" );

   VolumeL = (DeliBase->SndVol * DeliBase->SndLBal) / 64;
   VolumeR = (DeliBase->SndVol * DeliBase->SndRBal) / 64;

   SetVolume( player_config.boost_volume );

   return 0;
}

/* #23 Begin */
/*******************************************************************************/
ULONG __asm __saveds DurationCalc( void ) {

   if( mps ) {
      struct ExecBase *SysBase = (struct ExecBase *)(*(ULONG *)0x4);

      return (ULONG)(((double)mps->ms_duration * (double)SysBase->ex_EClockFrequency) / 1000.0);
   }
   return 0;
}

ULONG __asm __saveds MiscText( void ) {

   return (ULONG)"*** Misc Text ***\n*** Line 2 ***";
}

/* #23 End */

/*******************************************************************************/

/* Show a message to the user */

void __stdargs Message(UBYTE *Msg,...)
{
   va_list Arg;
   struct EasyStruct Req = {
      sizeof( struct EasyStruct ),
      0, "MPEGAPlayer message",
      0, "Okay"
   };

   Req.es_TextFormat = Msg;
   va_start( Arg, Msg );

   if( IntuitionBase ) {
      EasyRequestArgs( NULL, &Req, 0, Arg );
   }
   else {
      VPrintf( Msg, Arg );
      Printf( "\n" );
   }

   va_end( Arg );
}
