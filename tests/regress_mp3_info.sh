#!/bin/sh
# Verify standalone --info metadata inspection and its no-output behavior.

set -eu

MP3DEC=${MP3DEC:-./amiga_mp3dec}
tmp=${TMPDIR:-/tmp}/libhelix-mp3-info-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp"

python3 - "$tmp/tagged.mp3" <<'PY'
import struct
import sys


def frame(frame_id, text):
    payload = b"\x03" + text.encode("utf-8")
    return frame_id.encode("ascii") + struct.pack(">I", len(payload)) + b"\0\0" + payload

frames = frame("TIT2", "Info Test") + frame("TPE1", "Helix Artist") + frame("TALB", "Test Album")
size = len(frames)
synchsafe = bytes(((size >> 21) & 0x7f, (size >> 14) & 0x7f, (size >> 7) & 0x7f, size & 0x7f))
id3v2 = b"ID3\x03\x00\x00" + synchsafe + frames
# MPEG-1 Layer III, 128 kbps, 44100 Hz, stereo, followed by enough padding for probing.
audio = b"\xff\xfb\x90\x00" + bytes(1024)
id3v1 = b"TAG" + b"V1 Title".ljust(30, b"\0") + b"V1 Artist".ljust(30, b"\0") + b"V1 Album".ljust(30, b"\0") + b"1999" + bytes(30) + bytes((13,))
open(sys.argv[1], "wb").write(id3v2 + audio + id3v1)
PY

out=$($MP3DEC --info "$tmp/tagged.mp3")
printf '%s\n' "$out"
printf '%s\n' "$out" | grep '^ID3v2 detected: yes$' >/dev/null
printf '%s\n' "$out" | grep '^ID3v2 version: 2.3.0$' >/dev/null
printf '%s\n' "$out" | grep '^ID3v2 size skipped: ' >/dev/null
printf '%s\n' "$out" | grep '^ID3v2: 2.3.0 ' >/dev/null
printf '%s\n' "$out" | grep '^first MPEG frame offset: ' >/dev/null
printf '%s\n' "$out" | grep '^title: Info Test$' >/dev/null
printf '%s\n' "$out" | grep '^artist: Helix Artist$' >/dev/null
printf '%s\n' "$out" | grep '^album: Test Album$' >/dev/null
printf '%s\n' "$out" | grep '^MPEG audio: version 1, layer 3$' >/dev/null
printf '%s\n' "$out" | grep '^sample rate: 44100 Hz$' >/dev/null
printf '%s\n' "$out" | grep '^bitrate: 128000 bps$' >/dev/null
printf '%s\n' "$out" | grep '^ID3v1 title: V1 Title$' >/dev/null
if [ -e "$tmp/tagged.pcm" ]; then
	printf '%s\n' 'FAIL: standalone --info created an output file' >&2
	exit 1
fi
python3 - "$tmp/untagged.mp3" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"\xff\xfb\x90\x00" + bytes(1024))
PY
plain=$($MP3DEC --info "$tmp/untagged.mp3")
printf '%s\n' "$plain" | grep '^ID3v2 detected: no$' >/dev/null
printf '%s\n' "$plain" | grep '^ID3v2 size skipped: 0 bytes$' >/dev/null
printf '%s\n' "$plain" | grep '^first MPEG frame offset: 0$' >/dev/null

python3 - "$tmp/utf16.mp3" <<'PY'
import struct
import sys


def syncsafe(size):
    return bytes(((size >> 21) & 0x7f, (size >> 14) & 0x7f, (size >> 7) & 0x7f, size & 0x7f))


def text_frame(frame_id, text, encoding=1):
    if encoding == 1:
        body = b"\x01\xff\xfe" + text.encode("utf-16le")
    elif encoding == 2:
        body = b"\x02" + text.encode("utf-16be")
    else:
        body = b"\x00" + text.encode("latin-1")
    return frame_id.encode("ascii") + struct.pack(">I", len(body)) + b"\0\0" + body

frames = b"".join((
    text_frame("TIT2", "Since You Been Gone", 1),
    text_frame("TPE1", "Rainbow", 1),
    text_frame("TALB", "Classic Rock The Ultimate Collection CD3", 2),
    text_frame("TCON", "Rock", 1),
    text_frame("TRCK", "1/18", 0),
))
open(sys.argv[1], "wb").write(b"ID3\x03\x00\x00" + syncsafe(len(frames)) + frames + b"\xff\xfb\x90\x00" + bytes(1024))
PY
utf16=$($MP3DEC --info "$tmp/utf16.mp3")
printf '%s\n' "$utf16"
printf '%s\n' "$utf16" | grep '^title: Since You Been Gone$' >/dev/null
printf '%s\n' "$utf16" | grep '^artist: Rainbow$' >/dev/null
printf '%s\n' "$utf16" | grep '^album: Classic Rock The Ultimate Collection CD3$' >/dev/null
printf '%s\n' "$utf16" | grep '^genre: Rock$' >/dev/null
printf '%s\n' "$utf16" | grep '^track: 1/18$' >/dev/null

printf '%s\n' 'MP3 info regression passed'
