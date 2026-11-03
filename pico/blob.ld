/* SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 TheGammaSqueeze */
/* armGBA-pico: link script for the loadable core blob.
 *
 * The LK loads the flat image to a fixed DRAM address, zeroes [load_size, total_span), syncs the
 * caches and calls the entry found through the header. The address is the GBA core slot in the LK's
 * write back window, the same one the gpSP blob uses (only one GBA core is loaded at a time): it
 * must end before 0x4E800000, where the GB/GBC core loads. Everything big (the machine, the ROM,
 * the recompiler's code buffer, its tables) is allocated from the arena at run time, not here.
 *
 * Header (image offset 0), the same layout as the gpSP blob's:
 *   u32 magic      0x41424743 "GCBA"
 *   u32 version    3
 *   u32 entry      offset of gba_core_blob_init
 *   u32 load_size  bytes present in the file
 *   u32 total_span bytes to reserve including BSS
 */
ENTRY(gba_core_blob_init)

BLOB_VMA = 0x4E400000;

SECTIONS
{
	. = BLOB_VMA;
	.header : {
		LONG(0x41424743);
		LONG(3);
		LONG(gba_core_blob_init - BLOB_VMA);
		LONG(__blob_load_end - BLOB_VMA);
		LONG(__blob_end - BLOB_VMA);
	}
	.text : { KEEP(*(.text.gba_core_blob_init)) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.init_array : { __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .; }
	/* the padding to 16 bytes is inside .data so the flat image always carries it: outside a
	 * section objcopy dropped it, and the image came out shorter than the load size in the header
	 * whenever the data did not end on a 16 byte boundary */
	.data : { *(.data .data.*) . = ALIGN(16); }
	__blob_load_end = .;
	.bss (NOLOAD) : {
		__bss_start__ = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end__ = .;
	}
	__blob_end = .;
	end = .;
	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note.*) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(__blob_end <= 0x4E800000, "armGBA-pico: the blob runs into the GB/GBC core slot at 0x4E800000")
