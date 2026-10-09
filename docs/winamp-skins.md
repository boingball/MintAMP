# MintAMP classic Winamp skin editions

`MintAMP-SGT` uses GadTools utility windows; `MintAMP-SR` uses ReAction/ClassAct
utility windows. Both have the same custom Intuition player window and reuse
the respective frontend's playback, network, playlist and decoder code.
The existing MintAMP and MintAMP-GT editions remain available.

## Build

```sh
make -f Makefile.amiga skins RADIO=1 SSL=1 CPU=30
make -f Makefile.amiga skins RADIO=1 SSL=1 CPU=60
```

Individual targets are `sgt` / `sr` (or `MintAMP-SGT` / `MintAMP-SR`);
`sslsgt` / `sslsr` enable Internet radio and AmiSSL. The release target also
includes both skin editions and their Workbench icons. As with the other
editions, clean or use separate output filenames when changing CPU/compiler
configuration. Keep the CPU-matched external decoder modules with the player.

## First launch

Place a downloaded classic skin at `PROGDIR:Skins/base-2.91.wsz`, or select one
in the file requester on first launch. Skin artwork is supplied by the user.
The uploaded Winamp 2.91 base skin was used for development validation.

If skin selection is cancelled, or the skin window cannot be created, the
native frontend remains usable. The last successfully selected skin and scale
are saved separately for SGT and SR. Audio settings and radio favourites use
the existing frontend's settings.

Shell examples:

```text
MintAMP-SGT --skin Work:Skins/base-2.91.wsz
MintAMP-SR --skin Work:Skins/another.wsz --double
MintAMP-SR Music:track.mp3 --skin Work:Skins/another.wsz
```

Workbench tooltypes:

```text
SKIN=Work:Skins/base-2.91.wsz
DOUBLESIZE=YES
```

The normal size is 275x116; double size is 550x232. Double size is disabled
when it cannot fit the current Workbench screen. Integer scaling applies to
graphics, controls and mouse coordinates together. Drag the skin's title bar
to move the player. Right-click for the native MintAMP menu.

## Controls

| Control | Action |
| --- | --- |
| Play / Space | Play the selected audio file or Internet stream |
| Stop / X | Stop through the existing playback shutdown path |
| Previous / Next | Select and play the previous/next playlist entry; wait for the old child to exit first |
| Eject / O | Open the native audio/playlist file requester |
| Volume | Change playback volume immediately; save on release |
| Position bar | Seek a playing local track with known duration; live radio cannot seek |
| EQ / T / menu Settings | Toggle the native settings window |
| PL / P | Open/close the native playlist window |
| Visualizer area / R | Open the native Internet Radio browser |
| S / menu Load skin | Select a different classic skin |
| D / upper-right size buttons | Toggle normal/double size |
| Close button / menu Quit / Ctrl-C | Quit and stop playback |

Closing the native settings window hides it and leaves the skin player running.
Radio metadata uses the existing live ICY updates and the skin's bitmap font.
The player redraws only changed components. A stationary state produces no
draw calls. No FFT or animated visualizer is introduced.

Pause, balance, shuffle and repeat are not implemented by this skin frontend;
clicking their graphics displays an explicit notice. EQ opens MintAMP's
settings, not a ten-band equalizer. Winamp skins supply appearance, not DSP or
additional playback features.

## Format and display compatibility

Supported: classic `.wsz` / `.zip` archives with stored or deflated entries;
Windows BMP with 1/4/8-bit palettes or 24/32-bit RGB; RLE8 and RLE4; top-down
uncompressed BMP; case-insensitive filenames and files inside subdirectories.
MAIN.BMP, CBUTTONS.BMP and TEXT.BMP are required. Optional standard player
images use the skin background, bitmap font, or neutral slider controls when
absent. Present but malformed/undersized images are rejected with an error.

Optional playlist and equalizer skin images are not used: those windows retain
their native toolkit appearance. REGION.TXT shaped windows, custom cursors,
windowshade, modern `.wal` skins, and Winamp plug-ins are not supported.

Archives and BMPs are bounded and CRC checked. Failed skin replacements keep
the current skin. ZIP data is read in memory; paths are never extracted to disk.

Graphics are converted to native cached bitmaps on load. On graphics.library
V39+, the renderer obtains shared screen pens and releases them on disposal.
V37/V38 use the existing screen palette without calling V39 APIs. Colour
remapping depends on the Workbench palette; low-colour screens will not match
the original skin exactly. Cached colours use a 12-bit lookup and a limited
shared-pen budget to avoid exhausting the desktop palette. RTG screens are the
preferred target for visual fidelity. Real hardware testing is still required
for refresh, dragging, palette behaviour and audio headroom.

## Validation

```sh
make -f Makefile.amiga skin-format-test
make -f Makefile.amiga skin-format-test SKIN_TEST_FILE=/path/to/base-2.91.wsz
```

Host tests cover generated BMP encodings, stored/deflated archives, missing
optional images, invalid dimensions, truncated RLE/ZIP, CRC failures, case and
directory handling, failed replacement preservation, hit testing, and identical
full/incremental rendering. The optional reference-skin check independently
compares decoded pixels using Pillow/ImageMagick. CI compiles both editions for
68030 and 68060; this is compile validation, not an emulated Amiga runtime test.

Sprite-layout reference: [Webamp](https://github.com/captbaritone/webamp),
`packages/webamp/js/skinSprites.ts` (MIT). The C loader, renderer and Amiga
integration are implemented here; no browser/JavaScript runtime is required.
