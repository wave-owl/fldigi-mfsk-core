#!/bin/sh
# fldigi 4.2.13 decodes the picture overs of tests/picture_util.h (AC-837.1): the bench
# receiver enters picture mode at the header and prints the text after the picture.
#   FLDIGI_BENCH_BIN=/path/to/src-bench/src/fldigi sh tests/golden/make_picture_golden.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
bin=${FLDIGI_BENCH_BIN:-fldigi}
make -C "$root" --no-print-directory build/make_picture_vector >/dev/null
mkdir -p "$here/pic"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for spec in MFSK16:39 MFSK32:40 MFSK64:45 MFSK128:46; do
    sub=${spec%:*}
    id=${spec#*:}
    for color in 0 1; do
        stem=$(echo "$sub" | tr 'A-Z' 'a-z')_$( [ "$color" = 1 ] && echo color || echo grey)
        "$root/build/make_picture_vector" "$sub" "$color" "$tmp/$stem.wav"
        mkdir -p "$tmp/cfg"
        echo "<FLDIGI_DEFS></FLDIGI_DEFS>" > "$tmp/cfg/fldigi_def.xml"
        "$bin" --config-dir "$tmp/cfg" --benchmark-modem "$id" --benchmark-frequency 1500 \
            --benchmark-input "$tmp/$stem.wav" --benchmark-output "$here/pic/$stem.txt" \
            >/dev/null 2>&1
        echo "$stem"
    done
done
