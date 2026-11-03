/* armGBA-pico: LCD Ghosting and Interlaced Rows on the core's 240 x 160 RGB565 picture.
 *
 * This is armGBA's own LCD model, from upstream/libretro/lcd.c (Copyright (c) 2026 Magnominus,
 * MPL-2.0): buildNext, the ghost state and ghostJob, cut down to what this target needs. The
 * libretro frontend applies it to the picture it sends to RetroArch; here there is no RetroArch, so
 * the same model runs on the frame before the LK takes it. Differences from upstream, all forced by
 * the target, none in the model itself:
 *   - only the 16 bit (RGB565) path and only the Advance's panel (the TFT time constants), since
 *     this file only ever sees Game Boy Advance pictures;
 *   - one thread: upstream shares the rows out among worker threads, the LK gives the core one CPU;
 *   - the rerun check: upstream skips frames RetroArch runs again for run ahead; here the caller
 *     only processes a frame once, when it is shown (pico_core.c).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
 * the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pico_lcd.h"

/* upstream: screenTimes, milliseconds to darken and to lighten; the Color's and Advance's TFT */
static const float tftTimes[2] = { 15, 30 };

static uint8_t next5[2][256 * 32], next6[2][256 * 64];
static int builtFor = -1;

/* upstream buildNext, the 5 and 6 bit tables */
static void buildNext(int strength, int bits, uint8_t* table, float scale) {
	static float linear[256], bounds[255];
	if (!linear[255]) {
		for (int v = 0; v < 256; ++v) {
			linear[v] = (float) pow(v / 255.0, 2.2);
			if (v < 255) {
				bounds[v] = (float) pow((v + 0.5) / 255, 2.2);
			}
		}
	}
	float const dark = (float) (1 - exp(-16.743 * 4 / strength / (tftTimes[0] * scale)));
	float const light = (float) (1 - exp(-16.743 * 4 / strength / (tftTimes[1] * scale)));
	int const codes = 1 << bits;
	for (int now = 0; now < 256; ++now) {
		for (int code = 0; code < codes; ++code) {
			int const full = bits == 6 ? code << 2 | code >> 4 : code << 3 | code >> 2;
			float const diff = linear[full] - linear[now];
			float const moved = linear[now] + diff * (diff < 0 ? dark : light);
			int next = 0;
			for (int step = 128; step; step >>= 1) {
				next += next + step <= 255 && bounds[next + step - 1] < moved ? step : 0;
			}
			if (next == now && now != full) {
				next += full > now ? 1 : -1;
			}
			table[now * codes + code] = (uint8_t) next;
		}
	}
}

#define W 240
#define H 160

static struct {
	uint8_t levels[W * H * 3];
	uint8_t busy[W * H];
	uint16_t rowBusy[H];
	uint16_t last[W * H];
	uint16_t shown[W * H];
	bool valid;
	unsigned frames;
} ghost;

void picoLcdReset(void) { ghost.valid = false; }

/* upstream ghostJob for every row, 16 bit pixels */
static void ghostRows(const uint16_t* picture, bool alternate) {
	for (unsigned y = 0; y < H; ++y) {
		const uint16_t* const in = picture + y * W;
		uint16_t* const before = ghost.last + y * W;
		uint16_t* const out = ghost.shown + y * W;
		if (!ghost.rowBusy[y] && !memcmp(in, before, W * 2)) {
			memcpy(out, in, W * 2);
			continue;
		}
		int const weak = alternate && (y + ghost.frames) % 2;
		uint8_t* level = ghost.levels + y * W * 3;
		uint8_t* moving = ghost.busy + y * W;
		unsigned count = 0;
		const uint8_t* const t5 = next5[weak];
		const uint8_t* const t6 = next6[weak];
		for (unsigned x = 0; x < W; ++x, level += 3) {
			uint32_t const input = in[x];
			if (input == before[x] && !moving[x]) {
				out[x] = (uint16_t) input;
				continue;
			}
			uint32_t const r = t5[level[0] << 5 | input >> 11];
			uint32_t const g = t6[level[1] << 6 | (input >> 5 & 0x3F)];
			uint32_t const b = t5[level[2] << 5 | (input & 0x1F)];
			level[0] = (uint8_t) r;
			level[1] = (uint8_t) g;
			level[2] = (uint8_t) b;
			uint32_t const output = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
			bool const still = output != input || (r & 7) != r >> 5 || (g & 3) != g >> 6 || (b & 7) != b >> 5;
			moving[x] = still;
			count += still;
			out[x] = (uint16_t) output;
		}
		memcpy(before, in, W * 2);
		ghost.rowBusy[y] = (uint16_t) count;
	}
}

/* upstream showPicture, for a 240 x 160 RGB565 frame; strength 1..4 = 25 % .. 100 % */
const uint16_t* picoLcdProcess(const uint16_t* picture, int strength, bool interlaced) {
	if (strength <= 0) {
		ghost.valid = false;
		return picture;
	}
	/* Interlaced Rows on the Advance's panel: the harder driven rows' share of the time and the
	 * others' so that the two frames' rates add up to two plain ones */
	bool const alternate = interlaced;
	float const lead = !alternate ? 1 : 0.8f;
	int const wanted = (strength - 1) * 2 + (alternate ? 1 : 0);
	if (builtFor != wanted) {
		builtFor = wanted;
		for (int d = 0; d < (alternate ? 2 : 1); ++d) {
			float const scale = d ? 1 / (2 - 1 / lead) : lead;
			buildNext(strength, 5, next5[d], scale);
			buildNext(strength, 6, next6[d], scale);
		}
	}
	++ghost.frames;
	if (!ghost.valid) {
		memcpy(ghost.last, picture, sizeof ghost.last);
		memset(ghost.busy, 0, sizeof ghost.busy);
		memset(ghost.rowBusy, 0, sizeof ghost.rowBusy);
		for (unsigned i = 0; i < W * H; ++i) {
			uint16_t const c = picture[i];
			ghost.levels[i * 3] = (uint8_t) ((c >> 11) << 3 | (c >> 13));
			ghost.levels[i * 3 + 1] = (uint8_t) ((c >> 5 & 0x3F) << 2 | (c >> 9 & 3));
			ghost.levels[i * 3 + 2] = (uint8_t) ((c & 0x1F) << 3 | (c >> 2 & 7));
		}
		ghost.valid = true;
		return picture;
	}
	ghostRows(picture, alternate);
	return ghost.shown;
}
