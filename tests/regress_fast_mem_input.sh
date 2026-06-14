#!/bin/sh
# Verify that --fast-mem uses the RAM-backed input path without changing decode results.

set -eu

MP3DEC=${MP3DEC:-./amiga_mp3dec}
tmp=${TMPDIR:-/tmp}/libhelix-fast-mem-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp"

dd if=/dev/zero of="$tmp/no-sync.mp3" bs=1024 count=64 2>/dev/null
"$MP3DEC" --decode-only "$tmp/no-sync.mp3" > "$tmp/disk.out"
"$MP3DEC" --fast-mem --decode-only "$tmp/no-sync.mp3" > "$tmp/ram.out"

if ! sed -n '1p' "$tmp/ram.out" | grep '^fast-mem input preload: copying 65536 bytes to Fast RAM$' >/dev/null; then
	printf '%s\n' 'FAIL: --fast-mem did not report the expected preload size' >&2
	exit 1
fi
sed '1d' "$tmp/ram.out" > "$tmp/ram-stripped.out"
diff -u "$tmp/disk.out" "$tmp/ram-stripped.out"
printf '%s\n' 'fast-mem input regression passed'
