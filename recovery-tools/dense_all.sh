#!/bin/bash
# Dense-scan all probe-found regions (padded ±32MiB), one CSV per region, then merge.
set -e
cd "$(dirname "$0")"
IMG=/run/media/qaidvoid/HDDD/image.dd
PAD=$((32*1024*1024))
mkdir -p dense
i=0
total=0
while read -r tag start end; do
    [ "$tag" = "REGION" ] || continue
    s=$((start > PAD ? start - PAD : 0))
    e=$((end + PAD))
    total=$((total + e - s))
    echo "region $i: $s .. $e ($(( (e-s)>>20 )) MiB)" >&2
    ./btrscan dense "$IMG" "$s" "$e" > "dense/region$i.csv"
    i=$((i+1))
done < probe-regions.txt
echo "scanned $i regions, $((total>>30)) GiB total" >&2
# merge: keep single header
head -1 dense/region0.csv > dense-all.csv
for f in dense/region*.csv; do tail -n +2 "$f"; done >> dense-all.csv
wc -l dense-all.csv >&2
