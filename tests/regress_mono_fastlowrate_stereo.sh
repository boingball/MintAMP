#!/bin/sh
# Regression for stereo input decoded with --mono --fast-lowrate.
# Usage: MP3DEC=./amiga_mp3dec STEREO_44100_MP3=/path/to/stereo-44100.mp3 \
#   sh tests/regress_mono_fastlowrate_stereo.sh
# The fixture should be a stereo 44100 Hz MP3. The test asserts that mono output
# accounting is duration * 11025-ish, not double-counted stereo/interleaved samples.

set -eu

MP3DEC=${MP3DEC:-./amiga_mp3dec}
FIXTURE=${STEREO_44100_MP3:-}
RATE=11025

if [ -z "$FIXTURE" ]; then
	printf '%s\n' "SKIP: set STEREO_44100_MP3 to a stereo 44100 Hz MP3 fixture" >&2
	exit 77
fi
if [ ! -r "$FIXTURE" ]; then
	printf '%s\n' "FAIL: cannot read fixture: $FIXTURE" >&2
	exit 1
fi

out=$($MP3DEC --decode-only --bench --fast-lowrate --rate "$RATE" --mono "$FIXTURE")
printf '%s\n' "$out"

input_channels=$(printf '%s\n' "$out" | awk -F': ' '/^input channels:/ { print $2; exit }')
output_channels=$(printf '%s\n' "$out" | awk -F': ' '/^output channels:/ { print $2; exit }')
total_samples=$(printf '%s\n' "$out" | awk -F': ' '/^total emitted samples:/ { print $2; exit }')
per_channel_samples=$(printf '%s\n' "$out" | awk -F': ' '/^per-channel emitted samples:/ { print $2; exit }')
decoded_frames=$(printf '%s\n' "$out" | awk -F': ' '/^decoded frames:/ { print $2; exit }')
input_rate=$(printf '%s\n' "$out" | awk -F': ' '/^input sample rate:/ { sub(/ Hz$/, "", $2); print $2; exit }')
actual_rate=$(printf '%s\n' "$out" | awk -F': ' '/^output sample rate:/ { sub(/ Hz$/, "", $2); print $2; exit } /^actual fast-lowrate output rate:/ { sub(/ Hz$/, "", $2); print $2; exit }')

if [ "$input_channels" != "2" ]; then
	printf '%s\n' "FAIL: expected input channels 2, got ${input_channels:-missing}" >&2
	exit 1
fi
if [ "$output_channels" != "1" ]; then
	printf '%s\n' "FAIL: expected output channels 1, got ${output_channels:-missing}" >&2
	exit 1
fi
if [ "$total_samples" != "$per_channel_samples" ]; then
	printf '%s\n' "FAIL: mono total samples ($total_samples) must equal per-channel samples ($per_channel_samples)" >&2
	exit 1
fi

awk -v samples="$per_channel_samples" -v frames="$decoded_frames" -v inrate="$input_rate" -v actualrate="$actual_rate" 'BEGIN {
	/* 44100 Hz MP3 frames are MPEG-1 Layer III: 1152 input samples per frame. */
	expected = frames * 1152 * actualrate / inrate;
	lo = expected * 0.98;
	hi = expected * 1.02;
	if (samples < lo || samples > hi) {
		printf("FAIL: samples %lu outside expected mono range %.0f..%.0f (frames %lu, input rate %d, output rate %d)\n", samples, lo, hi, frames, inrate, actualrate) > "/dev/stderr";
		exit 1;
	}
	if (samples > expected * 1.5) {
		printf("FAIL: samples look stereo-double-counted: %lu vs expected %.0f\n", samples, expected) > "/dev/stderr";
		exit 1;
	}
}'
