# Third party notices

The armGBA-pico blob (`armgba-pico.blob`) is built from the following. Everything in it is under
the Mozilla Public License 2.0 except the C runtime parts listed last, which are under permissive
licences that only ask for their notices to be kept.

## armGBA (MPL-2.0)

- Source: <https://github.com/MagnominusGit/armGBA>, the `upstream/` submodule, at the commit
  recorded in this repository (and in `BUILD_INFO.txt` of a release).
- Copyright (c) 2026 Magnominus. Only `upstream/src/` (the Game Boy Advance core, including the
  built in BIOS image generated from `upstream/bios/`) is compiled; both are MPL-2.0
  (`upstream/LICENSES/MPL-2.0.txt`, shipped as `licenses/armGBA-MPL-2.0.txt`).
- armGBA builds on **mGBA** (Jeffrey Pfau and contributors, MPL-2.0); files that follow mGBA's code
  carry its copyright notice as well. See upstream's README, "Credits" and "License".
- armGBA as a whole repository is distributed under the GPL, version 2, because its libretro core
  links Gambatte. None of the GPL parts (`gambatte/`, `armulators/`) and none of the libretro
  frontend are built into this blob.
- `pico/pico_lcd.c` is armGBA's LCD Ghosting model from `upstream/libretro/lcd.c` (MPL-2.0),
  reduced for this target; the audio low-pass filter and solar sensor levels in `pico/pico_core.c`
  follow `upstream/libretro/libretro.c` (MPL-2.0).

## This repository (MPL-2.0)

The glue in `pico/`, the patches in `patches/`, the build and packaging scripts and the test harness
in `tools/`: Copyright (c) 2026 TheGammaSqueeze, MPL-2.0 (`LICENSE`). `pico/gba_core_abi.h` is a
copy of the interface header of the GammaOS LK (lk-mt6785) by the same author, provided here under
MPL-2.0.

## The C runtime linked into the blob

- **newlib** (Red Hat and the newlib contributors): the string, formatted I/O, conversion, locale
  and time functions the core calls (the link map, `out/armgba-pico.map`, lists every object).
  newlib is a collection of files under BSD style and similar permissive licences; the notices
  are in newlib's `COPYING.NEWLIB` and, for the toolchain the releases are built with (Ubuntu's
  `libnewlib-arm-none-eabi`), in the copyright file shipped in a release as
  `licenses/newlib-COPYRIGHT.txt`.
- **libgcc** (the GCC project): integer division helpers. GPL-3.0 with the GCC Runtime Library
  Exception, which places no conditions on the compiled blob.

## Corresponding source

The MPL asks that whoever receives the blob can obtain its source. Every release carries
`armgba-pico-<version>-src.tar.gz`, this repository with the upstream submodule at the exact commit
the blob was built from. `./build.sh` rebuilds the blob from it.
