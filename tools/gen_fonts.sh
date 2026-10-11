#!/bin/sh
# Regenerate main/fonts from Montserrat TTFs (https://github.com/JulietaUla/Montserrat).
# usage: tools/gen_fonts.sh <dir with Montserrat-{Medium,SemiBold,Bold}.ttf>
set -e
SRC=$1
# ASCII, Swedish/Nordic letters, degree, middle dot, en dash, ellipsis, arrows
R='0x20-0x7E,0xB0,0xB7,0xC4,0xC5,0xC9,0xD6,0xDC,0xE4,0xE5,0xE9,0xF6,0xFC,0x2013,0x2026,0x2190,0x2192'
cd "$(dirname "$0")/../main/fonts"
gen() { npx -y lv_font_conv@1.5.2 --no-compress --font "$SRC/Montserrat-$1.ttf" -r "$R" \
        --size "$2" --bpp 4 --format lvgl --lv-include lvgl.h -o "$3.c"; }
gen Medium 12 sl_font_12
gen SemiBold 16 sl_font_16
gen Bold 22 sl_font_22
