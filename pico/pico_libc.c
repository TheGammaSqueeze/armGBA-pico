/* armGBA-pico: the C runtime the core gets inside the bootloader.
 *
 * Copyright (c) 2026 TheGammaSqueeze
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
 * the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The blob links newlib for strings, formatting and maths. This file supplies what newlib expects
 * from an operating system, and what the core needs that a bootloader does not have:
 *
 *   - malloc and friends over the arena the LK hands to core_init (pico_heap_init). The recompiler
 *     allocates and frees a few small records per compiled block, so this is a real allocator:
 *     exact size classes for small requests, an address ordered free list with coalescing for large
 *     ones, everything 16 byte aligned.
 *   - mmap for the recompiler's code buffer: a page aligned block from the same heap. The LK maps
 *     the arena normal, write back and executable.
 *   - pico_clear_cache: what __builtin___clear_cache becomes (include/pico_time.h). GCC for
 *     arm-none-eabi expands the builtin to nothing, and the recompiler's new code would never reach
 *     the instruction side. Here: clean the data cache and invalidate the instruction cache by line
 *     to the point of unification, then the barriers.
 *   - pthread stubs (never used, see include/pthread.h), clock_gettime, getenv, abort and the
 *     newlib system call stubs.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "include/pthread.h"
#include "include/sys/mman.h"

/* ---- heap --------------------------------------------------------------------------------------- */

#define ALIGN        16u
#define HDR          16u                  /* header before every payload, keeps payloads aligned */
#define SMALL_MAX    2048u                /* exact classes up to here, in ALIGN steps */
#define NCLASS       (SMALL_MAX / ALIGN)
#define MAGIC_USED   0xA110CA7Eu
#define MAGIC_FREE   0xF4EEB10Cu

struct hdr {
	uint32_t size;       /* payload capacity in bytes */
	uint32_t magic;
	struct hdr *next;    /* free lists only */
	uint32_t pad;
};

static uint8_t *s_base, *s_top, *s_end;
static struct hdr *s_small[NCLASS + 1];
static struct hdr *s_large;               /* address ordered */
static size_t s_peak;

void pico_heap_init(void *base, size_t size)
{
	uintptr_t b = ((uintptr_t) base + ALIGN - 1) & ~(uintptr_t) (ALIGN - 1);
	s_base = s_top = (uint8_t *) b;
	s_end = (uint8_t *) base + size;
	memset(s_small, 0, sizeof s_small);
	s_large = NULL;
	s_peak = 0;
}
size_t pico_heap_used(void) { return (size_t) (s_top - s_base); }
size_t pico_heap_peak(void) { return s_peak; }
size_t pico_heap_size(void) { return (size_t) (s_end - s_base); }

static int ours(const void *p)
{
	return (const uint8_t *) p >= s_base + HDR && (const uint8_t *) p < s_top;
}

static struct hdr *carve(size_t cap)
{
	struct hdr *h;
	if ((size_t) (s_end - s_top) < cap + HDR) return NULL;
	h = (struct hdr *) s_top;
	s_top += cap + HDR;
	if ((size_t) (s_top - s_base) > s_peak) s_peak = (size_t) (s_top - s_base);
	h->size = (uint32_t) cap;
	return h;
}

static struct hdr *large_take(size_t cap)
{
	struct hdr **pp = &s_large, *h;
	for (; (h = *pp) != NULL; pp = &h->next) {
		if (h->size < cap) continue;
		*pp = h->next;
		if (h->size >= cap + HDR + SMALL_MAX) {   /* split off the tail as a new free block */
			struct hdr *t = (struct hdr *) ((uint8_t *) h + HDR + cap);
			t->size = h->size - (uint32_t) cap - HDR;
			t->magic = MAGIC_FREE;
			t->next = *pp;
			*pp = t;
			h->size = (uint32_t) cap;
		}
		return h;
	}
	return NULL;
}

static void large_give(struct hdr *h)
{
	struct hdr **pp = &s_large, *prev = NULL;
	h->magic = MAGIC_FREE;
	while (*pp && *pp < h) { prev = *pp; pp = &(*pp)->next; }
	h->next = *pp;
	*pp = h;
	/* merge with the next block, then with the previous one */
	if (h->next && (uint8_t *) h + HDR + h->size == (uint8_t *) h->next) {
		h->size += HDR + h->next->size;
		h->next = h->next->next;
	}
	if (prev && (uint8_t *) prev + HDR + prev->size == (uint8_t *) h) {
		prev->size += HDR + h->size;
		prev->next = h->next;
		h = prev;
	}
	/* a free block at the top gives its space back to the bump pointer */
	if ((uint8_t *) h + HDR + h->size == s_top) {
		struct hdr **q = &s_large;
		while (*q && *q != h) q = &(*q)->next;
		if (*q) *q = h->next;
		s_top = (uint8_t *) h;
	}
}

void *malloc(size_t n)
{
	size_t cap = (n + ALIGN - 1) & ~(size_t) (ALIGN - 1);
	struct hdr *h;
	if (!cap) cap = ALIGN;
	if (cap <= SMALL_MAX) {
		unsigned c = (unsigned) (cap / ALIGN);
		if ((h = s_small[c]) != NULL) s_small[c] = h->next;
		else h = carve(cap);
	} else {
		h = large_take(cap);
		if (!h) h = carve(cap);
	}
	if (!h) { errno = ENOMEM; return NULL; }
	h->magic = MAGIC_USED;
	h->next = NULL;
	return (uint8_t *) h + HDR;
}

void free(void *p)
{
	struct hdr *h;
	if (!p || !ours(p)) return;        /* NULL, or a buffer the host owns (the adopted ROM) */
	h = (struct hdr *) ((uint8_t *) p - HDR);
	if (h->magic != MAGIC_USED) return;
	if (h->size <= SMALL_MAX) {
		unsigned c = h->size / ALIGN;
		h->magic = MAGIC_FREE;
		h->next = s_small[c];
		s_small[c] = h;
	} else {
		large_give(h);
	}
}

void *calloc(size_t a, size_t b)
{
	size_t n = a * b;
	void *p;
	if (b && n / b != a) { errno = ENOMEM; return NULL; }
	p = malloc(n);
	if (p) memset(p, 0, n);
	return p;
}

void *realloc(void *p, size_t n)
{
	struct hdr *h;
	void *q;
	if (!p) return malloc(n);
	if (!n) { free(p); return NULL; }
	h = (struct hdr *) ((uint8_t *) p - HDR);
	if (ours(p) && h->magic == MAGIC_USED && n <= h->size) return p;
	q = malloc(n);
	if (!q) return NULL;
	memcpy(q, p, ours(p) ? (h->size < n ? h->size : n) : n);
	free(p);
	return q;
}

void *memalign(size_t align, size_t n)
{
	/* only the code buffer asks; pad and remember nothing: it is never freed while the core lives */
	uint8_t *p = malloc(n + align);
	if (!p) return NULL;
	return (void *) (((uintptr_t) p + align - 1) & ~(uintptr_t) (align - 1));
}
int posix_memalign(void **out, size_t align, size_t n)
{
	void *p = memalign(align, n);
	if (!p) return ENOMEM;
	*out = p;
	return 0;
}

/* ---- mmap: the recompiler's code buffer ---------------------------------------------------------- */

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *p;
	(void) addr; (void) prot; (void) flags; (void) fd; (void) offset;
	p = memalign(4096, length);
	return p ? p : MAP_FAILED;
}
int munmap(void *addr, size_t length) { (void) addr; (void) length; return 0; }
int mprotect(void *addr, size_t len, int prot) { (void) addr; (void) len; (void) prot; return 0; }

/* ---- caches ---------------------------------------------------------------------------------------- */

/* every __builtin___clear_cache in the core comes here (see include/pico_time.h): GCC for
 * arm-none-eabi would otherwise compile the builtin to nothing */
void pico_clear_cache(void *start, void *end)
{
	uintptr_t a = (uintptr_t) start & ~(uintptr_t) 63, e = (uintptr_t) end;
	uint32_t cpsr;
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
	if ((cpsr & 0x1F) == 0x10) {
		/* user mode: the host harness under qemu-arm (tools/harness.c). The cache operations are
		 * privileged there; Linux's ARM cacheflush call does the same job */
		register uintptr_t r0 __asm__("r0") = (uintptr_t) start;
		register uintptr_t r1 __asm__("r1") = (uintptr_t) end;
		register uintptr_t r2 __asm__("r2") = 0;
		register uintptr_t r7 __asm__("r7") = 0xF0002;
		__asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
		return;
	}
	for (uintptr_t p = a; p < e; p += 64)
		__asm__ volatile("mcr p15, 0, %0, c7, c11, 1" : : "r"(p) : "memory");   /* DCCMVAU */
	__asm__ volatile("dsb ish" : : : "memory");
	for (uintptr_t p = a; p < e; p += 64)
		__asm__ volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(p) : "memory");    /* ICIMVAU */
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 6" : : "r"(0) : "memory");         /* BPIALL */
	__asm__ volatile("dsb ish\n\tisb" : : : "memory");
}
void __clear_cache(void *start, void *end) { pico_clear_cache(start, end); }

/* ---- threads (never started on this target) -------------------------------------------------------- */

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{ (void) t; (void) a; (void) fn; (void) arg; return EAGAIN; }
int pthread_join(pthread_t t, void **ret) { (void) t; (void) ret; return 0; }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void) m; (void) a; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void) m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { (void) m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { (void) m; return 0; }
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void) c; (void) a; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void) c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void) c; (void) m; return 0; }
int pthread_cond_signal(pthread_cond_t *c) { (void) c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { (void) c; return 0; }

/* ---- time, environment, exits --------------------------------------------------------------------- */

extern long pico_host_time(void);          /* pico_core.c: the LK's real time clock, seconds */
extern uint64_t pico_host_ms(void);        /* pico_core.c: a monotonic millisecond count */

int clock_gettime(clockid_t clk, struct timespec *ts)
{
	uint64_t ms = pico_host_ms();
	(void) clk;
	ts->tv_sec = (time_t) (ms / 1000u);
	ts->tv_nsec = (long) (ms % 1000u) * 1000000L;
	return 0;
}
time_t time(time_t *t)
{
	time_t v = (time_t) pico_host_time();
	if (t) *t = v;
	return v;
}
char *getenv(const char *name) { (void) name; return NULL; }
void abort(void) { for (;;) __asm__ volatile("wfi"); }
void exit(int code) { (void) code; abort(); }
void _exit(int code) { (void) code; abort(); }

/* ---- newlib system calls ----------------------------------------------------------------------------- */

/* Output goes nowhere in the LK. Under the PC harness (qemu-arm user mode) the core may print
 * (debug builds): a write system call when the CPU is in user mode. */
int _write(int fd, const void *buf, size_t n)
{
	unsigned cpsr;
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
	if ((cpsr & 0x1Fu) == 0x10u) {
		register int r0 __asm__("r0") = fd;
		register const void *r1 __asm__("r1") = buf;
		register size_t r2 __asm__("r2") = n;
		register int r7 __asm__("r7") = 4;   /* write */
		__asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
		return r0;
	}
	(void) fd; (void) buf;
	return (int) n;
}
int _read(int fd, void *buf, size_t n) { (void) fd; (void) buf; (void) n; return 0; }
int _close(int fd) { (void) fd; return -1; }
int _lseek(int fd, int off, int whence) { (void) fd; (void) off; (void) whence; return 0; }
int _fstat(int fd, struct stat *st) { (void) fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void) fd; return 1; }
int _open(const char *path, int flags, int mode) { (void) path; (void) flags; (void) mode; errno = ENOENT; return -1; }
int _kill(int pid, int sig) { (void) pid; (void) sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
int _gettimeofday(void *tv, void *tz) { (void) tv; (void) tz; return 0; }
void *_sbrk(ptrdiff_t incr) { (void) incr; errno = ENOMEM; return (void *) -1; }   /* malloc is ours */

/* newlib's reentrant entry points, so its own malloc (which would need _sbrk) is never pulled in */
struct _reent;
void *_malloc_r(struct _reent *r, size_t n) { (void) r; return malloc(n); }
void _free_r(struct _reent *r, void *p) { (void) r; free(p); }
void *_calloc_r(struct _reent *r, size_t a, size_t b) { (void) r; return calloc(a, b); }
void *_realloc_r(struct _reent *r, void *p, size_t n) { (void) r; return realloc(p, n); }
