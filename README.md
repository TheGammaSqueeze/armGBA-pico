# armGBA-pico

[![Support armGBA on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20armGBA-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/magnominus)

> **Enjoying armGBA-pico? Please support the upstream project.** All of the emulation here is
> Magnominus's work on [armGBA](https://github.com/MagnominusGit/armGBA). If this core makes your
> games better, consider buying them a coffee on [Ko-fi](https://ko-fi.com/magnominus).

[armGBA](https://github.com/MagnominusGit/armGBA) by Magnominus, built as a loadable Game Boy
Advance core for the GammaOS bootloader emulator on the Konkr Pocket Advance (MediaTek MT6785,
`lk-mt6785`, branch `lk-gba-emu-sd-card-kpa`).

All of the emulation is armGBA's. This repository adds no emulation code: it holds the upstream
tree as a submodule, the smallest possible set of patches, the glue that lets armGBA run inside
a bootloader with no operating system, and a build script.

**This is an experimental project and does not accept issues.** Please do not report problems you
see with armGBA-pico to armGBA upstream either: this build runs armGBA in a bootloader on hardware
upstream does not target, so its behaviour here says nothing about armGBA itself.

## Credits

- **armGBA** (Magnominus, MPL-2.0, [support them on Ko-fi](https://ko-fi.com/magnominus)): the Game Boy Advance emulator, its recompiler for 32 bit ARM,
  its renderer and its LCD model. See upstream's README for its own credits, notably **mGBA**
  (Jeffrey Pfau and contributors, MPL-2.0), whose renderer, bus and I/O behaviour, save chips,
  cartridge GPIO devices, save overrides, sound channels and cheat engine armGBA builds on.
- `pico/pico_lcd.c` is armGBA's LCD Ghosting and Interlaced Rows model from
  `upstream/libretro/lcd.c`, reduced to the 16 bit, single thread path (see the file's header).
- The audio low-pass filter and the solar sensor levels in `pico/pico_core.c` follow
  `upstream/libretro/libretro.c`.

## License

Only armGBA's Game Boy Advance core (`upstream/src/`, MPL-2.0) is built; armGBA's libretro
frontend and its Gambatte based Game Boy core (GPL-2.0) are not part of the blob. The glue in
`pico/`, `patches/`, `tools/` and the build scripts are MPL-2.0 as well, so the blob as a whole is
under the Mozilla Public License 2.0 (`LICENSE`). Each file keeps its own notice. The blob also
links a few newlib routines (BSD style licences) and libgcc's division helpers (GCC Runtime Library
Exception); `THIRD_PARTY_NOTICES.md` lists everything, and every release carries the licence texts
and the complete source the blob was built from.

## Layout

| Path | What it is |
| --- | --- |
| `upstream/` | armGBA, as a git submodule, never modified in place |
| `patches/` | changes to upstream, applied by `build.sh` to a copy in `out/src` |
| `pico/pico_core.c` | the LK's core interface (`gba_core_abi.h`, version 3) filled in for armGBA |
| `pico/pico_libc.c` | heap, `mmap` for the recompiler, cache maintenance, newlib's system calls |
| `pico/pico_lcd.c` | armGBA's LCD Ghosting model, see above |
| `pico/include/` | headers the bootloader target lacks (`sys/mman.h`, `pthread.h`, `clock_gettime`) |
| `pico/gba_core_abi.h` | a copy of `lk-mt6785/emu/gba/gba_core_abi.h`; the LK's copy is the reference |
| `pico/blob.ld` | link script: flat image, fixed address, 20 byte header |
| `build.sh` | builds `out/armgba-pico.blob` |
| `tools/package.sh` | packs a release: the SD card zip and the source archive, into `dist/` |
| `tools/harness.c`, `tools/build_harness.sh` | runs the blob on a PC under qemu-arm, as the LK does |
| `.github/workflows/build.yml` | CI: builds every push, publishes a release for a `v*` tag |
| `THIRD_PARTY_NOTICES.md` | what is in the blob and under which licence |

### Patches

| Patch | Why |
| --- | --- |
| `0001-host-rom-buffer.patch` | `gbaLoadRom` normally copies the image into a 32 MiB buffer of its own. Built with `ARMGBA_HOST_ROM_BUFFER`, it uses the buffer the host has already loaded the image into. The bootloader's 62 MiB arena has no room for two 32 MiB ROM buffers next to the recompiler's 16 MiB of code. That buffer stays the host's: loading the next game never frees it, and `gbaMapRom` (mapping an image from its file) is left out, since the bootloader has no files to map. Without the define, upstream's code is unchanged. |
| `0002-host-render-skip.patch` | Built with `ARMGBA_HOST_RENDER_SKIP`, `renderScanline` leaves the drawing out while the host's `armgbaRenderSkip` is set, keeping all of its per line bookkeeping: a line that changed stays dirty, so the next frame drawn redraws it, and a line that did not change keeps its pixels as it always does. The flag is a global, not part of `struct GBA`, so save states are unchanged. Without the define, upstream's code is unchanged. |
| `0003-load-keeps-code.patch` | Built with `ARMGBA_HOST_LOAD_KEEPS_CODE`, loading a state no longer throws all compiled code away when a work RAM word it was compiled from changed. Each such word goes through what a store to it does (kept when only an operand the code reads from memory changed, else the blocks compiled from it are removed); code in VRAM and changed wait states still flush everything. Run ahead loads a state every frame: in Mario & Luigi 1183 of 1200 loads flushed all the code, and the recompiling made the frame times uneven enough to starve the audio (skips). Under the harness the same 1200 frames with run ahead 3 took 9.4 s instead of 109.5 s, with the same pictures and sound as without run ahead. Without the define, upstream's code is unchanged. |
| `0004-sram-patched-eeprom.patch` | Images made for reproduction cartridges, which have SRAM where the original has an EEPROM, rewrite the save library's EEPROM block read and write as copies to and from SRAM. Upstream's override list gives Super Mario Advance 2 and 3 an EEPROM, so with such an image their saves went nowhere (Super Mario Advance 3 stopped on "Your saved data is corrupted."). An EEPROM the list forces now becomes SRAM when the image holds that rewritten copy. Sent upstream as a pull request; it goes once upstream has it. |
| `0005-load-host-copy.patch` | Built with `ARMGBA_HOST_LOAD_COPY`, loading a state takes the copy of the running machine it keeps aside (about 770 KiB) from the host instead of allocating a fresh one each time; armGBA-pico makes it once at the core's start. Run ahead loads a state every frame, and in the bootloader's heap, which the recompiler's churn keeps cutting up, a fresh copy came after a while to find no room: every load was then refused, the look-ahead frames stayed, and the game ran at twice its speed (Super Mario Advance 3, after some minutes of play). Without the define, upstream's code is unchanged. |
| `0006-host-code-size.patch` | Built with `ARMGBA_HOST_CODE_MIB` (24 here), the recompiler's code buffer is that many MiB instead of upstream's 16 on 32 bit ARM, still within the 32 MiB the A32 branches between its blocks reach. The LK gives the core a 128 MiB window of its own in high DRAM, so the bigger buffer costs nothing, and a game that compiles a lot (Super Mario Advance 3 in fast forward filled 16 MiB about every 30 seconds) throws all its code away and compiles it again half as often. Without the define, upstream's code is unchanged. |

## Building

Needs `arm-none-eabi-gcc` with newlib (tested with 10.3-2021.07) and git.

```
git clone --recurse-submodules <this repository>   # or: git submodule update --init upstream
./build.sh
```

Output: `out/armgba-pico.blob`, plus the ELF and a link map. The script prints the size, how much
of the 4 MiB core slot it takes, and its SHA-256.

Build settings, chosen for speed on the device: ARMv8-A in 32 bit ARM state, tuned for the
Cortex-A55 (the LK runs on CPU 0 of the MT6785, an in order A55, MIDR 0x411fd050), NEON and
hardware floating point (softfp calling convention, as the LK's own NEON code uses), `-O3` and link
time optimisation, armGBA's 32 bit recompiler (`src/cpu/a32`) and its NEON renderer, exactly as
upstream selects them for an `arm` processor. On the device, 600 frames from the same save state
at 599 MHz took 5.38 ms a frame tuned for the A55 against 5.52 ms tuned for the A76, with the same
pictures.

For experiments `TUNE` (default `cortex-a55`), `OPT` (default `-O3`) and `EXTRA_CFLAGS` override
the flags, e.g. `TUNE=cortex-a76 ./build.sh`. Releases use the defaults.

## Releases and CI

`.github/workflows/build.yml` builds the blob on Ubuntu 22.04 (its `gcc-arm-none-eabi` is the
10.3-2021.07 toolchain the blob is developed with) for every push and pull request, builds the
harness, and keeps the packaged result as a workflow artifact. Pushing a tag named `v*` (for
example `git tag v1.4-pico1 && git push origin v1.4-pico1`) also publishes a GitHub release
with:

- `armgba-pico-<version>.zip`: `cores/armgba-pico.blob` (copy the `cores` folder to the root of the
  SD card), its SHA-256, `BUILD_INFO.txt`, `LICENSE`, `THIRD_PARTY_NOTICES.md` and `licenses/`;
- `armgba-pico-<version>-src.tar.gz`: this repository with the upstream submodule at the commit
  the blob was built from (the source the MPL asks to go with the blob);
- `armgba-pico.blob` on its own.

Locally the same package comes from `./build.sh && tools/package.sh`.

## Testing on a PC

`tools/harness.c` maps the blob and the arena at the LK's addresses and drives it exactly as the
LK's driver does (a CPU thread that yields once a frame, pre and post frame calls), under
qemu-arm. It prints a picture hash every 60 frames (`HARNESS_SKIP=k` draws only one frame in k, as fast
forward does; the hashes must not change), checks that a save state taken and loaded
reproduces the same frames (run ahead and rewind rely on it) and that the audio rate is the
declared one. Picture hashes equal between two builds mean the two emulate identically.

More switches, all environment variables:

| Variable | What it does |
| --- | --- |
| `HARNESS_SKIP=k` | draw one frame in k, as fast forward does |
| `HARNESS_STATE=file` | load that save state (as the LK keeps it on the card) after the first frame |
| `HARNESS_SHOTS=0,1,30` | with `HARNESS_STATE`: write those frames after the load as PPM files |
| `HARNESS_FILL=n` | the LK's white screen fill after frame n |
| `HARNESS_CYCLE=n` | save and load the state back after frame n |
| `HARNESS_STACK_KB` | the CPU thread's stack (64 KiB, as in the LK) |

`EXTRA_PATCHES=/path/a.patch ./build.sh` applies more patches after `patches/` for experiments
(a debug check of the renderer, say); under the harness the core can print to stderr. Releases
use neither.

```
tools/build_harness.sh        # needs arm-linux-gnueabihf-gcc
qemu-arm-static out/harness out/armgba-pico.blob gba_bios.bin game.gba 1200 last.ppm
```

## Installing

Copy `out/armgba-pico.blob` to the SD card as `/cores/armgba-pico.blob`. That is all: the LK uses
it for Game Boy Advance games when it is there, and gpSP from boot_b when it is not.

## How the LK uses it

- Boot and the GBA BIOS intro always run on gpSP from boot_b, so the intro starts before the SD
  card is mounted. When a Game Boy Advance game is started, the LK loads the blob from the card
  into the GBA core slot (0x4E400000) and starts the game on armGBA. The CPU thread that runs the
  core is parked at a frame boundary during the switch and re-enters the new core.
- The in-game Pico menu has **GBA Core** (armGBA or gpSP, from the next game) and **Core
  Settings**, which lists the options this blob offers. The choices are kept on the card in
  `/gba/armgba-pico.cfg`.
- Everything the driver does for gpSP works the same: fast forward, rewind, run ahead, save states,
  battery saves, resume on boot, the overdrive and BFI, the FPS counter.
- Battery saves are shared with gpSP (`/saves/gba`): both keep a 128 KiB save buffer with the data
  at the start. Save states are not compatible between the two cores, so armGBA keeps its own in
  `/states/armgba`.

### Core Settings

| Option | Values | Notes |
| --- | --- | --- |
| BIOS | Nintendo (SD card), armGBA (built in) | from the next game start; the card's `gba_bios.bin` is still needed for the boot intro |
| LCD Ghosting | Off, 25 %, 50 %, 75 %, 100 % | armGBA's model of the Advance's panel |
| Interlaced Rows | Off, On | with LCD Ghosting, alternate rows driven harder each frame, as on the console |
| Audio Low-Pass Filter | Off, 20 %, 40 %, 60 %, 80 % | |
| Solar Sensor | 0 to 10 | the sunlight level for Boktai and other solar sensor cartridges |

armGBA's other options are either already provided by the LK (BIOS intro skip, colour
correction, the LCD filters, turbo buttons, the restart combo) or need hardware and a frontend this
target does not have (link cable and netplay, tilt and gyro sensors, threaded video, the Game Boy
core).

## Target notes for maintainers

- **The interface.** `pico/gba_core_abi.h` is the contract with the LK: a table of functions and
  pointers the LK's GBA driver calls (`gba_core_exports`) and the services it gives the core
  (`gba_core_imports`). Version 2 is gpSP's; version 3 adds the core's name, its audio rate, its
  state folder and the option table. Keep the copy here identical to the LK's.
- **Threads.** The LK runs the core on one thread that yields once per frame. armGBA's render
  thread (Threaded Video) is never started; `pico/include/pthread.h` only lets its file compile.
- **NEON.** The LK does not save NEON or VFP registers on a thread switch. The core's thread and
  the LK's emulator thread (which runs the LK's own NEON display code) strictly alternate, one
  frame each, and no other LK thread uses NEON, so the registers are never shared mid-use. Any
  future LK thread that uses NEON while a game runs breaks this.
- **Memory.** The LK hands `core_init` a 62 MiB arena at 0x50000000. `pico/pico_libc.c` runs the
  heap there: the 32 MiB ROM buffer, the 16 MiB recompiler code buffer (the arena is mapped
  executable), the machine (about 770 KiB) and the recompiler's tables. The LK's menu uses the same
  arena while it is open, so the LK calls `core_init` again before every game.
- **Audio.** armGBA makes 32768 Hz stereo; the LK's GBA audio path takes gpSP's 65536 Hz, so
  `post_frame` doubles the rate by linear interpolation.
- **The picture buffer.** armGBA keeps a line's pixels while nothing it shows changes. The LK
  fills the core's screen white at a game start (`screen_fill`, made for gpSP, which draws every
  line every frame); the glue then marks every line for drawing, or the white stayed on the lines
  that did not change until the next scene. A state loaded by the user (Load State, resume) is
  drawn again in full too, so a state saved while a picture was wrong heals at once; run ahead
  and rewind loads (the LK's `load_light`) keep the line cache.
- **Render skip.** For frames the LK will not show (fast forward between presents, run ahead's
  look-ahead frames) it sets the core's skip flag, and `pre_frame` hands it to the renderer
  (patch 0002). The emulation is unchanged; only the drawing is left out. Shown frames are
  identical to drawing every frame: checked with `HARNESS_SKIP` on 3000 frames and on the device
  (Iridion 3D, same picture hash, 4.8 against 4.3 ms a frame at 599 MHz drawing one frame in six).
