# MintAMP — Mini Internet Amiga Media Player

Classic AmigaOS m68k audio player with two GUI editions: MintAMP for ReAction/ClassAct and MintAMP-GT for GadTools. Both editions use the same Helix-based playback engine and support local media and HTTP/HTTPS internet radio, with modular AAC, FLAC, Ogg Vorbis, WMA, WAV and IFF decoders, Radio Browser search, ICY metadata, station artwork, and Paula audio output.

This started as an Amiga port of the Helix fixed-point MP3 decoder. It has grown into a small but surprisingly capable classic Amiga audio player for real hardware and emulators.

<img width="713" height="692" alt="image" src="https://github.com/user-attachments/assets/fcc2acc5-2900-47e7-b5aa-273fd0c4520f" />


```text
Classic AmigaOS • m68k • Paula • MP3 • AAC • FLAC • Ogg Vorbis • WMA • WAV • IFF-8SVX • HTTP/HTTPS radio • ReAction • GadTools
```

## Highlights

- Local MP3 playback using the Helix fixed-point decoder
- m68k optimised MP3 decode path for 030+ builds
- AAC-LC ADTS playback through external `aac.decoder`
- Optional AAC m68k helper paths with `AACASM=1`
- FLAC playback through external `flac.decoder`
- Ogg Vorbis playback through fixed-point Tremor in `ogg.decoder`
- Classic WMAv1/WMAv2 playback through `wma.decoder`
- PCM WAV playback through `wav.decoder`
- Amiga IFF-8SVX playback through `iff.decoder`
- Modular decoder loading
- ReAction/ClassAct GUI: `MintAMP`
- GadTools GUI: `MintAMP-GT`
- CLI playback mode: `amiga_mp3dec.fastexp`
- Direct HTTP and HTTPS internet radio with `RADIO=1 SSL=1`
- Optional AmiSSL certificate verification with `SSLCERTS=1`
- Radio Browser station search
- ICY metadata parsing and live title/artist updates
- Station name, genre, bitrate and content-type display
- JPEG, PNG, WebP, ICO and SVG station artwork support in both GUI editions
- Resilient radio buffering and reconnect handling
- Paula audio.device playback output
- Fast low-rate playback options for slower classic systems

## Project status

MintAMP is active classic Amiga development. Older development builds were named MiniAMP3/minimp3r; compatibility make targets are retained where practical.

Current focus areas:

- ReAction GUI polish
- GadTools GUI parity
- internet radio stability
- station artwork and logo handling
- decoder performance and compatibility improvements
- further m68k optimisation

## Screenshots

Screenshots are expected under:

```text
docs/screenshots/
```

Suggested screenshots:

- ReAction GUI playing a local MP3
- ReAction GUI showing Radio Browser results
- ReAction GUI playing HTTPS radio with ICY metadata and artwork
- GadTools GUI
- CLI playback output

## CLI / `fast030` edition

MintAMP also includes a command-line edition named `amiga_mp3dec.fastexp`. It uses the same Paula playback engine and external decoder modules as the GUI editions, without the ReAction/GadTools interface, station browser or artwork code.

The CLI edition is useful for Shell scripts, direct file/URL playback, decoder testing and lower-overhead setups.

Build the local-media CLI:

```sh
make -f Makefile.amiga fast030
```

Build it with HTTP radio support:

```sh
make -f Makefile.amiga radio030
```

Build it with HTTP and HTTPS/AmiSSL radio support:

```sh
make -f Makefile.amiga sslradio030
```

Example playback:

```text
amiga_mp3dec.fastexp --play music.mp3
amiga_mp3dec.fastexp --play music.flac
amiga_mp3dec.fastexp --play music.ogg
amiga_mp3dec.fastexp --play sample.wav
amiga_mp3dec.fastexp --play sample.iff
amiga_mp3dec.fastexp --play "https://example.com/direct-stream"
```

Useful playback controls include `--rate`, `--quality`, `--subband-cap`, `--mono`, `--stereo`, `--fake-stereo`, `--buffer-seconds`, `--volume` and `--fast-mem`. Run the binary without arguments to display the complete option list.

The `fast030` target name is retained for compatibility. The actual target CPU is selected with `CPU=00`, `20`, `30`, `40` or `60`; the release build uses the 68030-optimised path.

## Supported formats

| Format | Status | Notes |
|---|---:|---|
| MP3 | Working | Main supported format. Helix fixed-point decoder with m68k optimisation. |
| AAC-LC ADTS | Working | External `aac.decoder` module. ADTS `.aac` streams/files only. |
| FLAC | Working | External `flac.decoder` module. Performance depends heavily on CPU, output rate and file complexity. |
| Ogg Vorbis | Working | External `ogg.decoder` using the fixed-point Tremor decoder and libogg. |
| WMA | Experimental | External `wma.decoder` for classic WMAv1/WMAv2 audio in ASF containers. WMA Pro, Lossless and Voice are not supported. |
| WAV | Working | External `wav.decoder` for uncompressed PCM WAV: 8/16/24/32-bit integer, mono or stereo. |
| IFF-8SVX | Working | External `iff.decoder` for mono 8-bit samples, raw or Fibonacci-delta compressed. |
| HTTP MP3/AAC radio | Working | Direct `http://` MP3 and ADTS AAC/AAC+ streams. ICY metadata supported where provided. |
| HTTPS MP3/AAC radio | Working with AmiSSL | Build with `RADIO=1 SSL=1`. Uses AmiSSL and classic-Amiga-specific teardown quarantine for stability. |
| HTTPS certificate verification | Optional | Add `SSLCERTS=1` to use AmiSSL's installed CA certificates for peer/hostname verification. |
| Radio Browser search | Working | Used by the GUI radio search. |
| JPEG artwork | Working | picojpeg-compatible baseline decoder. |
| PNG artwork | Working | LodePNG decoder, compiled into both GUI editions. |
| WebP artwork | Working | Compact VP8/VP8L decoder for station artwork and favicons. |
| ICO artwork | Working | PNG-backed ICO and simple DIB-backed favicon entries. |
| SVG artwork | Working (subset) | Fixed-point `svgdec.c` renderer; see "Artwork notes" below. |
| HLS / M3U8 | Not supported | Out of scope currently. Direct stream URLs only. |

## Internet radio

Radio support is enabled at build time with `RADIO=1`.

Plain HTTP radio:

```sh
make -f Makefile.amiga radio030
```

HTTPS radio through AmiSSL:

```sh
make -f Makefile.amiga sslradio030
```

ReAction/ClassAct GUI with HTTPS radio:

```sh
make -f Makefile.amiga sslguir
```

GadTools GUI with HTTPS radio:

```sh
make -f Makefile.amiga sslgui
```

### AmiSSL certificates and `SSLCERTS=1`

By default, HTTPS radio uses TLS transport but does not verify the remote certificate chain or hostname. This keeps HTTPS playback compatible with older or incomplete classic Amiga AmiSSL setups.

Add `SSLCERTS=1` to enable certificate verification:

```sh
make -f Makefile.amiga sslguir SSLCERTS=1
```

`SSLCERTS=1` defines the certificate-verification path and tells MintAMP to use AmiSSL's installed CA certificate bundle. The Amiga-side AmiSSL install must have its current root certificates available, and the system clock must be sane, otherwise valid HTTPS streams may fail verification.

Useful certificate-verifying builds:

```sh
make -f Makefile.amiga sslradio030 SSLCERTS=1
make -f Makefile.amiga sslgui SSLCERTS=1
make -f Makefile.amiga sslguir SSLCERTS=1
```

Without `SSLCERTS=1`, HTTPS radio still uses TLS transport, but certificate verification is disabled for compatibility with classic Amiga setups.

### Debug/development build

The current heavy debug build used for radio, artwork, AmiSSL and heap-guard testing is:

```sh
make -f Makefile.amiga guir RADIO=1 SSL=1 SSLCERTS=1 DEBUG=1 HEAPGUARD=1
```

This enables:

- ReAction/ClassAct GUI build (`guir`)
- internet radio (`RADIO=1`)
- HTTPS/AmiSSL transport (`SSL=1`)
- AmiSSL certificate verification (`SSLCERTS=1`)
- verbose radio/AmiSSL/artwork diagnostics (`DEBUG=1`)
- MiniAMP heap guard instrumentation (`HEAPGUARD=1`)

Use this for debugging only. It is intentionally noisier and heavier than a normal release build.

### Known working radio streams

Plain HTTP MP3:

```text
http://ice1.somafm.com/groovesalad-128-mp3
```

HTTPS MP3:

```text
https://icecast.walmradio.com:8443/classic
```

Expected metadata includes station name, genre, bitrate, content type and live ICY `StreamTitle` updates where the station provides them.

## AmiSSL stability note

Classic AmigaOS/AmiSSL teardown can be fragile when rapidly switching HTTPS streams from short-lived playback child tasks.

MintAMP therefore uses a conservative stability pattern for HTTPS radio:

- the probe path reuses a shared probe `SSL_CTX` rather than creating/freeing one for every station probe
- probe SSL objects are quarantined on dangerous close/EOF paths
- playback child tasks skip/quarantine `SSL_free()`, `SSL_CTX_free()` and per-task `CleanupAmiSSL()` during stream teardown

This intentionally leaks small per-session AmiSSL objects during the app run. That is a trade-off for runtime stability on classic AmigaOS: repeated station switching previously reproduced delayed `Software Failure #80000008` crashes, while the quarantine path stopped the crash during repeated manual testing.

If you are changing the HTTPS radio code, do not remove this quarantine behaviour unless you have stress-tested repeated HTTPS station switching on the target AmigaOS/AmiSSL setup.

## Build requirements

- Bebbo m68k Amiga GCC toolchain
- `m68k-amigaos-gcc`
- `m68k-amigaos-nm`
- GNU Make
- Git with submodule support
- AmiSSL SDK headers for HTTPS radio builds
- AmiSSL root certificates for `SSLCERTS=1` certificate verification
- ReAction/ClassAct runtime classes for `MintAMP`
- `bsdsocket.library` at runtime for radio

Check the toolchain:

```sh
which m68k-amigaos-gcc
which m68k-amigaos-nm
m68k-amigaos-gcc --version | head -3
```

## Full build instructions

Full AmigaOS build instructions are in:

```text
docs/building-amiga.md
```

That document covers:

- clean repo sync
- submodule sync
- FLAC decoder build
- AAC decoder build and optional m68k helpers with `AACASM=1`
- Ogg Vorbis/Tremor decoder build and optional m68k helpers with `OGGASM=1`
- WAV, IFF-8SVX and WMA decoder builds
- radio builds with `RADIO=1`
- HTTPS/AmiSSL builds with `SSL=1`
- certificate-verifying HTTPS builds with `SSLCERTS=1`
- debug/heap-guard builds with `DEBUG=1 HEAPGUARD=1`
- decoder entrypoint checks
- copying files to Amiga/WinUAE
- runtime tests
- Git hygiene

## Quick build

From a clean checkout:

```sh
cd ~/Amiga-Programs/libhelix-mp3

git fetch origin --prune
git checkout master
git reset --hard origin/master
git clean -fd

git submodule sync --recursive
git submodule update --init --recursive
git submodule foreach --recursive 'git reset --hard && git clean -fd'

make -C decoders clean || true
find . -name "*.o" -delete
rm -f amiga_mp3dec.fastexp MintAMP MintAMP-GT
rm -f decoders/*.decoder decoders/*.decoder.map

make -C decoders flac
make -C decoders aac AACASM=1
make -C decoders ogg
make -C decoders wav
make -C decoders iff
make -C decoders wma
make -f Makefile.amiga sslguir
```

Useful alternative builds:

```sh
make -f Makefile.amiga fast030       # CLI/local playback focused build
make -f Makefile.amiga radio030      # CLI/radio build without HTTPS
make -f Makefile.amiga sslradio030   # CLI/radio build with HTTPS/AmiSSL
make -f Makefile.amiga gui RADIO=1   # GadTools GUI with radio
make -f Makefile.amiga guir RADIO=1  # ReAction GUI with radio
make -f Makefile.amiga sslgui        # GadTools GUI with HTTPS radio
make -f Makefile.amiga sslguir       # ReAction GUI with HTTPS radio/artwork
make -f Makefile.amiga sslguir SSLCERTS=1  # ReAction HTTPS radio with AmiSSL cert verification
make -f Makefile.amiga guir RADIO=1 SSL=1 SSLCERTS=1 DEBUG=1 HEAPGUARD=1  # heavy debug build
```

Verify decoder module entrypoints:

```sh
for module in aac flac ogg wav iff wma; do
  m68k-amigaos-nm -n "decoders/$module.decoder" | head -1
done
```

Every module should start with:

```text
00000000 T _DecoderModuleEntry
```

If anything appears before `_DecoderModuleEntry`, the module may crash when loaded.

## Runtime layout

Keep the player and decoder modules together.

Typical Amiga-side layout:

```text
MintAMP/
  MintAMP
  MintAMP-GT
  amiga_mp3dec.fastexp
  decoders/
    aac.decoder
    flac.decoder
    ogg.decoder
    wma.decoder
    wav.decoder
    iff.decoder
```

Depending on the build target/front-end, the executable may be:

```text
amiga_mp3dec.fastexp   CLI/local and radio playback
MintAMP-GT             GadTools GUI
MintAMP                ReAction/ClassAct GUI
```

## Runtime tests

Local MP3:

```text
amiga_mp3dec.fastexp --play test.mp3
```

AAC:

```text
amiga_mp3dec.fastexp --play test.aac
```

FLAC:

```text
amiga_mp3dec.fastexp --play test.flac
```

Ogg Vorbis:

```text
amiga_mp3dec.fastexp --play test.ogg
```

Classic WMA in ASF:

```text
amiga_mp3dec.fastexp --play test.wma
```

PCM WAV:

```text
amiga_mp3dec.fastexp --play test.wav
```

Amiga IFF-8SVX:

```text
amiga_mp3dec.fastexp --play test.iff
```

HTTP MP3 radio:

```text
amiga_mp3dec.fastexp --play "http://ice1.somafm.com/groovesalad-128-mp3"
```

HTTPS MP3 radio:

```text
amiga_mp3dec.fastexp --play "https://icecast.walmradio.com:8443/classic"
```

## Decoder modules

MintAMP's external decoder modules are:

| Module | Format / implementation |
|---|---|
| `aac.decoder` | Helix AAC through ESP8266Audio |
| `flac.decoder` | libfoxenflac |
| `ogg.decoder` | Xiph.Org Tremor and libogg |
| `wma.decoder` | Rockbox fixed-point libwma plus MintAMP's ASF demuxer |
| `wav.decoder` | MintAMP RIFF/PCM decoder |
| `iff.decoder` | MintAMP IFF-8SVX decoder |

Every external decoder module must export `DecoderModuleEntry` as its first real text symbol.

Required check:

```sh
for module in aac flac ogg wav iff wma; do
  m68k-amigaos-nm -n "decoders/$module.decoder" | head -1
done
```

Expected for every module:

```text
00000000 T _DecoderModuleEntry
```

This is important because the Amiga module loader expects to enter the decoder module at the correct offset. If compiler helper or library code appears before `DecoderModuleEntry`, the module can jump into the wrong code and crash.

## AAC notes

AAC support currently targets AAC-LC ADTS files and streams.

The AAC decoder uses the `decoders/esp8266audio` submodule, with the AAC source under:

```text
decoders/esp8266audio/src/libhelix-aac
```

The expected local symlink is:

```text
decoders/aac -> esp8266audio/src/libhelix-aac
```

Build AAC normally:

```sh
make -C decoders aac
```

Build AAC with optional m68k asm helpers:

```sh
make -C decoders aac AACASM=1
```

`AACASM=1` enables optional m68k helper paths for:

```text
AMIGA_M68K_ASM_AAC_HUFFMAN
AMIGA_M68K_ASM_AAC_DEQUANT
AMIGA_M68K_ASM_AAC_STEREO
AMIGA_M68K_ASM_AAC_IMDCT
```

The plain C fallback remains available by building without `AACASM=1`.

## FLAC notes

FLAC support is provided by `flac.decoder`.

Build:

```sh
make -C decoders flac
```

FLAC is heavier than MP3 and performance depends on CPU, file complexity, output rate and playback settings.

## Ogg Vorbis notes

Ogg Vorbis support is provided by `ogg.decoder`, using Xiph.Org's fixed-point Tremor decoder through Phil Schatzmann's Arduino port together with Xiph.Org libogg.

Build:

```sh
make -C decoders ogg
```

The default build applies MintAMP's optional m68k optimisation patch. Use `OGGASM=0` to build the portable C path.

## WAV, IFF-8SVX and WMA notes

The WAV and IFF modules are compact decoders written for MintAMP:

- `wav.decoder` accepts uncompressed integer PCM WAV files with 8, 16, 24 or 32-bit samples, mono or stereo
- `iff.decoder` accepts classic mono IFF-8SVX samples, both raw and Fibonacci-delta compressed

The WMA module accepts classic WMAv1 and WMAv2 codec streams in ASF containers. Its fixed-point codec core comes from Rockbox's libwma and is derived from FFmpeg; the ASF demuxer and MintAMP module integration are project-specific. WMA Pro, Lossless, Voice and other ASF-contained codecs are out of scope.

Build individually:

```sh
make -C decoders wav
make -C decoders iff
make -C decoders wma
```

## Artwork notes

Both GUI editions can fetch and display station artwork where the station or Radio Browser metadata provides a usable image URL.

Supported artwork decode paths:

- JPEG through a small picojpeg-compatible baseline decoder; the original picojpeg API/design is by Rich Geldreich
- PNG through vendored LodePNG by Lode Vandevenne
- WebP through `webpdec.c`, a compact VP8/VP8L implementation using portions of Google's libwebp reference decoder under its BSD licence
- ICO favicon files, including PNG-backed entries and simple DIB-backed icons
- SVG through `svgdec.c`, a fixed-point subset renderer designed for small classic-Amiga artwork

The SVG renderer supports the common paths, shapes, transforms, solid fills, simple strokes and limited gradient handling needed by station logos. It intentionally omits heavyweight browser features such as filters, external images, text layout, CSS stylesheets, masks and full animation. See `svgdec.h` and the source comments for the exact supported subset.

Artwork support is available in both GUI editions. The CLI build does not include or need the artwork decoders.

## Development notes

Do not commit generated files:

```text
*.o
*.decoder
*.decoder.map
*.fastexp
MintAMP-GT
MintAMP
test audio files
*:Zone.Identifier
```

Before committing:

```sh
git status --short
```

Recommended final test checklist before pushing decoder/player changes:

```text
MP3 local playback
AAC local playback
AAC TNS-heavy file
FLAC local playback
Ogg Vorbis local playback
WMA local playback
WAV local playback
IFF-8SVX local playback
HTTP MP3 radio playback
HTTPS MP3 radio playback
HTTPS certificate verification with SSLCERTS=1
Radio Browser search
ICY title/artist metadata updates
Station artwork display where provided
Stop on radio stream
Menus after stopping radio
Restart radio after stopping
Repeated manual HTTPS station switching
DecoderModuleEntry still at offset 0
```

## Credits

MintAMP's Amiga port, GUI front-ends, decoder-module integration, internet radio, artwork integration and m68k optimisation are by Darren Banfi (boingball).

Audio decoder components:

- MP3: Helix fixed-point decoder from RealNetworks
- AAC: Helix AAC through [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) by Earle F. Philhower III
- FLAC: [libfoxenflac](https://github.com/astoeckel/libfoxenflac) by Alexander Stoeckel
- Ogg Vorbis: Xiph.Org [Tremor](https://gitlab.xiph.org/xiph/tremor) through [Phil Schatzmann's Arduino port](https://github.com/pschatzmann/arduino-libvorbis-tremor), together with Xiph.Org [libogg](https://github.com/xiph/ogg)
- WMA: Rockbox fixed-point libwma, derived from FFmpeg's WMA decoder

Artwork decoder components:

- JPEG: picojpeg-compatible decoder; [picojpeg](https://github.com/richgel999/picojpeg) was created by Rich Geldreich
- PNG: [LodePNG](https://github.com/lvandeve/lodepng) by Lode Vandevenne
- WebP: compact MintAMP implementation using portions of Google's libwebp reference decoder

The MintAMP-specific ASF/WMA integration, WAV/PCM and IFF-8SVX decoders, and ICO, SVG and compact WebP artwork handlers were developed with assistance from Anthropic Claude.

## Licence

MintAMP combines components under several licences, including the Helix RPSL, LGPL, BSD-style and LodePNG terms. See the repository licence files and the included/submodule source trees for the complete terms. Third-party copyright and licence notices must be retained when redistributing binaries or source.
