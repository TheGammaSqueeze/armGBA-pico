/* armGBA-pico host harness: runs out/armgba-pico.blob exactly as the LK does, under qemu-arm.
 *
 * Copyright (c) 2026 TheGammaSqueeze
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
 * the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The blob is position dependent: it is mapped at the LK's core slot (0x4E400000) and given the
 * LK's arena (0x50000000, 62 MiB). Like the LK's GBA driver, a second thread runs the core's
 * cpu_loop and hands over to the main thread once per frame (yield_to_main), and the main thread
 * calls pre_frame / post_frame around each hand over. This finds faults in the glue and in the
 * recompiler's interplay with it on a PC, with gdb, instead of on the device.
 *
 *   tools/build_harness.sh
 *   qemu-arm-static -L /usr/arm-linux-gnueabihf out/harness out/armgba-pico.blob BIOS ROM [frames] [out.ppm]
 *
 * HARNESS_SKIP=k draws one frame in k, as fast forward does, to check that skipping the drawing
 * leaves the shown frames exactly as without it.
 *
 * It also checks what the LK relies on: a save state taken and loaded back reproduces the same
 * following frames (run ahead and rewind depend on it), and the audio rate is the one the table
 * declares.
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "../pico/gba_core_abi.h"

#define SLOT_VMA  0x4E400000u
#define SLOT_SIZE 0x00400000u
#define ARENA_VMA 0x50000000u
#define ARENA_SZ  (62u << 20)

static const struct gba_core_exports *g;
static sem_t s_cpu, s_main;
static unsigned long long s_audio_frames;
static unsigned s_audio_calls;

static unsigned s_audio_hash = 2166136261u;
static void audio_submit(const short *s, unsigned frames)
{
	unsigned i;
	for (i = 0; i < frames * 2u; i++) { s_audio_hash ^= (unsigned short) s[i]; s_audio_hash *= 16777619u; }
	s_audio_frames += frames; s_audio_calls++;
}
static long host_time(void) { return (long) time(NULL); }
static unsigned host_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned) (ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
static void yield_to_main(void) { sem_post(&s_main); sem_wait(&s_cpu); }

static const struct gba_core_imports s_imports = { audio_submit, host_time, yield_to_main, host_ms };

static void *cpu_thread(void *arg) { (void) arg; sem_wait(&s_cpu); g->cpu_loop(); return NULL; }

/* HARNESS_KEYS="first-last:mask,...": the GBA key mask (bit 0 A, 1 B, 2 Select, 3 Start, 4 Right,
 * 5 Left, 6 Up, 7 Down, 8 R, 9 L) held over frames first..last, counted from the first frame run */
static int s_frame_no;
static unsigned keys_for(int frame)
{
	const char *k = getenv("HARNESS_KEYS");
	unsigned m = 0;
	while (k && *k) {
		int a = (int) strtol(k, (char **) &k, 10), b = a;
		if (*k == '-') b = (int) strtol(k + 1, (char **) &k, 10);
		if (*k == ':') { unsigned v = (unsigned) strtoul(k + 1, (char **) &k, 0); if (frame >= a && frame <= b) m |= v; }
		while (*k && *k != ',') k++;
		if (*k == ',') k++;
	}
	return m;
}

static void run_frame(void)
{
	if (g->set_keys) g->set_keys(keys_for(s_frame_no));
	s_frame_no++;
	g->pre_frame(); sem_post(&s_cpu); sem_wait(&s_main); g->post_frame();
}

static unsigned hash_screen(void)
{
	const unsigned short *p = g->screen();
	unsigned h = 2166136261u;
	for (int i = 0; i < 240 * 160; i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static void *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	void *b;
	long n;
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	b = malloc((size_t) n);
	if (fread(b, 1, (size_t) n, f) != (size_t) n) { perror("read"); exit(1); }
	fclose(f);
	*size = (size_t) n;
	return b;
}

static void write_ppm(const char *path)
{
	const unsigned short *p = g->screen();
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6\n240 160\n255\n");
	for (int i = 0; i < 240 * 160; i++) {
		unsigned c = p[i];
		unsigned char rgb[3] = { (unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2), (unsigned char) ((c & 31) << 3) };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	size_t bsz, biossz, romsz;
	unsigned *hdr;
	unsigned char *slot, *bios, *rom;
	int frames = argc > 4 ? atoi(argv[4]) : 600;
	pthread_t t;

	setvbuf(stdout, NULL, _IOLBF, 0);   /* line by line, so a hang shows how far it got */
	if (argc < 4) { fprintf(stderr, "usage: %s blob bios rom [frames] [out.ppm]\n", argv[0]); return 2; }
	slot = mmap((void *) SLOT_VMA, SLOT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mmap((void *) ARENA_VMA, ARENA_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED || slot == MAP_FAILED) {
		perror("mmap"); return 1;
	}
	{
		unsigned char *b = slurp(argv[1], &bsz);
		hdr = (unsigned *) b;
		if (hdr[0] != GBA_CORE_ABI_MAGIC || hdr[3] > bsz || hdr[4] > SLOT_SIZE) { fprintf(stderr, "bad blob header\n"); return 1; }
		memcpy(slot, b, hdr[3]);
		memset(slot + hdr[3], 0, hdr[4] - hdr[3]);
		__builtin___clear_cache((char *) slot, (char *) slot + hdr[4]);
		g = ((gba_core_blob_init_fn) (void *) (slot + hdr[2]))(&s_imports);
	}
	if (!g || g->magic != GBA_CORE_ABI_MAGIC) { fprintf(stderr, "init rejected\n"); return 1; }
	printf("core: v%u %s rate=%u states=%s\n", g->version, g->version >= 3 ? g->name : "?", g->version >= 3 ? g->audio_rate : 0,
	       g->version >= 3 ? g->state_dir : "?");
	if (g->core_init((void *) ARENA_VMA, ARENA_SZ) != 0) { fprintf(stderr, "core_init failed\n"); return 1; }
	bios = slurp(argv[2], &biossz);
	rom = slurp(argv[3], &romsz);
	if (romsz > g->rom_capacity()) romsz = g->rom_capacity();
	memcpy(g->rom_ptr(), rom, romsz);
	if (g->core_start((unsigned) romsz, bios) != 0) { fprintf(stderr, "core_start failed\n"); return 1; }
	{	/* HARNESS_SAV=file: the battery save, put into the save buffer after core_start as the LK does */
		const char *sv = getenv("HARNESS_SAV");
		if (sv) {
			size_t n;
			unsigned char *b = slurp(sv, &n);
			if (n > g->backup_size()) n = g->backup_size();
			memcpy(g->backup_ptr(), b, n);
			printf("save loaded: %s (%zu bytes)\n", sv, n);
		}
	}
	printf("started: rom %zu bytes, state %u bytes, save buffer %u bytes, heap %u KiB\n", romsz, g->state_size(), g->backup_size(),
	       g->version >= 3 && g->heap_used ? g->heap_used() / 1024u : 0);

	sem_init(&s_cpu, 0, 0);
	sem_init(&s_main, 0, 0);
	{	/* the LK's gba_cpu thread has a 64 KiB stack (gba_driver.c thread_create); HARNESS_STACK_KB overrides */
		pthread_attr_t a;
		const char *e = getenv("HARNESS_STACK_KB");
		size_t kb = e ? (size_t) atoi(e) : 64;
		pthread_attr_init(&a);
		pthread_attr_setstacksize(&a, kb * 1024u);
		printf("cpu thread stack %zu KiB\n", kb);
		pthread_create(&t, &a, cpu_thread, NULL);
	}
	{	/* HARNESS_RA=pf: the LK's run ahead, per committed frame: save, pf muted look ahead frames
		 * (all but the last render skipped), load back (load_light set, as the LK does). The audio
		 * of the committed frames must be the same stream as without it. */
		const char *ra = getenv("HARNESS_RA");
		if (ra) {
			int pf = atoi(ra), i, k;
			void *st = malloc(g->state_size());
			for (i = 0; i < frames; i++) {
				run_frame();
				if (pf > 0) {
					g->state_save(st);
					*g->audio_suppress = 1;
					for (k = 0; k < pf; k++) {
						if (g->skip_next_frame) *g->skip_next_frame = (k < pf - 1);
						run_frame();
					}
					if (g->skip_next_frame) *g->skip_next_frame = 0;
					*g->audio_suppress = 0;
					*g->load_light = 1;
					g->state_load(st);
					*g->load_light = 0;
				}
				if ((i + 1) % 300 == 0)
					printf("committed %d: audio frames %llu calls %u hash %08x picture %08x\n", i + 1, s_audio_frames, s_audio_calls,
					       s_audio_hash, hash_screen());
			}
			return 0;
		}
	}
	{	/* HARNESS_STATE=file: load that save state (as the LK stores it on the card) after the first
		 * frame, then write HARNESS_SHOTS (comma separated frame numbers after the load) as
		 * <out>_<n>.ppm, to look at the pictures that follow a load */
		const char *sp = getenv("HARNESS_STATE");
		if (sp) {
			size_t ssz;
			void *st = slurp(sp, &ssz);
			const char *shots = getenv("HARNESS_SHOTS");
			const char *base = argc > 5 ? argv[5] : "state";
			int after = 0, next = -1;
			char nb[256];
			run_frame();
			if (ssz != g->state_size()) { fprintf(stderr, "state is %zu bytes, the core's is %u\n", ssz, g->state_size()); return 1; }
			g->state_load(st);
			printf("state loaded: %s\n", sp);
			if (shots) next = atoi(shots);
			if (next == 0) {
				snprintf(nb, sizeof nb, "%s_%d.ppm", base, 0);
				write_ppm(nb);
				printf("the loaded picture hash %08x -> %s\n", hash_screen(), nb);
				shots = strchr(shots, ',');
				if (shots) { shots++; next = atoi(shots); } else next = -1;
			}
			for (after = 1; after <= frames; after++) {
				const char *e = getenv("HARNESS_SKIP");
				int k = e ? atoi(e) : 0;
				if (k > 1 && g->skip_next_frame) *g->skip_next_frame = (after % k) != 0;   /* every k-th frame drawn */
				run_frame();
				if (k > 1 && g->skip_next_frame) *g->skip_next_frame = 0;
				{	/* HARNESS_FILL=n: the LK's white fill of the screen at a game start, after frame n */
					const char *f = getenv("HARNESS_FILL");
					if (f && atoi(f) == after) { g->screen_fill(0xFFFF); printf("screen fill after frame %d\n", after); }
				}
				{	/* HARNESS_CYCLE=n: save and load the state back after frame n (a quick save + load) */
					const char *c = getenv("HARNESS_CYCLE");
					if (c && atoi(c) == after) {
						void *t = malloc(g->state_size());
						g->state_save(t);
						g->state_load(t);
						free(t);
						printf("save + load after frame %d\n", after);
					}
				}
				if ((after % 60) == 0) printf("after %d hash %08x\n", after, (after % (k > 1 ? k : 1)) == 0 ? hash_screen() : 0u);
				if (shots && after == next) {
					snprintf(nb, sizeof nb, "%s_%d.ppm", base, after);
					write_ppm(nb);
					printf("frame %d after the load hash %08x -> %s\n", after, hash_screen(), nb);
					shots = strchr(shots, ',');
					if (shots) { shots++; next = atoi(shots); } else next = -1;
				}
			}
			return 0;
		}
	}
	{	/* HARNESS_SKIP=k: like fast forward, only every k-th frame is drawn (the LK's skip flag on
		 * the others). Frames 60, 120, ... are always drawn, so their hashes must equal a run
		 * without skipping. */
		const char *e = getenv("HARNESS_SKIP");
		int k = e ? atoi(e) : 0;
		if (k > 1 && 60 % k) { fprintf(stderr, "HARNESS_SKIP must divide 60\n"); return 2; }
		if (k > 1) printf("render skip: drawing 1 frame in %d\n", k);
		const char *shots = getenv("HARNESS_SHOTS");   /* frame numbers to write as <out>_<n>.ppm */
		for (int i = 0; i < frames; i++) {
			if (k > 1 && g->skip_next_frame) *g->skip_next_frame = (i % k) != k - 1;
			run_frame();
			if ((i + 1) % 60 == 0) printf("frame %d hash %08x audio %llu\n", i + 1, hash_screen(), s_audio_frames);
			if (shots && atoi(shots) == i + 1) {
				char nb[256];
				snprintf(nb, sizeof nb, "%s_%d.ppm", argc > 5 ? argv[5] : "shot", i + 1);
				write_ppm(nb);
				shots = strchr(shots, ',');
				if (shots) shots++;
			}
		}
		{	/* HARNESS_SAVOUT=file: the save buffer as the LK would write it to the card */
			const char *so = getenv("HARNESS_SAVOUT");
			if (so) { FILE *f = fopen(so, "wb"); fwrite(g->backup_ptr(), 1, g->backup_size(), f); fclose(f); }
		}
		if (g->skip_next_frame) *g->skip_next_frame = 0;
	}

	/* state round trip: save, run 30 frames, load, run the same 30 frames, compare */
	{
		unsigned n = g->state_size(), h1, h2;
		void *st = malloc(n);
		g->state_save(st);
		for (int i = 0; i < 30; i++) run_frame();
		h1 = hash_screen();
		g->state_load(st);
		for (int i = 0; i < 30; i++) run_frame();
		h2 = hash_screen();
		printf("state round trip: %08x %08x %s\n", h1, h2, h1 == h2 ? "same" : "DIFFERENT");
		free(st);
	}
	printf("audio: %llu frames in %u calls over %d frames = %.1f per frame (65536 Hz expects %.1f)\n",
	       s_audio_frames, s_audio_calls, frames + 60, (double) s_audio_frames / (frames + 60), 65536.0 / 59.7275);
	if (argc > 5) write_ppm(argv[5]);
	return 0;
}
