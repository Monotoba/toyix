# Chapter 4 — Reading the Memory Map and Building a Physical Page Allocator

Before we add paging, a heap, processes, filesystems, or user programs, the kernel must answer a basic question:

> Which physical 4 KiB pages of RAM are safe for me to use?

Right now, our kernel can boot, print text, catch CPU exceptions, and receive timer/keyboard interrupts. But it still cannot safely allocate memory.

This chapter adds the first memory-management layer:

```text
Multiboot memory map parser
  ↓
physical memory manager
  ↓
bitmap of 4 KiB page frames
  ↓
pmm_alloc_page()
pmm_free_page()
```

The Multiboot specification says the bootloader may provide a memory map through `mmap_addr` and `mmap_length` when the appropriate flag bit is set; each entry identifies a base address, length, and type. We will use entries of type `1` as available RAM and treat everything else as reserved. ([GNU Operating System][1])

---

# 1. The big idea

Physical memory is divided into fixed-size frames:

```text
physical address 0x00000000 → frame 0
physical address 0x00001000 → frame 1
physical address 0x00002000 → frame 2
...
```

We will use 4 KiB frames because 4 KiB is the normal base page size on 32-bit x86 paging.

A physical page allocator answers:

```c
uintptr_t page = pmm_alloc_page();
```

and returns the physical address of a free 4 KiB frame.

Later, the virtual memory manager will map that physical page into some virtual address.

For now, we are still running without paging, so physical addresses and linear addresses are effectively the same because we installed flat segmentation in Chapter 2.

---

# 2. Bitmap allocation

There are several ways to track free physical pages:

| Method                   | Notes                                          |
| ------------------------ | ---------------------------------------------- |
| Bitmap                   | Simple, compact, slower scan                   |
| Stack/list of free pages | Fast allocation, uses memory inside free pages |
| Buddy allocator          | Better for contiguous power-of-two blocks      |
| Per-page metadata array  | Flexible, more memory overhead                 |

We will start with a **bitmap**.

OSDev describes a bitmap page-frame allocator as one of the common physical memory manager designs: each bit represents whether a page frame is free or used, and the allocator scans for a free bit. ([OSDev Wiki][2])

Our rule:

```text
bit = 1 → page is used/reserved
bit = 0 → page is free
```

A full 4 GiB 32-bit physical address space contains:

```text
4 GiB / 4 KiB = 1,048,576 frames
```

A bitmap for that many frames needs:

```text
1,048,576 bits / 8 = 131,072 bytes = 128 KiB
```

That is acceptable for this tutorial kernel. It avoids the early chicken-and-egg problem of needing memory allocation before we have a memory allocator.

---

# 3. Patch overview

Add:

```text
arch/x86/
└── multiboot.h

include/kernel/
└── pmm.h

kernel/
└── pmm.c
```

Modify:

```text
include/kernel/console.h
kernel/console.c
kernel/kmain.c
linker.ld
Makefile
tests/smoke.sh
```

---

# 4. Update `include/kernel/console.h`

Add decimal output support.

```c
// include/kernel/console.h
#ifndef TOYIX_KERNEL_CONSOLE_H
#define TOYIX_KERNEL_CONSOLE_H

#include <stdint.h>

typedef struct console_driver {
    const char *name;
    void (*init)(void);
    void (*putc)(char c);
} console_driver_t;

void console_register(const console_driver_t *driver);
void console_init_all(void);

void console_putc(char c);
void console_write(const char *text);
void console_writeln(const char *text);
void console_write_hex32(uint32_t value);
void console_write_u32_dec(uint32_t value);

#endif
```

## Why add decimal output now?

Hexadecimal is excellent for addresses:

```text
0x00100000
0x0009FC00
0xB8000
```

But memory totals, page counts, and timer frequencies are often easier to understand in decimal:

```text
32768 pages
134217728 bytes
100 Hz
```

So the console layer needs both.

---

# 5. Update `kernel/console.c`

Replace the previous file with this version.

```c
// kernel/console.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"

#define MAX_CONSOLE_DRIVERS 4

static const console_driver_t *drivers[MAX_CONSOLE_DRIVERS];
static size_t driver_count = 0;

void console_register(const console_driver_t *driver) {
    if (driver == NULL) {
        return;
    }

    if (driver_count >= MAX_CONSOLE_DRIVERS) {
        return;
    }

    drivers[driver_count++] = driver;
}

void console_init_all(void) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->init != NULL) {
            drivers[i]->init();
        }
    }
}

void console_putc(char c) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->putc != NULL) {
            drivers[i]->putc(c);
        }
    }
}

void console_write(const char *text) {
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        console_putc(*text++);
    }
}

void console_writeln(const char *text) {
    console_write(text);
    console_putc('\n');
}

void console_write_hex32(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";

    console_write("0x");

    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        console_putc(digits[nibble]);
    }
}

void console_write_u32_dec(uint32_t value) {
    char buffer[11];
    size_t index = 0;

    if (value == 0) {
        console_putc('0');
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        console_putc(buffer[--index]);
    }
}
```

## Why this function is intentionally small

This is not `printf`.

A full `printf` implementation requires format parsing, width handling, padding, signed/unsigned conversions, and varargs. We will eventually want something like `kprintf`, but for now small, explicit output functions are easier to audit.

Kernel code benefits from boring clarity.

---

# 6. Update `linker.ld`

Replace the linker script with this version.

```ld
/* linker.ld */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    __kernel_start = .;

    .multiboot ALIGN(4) :
    {
        KEEP(*(.multiboot))
    }

    .text ALIGN(4K) :
    {
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }

    __kernel_end = .;
}
```

## Why we added linker symbols

The physical memory manager must not hand out pages containing the kernel itself.

These symbols give C code the physical range occupied by the kernel image:

```c
extern char __kernel_start[];
extern char __kernel_end[];
```

Then the PMM can reserve:

```text
[__kernel_start, __kernel_end)
```

That prevents future allocation from overwriting the kernel’s code, data, stack, console driver state, GDT, IDT, or bitmap.

---

# 7. Add `arch/x86/multiboot.h`

```c
// arch/x86/multiboot.h
#ifndef TOYIX_ARCH_X86_MULTIBOOT_H
#define TOYIX_ARCH_X86_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

#define MULTIBOOT_INFO_MEMORY      (1u << 0)
#define MULTIBOOT_INFO_BOOT_DEVICE (1u << 1)
#define MULTIBOOT_INFO_CMDLINE     (1u << 2)
#define MULTIBOOT_INFO_MODS        (1u << 3)
#define MULTIBOOT_INFO_AOUT_SYMS   (1u << 4)
#define MULTIBOOT_INFO_ELF_SHDR    (1u << 5)
#define MULTIBOOT_INFO_MEM_MAP     (1u << 6)

typedef struct multiboot_info {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;

    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) multiboot_info_t;

typedef struct multiboot_mmap_entry {
    uint32_t size;
    uint32_t base_addr_low;
    uint32_t base_addr_high;
    uint32_t length_low;
    uint32_t length_high;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

#endif
```

## Why this struct is partial

The Multiboot information structure contains more fields than we need right now.

We define enough to reach:

```text
flags
mem_lower
mem_upper
mmap_length
mmap_addr
```

This is safe because the fields we use are at their official positions in the structure. We are not claiming this is a complete Multiboot parser.

The Multiboot memory map is a buffer of variable-sized entries. Each entry begins with a `size` field, and the next entry is located by adding `size + sizeof(size)` bytes. The specification describes `mmap_addr` as the address of the buffer and `mmap_length` as its total length when the memory-map flag is set. ([GNU Operating System][1])

---

# 8. Add `include/kernel/pmm.h`

```c
// include/kernel/pmm.h
#ifndef TOYIX_KERNEL_PMM_H
#define TOYIX_KERNEL_PMM_H

#include <stdint.h>
#include "arch/x86/multiboot.h"

#define PMM_PAGE_SIZE 4096u
#define PMM_INVALID_PAGE 0u

typedef struct pmm_stats {
    uint32_t total_frames;
    uint32_t usable_frames;
    uint32_t reserved_frames;
    uint32_t free_frames;
    uint32_t used_frames;
    uint32_t highest_physical_addr;
} pmm_stats_t;

void pmm_init(const multiboot_info_t *mbi);

uintptr_t pmm_alloc_page(void);
void pmm_free_page(uintptr_t physical_addr);

pmm_stats_t pmm_get_stats(void);
void pmm_dump_stats(void);
void pmm_test_once(void);

#endif
```

## Why return physical addresses as `uintptr_t`

A physical page allocator returns addresses, not pointers to typed objects.

Eventually, a returned physical page might be:

```text
mapped into kernel virtual memory
mapped into user virtual memory
used as a page table
used for DMA
used for a filesystem cache page
```

Using `uintptr_t` makes it explicit that this is an integer address.

For this 32-bit kernel, `uintptr_t` is 32 bits.

---

# 9. Add `kernel/pmm.c`

```c
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
```

---

# 10. How the PMM works

The initialization strategy is deliberately conservative:

```text
1. Mark every possible frame as used.
2. Read the Multiboot memory map.
3. For each available RAM region, mark those pages free.
4. Reserve low memory.
5. Reserve the kernel image.
6. Reserve the Multiboot information and memory-map buffer.
7. Allocate only from what remains free.
```

That “used by default” policy is important.

A kernel should not assume memory is free merely because it exists. ACPI tables, ROM areas, MMIO windows, BIOS regions, bootloader structures, and device memory can appear in the physical map. OSDev’s x86 memory-map material emphasizes that PC physical memory contains special regions and that an OS needs a memory map rather than assuming all addresses are usable RAM. ([OSDev Wiki][3])

---

# 11. Why reserve the first MiB?

We reserve:

```text
0x00000000 - 0x000FFFFF
```

for now.

That area includes historically important PC-compatible regions such as the interrupt vector table, BIOS data area, extended BIOS data area, VGA memory region, ROM areas, and bootloader scratch space. OSDev’s x86 memory-map page describes these conventional low-memory regions and why they should not be treated as ordinary RAM. ([OSDev Wiki][3])

Could a more advanced kernel use some low-memory pages? Yes.

Should our beginner kernel allocate from them today? No.

Conservative beats clever here.

---

# 12. Why reserve the Multiboot memory map?

The memory map itself lives somewhere in RAM.

If we free every available RAM region and forget that GRUB placed the memory map inside one of those regions, our allocator might hand that page out and overwrite the memory map while we are still using it.

So we reserve:

```c
reserve_range(mbi->mmap_addr, mbi->mmap_addr + mbi->mmap_length);
```

Later we may copy boot information into kernel-owned memory and then release the original bootloader pages. For now, reserving them is simpler and safer.

---

# 13. Update `kernel/kmain.c`

Replace the previous `kernel/kmain.c` with this version.

```c
// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/multiboot.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/idle.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    console_register(&serial_console_driver);
    console_register(&vga_text_console_driver);
    console_init_all();

    console_writeln("Toyix kernel alive");

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        console_writeln("Boot protocol: Multiboot OK");
    } else {
        console_write("Boot protocol: unexpected magic ");
        console_write_hex32(multiboot_magic);
        console_putc('\n');
        kernel_panic("unsupported boot protocol");
    }

    const multiboot_info_t *mbi =
        (const multiboot_info_t *)(uintptr_t)multiboot_info_addr;

    console_write("Multiboot info at ");
    console_write_hex32(multiboot_info_addr);
    console_putc('\n');

    gdt_init();
    idt_init();
    pic_init();

    pmm_init(mbi);
    pmm_test_once();

    pit_init(100);
    keyboard_init();

    console_writeln("Interrupt hardware: configured");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    interrupts_enable();

    console_writeln("Interrupts: enabled");

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: paging and a kernel heap.");

    kernel_idle();
}
```

## Why PMM initialization happens before interrupts are enabled

The PMM itself does not require interrupts.

Initializing memory before enabling interrupts gives us a quieter boot sequence:

```text
console
GDT
IDT
PIC remap
physical memory manager
timer
keyboard
enable interrupts
```

This keeps early boot deterministic.

Later, once we have more drivers, memory allocation may happen during driver initialization. That is another reason the PMM must come early.

---

# 14. Update `Makefile`

Add `kernel/pmm.o` to the object list.

```make
# Makefile

TARGET      ?= i686-elf
CC          := $(TARGET)-gcc
AS          := nasm
GRUB_FILE   := grub-file
GRUB_MKRESCUE := grub-mkrescue
QEMU        := qemu-system-i386

CFLAGS := -std=gnu11 \
          -ffreestanding \
          -O2 \
          -Wall \
          -Wextra \
          -Werror \
          -m32 \
          -fno-stack-protector \
          -fno-pic \
          -fno-pie \
          -Iinclude \
          -I. \
          $(CFLAGS_EXTRA)

LDFLAGS := -T linker.ld \
           -ffreestanding \
           -O2 \
           -nostdlib \
           -lgcc

OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o

.PHONY: all clean iso run test test-exception

all: build/kernel.elf

build/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

iso: build/kernel.elf grub.cfg
	@mkdir -p build/iso/boot/grub
	cp build/kernel.elf build/iso/boot/kernel.elf
	cp grub.cfg build/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o build/toyix.iso build/iso

run: iso
	$(QEMU) -cdrom build/toyix.iso -serial stdio

test: iso
	$(GRUB_FILE) --is-x86-multiboot build/kernel.elf
	@mkdir -p build
	@timeout 5s $(QEMU) \
		-cdrom build/toyix.iso \
		-serial stdio \
		-display none \
		-monitor none \
		-no-reboot \
		> build/test.log || true
	grep -q "Toyix kernel alive" build/test.log
	grep -q "Boot protocol: Multiboot OK" build/test.log
	grep -q "PMM: parsing Multiboot memory map" build/test.log
	grep -q "PMM: physical page bitmap initialized" build/test.log
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, IRQ, and PMM smoke test passed."

test-exception:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_TEST_EXCEPTION
	@mkdir -p build
	@timeout 5s $(QEMU) \
		-cdrom build/toyix.iso \
		-serial stdio \
		-display none \
		-monitor none \
		-no-reboot \
		> build/exception.log || true
	grep -q "Triggering test exception with UD2" build/exception.log
	grep -q "CPU EXCEPTION" build/exception.log
	grep -q "Invalid Opcode" build/exception.log
	grep -q "KERNEL PANIC" build/exception.log
	@echo "Exception handling test passed."

clean:
	rm -rf build
```

---

# 15. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception

echo "All Chapter 4 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 16. Expected output

The exact memory map will vary depending on QEMU, GRUB, and machine configuration, but you should see something structurally like:

```text
Toyix kernel alive
Boot protocol: Multiboot OK
Multiboot info at 0x0000XXXX
GDT: installed flat kernel code/data segments
IDT: installed CPU exceptions and hardware IRQ gates
PIC: remapped IRQs to vectors 0x20-0x2F
PMM: parsing Multiboot memory map
MMAP: base=0x00000000 length=0x0009FC00 type=1
MMAP: base=0x0009FC00 length=0x00000400 type=2
MMAP: base=0x00100000 length=0x07EE0000 type=1
PMM: physical page bitmap initialized
PMM: highest physical address 0x07FE0000
PMM: usable frames 32607
PMM: free frames 32100
PMM: used/reserved frames 1019476
PMM test: allocated 0x001XXXXX and 0x001XXXXX
PMM test: allocation/free sanity check passed
PIT: timer running at 100 Hz
Keyboard: IRQ1 handler installed
Interrupt hardware: configured
Interrupts: enabled
Timer: observed 3 ticks
Try typing in the QEMU window.
Next stop: paging and a kernel heap.
```

Do not expect the numbers above to match exactly.

The important signs are:

```text
PMM parses at least one available region
PMM reports free frames
PMM allocates two distinct aligned pages
PMM frees them without panic
timer still ticks afterward
```

---

# 17. Important limitation in this PMM

This PMM tracks up to 4 GiB of physical address space.

That is fine for a 32-bit beginner kernel.

But modern machines may have memory above 4 GiB. Our code deliberately ignores RAM above the 32-bit physical range. A later PAE or x86-64 version would need wider physical addresses and a different paging design.

Also, this PMM is not fast. It scans the bitmap from frame 256 every time:

```c
for (uint32_t frame = 256; frame < PMM_MAX_FRAMES; ++frame)
```

That is acceptable for early boot and a teaching kernel. Later we can improve it by storing a `next_free_hint`, using a buddy allocator, or layering a zone allocator on top.

---

# 18. A small improvement you may want immediately

The current allocator returns the first free page every time.

After freeing a page, the next allocation may return that same page again. That is legal.

But for debugging, you might prefer newly freed pages to be filled with a pattern such as:

```text
0xCC
0xDEADBEEF
0xA5
```

We are not doing that yet because we do not yet have a safe virtual mapping abstraction. Once paging is enabled, page poisoning becomes much easier.

---

# 19. Common failures

## Failure: `Multiboot memory map is missing`

Check that the Multiboot header flags in `boot.asm` request memory information:

```asm
MB_FLAGS equ 0x00000003
```

That asks for memory info and module alignment, but the detailed memory map is controlled by what GRUB provides in the Multiboot info flags. In practice, GRUB usually provides it for this setup, but the kernel must still check the flag rather than blindly trusting `mmap_addr`. The Multiboot spec uses the flags field to indicate which information fields are valid. ([GNU Operating System][1])

## Failure: page allocations return `0x00000000`

`0x00000000` is our invalid return value:

```c
#define PMM_INVALID_PAGE 0u
```

That means the allocator found no free page. Likely causes:

```text
memory map not parsed correctly
all available regions accidentally reserved
bitmap bit meaning reversed
mmap cursor calculation wrong
```

Check this line carefully:

```c
cursor += entry->size + sizeof(entry->size);
```

Do not use:

```c
cursor += sizeof(multiboot_mmap_entry_t);
```

The Multiboot memory map uses variable-sized entries.

## Failure: free-frame count is huge or nonsensical

Likely causes:

```text
freeing ranges that extend beyond 4 GiB
overflow in base + length
wrong alignment
treating reserved entries as available
```

Only entries with:

```c
entry->type == 1
```

should be treated as usable RAM.

## Failure: kernel crashes after PMM test

Likely cause:

```text
kernel image range not reserved
bitmap not reserved indirectly
Multiboot structure not reserved
```

The bitmap is a static global inside the kernel image, so reserving the kernel range also reserves the bitmap.

This is why the linker symbols matter.

---

# 20. Where we are now

We now have:

```text
boot
console
GDT
IDT
exceptions
PIC
timer IRQ
keyboard IRQ
Multiboot memory map parser
physical page allocator
```

That is a meaningful kernel foundation.

The dependency chain now looks like:

```text
boot.asm
  ↓
kernel_main
  ↓
console
  ↓
GDT / IDT
  ↓
PIC
  ↓
PMM
  ↓
drivers using IRQs
```

And the memory side now begins:

```text
Multiboot memory map
  ↓
available physical regions
  ↓
bitmap
  ↓
4 KiB page allocation
```

---

# 21. Commit this chapter

After the tests pass:

```bash
git status
git add .
git commit -m "Parse Multiboot memory map and add physical page allocator"
```

That gives you a clean checkpoint before paging.

---

# 22. Next chapter

The next major step is **paging**.

We will build:

```text
page directory
page tables
identity map low memory
map kernel memory
enable CR0.PG
handle page faults more intelligently
```

Once paging works, we can build a real kernel heap:

```text
kmalloc()
kfree()
```

The important transition will be this:

```text
physical page allocator
  ↓
virtual memory manager
  ↓
kernel heap
```

Right now, the PMM can give us physical pages. The next chapter teaches the CPU how to map those pages into virtual memory.

[1]: https://www.gnu.org/software/grub/manual/multiboot/multiboot.txt "GRUB Multiboot Specification"
[2]: https://wiki.osdev.org/Page_Frame_Allocation "Page Frame Allocation"
[3]: https://wiki.osdev.org/Memory_Map_%28x86%29 "Memory Map (x86) - OSDev Wiki"

---

# Resources

- [Chapter 04 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_04)
- [GNU GRUB Multiboot Specification](https://www.gnu.org/software/grub/manual/multiboot/multiboot.txt)
- [OSDev Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation)
- [OSDev x86 Memory Map](https://wiki.osdev.org/Memory_Map_%28x86%29)
- [OSDev Memory Management](https://wiki.osdev.org/Memory_Management)
- [OSDev Detecting Memory](https://wiki.osdev.org/Detecting_Memory_%28x86%29)

That completes the fourth Toyix milestone: reading the Multiboot memory map and allocating physical 4 KiB page frames.

Happy Coding!
