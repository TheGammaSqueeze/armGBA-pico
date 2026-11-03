/* SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 TheGammaSqueeze */
/* armGBA-pico: the one mmap the core does is its JIT code buffer (src/cpu/jit.c). In the
 * bootloader there is no virtual memory to ask for, so pico_libc.c hands out an aligned block
 * of the emulator arena, which the LK maps normal, write back and executable. */
#ifndef ARMGBA_PICO_SYS_MMAN_H
#define ARMGBA_PICO_SYS_MMAN_H
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED ((void *) -1)
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
#endif
