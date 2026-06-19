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

    File    :   AUDIOMAN.C

    Author  :   Stéphane TAVENARD

    $VER:   AUDIOMAN.C  0.3  (18/05/1997)

    (C) Copyright 1997-1997 Stéphane TAVENARD
        All Rights Reserved

    #Rev|   Date   |                      Comment
    ----|----------|--------------------------------------------------------
    0   |17/04/1997| Initial revision                                     ST
    1   |25/04/1997| Use V4 of AHI now                                    ST
    2   |13/05/1997| Use of circular buffer instead of multiple buffers   ST
    3   |18/05/1997| Corrected some alloc bugs                            ST
    4   |22/05/1997| Try to use audio port signals against sound function ST
    5   |23/05/1997| Dynamic audio buffer size (with sample frequency)    ST
    6   |25/05/1997| Added 8 bits samples                                 ST

    ------------------------------------------------------------------------

    AUDio MANager for audio port

------------------------------------------------------------------------------*/

#include "DEFS.H"
#include <time.h>
#include "AudioPort.h"
#include <devices/ahi.h>
#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/ahi.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <pragmas/dos_pragmas.h>
#include <pragmas/exec_pragmas.h>
#include "AUDIOMAN.H"

#include <proto/intuition.h>

#define DEBUG

#ifdef DEBUG
#define DEBUG_COLOR( c ) *((volatile UINT16 *)0xDFF180) = (UINT16)c
#else
#define DEBUG_COLOR( c )
#endif

#define USE_AHI_V4 // #1

//#define USE_AUDIO_PORT_SIGNAL // #4

#define DYNAMIC_BUFFER_SIZE   // #5

#ifdef USE_AUDIO_PORT_SIGNAL // #4
#define AUM_MANAGER_TASK_PRI  90
#else
#define AUM_MANAGER_TASK_PRI  21
#endif

#define AUM_MANAGER_TASK_NAME "AUM_Processor"

// If using sound function instead of signals

#define AHI_BUFFERS 2

typedef struct {
   struct Message msg; // should be at the first place
   AUM_SAMPLE sample;
} AUM_WRITE_MSG;

typedef struct {
   INT32                        open_count;
   struct MsgPort               *AHImp;
   struct AHIRequest            *AHIio;
   BYTE                         AHIDevice;
   struct AHIAudioCtrl          *actrl;
   struct AHISampleInfo         asamp[ AUM_MAX_CHANNELS ][ AHI_BUFFERS ];
   INT16                        samp_index;
} AUM_AHI;

typedef struct {
   struct MsgPort *msg_port;
   struct MsgPort *reply_port;
   struct Task *signal_task; // Task to wake up
   AU_PORT *ap;
   AUM_AHI ahi;
   INT16 state;

   void *sample[ AUM_MAX_CHANNELS ]; // #6: void
   INT32 sample_size;
   INT32 sample_r;
   INT32 sample_w;
   INT32 samples_remain; // Samples remaining

   INT32 buffer_size;    // Audio port buffer size

   AUM_PARAMS params;
   AUM_WRITE_MSG *wait_message;
#ifdef USE_AUDIO_PORT_SIGNAL // #4
   BYTE audio_signal;
#endif
   struct SignalSemaphore data_semaphore;
   int error;
} AUM_CTRL;

struct Library *AHIBase = NULL;

static AUM_CTRL       actrl;
static AUM_WRITE_MSG  am;
static struct Process *audio_manager_process = NULL; //# 3 : NULL

static BOOL audio_opened = FALSE;

static void reset_audio( AUM_CTRL *ac )
/*-------------------------------------
   Reset the audio manager controls
   Reply to the pending messages if necessary
*/
{
   if( ac->wait_message ) {
      ReplyMsg( (struct Message *)ac->wait_message );
   }
   ac->wait_message = NULL;
   ac->sample_r = 0;
   ac->sample_w = 0;
   ac->samples_remain = 0;
}

static int write_sample( AUM_CTRL *ac, AUM_SAMPLE *aus )
/*------------------------------------------------------
   Write a sample to the sample buffer
   Return 0 if Ok, -1 if sample buffer too small
*/
{
   INT16 ch;
   INT32 to_fill;
   INT32 count = aus->size;
   INT8 *src[ AUM_MAX_CHANNELS ]; // #6
   INT8 *dest[ AUM_MAX_CHANNELS ]; // #6
   UINT32 fill_bytes; // #6
   INT32 dest_offset; // #6

   if( (ac->sample_size - ac->samples_remain) < count ) return -1;

//DEBUG_COLOR( 0x00F );

   src[ 0 ] = (INT8 *)aus->sample[ 0 ];
   src[ 1 ] = (INT8 *)aus->sample[ 1 ];
   dest[ 0 ] = (INT8 *)ac->sample[ 0 ];
   dest[ 1 ] = (INT8 *)ac->sample[ 1 ];
   while( count > 0 ) {
      // samples count before top of buffer
      to_fill = ac->sample_size - ac->sample_w;
      if( to_fill > count ) to_fill = count;

      if( to_fill > 0 ) {
         if( ac->params.bits == 8 ) { // #6
            fill_bytes = to_fill;
            dest_offset = ac->sample_w;
         }
         else {
            fill_bytes = to_fill<<1;
            dest_offset = ac->sample_w<<1;
         }

         for( ch=0; ch<ac->params.channels; ch++ ) {
            memcpy( dest[ ch ] + dest_offset, src[ ch ], fill_bytes ); // #6
            src[ ch ] += fill_bytes; // #6
         }
         ac->sample_w += to_fill;
         if( ac->sample_w >= ac->sample_size ) ac->sample_w = 0;
         // Protect this against audio interrupts which modify ac->samples_remain
         Disable();
         ac->samples_remain += to_fill;
         Enable();
         count -= to_fill;
      }
      else break; // ???
   }
   return 0;
}

static int write_audio( AUM_CTRL *ac, BOOL immediate )
/*----------------------------------------------------
   Write the current sample buffer to audio port
   Return -1 if end of samples reached
*/
{
   INT16 ch;
   INT32 to_fill;
   INT32 count = ac->buffer_size;
   // WARNING: 16 bit samples
   AUM_SAMPLE samp;
   INT8 *src[ AUM_MAX_CHANNELS ]; // #6
   INT8 *dest[ AUM_MAX_CHANNELS ]; // #6
   UINT32 fill_bytes; // #6
   INT32 src_offset; // #6

#if 0
   if( ac->wait_message ) {
      if( !write_sample( ac, &ac->wait_message->sample ) ) {
         ReplyMsg( (struct Message *)ac->wait_message );
         ac->wait_message = NULL;
//DEBUG_COLOR( 0xF00 );
      }
   }
#endif

   if( ac->samples_remain == 0 ) return -1;

   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      dest[ 0 ] = (INT8 *)ac->ahi.asamp[ 0 ][ ac->ahi.samp_index ].ahisi_Address; // #6
      dest[ 1 ] = (INT8 *)ac->ahi.asamp[ 1 ][ ac->ahi.samp_index ].ahisi_Address; // #6
   }
   else {
      dest[ 0 ] = (INT8 *)ac->ap->l_wave; // #6
      dest[ 1 ] = (INT8 *)ac->ap->r_wave; // #6
   }
   samp.size = ac->buffer_size;
   samp.sample[ 0 ] = dest[ 0 ]; // #6
   samp.sample[ 1 ] = dest[ 1 ]; // #6

   src[ 0 ] = (INT8 *)ac->sample[ 0 ]; // #6
   src[ 1 ] = (INT8 *)ac->sample[ 1 ]; // #6

   while( count > 0 ) {
      // samples count before top of buffer
      to_fill = ac->sample_size - ac->sample_r;
      if( to_fill > ac->samples_remain ) to_fill = ac->samples_remain;
      if( to_fill > count ) to_fill = count;

      if( to_fill > 0 ) {
         if( ac->params.bits == 8 ) { // #6
            fill_bytes = to_fill;
            src_offset = ac->sample_r;
         }
         else {
            fill_bytes = to_fill<<1;
            src_offset = ac->sample_r<<1;
         }
         for( ch=0; ch<ac->params.channels; ch++ ) {
            memcpy( dest[ ch ], src[ ch ] + src_offset, fill_bytes ); // #6
            dest[ ch ] += fill_bytes; // #6
         }
         ac->sample_r += to_fill;
         if( ac->sample_r >= ac->sample_size ) ac->sample_r = 0;
         ac->samples_remain -= to_fill;
         count -= to_fill;
      }
      else { // set to 0 rest of sample
         if( ac->params.bits == 8 ) { // #6
            fill_bytes = count;
         }
         else {
            fill_bytes = count<<1;
         }
         for( ch=0; ch<ac->params.channels; ch++ ) {
            memset( dest[ ch ], 0, fill_bytes ); // #6
         }
         count = 0;
      }
   }

//DEBUG_COLOR( 0x0F0 );

   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      ULONG flags;
      INT16 index;

      if( immediate ) flags = AHISF_IMM;
      else flags = 0;

      // Important, this function is reantrant by interrupt
      // So update samp_index before call to AHI_SetSound

      index = ac->ahi.samp_index;
      ac->ahi.samp_index++;
      if( ac->ahi.samp_index >= AHI_BUFFERS ) ac->ahi.samp_index = 0;

#ifdef USE_AHI_V4 // #1

      if( immediate ) {
         if( ac->params.channels == 1 ) {
            AHI_Play( ac->ahi.actrl,
                      AHIP_BeginChannel, 0,
                      AHIP_Freq,         ac->params.frequency,
                      AHIP_Sound,        index,
                      AHIP_EndChannel,   NULL,
                      TAG_DONE );
         }
         else {
            AHI_Play( ac->ahi.actrl,
                      AHIP_BeginChannel, 0,
                      AHIP_Freq,         ac->params.frequency,
                      AHIP_Sound,        index,
                      AHIP_EndChannel,   NULL,

                      AHIP_BeginChannel, 1,
                      AHIP_Freq,         ac->params.frequency,
                      AHIP_Sound,        AHI_BUFFERS + index,
                      AHIP_EndChannel,   NULL,

                      TAG_DONE );
         }
      }
      else {
         for( ch=0; ch<ac->params.channels; ch++ ) {
            AHI_SetSound( ch, (ch * AHI_BUFFERS) + index, 0, 0, ac->ahi.actrl, flags );
         }
      }
#else

#ifdef AUDIO_STEREO
      if( ac->params.channels == 1 ) {
         AHI_SetSound( 0, index, 0, 0, ac->ahi.actrl, flags );
         AHI_SetSound( 1, index, 0, 0, ac->ahi.actrl, flags );
      }
      else {
         AHI_SetSound( 0, index, 0, 0, ac->ahi.actrl, flags );
         AHI_SetSound( 1, AHI_BUFFERS + index, 0, 0, ac->ahi.actrl, flags );
      }
#else
      for( ch=0; ch<ac->params.channels; ch++ ) {
         AHI_SetSound( ch, (ch * AHI_BUFFERS) + index, 0, 0, ac->ahi.actrl, flags );
      }
#endif
      if( immediate ) {
#ifdef AUDIO_STEREO
         for( ch=0; ch<AUM_MAX_CHANNELS; ch++ ) {
#else
         for( ch=0; ch<ac->params.channels; ch++ ) {
#endif
            AHI_SetFreq( ch, ac->params.frequency, ac->ahi.actrl, AHISF_IMM );
         }
      }

#endif

   }
   else {
      if( ac->ap ) {
         ac->ap->flags &= ~(AUF_FILTER|AUF_FREQ|AUF_VOL);
         ac->ap->command = AUC_WRITE;
         ac->ap->wave_length = ac->buffer_size;
         AU_write( ac->ap );
      }
   }

   if( ac->wait_message ) {
      if( !write_sample( ac, &ac->wait_message->sample ) ) {
         ReplyMsg( (struct Message *)ac->wait_message );
         ac->wait_message = NULL;
//DEBUG_COLOR( 0xF00 );
      }
   }

   if( ac->params.sample_started ) {
      ac->params.sample_started( &samp );
   }

   return 0;
}

static int set_audio_volume( AUM_CTRL *ac )
/*----------------------------------------------------
   Set the volume of audio
   Return 0 if Ok
*/
{
   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      UINT32 l_vol, r_vol, m_vol, max_vol;

      if( !ac->ahi.actrl ) return 0;

      m_vol = 0;
      l_vol = ac->params.volume[ 0 ];
      r_vol = ac->params.volume[ 1 ];
      max_vol = l_vol;
      if( ac->params.channels > 1 ) {
         if( r_vol > max_vol ) max_vol = r_vol;
      }
      if( max_vol > 64 ) { // Constraint to 64
         l_vol = (64 * l_vol) / max_vol;
         r_vol = (64 * r_vol) / max_vol;
         m_vol = (max_vol * 0x10000) / 64;
      }

      l_vol = (l_vol * 0x10000) / 64;
      r_vol = (r_vol * 0x10000) / 64;

#ifdef AUDIO_STEREO
      AHI_SetVol( 0, l_vol, 0x00000L, ac->ahi.actrl, AHISF_IMM ); // Left
      AHI_SetVol( 1, r_vol, 0x10000L, ac->ahi.actrl, AHISF_IMM ); // Right
#else
      if( ac->params.channels == 1 ) { // Mono
         AHI_SetVol( 0, l_vol, 0x8000L, ac->ahi.actrl, AHISF_IMM );
      }
      else { // Stereo
         AHI_SetVol( 0, l_vol, 0x00000L, ac->ahi.actrl, AHISF_IMM ); // Left
         AHI_SetVol( 1, r_vol, 0x10000L, ac->ahi.actrl, AHISF_IMM ); // Right
      }
#endif
      if( m_vol > 0 ) {
         struct AHIEffMasterVolume master_vol;

         master_vol.ahie_Effect = AHIET_MASTERVOLUME;
         master_vol.ahiemv_Volume = m_vol;

         AHI_SetEffect( &master_vol, ac->ahi.actrl );
      }

   }
   else {
#if 1
      if( ac->ap ) {
         AU_control( ac->ap, AUC_LVOL, ac->params.volume[ 0 ] );
         AU_control( ac->ap, AUC_RVOL, ac->params.volume[ 1 ] );
         ac->ap->l_vol = ac->params.volume[ 0 ]; // ???
         ac->ap->r_vol = ac->params.volume[ 1 ]; // ???
      }
#else
      if( ac->ap ) {
         Disable();
         ac->ap->l_vol = ac->params.volume[ 0 ];
         ac->ap->r_vol = ac->params.volume[ 1 ];
         ac->ap->flags &= ~(AUF_FILTER|AUF_FREQ|AUF_VOL);
         ac->ap->flags |= AUF_VOL;
         ac->ap->command = AUC_CONTROL;
         AU_write( ac->ap );
         ac->ap->flags &= ~AUF_VOL;
         Enable();
      }
#endif
   }
   return 0;
}


static int audio_play( AUM_CTRL *ac, BOOL on )
/*----------------------------------------------------
   Controls of audio play
   Return 0 if Ok
*/
{
   int err = 0;

   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      if( ac->ahi.actrl ) {
         err = AHI_ControlAudio( ac->ahi.actrl, AHIC_Play, on, TAG_DONE );
      }
   }
   else {
#if 1
      if( ac->ap ) AU_control(ac->ap, AUC_PLAY, (ULONG)on );
#else
      if( on ) {
         err = write_audio( ac, TRUE );
      }
      else {
         Disable();
         ac->ap->command = AUC_STOP;
         AU_write( ac->ap );
         Enable();
      }
#endif
   }
   return err;
}

static int audio_stop( AUM_CTRL *ac )
/*----------------------------------------------------
   Stop audio
   Return 0 if Ok
*/
{
   int err = 0;
   INT16 ch;

   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      if( ac->ahi.actrl ) {
         for( ch=0; ch<ac->params.channels; ch++ ) {
            AHI_SetSound( ch, AHI_NOSOUND, 0, 0, ac->ahi.actrl, AHISF_IMM );
         }
      }
   }
   else {
      if( ac->ap ) AU_control( ac->ap, AUC_STOP, (ULONG)TRUE );
   }

//   err = audio_play( ac, FALSE );
//   if( err ) return err;

   reset_audio( ac );
   return err;
}

static int set_audio_frequency( AUM_CTRL *ac  )
/*---------------------------------------------
   Controls of audio frequency
   Return 0 if Ok
*/
{
   int err = 0;

   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      INT16 ch;

      if( ac->ahi.actrl ) {
         for( ch=0; ch<ac->params.channels; ch++ ) {
            AHI_SetFreq( ch, (ULONG)ac->params.frequency, ac->ahi.actrl, AHISF_IMM );
         }
      }
   }
   else {
      if( ac->ap ) AU_control( ac->ap, AUC_FREQ, (ULONG)ac->params.frequency );
   }
   return err;
}

// AudioPort sound function

static SAVEDS void AUM_sound_func( void )
/*---------------------------------------
   Sound function called after each
   audio sample start
*/
{
#ifdef USE_AUDIO_PORT_SIGNAL // #4
   Signal( &audio_manager_process->pr_Task, 1<<actrl.audio_signal );
#else
//DEBUG_COLOR( 0xF00 );
   if( write_audio( &actrl, FALSE ) ) { // End of samples
      actrl.state = AUM_STA_STOP;
   }
#endif
}

// AHI sound function

static ASM SAVEDS ULONG SoundFunc( REG(a0) struct Hook *hook,
                            REG(a2) struct AHIAudioCtrl *ahi_ctrl,
                            REG(a1) struct AHISoundMessage *smsg )
/*----------------------------------------------------------------
*/
{
   INT16 ch;

   // We only need notify of last channel
   if( smsg->ahism_Channel != (actrl.params.channels-1) ) return 0;

   // Update the current playing buffer
//DEBUG_COLOR( 0xF00 );
   if( write_audio( &actrl, FALSE ) ) { // End of samples
#ifdef AUDIO_STEREO
      for( ch=0; ch<AUM_MAX_CHANNELS; ch++ ) {
#else
      for( ch=0; ch<actrl.params.channels; ch++ ) {
#endif
         AHI_SetSound( ch, AHI_NOSOUND, 0, 0, actrl.ahi.actrl, NULL );
         AHI_SetFreq( ch, 0, actrl.ahi.actrl, AHISF_IMM );
      }
      actrl.state = AUM_STA_STOP;
   }
   return 0;
}

struct Hook SoundHook = {
   0,0,
   SoundFunc,
   NULL,
   NULL,
};


static void close_AHI_device( AUM_CTRL *ac )
/*------------------------------------------
   Close AHI device & ressources
*/
{
   if( ac->ahi.open_count > 0 ) {
      ac->ahi.open_count--;
      if( ac->ahi.open_count == 0 ) {
         if( !ac->ahi.AHIDevice ) CloseDevice( (struct IORequest *)ac->ahi.AHIio );
         ac->ahi.AHIDevice = -1;
         if( ac->ahi.AHIio ) DeleteIORequest( (struct IORequest *)ac->ahi.AHIio );
         ac->ahi.AHIio = NULL;
         if( ac->ahi.AHImp ) DeleteMsgPort( ac->ahi.AHImp );
         ac->ahi.AHImp = NULL;
      }
   }
}

static int open_AHI_device( AUM_CTRL *ac )
/*----------------------------------------
   Open AHI device & ressources
*/
{
   if( ac->ahi.open_count == 0 ) {
      ac->ahi.AHImp = CreateMsgPort();
      if( !ac->ahi.AHImp ) return AUM_ERR_ALLOC;

      ac->ahi.AHIio = (struct AHIRequest *)CreateIORequest( ac->ahi.AHImp,
                                                            sizeof( struct AHIRequest ) );
      if( !ac->ahi.AHIio ) {
         DeleteMsgPort( ac->ahi.AHImp );
         ac->ahi.AHImp = NULL;
         return AUM_ERR_ALLOC;
      }
      ac->ahi.AHIio->ahir_Version = 4; // #1
      ac->ahi.AHIDevice = OpenDevice( AHINAME, AHI_NO_UNIT, (struct IORequest *)ac->ahi.AHIio, NULL );
      if( ac->ahi.AHIDevice ) {
         DeleteIORequest( (struct IORequest *)ac->ahi.AHIio );
         ac->ahi.AHIio = NULL;
         DeleteMsgPort( ac->ahi.AHImp );
         ac->ahi.AHImp = NULL;
         return AUM_ERR_DEVICE;
      }
      AHIBase = (struct Library *)ac->ahi.AHIio->ahir_Std.io_Device;
   }
   ac->ahi.open_count++;
   return AUM_ERR_NONE;
}




static int get_audio_mode( AUM_CTRL *ac, AUM_MODE *am )
/*-----------------------------------------------------
*/
{
   int err = AUM_ERR_NONE;
   struct AHIAudioModeRequester *amreq;

   if( am->device == AUM_DEVICE_AHI ) {
      err = open_AHI_device( ac );
      if( err ) return err;

      amreq = AHI_AllocAudioRequest( AHIR_PubScreenName,"",
                                     AHIR_TitleText,"Select an audio mode",
                                     AHIR_DoDefaultMode, TRUE,
                                     TAG_DONE );
      if( !amreq ) {
         err = AUM_ERR_ALLOC;
      }
      else {
         if( !AHI_AudioRequest( amreq, TAG_DONE ) ) { // Aborted
            err = AUM_ERR_ABORT;
         }
         else {
            am->id = amreq->ahiam_AudioID;
            if( !AHI_GetAudioAttrs( am->id, NULL,
                                    AHIDB_BufferLen, 256,
                                    AHIDB_Name, am->name ) ) {
               err = AUM_ERR_FAILED;
            }
         }
         AHI_FreeAudioRequest( amreq );
      }
      close_AHI_device( ac );
   }
   else {
      am->id = 0;
      strcpy( am->name, "PAULA (Stéphane TAVENARD)" );
   }

   return err;
}

static void close_AHI( AUM_CTRL *ac )
/*-----------------------------------
*/
{
   INT16 ch, bu;

   for( ch=0; ch<AUM_MAX_CHANNELS; ch++ ) {
        for( bu=0; bu<AHI_BUFFERS; bu++ ) {
           if( ac->ahi.asamp[ ch ][ bu ].ahisi_Address ) {
              free( ac->ahi.asamp[ ch ][ bu ].ahisi_Address );
              ac->ahi.asamp[ ch ][ bu ].ahisi_Address = NULL;
           }
        }
   }
   if( AHIBase ) {
      if( ac->ahi.actrl ) AHI_FreeAudio( ac->ahi.actrl );
      ac->ahi.actrl = NULL;
   }
   close_AHI_device( ac );
}

static int open_AHI( AUM_CTRL *ac )
/*---------------------------------
*/
{
   int err;
   ULONG samp_type; // #6
   UINT32 samp_size; // #6

   err = open_AHI_device( ac );
   if( err ) return err;

   if( ac->params.mixing <= 0 ) ac->params.mixing = ac->params.frequency;

   ac->ahi.actrl = AHI_AllocAudio(
              AHIA_AudioID, ac->params.mode.id,
              AHIA_MixFreq, ac->params.mixing,
#ifdef AUDIO_STEREO
              AHIA_Channels, AUDIO_MAX_CHANNELS,
#else
              AHIA_Channels, ac->params.channels,
#endif
              AHIA_Sounds, AHI_BUFFERS * ac->params.channels,
              AHIA_SoundFunc, &SoundHook,
              AHIA_UserData, FindTask( NULL ),
              TAG_DONE );
   if( !ac->ahi.actrl ) {
      close_AHI( ac );
      return AUM_ERR_ALLOC;
   }

   {
      ULONG playsamples, mixfreq;
      INT16 ch, bu;
      INT32 buffer_size; // #5

      AHI_GetAudioAttrs( AHI_INVALID_ID, ac->ahi.actrl,
                         AHIDB_MaxPlaySamples, &playsamples,
                         TAG_DONE );
      AHI_ControlAudio( ac->ahi.actrl, AHIC_MixFreq_Query, &mixfreq, TAG_DONE);

      // Calculate the minimum buffer size needed for current audio driver
#if 1
//      buffer_size = playsamples;
      if( mixfreq > 0 ) {
         buffer_size = (playsamples * ac->params.frequency) / mixfreq;
      }
      else {
         buffer_size = playsamples;
      }
#else
      if( ac->params.frequency > 0 ) {
         buffer_size = playsamples * mixfreq / ac->params.frequency;
      }
      else {
         buffer_size = playsamples;
      }
#endif
      buffer_size &= ~1; // Even size
      if( buffer_size > ac->buffer_size ) ac->buffer_size = buffer_size;
#if 0
{
   struct EasyStruct Req = {
      sizeof( struct EasyStruct ),
      0, "MPEGAPlayer message",
      "AHI: ac->buffer_size=%ld playsamples=%ld mixfreq=%ld", "Okay"
   };

   if( IntuitionBase ) {
      EasyRequest( NULL, &Req, 0, ac->buffer_size, playsamples, mixfreq );
   }
}
#endif

      // Allocate ahi audio buffers
      if( ac->params.bits == 8 ) { // #6
         samp_type = AHIST_M8S;
         samp_size = ac->buffer_size;
      }
      else {
         samp_type = AHIST_M16S;
         samp_size = ac->buffer_size<<1;
      }
      for( ch=0; ch<ac->params.channels; ch++ ) {
         for( bu=0; bu<AHI_BUFFERS; bu++ ) {
            ac->ahi.asamp[ ch ][ bu ].ahisi_Type = samp_type; // #6
            ac->ahi.asamp[ ch ][ bu ].ahisi_Length = ac->buffer_size;
            ac->ahi.asamp[ ch ][ bu ].ahisi_Address = malloc( samp_size ); // #6
            if( !ac->ahi.asamp[ ch ][ bu ].ahisi_Address ) {
               close_AHI( ac );
               return AUM_ERR_ALLOC;
            }
         }
      }

      // "Load" samples now
      for( ch=0; ch<ac->params.channels; ch++ ) {
         for( bu=0; bu<AHI_BUFFERS; bu++ ) {
            if( AHI_LoadSound( (ch * AHI_BUFFERS) + bu,
                               AHIST_DYNAMICSAMPLE, &ac->ahi.asamp[ ch ][ bu ], ac->ahi.actrl ) ) {
               close_AHI( ac );
               return AUM_ERR_ALLOC;
            }
         }
      }

      // Audio setup
#ifdef AUDIO_STEREO
      for( ch=0; ch<AUDIO_MAX_CHANNELS; ch++ ) {
#else
      for( ch=0; ch<ac->params.channels; ch++ ) {
#endif
//         AHI_SetFreq( ch, ac->params.frequency, ac->ahi.actrl, AHISF_IMM );
         AHI_SetFreq( ch, 0, ac->ahi.actrl, AHISF_IMM );
      }

      set_audio_volume( ac );

      // Start play ?
      if( AHI_ControlAudio( ac->ahi.actrl, AHIC_Play, TRUE, TAG_DONE ) ) {
         close_AHI( ac );
         return AUM_ERR_START;
      }
   }

   return 0;
}

static void audio_close( AUM_CTRL *ac )
/*-------------------------------------
   Close the current audio port
*/
{
   INT16 ch;

   if( ac->params.mode.device == AUM_DEVICE_AHI ) {
      close_AHI( ac );
   }
   else {
      if( ac->ap ) {
         ac->ap->command = AUC_STOP;
         AU_write( ac->ap );
         AU_close( ac->ap );
         ac->ap = NULL;
      }
   }

   for( ch=0; ch<AUM_MAX_CHANNELS; ch++ ) {
      if( ac->sample[ ch ] ) free( ac->sample[ ch ] );
      ac->sample[ ch ] = NULL;
   }
}

static int audio_open( AUM_CTRL *ac )
/*-----------------------------------
   Open the audio port according to
   the AUM_CTRL
*/
{
   INT16 ch;
   UINT32 alloc_size; // #6

   audio_close( ac );

#ifdef DYNAMIC_BUFFER_SIZE   // #5
   ac->buffer_size = ac->params.frequency / 50;
   if( ac->buffer_size < 256 ) ac->buffer_size = 256;
   ac->buffer_size &= ~1;
#else
   ac->buffer_size = AUM_BUFFER_SIZE;
#endif
   if( ac->params.audio_size < (2 * ac->buffer_size) ) return AUM_ERR_PARAMS;

   ac->sample_size = ac->params.audio_size;
   if( ac->params.bits == 8 ) { // #6
      alloc_size = ac->sample_size;
   }
   else {
      alloc_size = ac->sample_size<<1;
   }
   for( ch=0; ch<ac->params.channels; ch++ ) {
      ac->sample[ ch ] = malloc( alloc_size ); // #6
      if( !ac->sample[ ch ] ) {
         audio_close( ac );
         return AUM_ERR_ALLOC;
      }
   }


   if( ac->params.mode.device == AUM_DEVICE_AHI ) { // AHI
      return open_AHI( ac );
   }
   else { // Audio Port
      AU_PORT *ap;
      UBYTE flags;
      AUM_PARAMS *p = &ac->params;

      if( p->bits == 8 ) { // #6
         flags = 0;
      }
      else {
         flags = AUF_16BITS;
      }
      if( p->channels > 1 ) flags |= AUF_STEREO;

      ap = AU_open( flags, ac->buffer_size );
      if( !ap ) return AUM_ERR_ALLOC;
      ac->ap = ap;

      ap->frequency = p->frequency;
      ap->l_vol = p->volume[ 0 ];
      ap->r_vol = p->volume[ 1 ];
      ap->flags = AUF_FILTER | AUF_FREQ | AUF_VOL | AUF_NOWAIT;
      ap->mixing_frequency = 0;

      if( p->mixing > 0 ) {
         if( p->mixing > 48000 ) p->mixing = 48000;
         else if( p->mixing < 8000 ) p->mixing = 8000;
         ap->mixing_frequency = p->mixing;
         ap->flags |= AUF_MIXING;
      }
      if( p->filter == AUM_FILTER_OFF ) ap->filter_on = FALSE;
      else if( p->filter == AUM_FILTER_ON ) ap->filter_on = TRUE;
      else if( p->filter == AUM_FILTER_AUTO ) {
         if( p->mixing > 0 ) ap->filter_on = (p->mixing < 14000)?TRUE:FALSE;
         else ap->filter_on = (p->frequency < 14000)?TRUE:FALSE;
      }
      else {
         ap->flags &= ~AUF_FILTER; // Don't set filter
      }
      ap->command = AUC_CONTROL;
      ap->sound_func = (APTR)AUM_sound_func;
      AU_write( ap );
   }

   return 0;
}


static SAVEDS void AUM_manager( void )
/*------------------------------------
   Audio manager task
*/
{
   ULONG signals;
   ULONG audio_signal_mask, cmd_signal_mask;
   BOOL next_audio = FALSE;
   BOOL cmd_reply = FALSE;
   AUM_WRITE_MSG *audio_message = NULL;

   actrl.ap = NULL;
#ifdef USE_AUDIO_PORT_SIGNAL // #4
   actrl.audio_signal = -1;
#endif
   actrl.msg_port = (struct MsgPort *)CreateMsgPort();
   if( !actrl.msg_port ) {
      actrl.state = AUM_STA_ERROR;
      return;
   }

   cmd_signal_mask = 1<<actrl.msg_port->mp_SigBit;

#ifdef USE_AUDIO_PORT_SIGNAL // #4
   actrl.audio_signal = AllocSignal( -1 ); // #4
   if( actrl.audio_signal == -1 ) {
      DeleteMsgPort( actrl.msg_port );
      actrl.msg_port = NULL;
      actrl.state = AUM_STA_ERROR;
      return;
   }
#endif

   actrl.state = AUM_STA_INIT;
   actrl.wait_message = NULL;
   reset_audio( &actrl );

   do {
#ifdef USE_AUDIO_PORT_SIGNAL // #4
      audio_signal_mask = 1<<actrl.audio_signal;
#else
      if( actrl.ap ) audio_signal_mask = 1<<actrl.ap->signal;
      else  audio_signal_mask = 0;
#endif
      signals = Wait( audio_signal_mask | cmd_signal_mask | SIGBREAKF_CTRL_C );

      if( signals & audio_signal_mask ) { // audio buffer was started
         next_audio = TRUE;
      }

      if( signals & cmd_signal_mask ) {
         audio_message = (AUM_WRITE_MSG *)GetMsg( actrl.msg_port );

         ObtainSemaphore( &actrl.data_semaphore );
#if 0
#ifdef USE_AUDIO_PORT_SIGNAL // #4
         Forbid();
#else
         Disable();
#endif
#endif

         cmd_reply = TRUE;
         actrl.error = AUM_ERR_NONE;

         if( actrl.state != AUM_STA_INIT ) {
            if( audio_message->sample.size > actrl.sample_size ) { // Arrrg...
               actrl.error = AUM_ERR_WRITE;
            }
            else {
               if( actrl.state == AUM_STA_STOP ) { // Audio has stopped because ran out of samples
                  if( write_sample( &actrl, &audio_message->sample ) ) {
                     actrl.error = AUM_ERR_WRITE;
                  }
                  else { // if audio buffer is big enough -> start play
//                     if( actrl.samples_remain >= (actrl.buffer_size<<1) ) {
                     if( actrl.samples_remain >= (actrl.buffer_size<<1) ) {
                        actrl.state = AUM_STA_PLAY;
                        next_audio = TRUE;
                     }
                  }
               }
               else {
                  if( write_sample( &actrl, &audio_message->sample ) ) {
                     actrl.wait_message = audio_message;
                     cmd_reply = FALSE;
                  }
               }
            }
         }
         else {
            actrl.error = AUM_ERR_WRITE;
         }
#if 0
#ifdef USE_AUDIO_PORT_SIGNAL // #4
         Permit();
#else
         Enable();
#endif
#endif
         ReleaseSemaphore( &actrl.data_semaphore );
      }

      if( signals & SIGBREAKF_CTRL_C ) { // Break signal
         actrl.state = AUM_STA_QUIT;
         next_audio = FALSE;
      }

      if( next_audio ) {
         if( write_audio( &actrl, TRUE ) ) { // End of samples
            actrl.state = AUM_STA_STOP;
         }
         next_audio = FALSE;
      }

      if( cmd_reply ) { // We have to reply to audio command
         ReplyMsg( (struct Message *)audio_message );
         cmd_reply = FALSE;
      }

   } while( actrl.state != AUM_STA_QUIT );

   audio_play( &actrl, FALSE );
//   audio_close( &actrl );  // #3 DO NOT CALL THIS, because this task has not called audio_open before
//   reset_audio( &actrl );  // #3 So Signals don't belong to this task !!!

#ifdef USE_AUDIO_PORT_SIGNAL // #4
   if( actrl.audio_signal != -1 ) {
      FreeSignal( actrl.audio_signal );
      actrl.audio_signal = -1;
   }
#endif

   if( actrl.msg_port ) {
      DeleteMsgPort( actrl.msg_port );
      actrl.msg_port = NULL;
   }

   Forbid();
   actrl.state = AUM_STA_NOTSTARTED;

} /* AUM_manager */

int AUM_query( AUM_MODE *aum_mode )
/*---------------------------------
   Query for an audio mode
   Input:
      aum_mode->device
   Output
      aum_mode->mode_id
      aum_mode->mode_name
   return 0 if Ok
*/
{
   if( !aum_mode ) return AUM_ERR_FAILED;

   return get_audio_mode( &actrl, aum_mode );
}

int AUM_config( AUM_PARAMS *aum_params )
/*--------------------------------------
   Configure audio
*/
{
   int err = 0;

   ObtainSemaphore( &actrl.data_semaphore );
   if( actrl.state == AUM_STA_INIT ) {
      actrl.params = *aum_params;
      err = audio_open( &actrl );
      if( !err ) {
         actrl.state = AUM_STA_STOP;
      }
   }
   ReleaseSemaphore( &actrl.data_semaphore );
   return err;
}

int AUM_write( AUM_SAMPLE *aum_sample )
/*-------------------------------------
   Write an audio sample
*/
{
   if( actrl.state == AUM_STA_NOTSTARTED ) return AUM_ERR_FAILED;

   // Send command
   am.sample = *aum_sample;
   PutMsg( actrl.msg_port, (struct Message *)&am );

   // Wait for reply
   WaitPort( am.msg.mn_ReplyPort );
   // Get message
   GetMsg( am.msg.mn_ReplyPort );
//   while( GetMsg( am.msg.mn_ReplyPort ) ) ;

   return actrl.error;

} /* AUM_write */

int AUM_control( AUM_CTRL_ID au_ctrl, ULONG value )
/*-------------------------------------------------
   Controls audio
*/
{
   int err = 0;

   if( actrl.state == AUM_STA_NOTSTARTED ) return AUM_ERR_FAILED;

   ObtainSemaphore( &actrl.data_semaphore );

   switch( au_ctrl ) {
      case AUM_CTRL_RESET:
         if( actrl.state != AUM_STA_INIT ) { // Stop & close audio
//            audio_play( &actrl, FALSE );
            audio_stop( &actrl );
            actrl.state = AUM_STA_INIT;
            audio_close( &actrl );
         }
         reset_audio( &actrl );
         break;
      case AUM_CTRL_STOP:
         if( actrl.state == AUM_STA_PLAY  ) { // Stop audio
            audio_stop( &actrl );
            actrl.state = AUM_STA_STOP;
         }
         break;
      case AUM_CTRL_PLAY:
         if( value ) {
            if( actrl.state == AUM_STA_INIT ) { // Open audio
               if( audio_open( &actrl ) ) { // Error
                  err = AUM_ERR_START;
               }
               else {
                  actrl.state = AUM_STA_STOP;
               }
            }
            if( actrl.state == AUM_STA_STOP ) {
               if( !write_audio( &actrl, TRUE ) ) { // Sample written
                  actrl.state = AUM_STA_PLAY;
               }
            }
            else if( actrl.state == AUM_STA_PAUSE ) {
               audio_play( &actrl, TRUE );
               actrl.state = AUM_STA_PLAY;
            }
         }
         else { // Stop
            if( (actrl.state == AUM_STA_PLAY) || (actrl.state == AUM_STA_STOP) ) {
               audio_play( &actrl, FALSE );
               actrl.state = AUM_STA_PAUSE;
            }
         }
         break;
      case AUM_CTRL_FREQ:
         actrl.params.frequency = value;
         if( actrl.state != AUM_STA_INIT ) {
            set_audio_frequency( &actrl );
         }
         break;
      case AUM_CTRL_LVOL:
         actrl.params.volume[ 0 ] = value;
         if( actrl.state != AUM_STA_INIT ) {
            set_audio_volume( &actrl );
         }
         break;
      case AUM_CTRL_RVOL:
         actrl.params.volume[ 1 ] = value;
         if( actrl.state != AUM_STA_INIT ) {
            set_audio_volume( &actrl );
         }
         break;
      case AUM_CTRL_REMAIN:
         if( value ) {
            *((INT32 *)value) = actrl.samples_remain;
         }
         break;
      case AUM_CTRL_STATE:
         if( value ) {
            *((INT32 *)value) = (INT32)actrl.state;
         }
         break;
      default:
         break;
   }

   ReleaseSemaphore( &actrl.data_semaphore );

   return err;

} /* AUM_control */

int AUM_close( void )
/*-------------------
*/
{
   if( !audio_opened ) return AUM_ERR_FAILED;

   // WE CLOSE AUDIO HERE
   (void)AUM_control( AUM_CTRL_RESET, TRUE );

   if( actrl.state != AUM_STA_NOTSTARTED ) {
      if( audio_manager_process ) { // Stop the process
         Signal( &audio_manager_process->pr_Task, SIGBREAKF_CTRL_C );
         // Wait for process to finish
         while( actrl.state != AUM_STA_NOTSTARTED ) {
            Delay( 1 );
         }
         audio_manager_process = NULL;
      }
   }

   if( actrl.reply_port ) DeleteMsgPort( actrl.reply_port );
   actrl.reply_port = NULL;

   audio_opened = FALSE;
   return AUM_ERR_NONE;
}

int AUM_open( void )
/*------------------
*/
{
   if( audio_opened ) return AUM_ERR_FAILED;
   audio_opened = TRUE;

   memset( &actrl, 0, sizeof( actrl ) );

   InitSemaphore( &actrl.data_semaphore );

   actrl.ahi.AHIDevice = -1;

   actrl.reply_port = (struct MsgPort *)CreateMsgPort();
   if( !actrl.reply_port ) {
      AUM_close();
      return AUM_ERR_ALLOC;
   }
   actrl.signal_task = FindTask( NULL ); // Task to signal
   actrl.state = AUM_STA_NOTSTARTED;

   am.msg.mn_Node.ln_Type = NT_MESSAGE;
   am.msg.mn_Node.ln_Pri = 0;
   am.msg.mn_Node.ln_Name = "AUM_Message";
   am.msg.mn_ReplyPort = actrl.reply_port;
   am.msg.mn_Length = sizeof( AUM_WRITE_MSG );

   // Launch the audio manager now
   audio_manager_process = CreateNewProcTags(
      NP_Entry, (void(*)())(AUM_manager),
      NP_Name, AUM_MANAGER_TASK_NAME,
      NP_Priority, AUM_MANAGER_TASK_PRI,
      NP_StackSize, 8000,
      TAG_END );

   if( !audio_manager_process ) {
      AUM_close();
      return AUM_ERR_ALLOC;
   }

   // Wait for AUM_manager to initialize

   while( actrl.state == AUM_STA_NOTSTARTED ) {
      Delay( 1 );
   }

   if( actrl.state == AUM_STA_ERROR ) {
      AUM_close();
      return AUM_ERR_FAILED;
   }

   return AUM_ERR_NONE;
}
