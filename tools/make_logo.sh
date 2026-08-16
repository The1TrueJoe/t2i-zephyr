#!/bin/sh
# Regenerate src/juno_logo.c from an SVG (or any image ImageMagick can read).
#
#   ./tools/make_logo.sh ~/Documents/Juno/Core/ui/src/juno-logo.svg [WIDTH] [HEIGHT]
#
# Output is RGB565 (LVGL native byte order) sized WIDTHxHEIGHT, flattened onto
# black — the splash draws on a black screen, so no alpha channel is needed.
set -eu

SRC=${1:?usage: make_logo.sh SVG [W] [H]}
W=${2:-220}
H=${3:-60}
OUT=$(dirname "$0")/../src/juno_logo.c
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

magick -background black -density 300 "$SRC" -resize "${W}x${H}" -flatten \
       -depth 8 "RGB:$TMP/raw"

W=$W H=$H OUT=$OUT RAW=$TMP/raw python3 - <<'PY'
import os
W, H = int(os.environ['W']), int(os.environ['H'])
raw = open(os.environ['RAW'], 'rb').read()
assert len(raw) == W * H * 3, f"expected {W*H*3} bytes, got {len(raw)}"

out = bytearray()
for i in range(W * H):
    r, g, b = raw[i*3], raw[i*3+1], raw[i*3+2]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    out += bytes((v & 0xFF, v >> 8))          # little-endian, LVGL native RGB565

rows = "\n".join("    " + " ".join("0x%02x," % c for c in out[i:i+16])
                 for i in range(0, len(out), 16))

open(os.environ['OUT'], 'w').write('''/*
 * Juno logo splash, generated from Juno/Core ui/src/juno-logo.svg
 * (rasterized to %dx%d, RGB565). Regenerate with tools/make_logo.sh.
 */
#include <lvgl.h>

static const uint8_t juno_logo_map[] = {
%s
};

const lv_image_dsc_t juno_logo = {
    .header = {
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_RGB565,
        .flags  = 0,
        .w      = %d,
        .h      = %d,
        .stride = %d,
    },
    .data_size = sizeof(juno_logo_map),
    .data      = juno_logo_map,
};
''' % (W, H, rows, W, H, W * 2))
print("wrote %s (%d bytes of bitmap)" % (os.environ['OUT'], len(out)))
PY
