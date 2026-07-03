# MiniAMP3 for Classic AmigaOS

Classic AmigaOS m68k audio player with local MP3 playback, modular AAC/FLAC decoders, ReAction and GadTools front-ends, Radio Browser search, ICY metadata, station artwork, and direct HTTP/HTTPS internet radio.

This started as an Amiga port of the Helix fixed-point MP3 decoder. It has grown into a small but surprisingly capable classic Amiga audio player for real hardware and emulators.

```text
Classic AmigaOS • m68k • Paula • MP3 • AAC • FLAC • HTTP/HTTPS radio • ReAction • GadTools
```

## Highlights

- Local MP3 playback using the Helix fixed-point decoder
- m68k optimised MP3 decode path for 030+ builds
- AAC-LC ADTS playback through external `aac.decoder`
- Optional AAC m68k helper paths with `AACASM=1`
- FLAC playback through external `flac.decoder`
- Modular decoder loading
- ReAction/ClassAct GUI: `minimp3r`
- GadTools GUI: `miniamp3`
- CLI playback mode: `amiga_mp3dec.fastexp`
- Direct HTTP and HTTPS internet radio with `RADIO=1 SSL=1`
- Optional AmiSSL certificate verification with `SSLCERTS=1`
- Radio Browser station search
- ICY metadata parsing and live title/artist updates
- Station name, genre, bitrate and content-type display
- JPEG and PNG station artwork support in the ReAction front-end
- Resilient radio buffering and reconnect handling
- Paula audio.device playback output
- Fast low-rate playback options for slower classic systems

## Project status

MiniAMP3 is active classic Amiga development.

Current focus areas:

- ReAction GUI polish
- GadTools GUI parity
- internet radio stability
- station artwork and logo handling
- AAC/FLAC performance improvements
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

## Supported formats

| Format | Status | Notes |
|---|---:|---|
| MP3 | Working | Main supported format. Helix fixed-point decoder with m68k optimisation. |
| AAC-LC ADTS | Working | External decoder module. ADTS `.aac` streams/files only. |
| FLAC | Working | External decoder module. Performance depends heavily on CPU, output rate and file complexity. |
| HTTP MP3/AAC radio | Working | Direct `http://` MP3 and ADTS AAC/AAC+ streams. ICY metadata supported where provided. |
| HTTPS MP3/AAC radio | Working with AmiSSL | Build with `RADIO=1 SSL=1`. Uses AmiSSL and classic-Amiga-specific teardown quarantine for stability. |
| HTTPS certificate verification | Optional | Add `SSLCERTS=1` to use AmiSSL's installed CA certificates for peer/hostname verification. |
| Radio Browser search | Working | Used by the GUI radio search. |
| JPEG artwork | Working | Station logo/artwork support in `minimp3r`. |
| PNG artwork | Working | `lodepng` is compiled into the ReAction front-end only. |
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

`SSLCERTS=1` defines the certificate-verification path and tells MiniAMP3 to use AmiSSL's installed CA certificate bundle. The Amiga-side AmiSSL install must have its current root certificates available, and the system clock must be sane, otherwise valid HTTPS streams may fail verification.

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

MiniAMP3 therefore uses a conservative stability pattern for HTTPS radio:

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
- ReAction/ClassAct runtime classes for `minimp3r`
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
- AAC decoder build
- AAC asm build with `AACASM=1`
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
rm -f amiga_mp3dec.fastexp miniamp3 minimp3r
rm -f decoders/*.decoder decoders/*.decoder.map

make -C decoders flac
make -C decoders aac AACASM=1
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
m68k-amigaos-nm -n decoders/aac.decoder | head -10
m68k-amigaos-nm -n decoders/flac.decoder | head -10
```

Both should start with:

```text
00000000 T _DecoderModuleEntry
```

If anything appears before `_DecoderModuleEntry`, the module may crash when loaded.

## Runtime layout

Keep the player and decoder modules together.

Typical Amiga-side layout:

```text
MiniAMP3/
  minimp3r
  miniamp3
  amiga_mp3dec.fastexp
  decoders/
    aac.decoder
    flac.decoder
```

Depending on the build target/front-end, the executable may be:

```text
amiga_mp3dec.fastexp   CLI/local and radio playback
miniamp3               GadTools GUI
minimp3r               ReAction/ClassAct GUI
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

HTTP MP3 radio:

```text
amiga_mp3dec.fastexp --play "http://ice1.somafm.com/groovesalad-128-mp3"
```

HTTPS MP3 radio:

```text
amiga_mp3dec.fastexp --play "https://icecast.walmradio.com:8443/classic"
```

## Decoder modules

External decoder modules must export `DecoderModuleEntry` as the first real text symbol.

Required check:

```sh
m68k-amigaos-nm -n decoders/aac.decoder | head -10
m68k-amigaos-nm -n decoders/flac.decoder | head -10
```

Expected:

```text
00000000 T _DecoderModuleEntry
```

This is important because the Amiga module loader expects to enter the decoder module at the correct offset. If compiler helper code or library code appears before `DecoderModuleEntry`, the module can jump into the wrong code and crash.

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

## Artwork notes

The ReAction/ClassAct front-end can fetch and display station artwork where the radio station or Radio Browser metadata provides a usable logo URL.

Supported artwork decode paths:

- JPEG through `picojpeg`
- PNG through vendored `lodepng`
- ICO favicon files, including PNG-backed ICO entries and simple DIB-backed icons

Artwork support is intentionally wired into `minimp3r` only. The GadTools and CLI builds do not need PNG/JPEG/ICO artwork support.

## Development notes

Do not commit generated files:

```text
*.o
*.decoder
*.decoder.map
*.fastexp
miniamp3
minimp3r
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

This project builds on fixed-point decoder work from the Helix family of decoders and related open-source audio decoder code.

Amiga port, decoder module integration, GUI work, radio streaming, artwork integration and m68k optimisation work by boingball.

## Licence

See the repository licence and the licences of included/submodule decoder sources.
