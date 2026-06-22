# Chapter 7 — A Real Virtual Memory Mapping Layer

In Chapter 5 we enabled paging, but only with a fixed identity map:

```text
virtual 0x00100000 → physical 0x00100000
virtual 0x00B8000 → physical 0x00B8000
```

In Chapter 6 we built a heap, but it still had an important weakness:

```text
heap pages must come from physical memory below 16 MiB
```

because only the first 16 MiB were identity-mapped.

Now we fix the architectural problem.

This chapter adds a small **virtual memory manager**, or VMM:

```c
vmm_map_page(virtual_addr, physical_addr, flags);
vmm_unmap_page(virtual_addr);
vmm_get_physical(virtual_addr);
```

This lets the kernel take any usable physical page and map it into a chosen virtual address.

Intel’s system programming manuals describe paging as the IA-32 mechanism that translates linear addresses through paging structures, with CR3 pointing at the current page directory and CR0.PG enabling paging. The current Intel manual page also confirms that Volume 3 covers operating-system support, including memory management, protection, and interrupt/exception handling. ([Intel][1])

---

# 1. Why this matters

Right now, our kernel memory stack is:

```text
PMM gives physical pages
  ↓
identity paging maps first 16 MiB
  ↓
heap can only safely use pages below 16 MiB
```

After this chapter, the stack becomes:

```text
PMM gives physical pages
  ↓
VMM maps physical pages into chosen virtual addresses
  ↓
heap can eventually grow in a virtual heap region
```

This is the conceptual transition from:

```text
physical memory as addresses
```

to:

```text
physical memory as backing storage for virtual address spaces
```

That is one of the most important ideas in operating-system construction.

---

# 2. What we will build

We will add:

```text
arch/x86/
├── vmm.c
└── vmm.h
```

We will modify:

```text
arch/x86/paging.asm
arch/x86/paging.c
arch/x86/paging.h
kernel/kmain.c
Makefile
tests/smoke.sh
```

The new layer will support:

```text
map one 4 KiB page
unmap one 4 KiB page
translate virtual → physical
allocate page tables on demand
invalidate TLB entries
run a mapping sanity test
```

We are not yet creating per-process address spaces. There is still only one kernel page directory. But the structure we build here is the beginning of the real VMM.

---

# 3. The page-table relationship

In 32-bit non-PAE paging:

```text
page directory
  ├── entry 0 → page table for virtual 0x00000000 - 0x003FFFFF
  ├── entry 1 → page table for virtual 0x00400000 - 0x007FFFFF
  ├── entry 2 → page table for virtual 0x00800000 - 0x00BFFFFF
  └── ...
```

Each page table maps 4 MiB because:

```text
1024 entries × 4096 bytes = 4 MiB
```

A virtual address is split like this:

```text
bits 31..22 → page directory index
bits 21..12 → page table index
bits 11..0  → offset inside page
```

So if we map:

```text
virtual 0xD0000000
```

then:

```text
directory index = 0xD0000000 >> 22 = 832
table index     = (0xD0000000 >> 12) & 0x3FF = 0
offset          = 0
```

If page-directory entry 832 does not exist yet, the VMM must allocate a page table, clear it, and install it into the page directory.

---

# 4. Update `arch/x86/paging.h`

Replace `arch/x86/paging.h` with this version.

```c
// arch/x86/paging.h
#ifndef TOYIX_ARCH_X86_PAGING_H
#define TOYIX_ARCH_X86_PAGING_H

#include <stdint.h>

#define X86_PAGE_SIZE 4096u

#define X86_PAGE_PRESENT  0x001u
#define X86_PAGE_WRITABLE 0x002u
#define X86_PAGE_USER     0x004u

#define X86_PAGE_WRITETHROUGH 0x008u
#define X86_PAGE_NOCACHE      0x010u
#define X86_PAGE_ACCESSED     0x020u
#define X86_PAGE_DIRTY        0x040u
#define X86_PAGE_GLOBAL       0x100u

#define X86_PAGE_FRAME_MASK   0xFFFFF000u
#define X86_PAGE_FLAGS_MASK   0x00000FFFu

void paging_init(void);
int paging_is_enabled(void);
void paging_test_identity_mapping(void);

uint32_t *paging_get_kernel_directory(void);

#endif
```

## What changed?

The VMM needs to know where the current kernel page directory is.

So we add:

```c
uint32_t *paging_get_kernel_directory(void);
```

We also define common page flags in one place.

The VMM should not duplicate magic constants such as:

```c
0x001
0x002
0xFFFFF000
```

Those are paging-layer facts, so they belong in `paging.h`.

---

# 5. Update `arch/x86/paging.asm`

Replace `arch/x86/paging.asm` with this expanded version.

```asm
; arch/x86/paging.asm
;
; Low-level paging control functions.
;
; C prototypes:
;   void paging_load_directory(uint32_t physical_addr);
;   void paging_enable_asm(void);
;   uint32_t paging_read_cr0(void);
;   uint32_t paging_read_cr2(void);
;   uint32_t paging_read_cr3(void);
;   void paging_invalidate_page(uintptr_t virtual_addr);
;   void paging_reload_cr3(void);

BITS 32

global paging_load_directory
global paging_enable_asm
global paging_read_cr0
global paging_read_cr2
global paging_read_cr3
global paging_invalidate_page
global paging_reload_cr3

paging_load_directory:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

paging_enable_asm:
    mov eax, cr0
    or eax, 0x80000000       ; CR0.PG
    mov cr0, eax
    ret

paging_read_cr0:
    mov eax, cr0
    ret

paging_read_cr2:
    mov eax, cr2
    ret

paging_read_cr3:
    mov eax, cr3
    ret

paging_invalidate_page:
    mov eax, [esp + 4]
    invlpg [eax]
    ret

paging_reload_cr3:
    mov eax, cr3
    mov cr3, eax
    ret
```

## Why TLB invalidation matters

The CPU caches virtual-to-physical translations in the TLB, the translation lookaside buffer.

If we change a page-table entry, the CPU may still have the old translation cached. So after mapping or unmapping a page, we must invalidate that virtual address.

For one page, we use:

```asm
invlpg [eax]
```

For larger changes, reloading `CR3` is a simple way to flush non-global translations.

---

# 6. Update `arch/x86/paging.c`

Only two changes are needed.

First, make sure the page-frame mask comes from `paging.h`, not a local duplicate. Remove this local line if it exists:

```c
#define PAGE_FRAME_MASK 0xFFFFF000u
```

Then change any use of `PAGE_FRAME_MASK` to:

```c
X86_PAGE_FRAME_MASK
```

For example:

```c
identity_tables[table][entry] =
    (physical_addr & X86_PAGE_FRAME_MASK) |
    X86_PAGE_PRESENT |
    X86_PAGE_WRITABLE;
```

and:

```c
page_directory[table] =
    ((uint32_t)&identity_tables[table][0] & X86_PAGE_FRAME_MASK) |
    X86_PAGE_PRESENT |
    X86_PAGE_WRITABLE;
```

Second, add this function near the bottom of `paging.c`:

```c
uint32_t *paging_get_kernel_directory(void) {
    return page_directory;
}
```

## Why expose the directory this way?

The page directory is still owned by the paging layer.

The VMM does not create it. The VMM merely modifies it through a controlled interface.

Later, when we support multiple address spaces, this will change. We may have:

```c
vmm_address_space_t *kernel_space;
vmm_address_space_t *current_space;
```

But today, a single kernel page directory is enough.

---

# 7. Add `arch/x86/vmm.h`

```c
// arch/x86/vmm.h
#ifndef TOYIX_ARCH_X86_VMM_H
#define TOYIX_ARCH_X86_VMM_H

#include <stdint.h>

#define VMM_OK 0
#define VMM_ERR_INVALID -1
#define VMM_ERR_NO_MEMORY -2
#define VMM_ERR_ALREADY_MAPPED -3
#define VMM_ERR_NOT_MAPPED -4

void vmm_init(void);

int vmm_map_page(
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
);

int vmm_unmap_page(uintptr_t virtual_addr);

uintptr_t vmm_get_physical(uintptr_t virtual_addr);

void vmm_test_once(void);

#endif
```

## Why return error codes instead of panicking?

Low-level memory code should not always panic.

Some failures are fatal during early boot. But later, callers may need to handle mapping failures gracefully.

For example:

```text
process requested invalid memory → return error to process
filesystem cache cannot grow → reclaim memory
driver mapping failed → fail driver initialization
```

So the VMM returns explicit status codes.

Our tests may still panic if a required operation fails.

---

# 8. Add `arch/x86/vmm.c`

```c
// arch/x86/vmm.c
#include <stdint.h>
#include "arch/x86/paging.h"
#include "arch/x86/vmm.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"

#define PAGE_DIRECTORY_ENTRIES 1024u
#define PAGE_TABLE_ENTRIES     1024u

/*
 * Page tables must be writable by the kernel while we are setting them up.
 *
 * Until the kernel maps all physical memory somewhere, we allocate page-table
 * pages below the original 16 MiB identity-mapped region.
 */
#define VMM_BOOTSTRAP_TABLE_LIMIT 0x01000000u

typedef uint32_t page_directory_entry_t;
typedef uint32_t page_table_entry_t;

static page_directory_entry_t *kernel_directory;
static uint32_t mapped_page_count;

extern void paging_invalidate_page(uintptr_t virtual_addr);
extern void paging_reload_cr3(void);

static int is_page_aligned(uintptr_t value) {
    return (value & (X86_PAGE_SIZE - 1u)) == 0;
}

static uint32_t directory_index(uintptr_t virtual_addr) {
    return (uint32_t)(virtual_addr >> 22);
}

static uint32_t table_index(uintptr_t virtual_addr) {
    return (uint32_t)((virtual_addr >> 12) & 0x3FFu);
}

static page_table_entry_t *table_from_pde(page_directory_entry_t pde) {
    uintptr_t physical = pde & X86_PAGE_FRAME_MASK;

    /*
     * This cast is only currently valid because page-table pages are allocated
     * below 16 MiB, which Chapter 5 identity-mapped.
     */
    return (page_table_entry_t *)physical;
}

static page_table_entry_t *get_or_create_page_table(
    uint32_t dir_index,
    uint32_t flags
) {
    page_directory_entry_t pde = kernel_directory[dir_index];

    if ((pde & X86_PAGE_PRESENT) != 0) {
        return table_from_pde(pde);
    }

    uintptr_t table_phys = pmm_alloc_page_below(VMM_BOOTSTRAP_TABLE_LIMIT);

    if (table_phys == PMM_INVALID_PAGE) {
        return 0;
    }

    page_table_entry_t *table = (page_table_entry_t *)table_phys;
    memset(table, 0, X86_PAGE_SIZE);

    uint32_t table_flags =
        X86_PAGE_PRESENT |
        X86_PAGE_WRITABLE;

    if ((flags & X86_PAGE_USER) != 0) {
        table_flags |= X86_PAGE_USER;
    }

    kernel_directory[dir_index] =
        (table_phys & X86_PAGE_FRAME_MASK) | table_flags;

    /*
     * The CPU may have cached the earlier non-present path. Reloading CR3 is
     * conservative and simple for this early kernel.
     */
    paging_reload_cr3();

    return table;
}

void vmm_init(void) {
    kernel_directory = paging_get_kernel_directory();
    mapped_page_count = 0;

    if (kernel_directory == 0) {
        kernel_panic("vmm_init could not get kernel page directory");
    }

    console_writeln("VMM: initialized kernel address-space mapper");
}

int vmm_map_page(
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
) {
    if (!is_page_aligned(virtual_addr) || !is_page_aligned(physical_addr)) {
        return VMM_ERR_INVALID;
    }

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    page_table_entry_t *table = get_or_create_page_table(dir, flags);

    if (table == 0) {
        return VMM_ERR_NO_MEMORY;
    }

    if ((table[tab] & X86_PAGE_PRESENT) != 0) {
        return VMM_ERR_ALREADY_MAPPED;
    }

    table[tab] =
        (physical_addr & X86_PAGE_FRAME_MASK) |
        (flags & X86_PAGE_FLAGS_MASK) |
        X86_PAGE_PRESENT;

    paging_invalidate_page(virtual_addr);
    mapped_page_count++;

    return VMM_OK;
}

int vmm_unmap_page(uintptr_t virtual_addr) {
    if (!is_page_aligned(virtual_addr)) {
        return VMM_ERR_INVALID;
    }

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    page_directory_entry_t pde = kernel_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return VMM_ERR_NOT_MAPPED;
    }

    page_table_entry_t *table = table_from_pde(pde);

    if ((table[tab] & X86_PAGE_PRESENT) == 0) {
        return VMM_ERR_NOT_MAPPED;
    }

    table[tab] = 0;
    paging_invalidate_page(virtual_addr);

    if (mapped_page_count > 0) {
        mapped_page_count--;
    }

    return VMM_OK;
}

uintptr_t vmm_get_physical(uintptr_t virtual_addr) {
    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);
    uint32_t offset = (uint32_t)(virtual_addr & (X86_PAGE_SIZE - 1u));

    page_directory_entry_t pde = kernel_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    page_table_entry_t *table = table_from_pde(pde);
    page_table_entry_t pte = table[tab];

    if ((pte & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    return (uintptr_t)((pte & X86_PAGE_FRAME_MASK) + offset);
}

void vmm_test_once(void) {
    console_writeln("VMM test: starting");

    uintptr_t physical = pmm_alloc_page();

    if (physical == PMM_INVALID_PAGE) {
        kernel_panic("VMM test could not allocate physical page");
    }

    /*
     * Pick a high virtual page outside the 16 MiB identity map.
     *
     * We are not using 0xC0000000 yet because that address is commonly used
     * later as the higher-half kernel base. This test address is temporary.
     */
    uintptr_t virtual_addr = 0xD0000000u;

    int rc = vmm_map_page(
        virtual_addr,
        physical,
        X86_PAGE_WRITABLE
    );

    if (rc != VMM_OK) {
        kernel_panic("VMM test failed to map page");
    }

    uintptr_t translated = vmm_get_physical(virtual_addr);

    if (translated != physical) {
        kernel_panic("VMM test translation mismatch");
    }

    volatile uint32_t *mapped = (volatile uint32_t *)virtual_addr;

    mapped[0] = 0x11223344u;
    mapped[1] = 0x55667788u;

    if (mapped[0] != 0x11223344u || mapped[1] != 0x55667788u) {
        kernel_panic("VMM test mapped read/write failed");
    }

    rc = vmm_unmap_page(virtual_addr);

    if (rc != VMM_OK) {
        kernel_panic("VMM test failed to unmap page");
    }

    pmm_free_page(physical);

    console_writeln("VMM test: map/translate/write/unmap sanity check passed");
}
```

---

# 9. Why page tables are still allocated below 16 MiB

This is subtle and important.

A page table is itself just a physical page containing 1024 page-table entries.

The CPU reads it through its physical address from the page directory.

But the kernel must also write into it as normal memory.

At this stage, we have not mapped all physical memory into a kernel virtual window. Therefore, if the PMM gives us a page table at physical address:

```text
0x04000000
```

then the CPU could use it as a page table, but the kernel could not safely write:

```c
memset((void *)0x04000000, 0, 4096);
```

because `0x04000000` is not currently identity-mapped.

So for now we allocate page-table pages below:

```text
0x01000000
```

This is a bootstrap compromise.

Later, we will create a physical-memory window or recursive page-directory mapping so the kernel can manipulate page tables more elegantly.

---

# 10. Why mapped data pages can come from anywhere

The page table itself must be writable by the kernel during setup.

But the physical page being mapped does not need to be identity-mapped.

For example:

```text
physical page: 0x04000000
virtual page:  0xD0000000
```

Once the VMM installs the page-table entry, the kernel can access the page through:

```c
(volatile uint32_t *)0xD0000000
```

That is the whole point of virtual memory.

---

# 11. Update `kernel/kmain.c`

Add:

```c
#include "arch/x86/vmm.h"
```

Then initialize and test the VMM after paging is enabled and before the heap test.

Here is the full updated file.

```c
// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/multiboot.h"
#include "arch/x86/paging.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "arch/x86/vmm.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/heap.h"
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

    paging_init();
    paging_test_identity_mapping();

    vmm_init();
    vmm_test_once();

    heap_init(4);
    heap_test_once();

#ifdef TOYIX_TRIGGER_PAGE_FAULT
    console_writeln("Triggering test page fault at 0xC0000000...");
    volatile uint32_t *bad = (volatile uint32_t *)0xC0000000u;
    uint32_t value = *bad;
    (void)value;
#endif

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
    console_writeln("Next stop: move heap growth onto VMM-mapped virtual pages.");

    kernel_idle_forever();
}
```

---

# 12. Update `Makefile`

Add:

```text
build/arch/x86/vmm.o
```

to the object list.

The relevant `OBJS` section becomes:

```make
OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the `test` target with VMM checks:

```make
	grep -q "VMM: initialized kernel address-space mapper" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
```

The key part of the test target should now include:

```make
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
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
	grep -q "Paging test: identity-mapped kernel data is readable/writable" build/test.log
	grep -q "VMM: initialized kernel address-space mapper" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	grep -q "Heap: initialized with 4 page(s)" build/test.log
	grep -q "Heap test: allocation/free sanity check passed" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, IRQ, PMM, paging, VMM, and heap smoke test passed."
```

---

# 13. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 7 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 14. Expected output

A normal boot should now include:

```text
Paging: enabled with identity map of first 16 MiB
Paging test: identity-mapped kernel data is readable/writable
VMM: initialized kernel address-space mapper
VMM test: starting
VMM test: map/translate/write/unmap sanity check passed
Heap: initialized with 4 page(s)
Heap test: allocation/free sanity check passed
Interrupts: enabled
Timer: observed 3 ticks
```

The important new line is:

```text
VMM test: map/translate/write/unmap sanity check passed
```

That proves the kernel can:

```text
allocate a physical page
map it at a high virtual address
translate the virtual address back to physical
write through the virtual address
unmap the page
free the physical page
```

That is a real virtual memory operation.

---

# 15. Common failures

## Failure: page fault inside `vmm_map_page`

Likely cause:

```text
new page table allocated above the identity-mapped region
```

Make sure page tables are allocated with:

```c
pmm_alloc_page_below(VMM_BOOTSTRAP_TABLE_LIMIT)
```

not:

```c
pmm_alloc_page()
```

The physical page being mapped can come from anywhere. The page table page itself must be writable by the kernel during setup.

## Failure: `VMM test translation mismatch`

Check:

```c
return (uintptr_t)((pte & X86_PAGE_FRAME_MASK) + offset);
```

and make sure the test address is page-aligned:

```c
uintptr_t virtual_addr = 0xD0000000u;
```

If you test with a non-page-aligned virtual address, `vmm_map_page()` should reject it.

## Failure: second run of `vmm_test_once()` says already mapped

The test unmaps the page before returning:

```c
vmm_unmap_page(virtual_addr);
```

If that failed silently, the second run would see the mapping still present.

Check:

```c
table[tab] = 0;
paging_invalidate_page(virtual_addr);
```

## Failure: unmapping then reading still works

That indicates stale TLB state.

Make sure `vmm_unmap_page()` calls:

```c
paging_invalidate_page(virtual_addr);
```

If in doubt during early development, use the heavier but simpler:

```c
paging_reload_cr3();
```

after unmapping. It is slower, but correctness matters first.

---

# 16. What this chapter gives us

We now have this stack:

```text
Multiboot memory map
  ↓
PMM: physical page allocation
  ↓
paging: page directory and page tables active
  ↓
VMM: map/unmap/translate virtual pages
  ↓
heap: still early, but ready to be moved onto VMM
```

The VMM is the missing bridge between raw physical memory and comfortable kernel services.

Before this chapter:

```text
a physical page was only useful if already identity-mapped
```

After this chapter:

```text
a physical page can be mapped wherever the kernel chooses
```

That is the key idea.

---

# 17. Important design limitations

This VMM still has several deliberate limitations.

It only manages the kernel’s current page directory:

```text
no per-process address spaces yet
```

It allocates page-table pages below 16 MiB:

```text
bootstrap compromise
```

It does not free empty page tables when the last page in a table is unmapped:

```text
acceptable for now
```

It does not track ownership of mapped physical pages:

```text
caller must free physical pages responsibly
```

It does not support copy-on-write, guard pages, user pages, or memory-mapped files yet.

That is fine. We are building in controlled layers.

---

# 18. Commit this chapter

After tests pass:

```bash
git status
git add .
git commit -m "Add virtual memory map and unmap operations"
```

---

# 19. Next chapter

Now we can refactor the heap properly.

The next chapter should move from:

```text
heap grows only from low identity-mapped contiguous physical pages
```

to:

```text
heap owns a high virtual address range
heap asks PMM for physical pages
heap maps them with VMM
physical pages no longer need to be contiguous
```

The new design will look like:

```text
kernel heap virtual base: 0xD1000000
heap virtual limit:       0xD2000000

kmalloc needs more memory
  ↓
PMM allocates any physical page
  ↓
VMM maps it into next heap virtual page
  ↓
heap free list receives new virtual block
```

That will remove the biggest weakness in the current heap.

---

# Resources

- [Chapter 07 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_07)
- [Intel 64 and IA-32 Software Developer Manuals][1]
- [OSDev Wiki: Paging](https://wiki.osdev.org/Paging)
- [OSDev Wiki: Page Tables](https://wiki.osdev.org/Page_Tables)
- [OSDev Wiki: Memory Management](https://wiki.osdev.org/Memory_Management)
- [OSDev Wiki: Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation)

That completes the seventh Toyix milestone: adding a real virtual memory mapping layer on top of paging and the physical page allocator.

Happy Coding!

[1]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel® 64 and IA-32 Architectures"
