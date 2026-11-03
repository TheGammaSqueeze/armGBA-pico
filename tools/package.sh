#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 TheGammaSqueeze
# Package a built blob for release (run ./build.sh first):
#
#   tools/package.sh            -> dist/armgba-pico-<version>.zip
#                                  dist/armgba-pico-<version>-src.tar.gz
#
# The zip is what goes on the SD card plus the licence texts: cores/armgba-pico.blob, LICENSE,
# THIRD_PARTY_NOTICES.md, licenses/ (armGBA's MPL-2.0 text and, when the toolchain ships it,
# newlib's copyright file, since the blob links parts of newlib). The source archive is the complete
# Source Code Form of the blob as the MPL asks for: this repository and the upstream submodule at
# the commit it was built from.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
[ -f out/armgba-pico.blob ] || { echo "out/armgba-pico.blob missing: run ./build.sh first" >&2; exit 1; }

VER=${VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo unknown)}
NAME=armgba-pico-$VER
STAGE=dist/$NAME
rm -rf "$STAGE" "dist/$NAME.zip" "dist/$NAME-src.tar.gz"
mkdir -p "$STAGE/cores" "$STAGE/licenses"

cp out/armgba-pico.blob "$STAGE/cores/"
cp LICENSE THIRD_PARTY_NOTICES.md README.md "$STAGE/"
cp upstream/LICENSES/MPL-2.0.txt "$STAGE/licenses/armGBA-MPL-2.0.txt"
NEWLIB_DOC=$(ls /usr/share/doc/libnewlib-arm-none-eabi/copyright 2>/dev/null || true)
if [ -n "$NEWLIB_DOC" ]; then
	cp "$NEWLIB_DOC" "$STAGE/licenses/newlib-COPYRIGHT.txt"
else
	echo "note: newlib's copyright file not found; see THIRD_PARTY_NOTICES.md" >&2
fi
(cd "$STAGE/cores" && sha256sum armgba-pico.blob > armgba-pico.blob.sha256)
{
	echo "armGBA-pico $VER"
	echo "upstream armGBA $(git -C upstream describe --tags --always 2>/dev/null || git -C upstream rev-parse --short HEAD)"
	echo "toolchain $(${CC:-arm-none-eabi-gcc} --version | head -1)"
	echo "blob sha256 $(cut -d' ' -f1 "$STAGE/cores/armgba-pico.blob.sha256")"
} > "$STAGE/BUILD_INFO.txt"
(cd dist && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME")

# Source Code Form: every tracked file, the submodule's included
git ls-files --recurse-submodules -z | tar --null -czf "dist/$NAME-src.tar.gz" --transform "s,^,$NAME-src/," -T -

echo "dist/$NAME.zip"
echo "dist/$NAME-src.tar.gz"
