# MintAMP classic Winamp skin editions

MintAMP-SGT uses GadTools utility windows; MintAMP-SR uses ReAction/ClassAct
utility windows. Both share the skinned Intuition player, playlist and EQ.
Playback, decoders, Internet radio, favourites, artwork and native settings
remain owned by the established frontend/backend. MintAMP and MintAMP-GT
remain separate, supported applications. This is partial classic Winamp 2.x
parity, not a Windows audio engine or plug-in host.

## Build and install

```sh
make -f Makefile.amiga skins RADIO=1 SSL=1 CPU=30
make -f Makefile.amiga skins RADIO=1 SSL=1 CPU=60 ASM60_GROUPS='lowrate060 huffman midside planars8'
```

Individual targets are `sgt` and `sr`. Use separate build directories or clean
when changing CPU/compiler/flags; keep CPU-matched AAC, FLAC, Ogg, WMA, WAV and
IFF decoder modules with the executable. SSL requires the existing AmiSSL SDK
and runtime. New audio/UI calculations use integer arithmetic. For an explicitly
soft-float application build, pass `EXTRA_CFLAGS=-msoft-float`; also validate
external modules and installed libraries on the intended FPU-less machine.
The existing compiler/CPU optimisation recipes are unchanged.

Place a user-supplied classic skin at `PROGDIR:Skins/base-2.91.wsz`, or choose
one at first launch. The uploaded Winamp 2.91 base skin is a validation input,
not repository artwork. If selection is cancelled or the skin window cannot
open, the native frontend remains available. Valid skins open directly, without
first flashing the native main window. Workbench menu colours/fonts are used.

```text
MintAMP-SGT --skin Work:Skins/base-2.91.wsz
MintAMP-SR Music:track.mp3 --skin Work:Skins/another.wsz --double
```

Workbench tooltypes are `SKIN=Work:Skins/base-2.91.wsz` and `DOUBLESIZE=YES`.
Normal/double sizes are 275x116 and 550x232. Scaling covers artwork, hit testing,
EQ, playlist and text. Oversized windows/scales are refused or fall back.

## Main controls

| Control | Behaviour |
| --- | --- |
| Play / X | Start playback; with no loaded input, play the selected/first playlist entry; resume a paused decoder; no restart while already playing |
| Pause / C / Space | Toggle buffer-boundary pause/resume |
| Stop / V | Existing interrupt, IO retirement and child shutdown path |
| Previous / Z; Next / B | Play a playlist entry after the old child exits; shuffle uses previous/forward history |
| Shuffle / J | Visit every entry once per cycle; avoid immediate repeats between cycles |
| Repeat / L | Cycle off, playlist and track; track repeat applies to natural completion, not explicit Next |
| Volume | Drag; existing volume setting/save path |
| Balance | Drag left/right attenuation using Paula channel volumes; mono output reports unavailable |
| Position | Drag a preview, seek on release; only an unpaused local MP3 with known duration |
| Timer | Click to toggle elapsed/remaining; remaining requires a known duration |
| Eject / O | Existing native audio/playlist requester |
| EQ / E | Toggle the skinned equaliser |
| PL / P | Toggle the compact playlist, with native fallback |
| T / menu Settings | Existing native settings and utility window |
| R / menu Internet Radio | Existing browser, favourites and network options |
| Visualisation area / menu | Cycle off, spectrum and oscilloscope |
| Menu Visual rate | 1, 2 or 4 analysis updates per second |
| Menu Playback statistics | Buffer length, spare time, underruns, radio buffered bytes and source/output details; optional title-line overlay |
| S / menu Load skin | Transactional skin replacement |
| D / upper-right size button | Normal/double size |
| W / shade button | Main windowshade; graphics and visual analysis stop while shaded |
| Close / menu Quit / Ctrl-C | Stop and quit |

Play/pause/stopped indicators follow backend state. Pause is acknowledged only
when the queued audio has finished: latency can be several configured buffers.
The decoder and any prepared/decode-ahead PCM are retained; resume continues
from that state. Refilling a mono buffer can add resume latency.
Stop still interrupts and retires the whole IO ring. Intentional pause and ring
catch-up are excluded from underrun accounting. A long radio pause can exhaust
network buffers or encounter a server timeout; no reconnect/resume is faked.

Elapsed time counts completed output samples, rather than assuming every MP3,
AAC or generic decode block contains 1152 samples. Updates are **buffer-granular**,
not a sample-accurate moving cursor. MPEG-2.5 and variable-sized blocks are
handled; pause accounts for all drained slots once. The classic four-digit
counter caps at 99:59. MP3 seeking remains the backend's bitrate-based estimate,
particularly approximate for VBR, with existing queued-buffer latency. Other
formats and live radio explicitly reject seeking.

Bitrate is updated from MP3 frame headers (or the radio bitrate hint where the
module supplies none); sample rate and mono/stereo describe the source, while
statistics identify mono/stereo Paula output separately. Unknown bitrate stays
unknown/zero. Scrolling titles use live ICY metadata with station-name fallback.
Both editions use the same bounded GUI animation rate.

## Playlist editor

PLEDIT.BMP supplies the frame; PLEDIT.TXT [Text] supplies Normal, Current,
NormalBG and SelectedBG colours. Missing/invalid colours use classic defaults.
ROM Topaz 8 supplies labels; Windows fonts are not loaded. This is another view
of the **same native arrays**, with shared move/remove/sort operations.

| Button | Operations |
| --- | --- |
| ADD | Files via the existing multi-file requester; one directory level of supported audio files; append playlist |
| REM | Selected rows; confirmed missing local files; clear |
| SEL | All, none, invert |
| MISC | Stable title/filename or full-path sorting; focused row up/down |
| LIST | Replace, save, append M3U/M3U8/PLS using the existing parser/writers |
| List options / menu Full playlist options | Complete native playlist, including URLs and radio favourites |

Menus use small native requesters so they remain readable on AGA screens.
Click selects; Ctrl-click toggles; Shift-click extends an anchored range.
Double-click plays on release, so beginning a drag does not start playback.
Return/Space plays; Up/Down navigates; Shift extends selection; Ctrl-Up/Down
moves the focused row. Keypad 7/1 goes first/last and keypad 9/3 pages up/down;
Delete removes selected rows, Ctrl-A selects all. Escape/P closes the playlist.
Drag a row vertically and release over a visible destination to reorder it.
Reordering currently moves one row, not an entire selected block, and has no
edge autoscroll. The scrollbar supports clicking and dragging.

Current, focused and selected entries are remapped with array edits. A native
list replacement invalidates obsolete shuffle history and pending row starts.
Skin-originated sorting/reordering retains the selection mask. The native view
has its existing single-focus selection UI; the extra multi-selection mask is
used by the skin. Cancelled, unreadable, empty, oversized or wholly unusable
loads retain the previous list. Replace uses a temporary rollback snapshot,
not a second persistent playlist. Imports remain bounded to 128 entries/96 KiB.
Directory addition is nonrecursive and bounded to 4096 examined entries.
Missing removal preserves URLs and permission/other ambiguous IO failures.

EXTINF/PLS Length values and durations learned for the current local track are
shown and preserved on save. The bottom total sums known lengths and appends
`+` when any lengths are unknown. Tracks are not decoded/probed en masse simply
to populate durations, and URLs are not contacted for missing-file checks.

The compact playlist remains fixed at 275x232 (17 rows), scaled at double size.
Its shade button opens full native options; playlist/EQ shade and sprite-based
resizing are not implemented. Missing artwork, insufficient memory or a small
screen uses the native playlist. Native settings/favourites remain accessible.

## Equaliser

EQMAIN.BMP supplies a separate 275x116 window, on/off, preamp, ten sliders and
presets. The classic band labels are 60, 170, 310, 600 Hz, 1, 3, 6, 12, 14 and
16 kHz. Gains/preamp are integer dB in -12..+12; combined gain is limited to
-24..+12 dB, with saturation/guard-bit recomputation before Helix IMDCT.

This is an **optional MP3-only spectral EQ**, default off. Cached integer gains
are applied to existing dequantised/alias-processed coefficients; no second
engine, sample-by-sample floating-point filters or FPU requirement is added.
An enabled flat EQ is a bit-identical bypass. The response is coarse,
particularly at low frequencies/short blocks, and does not reproduce Winamp's
biquad filter curves. Source/output Nyquist limits and the existing fast-lowrate
subband cap still apply. Bands wholly outside output bandwidth reject dragging;
partly retained bands have a reduced usable range. AAC, FLAC, Ogg, WMA, WAV and
IFF bypass EQ and report unsupported enable requests.

Flat and Bass cut presets plus save/load use the bounded `MintAMP-EQ-1` text
format (`.eq`), with preamp followed by ten gains. This is not Winamp binary
EQF compatibility. Invalid preset values leave settings unchanged. Automatic
per-track presets report unsupported. The skin graph is decorative; there is
no computed response curve. Missing EQMAIN reports unavailable. EQ settings
persist separately per skin edition. Test audio headroom and CPU spare time
before leaving EQ on for radio or a 68030 configuration.

## Visualisation and performance

The main area offers a coarse 16-bin spectrum or 64-point oscilloscope, default
off. A GUI request causes the audio-buffer submission path to copy only 64
signed 8-bit samples from the already prepared planar PCM, averaging stereo.
The request/acknowledgement handshake makes the snapshot stable while GUI code
reads it. There is no full-buffer copy, allocation, DFT or drawing inside audio
submission. Spectrum analysis uses a bounded 64-point integer DFT on the GUI
task; scope uses the same snapshot. Colours are currently fixed green/black;
VISCOLOR.TXT, logarithmic binning and peak decay are not implemented.

Analysis is limited to the selected 1/2/4 Hz and stops when disabled, paused,
stopped or main-shaded. Snapshot freshness is **limited by buffer submissions**:
large audio buffers can make even a 4 Hz visualisation refresh look static for
several seconds. A GUI-only quarter-second timer also scrolls long titles and
expires notices; it does not poll the network or alter the native playback timer.

Cached native bitmaps and incremental rendering remain in use. Stationary
states produce no draw calls. Palette/bitmap memory is bounded, but optional EQ
artwork adds a cached sheet; double size increases bitmap memory fourfold.
New controls compile without floating-point or emulated 64-bit multiply
instructions in the audited 68060 object. This does not prove real-time timing
on an Amiga: leave EQ/visualisation off until measured on the target system.
No claim of stutter-free enabled DSP/visualisation on a 68060 is made from host
tests alone. Native editions do not compile these new audio hooks.

## Windows and persistence

Nearby main/EQ/playlist edges snap on release (eight scaled pixels). Docked
windows move together; Ctrl-drag moves one window to detach it. Moves are
bounded to the screen. Positions, visibility, main shade, shuffle/repeat,
balance, time mode, EQ, visual rate/mode and debug overlay persist in each
edition's separate `ENV:/ENVARC:` State variable, alongside Skin/Scale. Native
audio settings and favourites keep their established storage. Restored
positions are clamped when screen/scale changes.

Native refresh handling, pressed playback/shuffle/repeat sprites, optional
artwork bounds, owned menu structures and cleanup remain shared. Classic skins
have no general hover sprite set. EQ presets/close controls do not yet animate
pressed sprites. Shade does not compact the remaining docked windows.

## Skin compatibility

Classic `.wsz`/`.zip`: stored/deflated entries, case-insensitive names, nested
paths, bounded ZIP/CRC checks. BMP: 1/4/8-bit palette, 24/32-bit RGB, RLE4/RLE8
and uncompressed top-down. MAIN.BMP, CBUTTONS.BMP and TEXT.BMP are required.
Missing optional player artwork uses background/font/neutral fallback controls;
malformed or undersized supplied sheets are rejected. Optional PLEDIT.BMP is
at least 276x110; EQMAIN.BMP at least 275x315. Failed replacements retain the
current resources. No files are extracted to disk.

V39+ uses obtained shared pens with disposal; V37/V38 uses the desktop palette.
AGA remapping depends on available colours; RTG/P96 improves fidelity.
REGION.TXT shaped windows, custom cursors, EQ_EX shade, modern `.wal` skins and
Winamp DLL plug-ins are not supported.

## Validation

```sh
make -f Makefile.amiga skin-format-test SKIN_TEST_FILE=/path/to/base-2.91.wsz
make -f Makefile.amiga skin-controls-test skin-eq-decode-test playlist-format-test
```

Tests cover generated BMP/ZIP encodings, corrupt/truncated/duplicate/undersized
assets, optional EQ/playlist sheets, failed replacement preservation, control
mapping, full/incremental player/playlist/EQ equality and stationary rendering;
shuffle cycles/history/repeat, selection/remapping/sorting/rollback, balance,
output-sample clock, pending track-switch transactions, EQ arithmetic/bypass, spectral analysis and docking.
An actual MPEG-2.5 tone decode verifies disabled/flat EQ sample equality and
cut/boost output energy. Playlist tests verify timed M3U/PLS round trips.
Existing decoder arithmetic, radio metadata/conversion and favourites tests
remain part of validation. CI builds skin and native editions for both CPUs.

Real Amiga tests are still required: pause/resume/stop and ring retirement,
seek/clock latency, mono/stereo balance, AGA/P96 refreshing/palettes, skin changes,
window grouping, memory recovery, preset/settings IO, decoder transitions and
long-running MP3/AAC/HTTPS radio with underrun measurements. Test 68030, 68060
with and without FPU, and PiStorm. Host sanitizers and cross-compiles cannot
substitute for audio.device/Intuition runtime tests. GCC 16.2-rc13 is the local
validation compiler; a new GCC 13.2.0 run is not yet available here.

Sprite reference: [Webamp](https://github.com/captbaritone/webamp),
`packages/webamp/js/skinSprites.ts` (MIT). Rendering and integration are native C.
