# Building and verifying the AmigaOS / m68k player

## Overview

This project builds an AmigaOS/m68k player with external decoder modules.

Current decoder support:

* MP3
* FLAC
* AAC-LC ADTS

AAC source comes from the ESP8266Audio submodule:

```text
decoders/esp8266audio
```

The AAC symlink points to the libhelix AAC source inside that submodule:

```text
decoders/aac -> esp8266audio/src/libhelix-aac
```

## Fresh clone / first setup

For a new checkout, clone recursively so the AAC submodule is available immediately:

```sh
git clone --recursive https://github.com/boingball/amiga-libhelix-mp3.git
cd amiga-libhelix-mp3
```

If the repository was already cloned without submodules, initialize and update the submodules afterwards:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

## Reset an existing dev checkout to current `master`

Use this sequence when you want to discard local build products and local source edits and return a development checkout to the current upstream `master` state.

> **Warning:** `git reset --hard` and `git clean -fd` delete local, uncommitted changes and untracked files. Save anything important before running these commands.

```sh
cd ~/Amiga-Programs/libhelix-mp3

git fetch origin --prune
git checkout master
git reset --hard origin/master
git clean -fd

git submodule sync --recursive
git submodule update --init --recursive
git submodule foreach --recursive 'git reset --hard && git clean -fd'
```

Then verify the checkout and submodule state:

```sh
git status --short --branch
git log --oneline -5
git submodule status decoders/esp8266audio
```

Expected results:

* local `master` matches `origin/master`;
* there is no ahead/behind status;
* `decoders/esp8266audio` points to the tested AAC fix commit, currently `71eea9a` or newer.

## Clean build commands

For a normal clean rebuild of decoder modules and the fast 030 player build:

```sh
make -C decoders clean || true
find decoders -name "*.o" -delete
rm -f amiga_mp3dec.fastexp
rm -f decoders/*.decoder decoders/*.decoder.map

make -C decoders flac
make -C decoders aac
make -f Makefile.amiga fast030
```

Optional full clean:

```sh
make -f Makefile.amiga clean || true
make -C decoders clean || true
find . -name "*.o" -delete
rm -f amiga_mp3dec.fastexp
rm -f decoders/*.decoder decoders/*.decoder.map
```

## Decoder module verification

After rebuilding decoder modules, verify that each module entrypoint is the first real text symbol:

```sh
m68k-amigaos-nm -n decoders/flac.decoder | head -10
m68k-amigaos-nm -n decoders/aac.decoder | head -10
```

Expected first real text symbol:

```text
00000000 T _DecoderModuleEntry
```

If `_DecoderModuleEntry` is not at `00000000`, the Amiga loader may jump into the wrong function, such as `_memset` or a libgcc helper, and crash with `80000004`.

## Known-good decoder milestone

Current stable milestone:

* FLAC builds reproducibly and plays.
* AAC decoder loads correctly.
* AAC-LC ADTS plays.
* AAC TNS big-endian/m68k corruption has been fixed in the ESP8266Audio submodule.
* Some edge-case MP3/AAC files may still be rejected as invalid; that is a parser/probing task for later.

## Submodule development workflow

Changes to AAC/libhelix source must be made and committed in the ESP8266Audio submodule first, then the parent repository must be updated to point at the new submodule commit.

For changes inside AAC/libhelix source:

```sh
cd decoders/esp8266audio
git checkout master
git pull
git checkout -b <branch-name>
```

Make source changes in:

```text
src/libhelix-aac/
```

Commit and push in ESP8266Audio first:

```sh
git add src/libhelix-aac/...
git commit -m "..."
git push -u origin <branch-name>
```

Then update the parent repository pointer:

```sh
cd ~/Amiga-Programs/libhelix-mp3
git add decoders/esp8266audio
git commit -m "Update ESP8266Audio submodule for ..."
git push origin master
```

## What not to commit

Do not commit:

* `*.o`
* `*.decoder`
* `*.map`
* `amiga_mp3dec.fastexp`
* test AAC/MP3/FLAC files
* Windows `:Zone.Identifier` files
* local build folders such as `libhelix-mp3-master-test/`

Recommended `.gitignore` entries if they are not already present:

```gitignore
*.o
*.decoder
*.map
*.fastexp
*:Zone.Identifier
```

## Suggested test files

Recommended test set:

* known-good MP3
* known-good FLAC 16-bit stereo 44.1 kHz
* AAC-LC mono sine
* AAC-LC stereo sine
* AAC-LC real music
* AAC-LC TNS-only test file

The TNS test is important because AAC real music glitches were previously traced to `TNS FilterRegion` on big-endian m68k.

## Scope of this documentation change

This documentation is intended to describe the current setup, reset, build, verification, and submodule workflow only. It does not require codec code, Makefiles, decoder modules, submodule pointers, or binary artefacts to change.
