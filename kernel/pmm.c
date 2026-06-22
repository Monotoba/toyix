// kernel/pmm.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/multiboot.h"
#include "kernel/console.h"
#include "kernel/pmm.h"
#include "kernel/panic.h"

#define PMM_MAX_FRAMES 1048576u
#define PMM_BITMAP_BYTES (PMM_MAX_FRAMES / 8u)

#define MULTIBOOT_MEMORY_AVAILABLE 1u

extern char __kernel_start[];
extern char __kernel_end[];

static uint8_t frame_bitmap[PMM_BITMAP_BYTES];

static uint32_t total_frames;
static uint32_t usable_frames;
static uint32_t reserved_frames;
static uint32_t free_frames;
static uint32_t highest_physical_addr;

static uint32_t align_down_page(uint32_t value) {
    return value & ~(PMM_PAGE_SIZE - 1u);
}

static uint32_t align_up_page(uint32_t value) {
    return (value + PMM_PAGE_SIZE - 1u) & ~(PMM_PAGE_SIZE - 1u);
}

static uint32_t frame_index_from_addr(uintptr_t physical_addr) {
    return (uint32_t)(physical_addr / PMM_PAGE_SIZE);
}

static uintptr_t addr_from_frame_index(uint32_t frame_index) {
    return (uintptr_t)(frame_index * PMM_PAGE_SIZE);
}

static int bitmap_test(uint32_t frame_index) {
    return (frame_bitmap[frame_index / 8u] & (1u << (frame_index % 8u))) != 0;
}

static void bitmap_set(uint32_t frame_index) {
    frame_bitmap[frame_index / 8u] =
        (uint8_t)(frame_bitmap[frame_index / 8u] | (1u << (frame_index % 8u)));
}

static void bitmap_clear(uint32_t frame_index) {
    frame_bitmap[frame_index / 8u] =
        (uint8_t)(frame_bitmap[frame_index / 8u] & ~(1u << (frame_index % 8u)));
}

static void mark_frame_used(uint32_t frame_index) {
    if (frame_index >= PMM_MAX_FRAMES) {
        return;
    }

    if (!bitmap_test(frame_index)) {
        bitmap_set(frame_index);

        if (free_frames > 0) {
            free_frames--;
        }
    }
}

static void mark_frame_free(uint32_t frame_index) {
    if (frame_index >= PMM_MAX_FRAMES) {
        return;
    }

    if (bitmap_test(frame_index)) {
        bitmap_clear(frame_index);
        free_frames++;
    }
}

static void reserve_range(uintptr_t start_addr, uintptr_t end_addr) {
    uint32_t start = align_down_page((uint32_t)start_addr);
    uint32_t end = align_up_page((uint32_t)end_addr);

    for (uint32_t addr = start; addr < end; addr += PMM_PAGE_SIZE) {
        mark_frame_used(frame_index_from_addr(addr));
    }
}

static void free_range(uintptr_t start_addr, uintptr_t end_addr) {
    uint32_t start = align_up_page((uint32_t)start_addr);
    uint32_t end = align_down_page((uint32_t)end_addr);

    for (uint32_t addr = start; addr < end; addr += PMM_PAGE_SIZE) {
        mark_frame_free(frame_index_from_addr(addr));
    }
}

static void mark_all_frames_used(void) {
    for (uint32_t i = 0; i < PMM_BITMAP_BYTES; ++i) {
        frame_bitmap[i] = 0xFFu;
    }

    free_frames = 0;
}

static void discover_from_mmap(const multiboot_info_t *mbi) {
    uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
    uint32_t cursor = mbi->mmap_addr;

    while (cursor < mmap_end) {
        const multiboot_mmap_entry_t *entry =
            (const multiboot_mmap_entry_t *)(uintptr_t)cursor;

        uint64_t base =
            ((uint64_t)entry->base_addr_high << 32) | entry->base_addr_low;

        uint64_t length =
            ((uint64_t)entry->length_high << 32) | entry->length_low;

        uint64_t end = base + length;

        console_write("MMAP: base=");
        console_write_hex32((uint32_t)base);
        console_write(" length=");
        console_write_hex32((uint32_t)length);
        console_write(" type=");
        console_write_u32_dec(entry->type);
        console_putc('\n');

        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            if (base < 0x100000000ull) {
                uint32_t usable_start = (uint32_t)base;
                uint32_t usable_end;

                if (end > 0x100000000ull) {
                    usable_end = 0xFFFFFFFFu;
                } else {
                    usable_end = (uint32_t)end;
                }

                free_range(usable_start, usable_end);

                uint32_t start_frame = frame_index_from_addr(
                    align_up_page(usable_start)
                );
                uint32_t end_frame = frame_index_from_addr(
                    align_down_page(usable_end)
                );

                if (end_frame > start_frame) {
                    usable_frames += end_frame - start_frame;
                }

                if (usable_end > highest_physical_addr) {
                    highest_physical_addr = usable_end;
                }
            }
        }

        cursor += entry->size + sizeof(entry->size);
    }
}

static void reserve_kernel_and_boot_data(const multiboot_info_t *mbi) {
    /*
     * Reserve the first MiB.
     *
     * The low-memory area contains BIOS data, interrupt vector table,
     * VGA memory, EBDA, bootloader leftovers, and other PC-compatible
     * legacy regions. We are not using it for general page allocation.
     */
    reserve_range(0x00000000u, 0x00100000u);

    reserve_range(
        (uintptr_t)__kernel_start,
        (uintptr_t)__kernel_end
    );

    /*
     * Reserve the visible part of the Multiboot information structure.
     */
    reserve_range(
        (uintptr_t)mbi,
        (uintptr_t)mbi + sizeof(multiboot_info_t)
    );

    /*
     * Reserve the Multiboot memory map buffer itself.
     */
    if ((mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0) {
        reserve_range(
            (uintptr_t)mbi->mmap_addr,
            (uintptr_t)mbi->mmap_addr + mbi->mmap_length
        );
    }
}

void pmm_init(const multiboot_info_t *mbi) {
    if (mbi == NULL) {
        kernel_panic("pmm_init received null Multiboot info pointer");
    }

    mark_all_frames_used();

    total_frames = PMM_MAX_FRAMES;
    usable_frames = 0;
    reserved_frames = 0;
    highest_physical_addr = 0;

    if ((mbi->flags & MULTIBOOT_INFO_MEM_MAP) == 0) {
        kernel_panic("Multiboot memory map is missing");
    }

    console_writeln("PMM: parsing Multiboot memory map");

    discover_from_mmap(mbi);
    reserve_kernel_and_boot_data(mbi);

    reserved_frames = total_frames - free_frames;

    console_writeln("PMM: physical page bitmap initialized");
    pmm_dump_stats();
}

uintptr_t pmm_alloc_page(void) {
    /*
     * Start at frame 256, which is physical address 1 MiB.
     * We keep low memory reserved for now.
     */
    for (uint32_t frame = 256; frame < PMM_MAX_FRAMES; ++frame) {
        if (!bitmap_test(frame)) {
            mark_frame_used(frame);
            return addr_from_frame_index(frame);
        }
    }

    return PMM_INVALID_PAGE;
}


uintptr_t pmm_alloc_page_below(uintptr_t max_exclusive) {
	uint32_t max_frame = (uint32_t)(max_exclusive / PMM_PAGE_SIZE);
	
	if (max_frame > PMM_MAX_FRAMES) {
		max_frame = PMM_MAX_FRAMES;
	}
	
	for (uint32_t frame = 256; frame < max_frame; ++frame) {
		if (!bitmap_test(frame)) {
			mark_frame_used(frame);
			return addr_from_frame_index(frame);
		}
	}
	
	return PMM_INVALID_PAGE;
}



uintptr_t pmm_alloc_contiguous_pages_below(
	uint32_t page_count,
	uintptr_t max_exclusive
) {
	if (page_count == 0) {
		return PMM_INVALID_PAGE;
	}
	
	uint32_t max_frame = (uint32_t)(max_exclusive / PMM_PAGE_SIZE);
	
	if (max_frame > PMM_MAX_FRAMES) {
		max_frame = PMM_MAX_FRAMES;
	}
	
	uint32_t run_start = 0;
	uint32_t run_length = 0;
	
	for (uint32_t frame = 256; frame < max_frame; ++frame) {
		if (!bitmap_test(frame)) {
			if (run_length == 0) {
				run_start = frame;
			}
			
			run_length++;
			
			if (run_length == page_count) {
				for (uint32_t i = 0; i < page_count; ++i) {
					mark_frame_used(run_start + i);
				}
				
				return addr_from_frame_index(run_start);
			}
		} else {
			run_start = 0;
			run_length = 0;
		}
	}
	
	return PMM_INVALID_PAGE;
}



void pmm_free_page(uintptr_t physical_addr) {
    if ((physical_addr & (PMM_PAGE_SIZE - 1u)) != 0) {
        kernel_panic("pmm_free_page received unaligned address");
    }

    if (physical_addr < 0x00100000u) {
        kernel_panic("pmm_free_page attempted to free low memory");
    }

    mark_frame_free(frame_index_from_addr(physical_addr));
}

pmm_stats_t pmm_get_stats(void) {
    pmm_stats_t stats;

    stats.total_frames = total_frames;
    stats.usable_frames = usable_frames;
    stats.reserved_frames = reserved_frames;
    stats.free_frames = free_frames;
    stats.used_frames = total_frames - free_frames;
    stats.highest_physical_addr = highest_physical_addr;

    return stats;
}

void pmm_dump_stats(void) {
    pmm_stats_t stats = pmm_get_stats();

    console_write("PMM: highest physical address ");
    console_write_hex32(stats.highest_physical_addr);
    console_putc('\n');

    console_write("PMM: usable frames ");
    console_write_u32_dec(stats.usable_frames);
    console_putc('\n');

    console_write("PMM: free frames ");
    console_write_u32_dec(stats.free_frames);
    console_putc('\n');

    console_write("PMM: used/reserved frames ");
    console_write_u32_dec(stats.used_frames);
    console_putc('\n');
}

void pmm_test_once(void) {
    uintptr_t page_a = pmm_alloc_page();
    uintptr_t page_b = pmm_alloc_page();

    if (page_a == PMM_INVALID_PAGE || page_b == PMM_INVALID_PAGE) {
        kernel_panic("PMM test failed: allocation returned invalid page");
    }

    if (page_a == page_b) {
        kernel_panic("PMM test failed: duplicate page allocation");
    }

    if ((page_a & (PMM_PAGE_SIZE - 1u)) != 0 ||
        (page_b & (PMM_PAGE_SIZE - 1u)) != 0) {
        kernel_panic("PMM test failed: unaligned page allocation");
    }

    console_write("PMM test: allocated ");
    console_write_hex32((uint32_t)page_a);
    console_write(" and ");
    console_write_hex32((uint32_t)page_b);
    console_putc('\n');

    pmm_free_page(page_b);
    pmm_free_page(page_a);

    console_writeln("PMM test: allocation/free sanity check passed");
}
