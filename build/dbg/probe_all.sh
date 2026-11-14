#!/bin/bash
. "$(dirname "$0")/../paths.sh"
# Summarize codec/size/fps/audio format of every movie, plus total duration.
cd "$GAME/data/movie"
tot=0
for f in *.wmv; do
  v=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,r_frame_rate,pix_fmt -of csv=p=0 "$f")
  a=$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels -of csv=p=0 "$f")
  d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f")
  echo "$v | $a"
  tot=$(echo "$tot + $d" | bc)
done | sort | uniq -c
ls *.wmv | wc -l
ls | grep -v '\.wmv$' | head
