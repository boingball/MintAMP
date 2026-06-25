# Building and verifying the AmigaOS / m68k player

This page documents the full known-good build flow for a fresh development machine or an existing checkout that needs to be synced, rebuilt, verified, copied to an Amiga, and runtime-tested.

The current Amiga player supports:

* local MP3 playback;
* local FLAC playback through `flac.decoder`;
* local AAC-LC ADTS playback through `aac.decoder`;
* optional AAC m68k assembly helpers with `AACASM=1`;
* HTTP MP3 internet radio with `RADIO=1`;
* ICY radio metadata display in the GUI;
* ReAction and GadTools front-ends.

## 1. Clean sync from existing checkout

Use this when you already have a development checkout and want it to match the current upstream `master` branch exactly:

```sh
cd ~/Amiga-Programs/libhelix-mp3

git fetch origin --prune
git checkout master
git reset --hard origin/master
git clean -fd
```

> **Warning:** `git reset --hard origin/master` discards tracked local changes, and `git clean -fd` deletes untracked files and directories. Do not run this destructive sync if you need to keep local work, test audio, generated files, or experimental changes.

Safer pre-flight version:

```sh
cd ~/Amiga-Programs/libhelix-mp3

git status --short
git stash push -u -m "temp before build sync"

git fetch origin --prune
git checkout master
git reset --hard origin/master
git clean -fd
```

The `git stash push -u` command saves tracked and untracked files before the destructive reset/clean. Review the stash later before dropping it.

## 2. Submodule sync

AAC source comes from the ESP8266Audio submodule:

```text
decoders/esp8266audio
```

The `decoders/aac` path is expected to point at the libhelix AAC source inside that submodule:

```text
decoders/aac -> esp8266audio/src/libhelix-aac
```

After syncing the parent repository, also sync, initialize, reset, and clean submodules:

```sh
git submodule sync --recursive
git submodule update --init --recursive
git submodule foreach --recursive 'git reset --hard && git clean -fd'
```

Verify the parent and AAC submodule state:

```sh
git status --short --branch
git submodule status decoders/esp8266audio
```

The parent checkout should be clean, and `decoders/esp8266audio` should be present at the commit recorded by the parent repository.

## 3. Toolchain check

The Bebbo m68k Amiga GCC toolchain is required. Confirm that the compiler and symbol tools are on `PATH` before building:

```sh
which m68k-amigaos-gcc
which m68k-amigaos-nm
m68k-amigaos-gcc --version | head -3
```

If either `which` command fails, install or activate the Bebbo toolchain before continuing.

## 4. Clean build artefacts

Remove stale decoder objects, decoder binaries, maps, and main player outputs before a known-good rebuild:

```sh
make -C decoders clean || true
find . -name "*.o" -delete
rm -f amiga_mp3dec.fastexp
rm -f minimp3r
rm -f decoders/*.decoder
rm -f decoders/*.decoder.map
```

## 5. Build FLAC decoder

Build the FLAC decoder module:

```sh
make -C decoders flac
```

Verify the module entrypoint:

```sh
m68k-amigaos-nm -n decoders/flac.decoder | head -10
```

Expected first real symbol:

```text
00000000 T _DecoderModuleEntry
```

If anything appears before `_DecoderModuleEntry`, the module can crash when loaded because the Amiga-side loader may enter the wrong code address.

## 6. Build AAC decoder

The AAC decoder can be built with the normal C implementation or with optional m68k assembly helpers.

### Normal C AAC build

```sh
make -C decoders clean || true
find decoders -name "*.o" -delete
rm -f decoders/aac.decoder decoders/aac.decoder.map

make -C decoders aac
```

Building without `AACASM=1` keeps the C fallback path.

### AAC m68k assembly helper build

```sh
make -C decoders clean || true
find decoders -name "*.o" -delete
rm -f decoders/aac.decoder decoders/aac.decoder.map

make -C decoders aac AACASM=1
```

`AACASM=1` enables the optional m68k AAC helpers:

* `AMIGA_M68K_ASM_AAC_HUFFMAN`
* `AMIGA_M68K_ASM_AAC_DEQUANT`
* `AMIGA_M68K_ASM_AAC_STEREO`
* `AMIGA_M68K_ASM_AAC_IMDCT`

After either AAC build, verify the module entrypoint:

```sh
m68k-amigaos-nm -n decoders/aac.decoder | head -10
```

Expected first real symbol:

```text
00000000 T _DecoderModuleEntry
```

If anything appears before `_DecoderModuleEntry`, the module can crash when loaded because the Amiga-side loader may enter the wrong code address.

## 7. Build main player

Build the normal fast 030 Amiga player:

```sh
make -f Makefile.amiga fast030
```

Build the radio-enabled fast 030 Amiga player:

```sh
make -f Makefile.amiga fast030 RADIO=1
```

`RADIO=1` enables HTTP MP3 streaming. It requires `bsdsocket.library` at runtime on the Amiga.

## 8. Quick full build block

The following condensed sequence performs a destructive sync, cleans build artefacts, builds the known-good FLAC decoder, builds the AAC decoder with optional m68k assembly helpers, builds the radio-enabled fast 030 player, and verifies decoder entrypoints:

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
rm -f amiga_mp3dec.fastexp minimp3r
rm -f decoders/*.decoder decoders/*.decoder.map

make -C decoders flac
make -C decoders aac AACASM=1
make -f Makefile.amiga fast030 RADIO=1

m68k-amigaos-nm -n decoders/aac.decoder | head -10
m68k-amigaos-nm -n decoders/flac.decoder | head -10
```

Both decoder checks should start with:

```text
00000000 T _DecoderModuleEntry
```

## 9. Files to copy to Amiga

Expected runtime layout on the Amiga:

```text
libhelix-mp3/
  minimp3r
  amiga_mp3dec.fastexp
  decoders/
    aac.decoder
    flac.decoder
```

The exact executable may depend on the selected build target/front-end. Copy the player executable you built, plus the decoder modules needed for the file types you want to play.

## 10. Runtime tests

### Local playback tests

Run local MP3, AAC-LC ADTS, and FLAC playback tests on the Amiga:

```sh
amiga_mp3dec.fastexp --play test.mp3
amiga_mp3dec.fastexp --play test.aac
amiga_mp3dec.fastexp --play test.flac
```

### HTTP MP3 radio test

For a `RADIO=1` build, test HTTP MP3 streaming:

```sh
amiga_mp3dec.fastexp --play "http://ice1.somafm.com/groovesalad-128-mp3"
```

### ReAction GUI radio test

Use this URL in the ReAction GUI:

```text
http://ice1.somafm.com/groovesalad-128-mp3
```

Expected radio behaviour:

* stream plays;
* title/artist update automatically from ICY `StreamTitle`;
* station/album shows `Groove Salad`;
* genre shows `Ambient Chill`;
* file info shows `Internet Radio MP3`, `128 kbps`, `audio/mpeg`;
* track shows `Live`;
* time shows elapsed / `Live`;
* Stop works without GUI corruption or hard lock;
* menus remain safe after stopping radio.

## 11. Final test checklist

Before treating a build as known-good, verify:

* [ ] local MP3 playback
* [ ] local AAC playback
* [ ] AAC TNS-heavy file still clean
* [ ] local FLAC playback
* [ ] HTTP MP3 radio playback
* [ ] ICY title/artist metadata updates
* [ ] Stop on radio stream
* [ ] Menus after stopping radio
* [ ] Restart radio after stopping
* [ ] No decoder entrypoint regression

## 12. Git hygiene

Do not commit generated files, local binaries, downloaded test media, or Windows alternate-data-stream marker files.

Do not commit:

* `*.o`
* `*.decoder`
* `*.decoder.map`
* `*.fastexp`
* `minimp3r`
* test audio files
* `*:Zone.Identifier`

Check the tree before committing:

```sh
git status --short
```

## 13. README link

The repository README should point to this page so new developers can find the complete Amiga build instructions quickly.
