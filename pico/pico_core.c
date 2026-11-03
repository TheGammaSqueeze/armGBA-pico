/* armGBA-pico: armGBA behind the GammaOS LK's Game Boy Advance core interface.
 *
 * Copyright (c) 2026 TheGammaSqueeze
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
 * the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The LK's Game Boy Advance driver (lk-mt6785 emu/gba/gba_driver.c) was written for gpSP and drives
 * any core through the table in gba_core_abi.h. This file fills that table for armGBA, so the whole
 * driver works unchanged: the BIOS intro, the menu, fast forward, rewind, run ahead, save states,
 * battery saves, resume on boot. What each entry means for gpSP and what it does here:
 *
 *   core_init(arena, size)   the heap over the arena, one machine, a 32 MiB ROM buffer, a scratch
 *                            buffer. Called again after another core used the arena: all fresh.
 *   core_start(rom, bios)    BIOS (the card's, or armGBA's own when the option says so), the ROM
 *                            already in the ROM buffer (used in place, patches/0001), reset past
 *                            the BIOS.
 *   enter_bios               reset into the BIOS instead, so the boot logo plays.
 *   cpu_loop                 runs on the driver's CPU thread: one frame, then yield, forever. The
 *                            driver longjmps back to the top after a state load; a frame always
 *                            ends at a frame boundary here, so that is safe.
 *   pre_frame / post_frame   keys and the solar sensor in; the frame's audio out (low pass filter,
 *                            discarded while the driver mutes run ahead's look ahead frames).
 *   screen                   the frame, through armGBA's LCD Ghosting model when it is on.
 *   state_*                  armGBA's states (the whole machine, about 770 KiB).
 *   backup_*                 the 128 KiB save storage, the same layout as gpSP's, so .sav files
 *                            carry over between the two cores.
 *
 * Version 3 of the table adds what gpSP did not need: the core's name, its audio rate (65536 Hz
 * after post_frame's 2x interpolation of armGBA's 32768 Hz, gpSP's rate), its own state folder (states are not compatible between cores), and the
 * options the LK lists in its Core Settings submenu.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gba.h"
#include "gba_core_abi.h"
#include "pico_lcd.h"

#include "pico_version.h"   /* written by build.sh: the upstream tag and this repo's revision */

void pico_heap_init(void *base, size_t size);
size_t pico_heap_used(void);
void *malloc(size_t n);

static const struct gba_core_imports *s_imp;
static struct GBA *s_gba;
static uint8_t *s_rom;
static unsigned s_rom_size;
static uint8_t *s_scratch;
static unsigned s_scratch_size;
static unsigned s_keys;
static uint32_t s_shown_frame = 0xFFFFFFFFu;
static const uint16_t *s_shown;
static unsigned s_frames;
static int32_t s_lp_last[2];
static int32_t s_up_last[2];   /* the last sample out, for the 2x interpolation */

/* the flags the driver shares with gpSP; only audio_suppress means anything here */
static int s_dynarec_enable = 1;
static volatile int s_audio_suppress;
static int s_load_light;
static unsigned int s_skip_next_frame;   /* the LK sets it for a frame it will not show (fast forward,
                                           * run ahead): pre_frame hands it to the renderer */
extern bool armgbaRenderSkip;             /* patches/0002-host-render-skip.patch */

/* ---- options ---------------------------------------------------------------------------------- */

enum { OPT_BIOS, OPT_GHOSTING, OPT_INTERLACED, OPT_LOWPASS, OPT_SOLAR, OPT_COUNT };

static const char *const v_bios[] = { "Nintendo (SD card)", "armGBA (built in)" };
static const char *const v_ghost[] = { "Off", "25%", "50%", "75%", "100%" };
static const char *const v_onoff[] = { "Off", "On" };
static const char *const v_lowpass[] = { "Off", "20%", "40%", "60%", "80%" };
static const char *const v_solar[] = { "0 (Dark)", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10 (Full Sun)" };

static const struct gba_core_option s_opts[OPT_COUNT] = {
	{ "armgba_bios", "BIOS", v_bios, 2, 0, GBA_CORE_OPT_RESTART },
	{ "armgba_lcd_ghosting", "LCD Ghosting", v_ghost, 5, 0, 0 },
	{ "armgba_interlaced", "Interlaced Rows", v_onoff, 2, 0, 0 },
	{ "armgba_lowpass", "Audio Low-Pass Filter", v_lowpass, 5, 0, 0 },
	{ "armgba_solar", "Solar Sensor", v_solar, 11, 5, 0 },
};
static int s_opt[OPT_COUNT] = { 0, 0, 0, 0, 5 };

static int opt_count(void) { return OPT_COUNT; }
static const struct gba_core_option *opt_info(int i) { return (i >= 0 && i < OPT_COUNT) ? &s_opts[i] : NULL; }
static int opt_get(int i) { return (i >= 0 && i < OPT_COUNT) ? s_opt[i] : 0; }
static void opt_set(int i, int v)
{
	if (i < 0 || i >= OPT_COUNT || v < 0 || v >= s_opts[i].nvalues) return;
	if (i == OPT_GHOSTING && v != s_opt[i]) picoLcdReset();
	s_opt[i] = v;
}

/* ---- host services for pico_libc.c -------------------------------------------------------------- */

long pico_host_time(void) { return (s_imp && s_imp->host_time) ? s_imp->host_time() : 0; }
uint64_t pico_host_ms(void) { return (s_imp && s_imp->host_ms) ? s_imp->host_ms() : 0; }

/* patches/0001: gbaLoadRom takes the ROM where the LK already put it. Everything past the image is
 * zeroed, as upstream's calloc'd buffer would be (out of range reads see 0). */
uint8_t *armgbaHostRomBuffer(const void *data, size_t size)
{
	(void) data;
	if (size < SIZE_ROM) memset(s_rom + size, 0, SIZE_ROM - size);
	return s_rom;
}

/* ---- the table ---------------------------------------------------------------------------------- */

static int core_init(void *arena, unsigned size)
{
	pico_heap_init(arena, size);
	s_gba = NULL;
	s_rom = malloc(SIZE_ROM);
	s_scratch_size = 4u << 20;
	s_scratch = malloc(s_scratch_size);
	if (!s_rom || !s_scratch) return -1;
	s_gba = gbaCreate();
	if (!s_gba) return -1;
	/* the LK takes RGB565 frames; armGBA draws BGR555 unless told (upstream libretro.c does the same) */
	s_gba->renderer.rgb565 = true;
	s_shown_frame = 0xFFFFFFFFu;
	picoLcdReset();
	return 0;
}

static int core_start(unsigned romsz, const void *bios16k)
{
	if (!s_gba) return -1;
	if (s_opt[OPT_BIOS] == 1 || !bios16k) {
		if (!gbaLoadBios(s_gba, armgbaBios, SIZE_BIOS)) return -1;
	} else if (!gbaLoadBios(s_gba, bios16k, SIZE_BIOS)) {
		return -1;
	}
	/* the LK keeps a battery save for every game: the save type is detected keeping what is there */
	s_gba->savedata.backed = true;
	s_gba->renderer.rgb565 = true;
	s_rom_size = romsz;
	if (!gbaLoadRom(s_gba, s_rom, romsz)) return -1;
	gbaReset(s_gba, true);
	s_shown_frame = 0xFFFFFFFFu;
	s_lp_last[0] = s_lp_last[1] = 0;
	picoLcdReset();
	return 0;
}

static void enter_bios(void) { if (s_gba) gbaReset(s_gba, false); }
static void reset(void) { if (s_gba) { gbaReset(s_gba, true); picoLcdReset(); } }

static void cpu_loop(void)
{
	for (;;) {
		uint32_t const frame = s_gba->video.frameCounter;
		do {
			gbaRunPart(s_gba);
		} while (s_gba->video.frameCounter == frame);
		s_frames++;
		s_imp->yield_to_main();
	}
}

static void pre_frame(void)
{
	static const int levels[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };   /* upstream libretro.c */
	if (!s_gba) return;
	armgbaRenderSkip = s_skip_next_frame != 0;   /* lines 0..159 of the frame about to run */
	gbaSetKeys(s_gba, (uint16_t) s_keys);
	if (s_gba->gpio.devices & HW_LIGHT_SENSOR) {
		int const level = s_opt[OPT_SOLAR];
		s_gba->gpio.luminance = (uint8_t) (0xFF - (0x16 + (level ? levels[level - 1] : 0)));
	}
}

static void post_frame(void)
{
	struct Audio *a;
	unsigned n;
	if (!s_gba) return;
	a = &s_gba->audio;
	n = a->outCount;
	a->outCount = 0;
	if (!n || s_audio_suppress) return;
	if (s_opt[OPT_LOWPASS]) {
		/* upstream libretro.c sendSound: each sample out is the last one weighted by lowPass, out of
		 * 0x10000, and the new one by the rest */
		int32_t const lowPass = s_opt[OPT_LOWPASS] * 20 * 0x10000 / 100;
		int16_t *k = (int16_t *) a->out;
		for (unsigned i = 0; i < n * 2; ++i) {
			s_lp_last[i & 1] = (s_lp_last[i & 1] * lowPass + k[i] * (0x10000 - lowPass)) >> 16;
			k[i] = (int16_t) s_lp_last[i & 1];
		}
	}
	/* The LK's GBA audio path was built for gpSP's 65536 Hz: its resampler only ever brings the
	 * rate down to the 48 kHz output. armGBA makes 32768 Hz, so every sample is preceded by the
	 * midpoint to the one before: 65536 Hz, linear interpolation. */
	{
		static int16_t up[4096 * 2 * 2];
		const int16_t *k = (const int16_t *) a->out;
		unsigned m = n > 4096 ? 4096 : n;
		for (unsigned i = 0; i < m; ++i) {
			int16_t const l = k[i * 2], r = k[i * 2 + 1];
			up[i * 4 + 0] = (int16_t) ((s_up_last[0] + l) >> 1);
			up[i * 4 + 1] = (int16_t) ((s_up_last[1] + r) >> 1);
			up[i * 4 + 2] = l;
			up[i * 4 + 3] = r;
			s_up_last[0] = l;
			s_up_last[1] = r;
		}
		s_imp->audio_submit(up, m * 2);
	}
}

static void frame_boundary_finish(void) { }
static unsigned int cpu_ticks(void) { return s_frames * 280896u; }
static void set_keys(unsigned mask) { s_keys = mask & 0x3FFu; }

static const unsigned short *screen(void)
{
	const uint16_t *f;
	if (!s_gba) return NULL;
	f = &s_gba->renderer.frame[0][0];
	if (!s_opt[OPT_GHOSTING]) return f;
	/* the model steps once per new frame, however often the driver asks for the picture */
	if (s_shown_frame != s_gba->video.frameCounter) {
		s_shown_frame = s_gba->video.frameCounter;
		s_shown = picoLcdProcess(f, s_opt[OPT_GHOSTING], s_opt[OPT_INTERLACED] != 0);
	}
	return s_shown ? s_shown : f;
}

static void screen_fill(unsigned short v)
{
	uint16_t *p;
	if (!s_gba) return;
	p = &s_gba->renderer.frame[0][0];
	for (unsigned i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) p[i] = v;
	/* armGBA redraws only the lines whose inputs changed and keeps the others' pixels: after the
	 * fill those pixels are not the game's any more, so every line is drawn again on the next
	 * frame. Without this the LK's white fill at a game start (made for gpSP, which draws every
	 * line every frame) stayed on the lines that did not change until the next scene. */
	memset(s_gba->renderer.scanlineDirty, 0xFF, sizeof(s_gba->renderer.scanlineDirty));
	picoLcdReset();
	s_shown_frame = 0xFFFFFFFFu;
}

static unsigned state_size(void) { return (unsigned) gbaStateSize(); }
static void state_save(void *buf) { if (s_gba) gbaSaveState(s_gba, buf); }
static void state_load(const void *buf)
{
	if (!s_gba) return;
	gbaLoadState(s_gba, buf, gbaStateSize());
	/* A state loaded by the user (Load State, resume) is drawn again in full on its first frame,
	 * so the picture never depends on the kept pixels in the state: one made while a picture was
	 * stale (the white fill bug, see screen_fill) heals at once instead of at the next scene.
	 * Run ahead and rewind load a state every frame (the LK sets load_light for them): those
	 * keep the line cache, a full redraw each time would cost them the render time it saves. */
	if (!s_load_light)
		memset(s_gba->renderer.scanlineDirty, 0xFF, sizeof(s_gba->renderer.scanlineDirty));
}
static void sound_ring_save(void *dst) { (void) dst; }          /* armGBA's state carries the audio */
static void sound_ring_load(const void *src) { (void) src; }
static unsigned char *rom_ptr(void) { return s_rom; }
static unsigned rom_capacity(void) { return SIZE_ROM; }
static unsigned char *scratch_ptr(void) { return s_scratch; }
static unsigned scratch_size(void) { return s_scratch_size; }
static void *backup_ptr(void) { return s_gba ? s_gba->savedata.storage : NULL; }
static unsigned backup_size(void) { return s_gba ? (unsigned) sizeof(s_gba->savedata.storage) : 0; }
static unsigned heap_used(void) { return (unsigned) pico_heap_used(); }

const struct gba_core_exports *gba_core_blob_init(const struct gba_core_imports *imp)
{
	static const struct gba_core_exports exports = {
		GBA_CORE_ABI_MAGIC, GBA_CORE_ABI_V3,
		core_init, core_start, enter_bios, reset,
		cpu_loop, pre_frame, post_frame,
		frame_boundary_finish, cpu_ticks, set_keys,
		screen, screen_fill,
		state_size, state_save, state_load,
		sound_ring_save, sound_ring_load,
		rom_ptr, rom_capacity,
		scratch_ptr, scratch_size,
		backup_ptr, backup_size,
		&s_dynarec_enable, &s_audio_suppress, &s_load_light,
		&s_skip_next_frame,
		ARMGBA_PICO_VERSION, 65536u, "/states/armgba",   /* armGBA makes 32768 Hz; post_frame doubles it */
		opt_count, opt_info, opt_get, opt_set,
		heap_used,
	};
	s_imp = imp;
	return &exports;
}
