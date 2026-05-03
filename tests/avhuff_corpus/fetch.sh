#!/bin/sh
# Fetch + synthesize the AVHuff regression corpus.
# See README.md for details. Idempotent; existing files are preserved.

set -eu

cd "$(dirname "$0")"

MAME_RAW="https://raw.githubusercontent.com/mamedev/mame/master/regtests/chdman"

mkdir -p regtest synth

for d in createld_avi_yuv2_3_frames_no_audio createld_avi_uyvy_3_frames_no_audio; do
    mkdir -p "regtest/$d"
    [ -f "regtest/$d/out.chd" ] || \
        curl -fsSL -o "regtest/$d/out.chd" "$MAME_RAW/output/$d/out.chd"
    [ -f "regtest/$d/in.avi" ] || \
        curl -fsSL -o "regtest/$d/in.avi" "$MAME_RAW/input/$d/in.avi"
done

# Synth: take the YUY2 input and mux in a short 48 kHz stereo sine track,
# then encode with chdman.
if [ ! -f synth/in_with_audio.avi ]; then
    ffmpeg -hide_banner -loglevel error -y \
        -i regtest/createld_avi_yuv2_3_frames_no_audio/in.avi \
        -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=0.063" \
        -c:v copy -c:a pcm_s16le -ar 48000 -ac 2 -shortest \
        synth/in_with_audio.avi
fi

[ -f synth/avhu_only.chd ] || \
    chdman createld -i synth/in_with_audio.avi -o synth/avhu_only.chd -c avhu -f
[ -f synth/flac_audio.chd ] || \
    chdman createld -i synth/in_with_audio.avi -o synth/flac_audio.chd -c flac,avhu -f

echo "corpus ready"
ls -la regtest/*/out.chd synth/*.chd
