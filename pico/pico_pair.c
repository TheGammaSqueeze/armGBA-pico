/* armGBA-pico: two linked Game Boy Advances in one core, for netplay.
 *
 * Copyright (c) 2026 TheGammaSqueeze
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
 * the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Two players over the Bluetooth link: a game linked by cable trades words every few thousand
 * cycles, far faster than the air carries a message there and back. So both consoles run BOTH
 * machines, joined by a cable inside the core, and only the buttons travel (the LK's lockstep).
 * Each console shows and plays its own player's machine; nothing here depends on which one, so
 * the two consoles step the two machines the same way and stay identical.
 *
 * Machine 0 is the host's game and the cable's parent, machine 1 the guest's. A frame goes in
 * slices: machine 0 runs PAIR_SLICE cycles, then machine 1 runs until it has done as many; machine
 * 0's frame ends the call. Every machine counts the cycles it has run since the pair started.
 *
 * The cable: when a machine clocks a transfer, its serial port asks for the other's word as the
 * transfer ends (finish). It gets the other machine's word as that machine has it now: at most a
 * slice old, well inside the shortest transfer. Its own word reaches the other machine on the
 * cycle the transfer ends in that machine's time: an event there if that machine is behind (the
 * usual case, machine 1 runs second), at once if it is already past.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gba.h"
#include "gba_core_abi.h"

void *malloc(size_t n);
void free(void *p);

#define PAIR_SLICE	1024		/* cycles machine 0 runs before machine 1 catches up */
#define PAIR_CODE_SIZE	(16u << 20)	/* each paired machine's recompiled code (both the same) */

static struct GBA *s_mach[2];		/* 0 the host's game, 1 the guest's */
static int s_on;
static uint64_t s_done[2];		/* cycles each machine has run since the pair started */
static uint32_t s_last[2];		/* each machine's clock when s_done was last brought up */
static unsigned s_keys[2];
static uint8_t *s_rom_other;		/* the other console's game */

/* a slice's end, and a word on its way to a machine */
static struct Event s_slice[2], s_deliver[2];
static struct {
	bool due;
	int mode;
	uint32_t word;
	uint64_t at;			/* in the receiving machine's s_done */
} s_in[2];
static struct SioLink s_cable[2];
static unsigned s_stat[4];		/* transfers clocked by 0 and 1, words delivered to 0 and 1 */

static void bring_up(int m)
{
	uint32_t now = (uint32_t) timingCurrentTime(s_mach[m]);
	s_done[m] += (uint32_t) (now - s_last[m]);
	s_last[m] = now;
}

/* the word arrives: multiplayer has the parent's and the child's in their places */
static void arrive(int m)
{
	uint32_t values[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
	int mode;
	bool ready;
	uint32_t mine = sioOutgoing(s_mach[m], &mode, &ready);
	s_in[m].due = false;
	if (s_in[m].mode == 2) {
		values[m ^ 1] = s_in[m].word & 0xFFFF;
		values[m] = mine & 0xFFFF;
	} else {
		values[0] = s_in[m].word;
	}
	sioReceive(s_mach[m], s_in[m].mode, values);
	s_stat[2 + m]++;
}

static void slice_end(struct GBA *gba, void *context, uint32_t late)
{
	(void) context; (void) late;
	gba->returnRequested = true;
	timingInterrupt(gba);
}
static void deliver_event(struct GBA *gba, void *context, uint32_t late)
{
	(void) gba; (void) late;
	arrive((int) (intptr_t) context);
}

/* a machine clocks a transfer: its word is noted for the other, to land as the transfer ends */
static void cable_begin(void *context, int mode, uint32_t out, int32_t cycles)
{
	int m = (int) (intptr_t) context, o = m ^ 1;
	bring_up(m);
	s_in[o].due = true;
	s_in[o].mode = mode;
	s_in[o].word = out;
	s_in[o].at = s_done[m] + (uint64_t) cycles;
	s_stat[m]++;
}

static bool cable_finish(void *context, uint32_t in[3])
{
	int m = (int) (intptr_t) context, o = m ^ 1, mode;
	bool ready;
	in[0] = sioOutgoing(s_mach[o], &mode, &ready);
	in[1] = 0xFFFF;
	in[2] = 0xFFFF;
	/* the other machine already ran past the end: it takes the word now */
	if (s_in[o].due) {
		bring_up(o);
		if (s_done[o] >= s_in[o].at)
			arrive(o);
	}
	return true;
}

/* before a machine runs: a word waiting for it lands on its cycle */
static void arm_delivery(int m)
{
	if (!s_in[m].due)
		return;
	bring_up(m);
	timingDeschedule(s_mach[m], &s_deliver[m]);
	if (s_done[m] >= s_in[m].at)
		arrive(m);
	else
		timingSchedule(s_mach[m], &s_deliver[m], (int32_t) (s_in[m].at - s_done[m]));
}

/* run machine m for up to cycles, returning early at its frame's end */
static void run_for(int m, int32_t cycles)
{
	struct GBA *g = s_mach[m];
	bring_up(m);
	arm_delivery(m);
	timingSchedule(g, &s_slice[m], cycles);
	gbaSetKeys(g, (uint16_t) s_keys[m]);
	gbaRunPart(g);
	timingDeschedule(g, &s_slice[m]);
	timingDeschedule(g, &s_deliver[m]);
	bring_up(m);
}

/* One frame of machine 0, and machine 1 alongside it. */
void pico_pair_frame(void)
{
	uint32_t const frame = s_mach[0]->video.frameCounter;
	do {
		int guard;
		run_for(0, PAIR_SLICE);
		for (guard = 0; guard < 64 && s_done[1] < s_done[0]; guard++) {
			uint64_t left = s_done[0] - s_done[1];
			run_for(1, left > 0x10000000u ? 0x10000000 : (int32_t) left);
		}
	} while (s_mach[0]->video.frameCounter == frame);
}

int pico_pair_on(void) { return s_on; }
void pico_pair_keys(unsigned k0, unsigned k1) { s_keys[0] = k0 & 0x3FF; s_keys[1] = k1 & 0x3FF; }
struct GBA *pico_pair_machine(int m) { return s_on ? s_mach[m ? 1 : 0] : NULL; }

/* where the other console's game is put (32 MiB, made on the first ask) */
uint8_t *pico_pair_rom_ptr(void)
{
	if (!s_rom_other)
		s_rom_other = malloc(SIZE_ROM);
	return s_rom_other;
}

/* armGBA-pico's ROM buffer hook (pico_core.c) asks whose buffer a load is for */
bool pico_pair_rom_is_other(const void *data) { return s_rom_other && data == s_rom_other; }

/* a machine as both consoles make it: fresh, armGBA's own BIOS (the same on every console), the
 * game from buf (size bytes), the code buffer the pair gives every machine */
static struct GBA *fresh(uint8_t *buf, unsigned size)
{
	extern size_t pico_code_size_next;
	struct GBA *g;
	pico_code_size_next = PAIR_CODE_SIZE;
	g = gbaCreate();
	pico_code_size_next = 0;
	if (!g)
		return NULL;
	g->renderer.rgb565 = true;
	g->savedata.backed = true;
	if (!gbaLoadBios(g, armgbaBios, SIZE_BIOS) || !gbaLoadRom(g, buf, size)) {
		gbaDestroy(g);
		return NULL;
	}
	gbaReset(g, true);
	return g;
}

/* The pair. Both machines are made afresh on both consoles, this console's own too (it is put
 * away, its state loaded again by the LK): armGBA keeps a little of how its recompiler ran across a
 * state load (where compiled code stops, idle loops), which moves events by a cycle or two, so a
 * machine that ran here before and the other console's new copy of it would part. Made the same
 * way, in the same order, with the same buffers, the two consoles' copies stay the same.
 * *mine: this console's running machine, replaced; its game is in my_rom (my_size bytes). The
 * other console's game is in pico_pair_rom_ptr (other_size bytes). self: this console's slot
 * (0 host, 1 guest). 0 paired; on a failure this console's game is put back alone. */
int pico_pair_open(struct GBA **mine, uint8_t *my_rom, unsigned my_size, int self, unsigned other_size)
{
	int i;
	if (s_on || !mine || !*mine || !s_rom_other)
		return -1;
	self = self ? 1 : 0;
	gbaDestroy(*mine);
	*mine = NULL;
	for (i = 0; i < 2; i++) {
		s_mach[i] = i == self ? fresh(my_rom, my_size) : fresh(s_rom_other, other_size);
		if (!s_mach[i]) {
			if (i == 1)
				gbaDestroy(s_mach[0]);
			s_mach[0] = s_mach[1] = NULL;
			*mine = fresh(my_rom, my_size);	/* alone again (its state is loaded back) */
			return -2;
		}
	}
	*mine = s_mach[self];
	for (i = 0; i < 2; i++) {
		s_slice[i].callback = slice_end;
		s_slice[i].context = NULL;
		s_slice[i].priority = 0xFF;
		s_slice[i].endsRun = true;
		s_deliver[i].callback = deliver_event;
		s_deliver[i].context = (void *) (intptr_t) i;
		s_deliver[i].priority = 0x81;
		s_deliver[i].endsRun = false;
		/* all of it: a word left from a session before is in the pair state (and its hash) */
		memset(&s_in[i], 0, sizeof s_in[i]);
		s_done[i] = 0;
		s_last[i] = (uint32_t) timingCurrentTime(s_mach[i]);
		s_keys[i] = 0;
		s_cable[i] = (struct SioLink) { .context = (void *) (intptr_t) i, .players = 2, .id = i,
						.ready = true, .begin = cable_begin, .finish = cable_finish,
						.wrote = NULL };
		sioAttach(s_mach[i], &s_cable[i]);
		s_stat[i] = s_stat[2 + i] = 0;
	}
	s_on = 1;
	return 0;
}

/* back to one machine (this console's) */
void pico_pair_close(int self)
{
	struct GBA *other;
	if (!s_on)
		return;
	self = self ? 1 : 0;
	other = s_mach[self ^ 1];
	sioAttach(s_mach[self], NULL);
	timingDeschedule(s_mach[self], &s_slice[self]);
	timingDeschedule(s_mach[self], &s_deliver[self]);
	gbaDestroy(other);
	s_mach[self ^ 1] = NULL;
	s_on = 0;
}

/* A reset during a session: both machines start their games again, the cable and the counts as
 * the pair's opening left them. Both consoles do it before the same frame, so they stay the same
 * (the reset clears each machine's events; the slices are scheduled afresh as they run). */
void pico_pair_reset(void)
{
	int i;
	if (!s_on)
		return;
	for (i = 0; i < 2; i++) {
		gbaReset(s_mach[i], true);
		memset(&s_in[i], 0, sizeof s_in[i]);
		s_done[i] = 0;
		s_last[i] = (uint32_t) timingCurrentTime(s_mach[i]);
		sioAttach(s_mach[i], &s_cable[i]);
	}
}

/* After a state went into a machine: its clock moved, so the counts start from there again. */
void pico_pair_restart_clock(int m)
{
	s_last[m] = (uint32_t) timingCurrentTime(s_mach[m]);
}

/* ---- the pair's whole state: both machines, the cable, how far each has run ----
 * 'P' 'R' version 0, machine 1's lead over machine 0 in cycles (signed 32), then for each machine
 * its waiting word (due, mode, 0, 0, word(4), cycles from now(4)), then both machine states. */
#define PAIR_HDR	32u

unsigned pico_pair_state_size(void) { return s_on ? PAIR_HDR + 2u * (unsigned) gbaStateSize() : 0u; }

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

void pico_pair_state_save(void *buf)
{
	uint8_t *p = buf;
	int i;
	if (!s_on)
		return;
	bring_up(0);
	bring_up(1);
	memset(p, 0, PAIR_HDR);
	p[0] = 'P'; p[1] = 'R'; p[2] = 1;
	put32(p + 4, (uint32_t) (int32_t) (int64_t) (s_done[1] - s_done[0]));
	for (i = 0; i < 2; i++) {
		uint8_t *e = p + 8 + 12 * i;
		if (!s_in[i].due)
			continue;		/* only a word on its way is part of the state */
		e[0] = 1;
		e[1] = (uint8_t) s_in[i].mode;
		put32(e + 4, s_in[i].word);
		put32(e + 8, (uint32_t) (s_in[i].at - s_done[i]));
	}
	gbaSaveState(s_mach[0], p + PAIR_HDR);
	gbaSaveState(s_mach[1], p + PAIR_HDR + gbaStateSize());
}

int pico_pair_state_load(const void *buf, unsigned size)
{
	const uint8_t *p = buf;
	int i;
	if (!s_on || size < pico_pair_state_size() || p[0] != 'P' || p[1] != 'R' || p[2] != 1)
		return -1;
	if (!gbaLoadState(s_mach[0], p + PAIR_HDR, gbaStateSize()) ||
	    !gbaLoadState(s_mach[1], p + PAIR_HDR + gbaStateSize(), gbaStateSize()))
		return -1;
	s_done[0] = 0;
	s_done[1] = (uint64_t) (int64_t) (int32_t) get32(p + 4);
	for (i = 0; i < 2; i++) {
		const uint8_t *e = p + 8 + 12 * i;
		s_in[i].due = e[0] != 0;
		s_in[i].mode = e[1];
		s_in[i].word = get32(e + 4);
		s_in[i].at = s_done[i] + get32(e + 8);
		s_last[i] = (uint32_t) timingCurrentTime(s_mach[i]);
	}
	return 0;
}

/* One machine's state (the start's exchange): 0 loaded */
int pico_pair_load_machine(int m, const void *state)
{
	if (!s_on || !gbaLoadState(s_mach[m ? 1 : 0], state, gbaStateSize()))
		return -1;
	pico_pair_restart_clock(m ? 1 : 0);
	return 0;
}

unsigned pico_pair_stat(unsigned what) { return what < 4 ? s_stat[what] : 0u; }
