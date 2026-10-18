#!/bin/sh
# Converts assets/title.mp3 into romfs/title.pcm (raw signed 16-bit little-endian, 32 kHz stereo).
set -e
cd "$(dirname "$0")/.."
mkdir -p romfs
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 assets/title.mp3)
FADE=$(python3 -c "print(max(0, float('$DUR') - 0.03))")
ffmpeg -y -i assets/title.mp3 -ac 2 -ar 32000 \
  -af "afade=t=in:d=0.02,afade=t=out:st=$FADE:d=0.03" -f s16le romfs/title.pcm
ls -l romfs/title.pcm
