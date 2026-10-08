#!/bin/sh
# Hexen II (Hammer of Thyrion / uHexen2) for the Miyoo Mini Plus.
# Software renderer over SDL 1.2: video goes straight to /dev/fb0 (vid_miyoo.c),
# sound goes through MI_AO (snd_miyoo.c). No x86 assembly, no OpenGL.
#
# Run INSIDE the toolchain container:
#   cd /root/workspace/uhexen2/engine/hexen2 && sh build_miyoomini.sh
# Add "clean" to rebuild everything:  sh build_miyoomini.sh clean

TOP=/root/workspace/uhexen2
SYSROOT=/root/workspace/mini/arm-buildroot-linux-gnueabihf/sysroot
MI_INC=/root/workspace/sdl2-miyoo/mini/inc
MI_LIB=/root/workspace/sdl2-miyoo/mini/lib
OUT=$TOP/build-miyoo

# Optional, to compare build variants without losing the previous binary:
#   EXTRA_CFLAGS="-O3" TAG=o3 sh build_miyoomini.sh clean
# (the extra flags go at the end of the compiler line, so they win over the
#  Makefile's -O2; the result comes out as hexen2-miyoo-o3)
EXTRA="${EXTRA_CFLAGS:-}"
SUF=""
[ -n "$TAG" ] && SUF="-$TAG"

cd "$TOP/engine/hexen2" || { echo "ERROR: folder $TOP/engine/hexen2 not found (this script runs INSIDE the container)"; exit 1; }

# --- quick checks, so a missing piece is explained instead of a wall of errors
for f in "$TOP/engine/h2shared/vid_miyoo.c" "$TOP/engine/h2shared/snd_miyoo.c"; do
    [ -f "$f" ] || { echo "ERROR: $f is missing"; exit 1; }
done
command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1 || { echo "ERROR: arm-linux-gnueabihf-gcc not found (are you inside the container?)"; exit 1; }
[ -f "$SYSROOT/usr/include/SDL/SDL.h" ] || { echo "ERROR: SDL.h not found in $SYSROOT/usr/include/SDL"; exit 1; }
[ -f "$MI_INC/mi_ao.h" ] || { echo "ERROR: mi_ao.h not found in $MI_INC"; exit 1; }

mkdir -p "$OUT"

if [ "$1" = "clean" ]; then
    make clean >/dev/null 2>&1
    rm -f hexen2
fi

# Notes on the make arguments:
#  - the compiler flags ride inside CC because the Makefile builds CFLAGS itself
#  - MACH_TYPE=arm / USE_X86_ASM=no: use the C versions of every routine
#  - no ALSA/OSS/SUN audio, no CD audio, no MIDI; music: Ogg Vorbis only, decoded
#    by stb_vorbis (h2shared/stb_vorbis.c, no external library), CD tracks played
#    from data1/music/trackNN.ogg
#  - SYSOBJ_SOFT_VID / SYSOBJ_SND swap in our video and sound drivers
make h2 -j4 \
    CC="arm-linux-gnueabihf-gcc -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve+simd -fno-strict-aliasing --sysroot=$SYSROOT" \
    MACH_TYPE=arm USE_X86_ASM=no X11BASE= \
    USE_ALSA=no USE_OSS=no USE_SUNAUDIO=no USE_CDAUDIO=no USE_MIDI=no \
    USE_CODEC_WAVE=no USE_CODEC_MP3=no USE_CODEC_VORBIS=no USE_CODEC_TIMIDITY=no \
    SYSOBJ_SOFT_VID=vid_miyoo.o SYSOBJ_SND=snd_miyoo.o \
    SDL_CFLAGS="-I$SYSROOT/usr/include/SDL -I$SYSROOT/usr/include -I$MI_INC -D_REENTRANT -DUSE_CODEC_VORBIS -DVORBIS_USE_STB $EXTRA" \
    SDL_LIBS="-L$SYSROOT/usr/lib -L$MI_LIB -lSDL -lmi_ao -lmi_sys -lmi_common -lpthread -Wl,--allow-shlib-undefined" \
    || { echo; echo "=== BUILD FAILED (see the first lines with 'error') ==="; exit 1; }

cp hexen2 "$OUT/hexen2-miyoo$SUF-debug" || exit 1
arm-linux-gnueabihf-strip -o "$OUT/hexen2-miyoo$SUF" "$OUT/hexen2-miyoo$SUF-debug" || exit 1

echo
echo "=== DONE ==="
ls -l "$OUT/hexen2-miyoo$SUF" "$OUT/hexen2-miyoo$SUF-debug"
md5sum "$OUT/hexen2-miyoo$SUF" "$OUT/hexen2-miyoo$SUF-debug"
