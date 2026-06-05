#!/bin/sh
# Verify large ID3v2/footer skipping and validated first-frame sync for disk and RAM input.

set -eu

MP3DEC=${MP3DEC:-./amiga_mp3dec}
tmp=${TMPDIR:-/tmp}/libhelix-large-id3-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp"

python3 - "$tmp/large-tag.mp3" <<'PY'
import sys

tag_size = 1500000
ss = bytes(((tag_size >> 21) & 0x7f, (tag_size >> 14) & 0x7f,
            (tag_size >> 7) & 0x7f, tag_size & 0x7f))
# ID3v2.3 with the footer flag set for exercising the required extra skip.
header = b"ID3\x03\x00\x10" + ss
# Put a sync-like but invalid reserved-layer header before the real frame header.
tag = b"\xff\xff\x90\x00" + bytes(tag_size - 4)
footer = bytes(10)
junk = b"junk-before-frame"
frame = b"\xff\xfb\x90\x00" + bytes(1024)
open(sys.argv[1], "wb").write(header + tag + footer + junk + frame)
PY

expected_skip=1500020
expected_frame=1500037
out=$($MP3DEC --info "$tmp/large-tag.mp3")
printf '%s\n' "$out"
printf '%s\n' "$out" | grep '^ID3v2 detected: yes$' >/dev/null
printf '%s\n' "$out" | grep '^ID3v2 version: 2.3.0$' >/dev/null
printf '%s\n' "$out" | grep "^ID3v2 size skipped: $expected_skip bytes$" >/dev/null
printf '%s\n' "$out" | grep "^first MPEG frame offset: $expected_frame$" >/dev/null
printf '%s\n' "$out" | grep '^MPEG audio: version 1, layer 3$' >/dev/null

$MP3DEC --decode-only "$tmp/large-tag.mp3" > "$tmp/disk.out" 2> "$tmp/disk.err"
$MP3DEC --fast-mem --decode-only "$tmp/large-tag.mp3" > "$tmp/ram.out" 2> "$tmp/ram.err"
sed '1d' "$tmp/ram.out" > "$tmp/ram-stripped.out"
diff -u "$tmp/disk.out" "$tmp/ram-stripped.out"
diff -u "$tmp/disk.err" "$tmp/ram.err"
printf '%s\n' 'large ID3/sync regression passed'

# An input with no decodable samples must fail before audio.device is opened.
printf 'not-an-mp3' > "$tmp/no-audio.mp3"
if $MP3DEC --play "$tmp/no-audio.mp3" > "$tmp/play.out" 2> "$tmp/play.err"; then
	printf '%s\n' 'FAIL: zero-sample playback unexpectedly succeeded' >&2
	exit 1
fi
grep '^no decoded samples; audio.device playback not started$' "$tmp/play.err" >/dev/null
if grep '^--play requires an AmigaOS audio.device build$' "$tmp/play.err" >/dev/null; then
	printf '%s\n' 'FAIL: audio.device was opened before samples were decoded' >&2
	exit 1
fi
printf '%s\n' 'zero-sample playback guard regression passed'
