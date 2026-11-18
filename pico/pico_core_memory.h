/*
 * pico_core_memory.h - a running core's memory, as the LK hands it to RetroAchievements (ra-pico).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * The same file in every core blob, the LK and ra-pico. A core describes its machine the way a
 * libretro core does: the blocks retro_get_memory_data gives (system RAM, save RAM, video RAM)
 * and the map a core announces with RETRO_ENVIRONMENT_SET_MEMORY_MAPS. A pico_memdesc is laid out
 * exactly as libretro's struct retro_memory_descriptor (and its flags have the same values), so
 * rcheevos' rc_libretro reads it as one; a core without libretro's headers fills it all the same.
 * The memory is read in place, from the thread that runs the core, between frames.
 */
#ifndef PICO_CORE_MEMORY_H
#define PICO_CORE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define PICO_MEMDESC_CONST      (1u << 0)   /* read only (a ROM) */
#define PICO_MEMDESC_BIGENDIAN  (1u << 1)
#define PICO_MEMDESC_SYSTEM_RAM (1u << 2)
#define PICO_MEMDESC_SAVE_RAM   (1u << 3)
#define PICO_MEMDESC_VIDEO_RAM  (1u << 4)

struct pico_memdesc {
	uint64_t flags;           /* PICO_MEMDESC_* */
	void *ptr;                /* the block */
	size_t offset;            /* where in the block this starts */
	size_t start;             /* the machine's address of it */
	size_t select;            /* address bits that must match start (0: worked out from len) */
	size_t disconnect;        /* address bits that do not reach the block */
	size_t len;
	const char *addrspace;    /* 0 */
};

struct pico_core_memory {
	void *system_ram;   unsigned system_ram_size;
	void *save_ram;     unsigned save_ram_size;
	void *video_ram;    unsigned video_ram_size;
	const struct pico_memdesc *descriptors;    /* 0 when the core has no map */
	unsigned num_descriptors;
};

#endif
