#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 TheGammaSqueeze
# armGBA-pico: build the armGBA core blob for the GammaOS LK (lk-mt6785, Konkr Pocket Advance).
#
#   ./build.sh            -> out/armgba-pico.blob (copy it to the SD card as /cores/armgba-pico.blob)
#   ./build.sh clean
#
# The upstream tree (submodule upstream/, MagnominusGit/armGBA) is never modified: its src/ is
# copied to out/src, the patches in patches/ are applied there, and that copy is compiled together
# with the glue in pico/. Only armGBA's Game Boy Advance core (src/, MPL-2.0) is built; its libretro
# frontend and its Gambatte based Game Boy core are not used (the LK has its own Game Boy core).
#
# Target: the LK runs the core in 32 bit ARM state on the Cortex-A76 (MT6785). Built for ARMv8-A
# with NEON and hardware floating point (softfp calling convention, as the LK's own NEON code), -O3
# and link time optimisation, with armGBA's 32 bit recompiler (src/cpu/a32) and its NEON renderer.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
OUT=$ROOT/out
CC=${CC:-arm-none-eabi-gcc}
OBJCOPY=${OBJCOPY:-arm-none-eabi-objcopy}

if [ "${1:-}" = clean ]; then rm -rf "$OUT"; exit 0; fi

if [ ! -f upstream/src/gba.c ]; then
	git submodule update --init upstream
fi

UPSTREAM_REV=$(git -C upstream describe --tags --always 2>/dev/null || git -C upstream rev-parse --short HEAD)
PICO_REV=$(git describe --always --dirty 2>/dev/null || echo unknown)
VERSION="armGBA ${UPSTREAM_REV} (pico ${PICO_REV})"

rm -rf "$OUT/src" "$OUT/obj"
mkdir -p "$OUT/obj"
printf '#define ARMGBA_PICO_VERSION "%s"\n' "$VERSION" > "$OUT/pico_version.h"
cp -a upstream/src "$OUT/src"
# EXTRA_PATCHES: more patches for experiments (debug checks), applied after these; releases use none
for p in patches/*.patch ${EXTRA_PATCHES:-}; do
	[ -e "$p" ] || continue
	echo ">> patch $(basename "$p")"
	case "$p" in /*) pp=$p;; *) pp=$ROOT/$p;; esac
	(cd "$OUT" && patch -p1 --quiet --forward < "$pp")
done

# The LK runs on CPU 0, a Cortex-A55 (MIDR 0x411fd050), an in-order core: scheduling for it
# matters. TUNE and EXTRA_CFLAGS override for experiments.
TUNE=${TUNE:-cortex-a55}
ARCH="-march=armv8-a+simd -mtune=$TUNE -marm -mfpu=neon-fp-armv8 -mfloat-abi=softfp"
CFLAGS="$ARCH -std=gnu11 ${OPT:--O3} -flto -ffunction-sections -fdata-sections -fno-common \
	${EXTRA_CFLAGS:-} -Wall -Wno-unused-parameter -Wno-unused-function \
	-DARMGBA_HOST_ROM_BUFFER -DARMGBA_HOST_LOAD_KEEPS_CODE -DARMGBA_HOST_LOAD_COPY -DARMGBA_HOST_CODE_MIB=30 -DARMGBA_HOST_LOADS_GO_BACK -DARMGBA_HOST_IDLE_TENURE -DARMGBA_HOST_MID_ENTRIES -I$ROOT/pico/include -I$ROOT/pico -I$OUT/src -I$OUT \
	-include $ROOT/pico/include/pico_time.h"

# armGBA's core sources (upstream CMakeLists.txt CORE_SOURCES) with the 32 bit ARM backend and
# the NEON renderer, as upstream selects them for an "arm" processor
CORE="bios-image.c bus.c cheats.c gba.c memory.c state.c timing.c audio/audio.c audio/psg.c
	cart/cart.c cart/ereader.c cart/gpio.c cart/overrides.c cart/savedata.c cpu/arm.c cpu/decode.c cpu/step.c
	io/dma.c io/io.c io/rfu.c io/sio.c io/timer.c video/render-thread.c video/render.c
	video/video.c video/render-neon.c cpu/jit.c cpu/a32/backend.c"
PICO="pico_core.c pico_lcd.c pico_libc.c"

OBJS=()
for f in $CORE; do
	o="$OUT/obj/core_$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
	$CC $CFLAGS -c "$OUT/src/$f" -o "$o"
	OBJS+=("$o")
done
for f in $PICO; do
	o="$OUT/obj/pico_${f%.c}.o"
	# the C runtime is compiled outside LTO: newlib's archive members reference its symbols
	# (_calloc_r, _write, ...) only at the final link, after LTO would already have dropped them.
	# It is also built -fno-builtin, as any malloc implementation must be: GCC otherwise turns
	# calloc's malloc + memset into a call to calloc (seen at -O2: calloc called itself forever).
	if [ "$f" = pico_libc.c ]; then FL="${CFLAGS/-flto/} -fno-builtin"; else FL="$CFLAGS"; fi
	$CC $FL -c "$ROOT/pico/$f" -o "$o"
	OBJS+=("$o")
done

$CC $CFLAGS -nostartfiles -Wl,--gc-sections -Wl,-T,"$ROOT/pico/blob.ld" -Wl,-Map,"$OUT/armgba-pico.map" \
	-o "$OUT/armgba-pico.elf" "${OBJS[@]}" -lm -lc -lgcc
$OBJCOPY -O binary "$OUT/armgba-pico.elf" "$OUT/armgba-pico.blob"

python3 - "$OUT/armgba-pico.blob" "$VERSION" <<'EOF'
import struct, sys, hashlib
d = open(sys.argv[1], 'rb').read()
magic, ver, entry, load, span = struct.unpack_from('<5I', d)
assert magic == 0x41424743 and ver == 3, "bad header"
assert load == len(d), "load size %d != file %d" % (load, len(d))
print("armgba-pico.blob: %s" % sys.argv[2])
print("  file %d bytes, span %d bytes (%.2f MiB of the 4 MiB slot), entry +0x%x" % (len(d), span, span / 1048576.0, entry))
print("  sha256 %s" % hashlib.sha256(d).hexdigest())
EOF
