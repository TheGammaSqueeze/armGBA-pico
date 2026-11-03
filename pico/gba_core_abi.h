/*
 * gba_core_abi.h - the contract between LK (frontend) and a loadable emulator core.
 *
 * WHY: the gpSP GBA core is ~993 KiB of text+data linked into lk_a, about half of the
 * 2 MB partition. To add a second core (GB/GBC) and free LK for other work, the cores
 * move OUT of lk_a into boot_b as loadable blobs: LK reads a blob from boot_b into a
 * reserved DRAM slot, does cache maintenance, and calls its init entry, which hands back
 * this export table. The LK driver then drives the core through the table (function
 * pointers) instead of direct symbol links.
 *
 * The boundary is already clean: the LK frontend (gba_driver.c) touches the core through
 * exactly the 26 symbols below (the gba_core_* wrapper API in gba_wrap.c/gba_shim.c plus
 * three shared flags), and the core's only real outward calls are the few imports below.
 * libc (memcpy/memset/printf/str*) and libgcc (__aeabi_* float/div) are STATICALLY
 * BUNDLED into the blob, so they are not part of this ABI.
 *
 * STATUS: contract definition (design step of option 1). Not yet wired into the build.
 *
 * OPEN DESIGN QUESTIONS (resolve when writing the loader + relinking the core):
 *  - Load model: link the blob to a FIXED reserved DRAM address in the WB window
 *    [0x4E000000,0x56000000) clear of the ROM buffer / translation caches / framebuffers
 *    / menu buffers (see [[ovl-layering]] for the crowded 0x54/0x55 region), OR build it
 *    position-independent. Fixed-address is simpler and the dynarec already runs from a
 *    fixed DRAM arena, so leaning that way.
 *  - The blob's own BSS (gpSP has ~1.7 MB: translation caches etc., already HAVE_MMAP
 *    DRAM pointers) must be placed/zeroed by the loader in the arena, not baked in the
 *    blob image.
 *  - memory_map_read / memory_map_write: gpSP memory-dispatch tables currently resolved
 *    outside libgpsp.a (memmap_win32.c is excluded from the core build); confirm whether
 *    they live LK-side (import) or should be bundled. Left out of the structs below until
 *    confirmed.
 *  - Cache maintenance: clean D-cache + invalidate I-cache over the loaded text before
 *    entry (the dynarec already does this for generated code via __clear_cache).
 *  - Blob placement in boot_b: pick an offset now that ROM(17M)/state(28M)/sav are being
 *    retired from boot_b (SD is the source of truth), leaving boot_b for LK/menu assets +
 *    core blobs.
 */
#ifndef GBA_CORE_ABI_H
#define GBA_CORE_ABI_H

#define GBA_CORE_ABI_MAGIC   0x41424743u   /* "GCBA" */
#define GBA_CORE_ABI_VERSION 2u   /* 2: skip_next_frame (the gpSP blob in boot_b) */
#define GBA_CORE_ABI_V3      3u   /* 3: name, audio rate, state folder, options (armGBA-pico from the SD card) */
#define GBA_CORE_ABI_MAX     GBA_CORE_ABI_V3

/* A core option the LK can show in the Core Settings submenu (version 3). The LK persists the
 * chosen index per key and hands it back with opt_set before the next game starts. */
#define GBA_CORE_OPT_RESTART 1u   /* takes effect when the next game starts */
struct gba_core_option {
	const char *key;              /* stable identifier, used in the saved settings */
	const char *label;            /* shown in the menu */
	const char *const *values;    /* value labels */
	int nvalues;
	int def;                      /* default index */
	unsigned flags;               /* GBA_CORE_OPT_* */
};

/* Services LK provides to the core (core -> LK calls). Everything else the core needs
 * (libc, libgcc) is bundled into the blob. */
struct gba_core_imports {
	void (*audio_submit)(const short *interleaved, unsigned frames); /* ayaneo_gba_audio_submit */
	long (*host_time)(void);                                         /* gba_host_time (RTC secs) */
	void (*yield_to_main)(void);                                     /* gba_yield_to_main: CPU
	                                                                   * thread yields at vblank;
	                                                                   * LK owns the longjmp/restart
	                                                                   * re-entry state. */
	/* added with version 3 (a version 2 core never reads it) */
	unsigned int (*host_ms)(void);                                   /* monotonic milliseconds */
};

/* The core's surface as the LK driver uses it (LK -> core calls). Names mirror the
 * current gba_core_* / gba_* symbols so the driver change is a mechanical swap of direct
 * calls for table calls. Grouped by role. */
struct gba_core_exports {
	unsigned int magic;      /* GBA_CORE_ABI_MAGIC - loader sanity check */
	unsigned int version;    /* GBA_CORE_ABI_VERSION */

	/* lifecycle */
	int  (*core_init)(void *arena, unsigned size);          /* gba_core_init */
	int  (*core_start)(unsigned romsz, const void *bios16k);/* gba_core_start */
	void (*enter_bios)(void);                               /* gba_core_enter_bios */
	void (*reset)(void);                                    /* reset_gba */

	/* per-frame drive (the CPU thread runs core_cpu_loop; the frontend paces via
	 * pre_frame/post_frame + the yield_to_main import) */
	void (*cpu_loop)(void);                                 /* gba_core_cpu_loop */
	void (*pre_frame)(void);                                /* gba_core_pre_frame */
	void (*post_frame)(void);                               /* gba_core_post_frame */
	void (*frame_boundary_finish)(void);                    /* gba_frame_boundary_finish */
	unsigned int (*cpu_ticks)(void);                        /* gba_core_cpu_ticks */
	void (*set_keys)(unsigned gba_mask);                    /* gba_core_set_keys */

	/* framebuffer */
	const unsigned short *(*screen)(void);                  /* gba_core_screen */
	void (*screen_fill)(unsigned short v);                  /* gba_core_screen_fill */

	/* savestate (the deterministic 512 KB machine state used by run-ahead) */
	unsigned (*state_size)(void);                           /* gba_core_state_size */
	void (*state_save)(void *buf);                          /* gba_core_state_save */
	void (*state_load)(const void *buf);                    /* gba_core_state_load */

	/* the sound ring the savestate omits (run-ahead snapshot/restore) */
	void (*sound_ring_save)(void *dst);                     /* gba_sound_ring_save */
	void (*sound_ring_load)(const void *src);               /* gba_sound_ring_load */

	/* buffers owned by the core (LK fills ROM, reads backup) */
	unsigned char *(*rom_ptr)(void);                        /* gba_core_rom_ptr */
	unsigned       (*rom_capacity)(void);                   /* gba_core_rom_capacity */
	unsigned char *(*scratch_ptr)(void);                    /* gba_core_scratch_ptr */
	unsigned       (*scratch_size)(void);                   /* gba_core_scratch_size */
	void          *(*backup_ptr)(void);                     /* gba_core_backup_ptr */
	unsigned       (*backup_size)(void);                    /* gba_core_backup_size */

	/* shared flags (LK reads/writes; the core owns the storage) */
	int          *dynarec_enable;      /* 0 = pure interpreter */
	volatile int *audio_suppress;      /* mute discarded run-ahead frames (g_gba_audio_suppress) */
	int          *load_light;          /* RAM-only dynarec flush on same-ROM rewind (g_gba_load_light) */
	/* version 2: set to 1 before a frame whose picture is never shown (fast forward and
	 * run-ahead look-ahead frames): the core skips the render, the emulation is unchanged */
	unsigned int *skip_next_frame;

	/* version 3 */
	const char *name;                                     /* "armGBA 1.3.1 pico" */
	unsigned audio_rate;                                  /* Hz of the s16 stereo given to audio_submit */
	const char *state_dir;                                /* savestates (not compatible between cores) */
	int  (*opt_count)(void);
	const struct gba_core_option *(*opt_info)(int index);
	int  (*opt_get)(int index);
	void (*opt_set)(int index, int value);
	unsigned (*heap_used)(void);                          /* diagnostics: arena bytes in use */
};

/* The blob's single fixed entry symbol. The loader resolves it at a known offset (e.g.
 * the first word of the blob is the offset to this function, or it lives at blob+0),
 * calls it with the imports, and receives the export table. Must validate magic/version. */
typedef const struct gba_core_exports *(*gba_core_blob_init_fn)(const struct gba_core_imports *imp);

#endif /* GBA_CORE_ABI_H */
