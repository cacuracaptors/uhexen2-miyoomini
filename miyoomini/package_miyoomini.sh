#!/bin/sh
# Hexen II for the Miyoo Mini Plus: puts the release together.
# Run INSIDE the toolchain container, after build_miyoomini.sh:
#   sh /root/workspace/uhexen2/miyoomini/package_miyoomini.sh v1.0.0
# Result: build-miyoo/release/Roms/... (extract to the SD card root as is);
# zip it in WSL (see the last lines printed).

TOP=/root/workspace/uhexen2
SYSROOT=/root/workspace/mini/arm-buildroot-linux-gnueabihf/sysroot
HERE="$TOP/miyoomini"
VERSION="${1:-dev}"
OUT="$TOP/build-miyoo/release"
GAME="$OUT/Roms/PORTS/Games/Hexen2"

[ -f "$TOP/build-miyoo/hexen2-miyoo" ] || { echo "ERROR: build first (sh build_miyoomini.sh)"; exit 1; }
SDLLIB="$SYSROOT/usr/lib/libSDL-1.2.so.0"
[ -f "$SDLLIB" ] || SDLLIB=$(find /root/workspace/mini -name 'libSDL-1.2.so.0*' 2>/dev/null | head -1)
[ -n "$SDLLIB" ] && [ -f "$SDLLIB" ] || { echo "ERROR: libSDL-1.2.so.0 not found in /root/workspace/mini"; exit 1; }

rm -rf "$OUT"
mkdir -p "$GAME/data1" "$GAME/userdir/data1" "$OUT/Roms/PORTS/Shortcuts" "$OUT/Roms/PORTS/Imgs" || exit 1

cp "$TOP/build-miyoo/hexen2-miyoo" "$GAME/hexen2" || exit 1
cp -L "$SDLLIB" "$GAME/libSDL-1.2.so.0" || exit 1
cp "$HERE/README.txt" "$GAME/README.txt" || exit 1
cp "$TOP/docs/COPYING" "$GAME/LICENSE.txt" || exit 1
cp "$HERE/autoexec.cfg" "$GAME/userdir/data1/autoexec.cfg" || exit 1
cp "$HERE/put_your_hexen2_files_here.txt" "$GAME/data1/" || exit 1
cp "$HERE/Hexen II.port" "$OUT/Roms/PORTS/Shortcuts/" || exit 1
if [ -f "$HERE/Hexen II.png" ]; then
	cp "$HERE/Hexen II.png" "$OUT/Roms/PORTS/Imgs/" || exit 1
else
	echo "WARNING: miyoomini/Hexen II.png is missing: the shortcut will have no image"
fi
chmod +x "$GAME/hexen2" "$OUT/Roms/PORTS/Shortcuts/Hexen II.port"

echo
echo "=== RELEASE $VERSION READY ==="
( cd "$OUT" && find Roms -type f | sort )
echo "libSDL taken from: $SDLLIB"
md5sum "$GAME/hexen2" "$GAME/libSDL-1.2.so.0"
echo
echo "To zip it, outside the container, from this repository's folder:"
echo "  cd build-miyoo/release && python3 -m zipfile -c ../HexenII-miyoomini-$VERSION.zip Roms"
