/* SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 TheGammaSqueeze */
/* armGBA-pico: forced into every core file (-include). newlib's <time.h> has no
 * CLOCK_MONOTONIC or clock_gettime for this target; the render thread file reads it. */
#ifndef ARMGBA_PICO_TIME_H
#define ARMGBA_PICO_TIME_H
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
int clock_gettime(clockid_t clk, struct timespec *ts);

/* The recompiler publishes its code with __builtin___clear_cache. On the bare metal arm-none-eabi
 * target GCC expands that builtin to nothing (there is no operating system call for it to make), so
 * new code would stay in the data cache while the CPU fetched stale bytes. Route it to the real
 * cache maintenance in pico_libc.c. */
void pico_clear_cache(void *start, void *end);
#define __builtin___clear_cache(a, b) pico_clear_cache((void *) (a), (void *) (b))
#endif
