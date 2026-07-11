Decoders for MiniAMP3

Flac - https://github.com/astoeckel/libfoxenflac - 	
Re-license as LGPLv2.1 or later

WAV - Plain RIFF/PCM — IMPLEMENTED, see wav_module.c
  Hand-written (no third-party library): 8/16/24/32-bit integer PCM, mono
  or stereo, classic and WAVE_FORMAT_EXTENSIBLE 'fmt ' headers. IEEE-float
  WAVs are rejected at open() (not decoded).

IFF/8SVX - Amiga "8-bit Voice" sample format — IMPLEMENTED, see iff_module.c
  Hand-written (no third-party library): mono, 8-bit signed PCM,
  sCompression 0 (raw) or 1 (Fibonacci-delta). Registers extensions
  8svx/iff/svx.

AAC - https://github.com/earlephilhower/ESP8266Audio/blob/master/src/libhelix-aac/aacdec.c
LibHelix AAC Decoder
Source: place libhelix-aac files in decoders/aac/ (see Makefile for instructions)

ALAC  - https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/alac.c
FFMPEG ALAC

OGG - Tremor (fixed-point Vorbis) + libogg — IMPLEMENTED, see ogg_module.c
  https://github.com/pschatzmann/arduino-libvorbis-tremor (upstream: https://gitlab.xiph.org/xiph/tremor)
  https://github.com/xiph/ogg
BSD-style license, same family as libogg and libvorbis, see LICENSE.txt for information.
Source: place Tremor in decoders/tremor/ and libogg in decoders/libogg/ (see Makefile
for submodule instructions). Chosen over https://github.com/edubart/minivorbis because
Tremor is integer/fixed-point only, matching the FLAC (libfoxenflac) and AAC
(libhelix-aac) decoders already in this project — no FPU dependency.

OPUS - https://github.com/xiph/opus/blob/main/src/opus_decoder.c
Also vendored via the ESP8266Audio submodule under src/libopus. Considered instead of
Tremor for OGG support and rejected for now: it's floating-point by default, and even
its --enable-fixed-point build is the full modern SILK+CELT reference codebase — a much
bigger port than this project's other decoders, with real-time decode on 68000/68020
uncertain.

M4A - Too much?

WMA - Classic WMA v1/v2 — IMPLEMENTED, see wma_module.c
  Vendored from Rockbox's fixed-point libwma (lib/rbcodec/codecs/{libwma,lib},
  https://www.rockbox.org/ — LGPLv2.1+, derived from FFmpeg's WMA decoder),
  not from FFmpeg directly: FFmpeg's libavcodec/wmadec.c is tightly coupled
  to its own framework (AVCodecContext, get_bits/vlc/mdct infra spread across
  libavcodec/libavutil) and isn't designed to be extracted standalone, whereas
  Rockbox already did that decoupling work for the same kind of embeddable,
  fixed-point, no-OS bare-metal target this project has (Coldfire/ARM/SH DAPs)
  — the same reasoning that led this project to libfoxenflac over reference
  libFLAC and Tremor over full libvorbis. Source: decoders/wma/ (wmadeci.c,
  wmafixed.c, mdct.c, mdct_lookup.c, fft-ffmpeg.c, ffmpeg_bitstream.c, plus
  wmadata.h/wmadec.h/wmafixed.h/etc.); platform.h, codecs.h and codeclib.h in
  that directory are this project's own minimal adaptation shims, not
  upstream Rockbox. WMA Pro / WMA Lossless / WMA Voice are out of scope.
  ASF container demuxing (WMA files are ASF containers) is original code —
  see wma_asf.c — since Rockbox's own ASF parsing is split across its
  buffered-file/id3-metadata layer in a way that doesn't map onto this
  project's DecoderReadCb/DecoderSeekCb I/O model.

WEBM - https://github.com/SAV1-org/SAV1

