# Chapter 5 — Turning On Paging

In Chapter 4, we built the first memory-management layer:

```text
Multiboot memory map
  ↓
physical page allocator
  ↓
pmm_alloc_page()
pmm_free_page()
```

Now we add the next layer: **paging**.

Paging lets the CPU translate a **virtual address** into a **physical address** through page tables. In 32-bit x86 paging without PAE, a virtual address is split into a page-directory index, page-table index, and page offset; page directories and page tables each contain 1024 entries, and the normal base page size is 4 KiB. OSDev’s paging and page-table references summarize this structure, while Intel’s IA-32 manuals are the architectural authority for CR0, CR3, page translation, exceptions, and paging control bits. ([OSDev Wiki][1])

This chapter will use **identity paging** first:

```text
virtual address 0x00100000 → physical address 0x00100000
virtual address 0x000B8000 → physical address 0x000B8000
virtual address 0x00300000 → physical address 0x00300000
```

Identity paging is the safest first paging step because the kernel can keep using the same addresses immediately before and after paging is enabled. OSDev specifically notes that identity mapping is useful when switching into paged mode because the switch code and important data remain reachable regardless of whether paging is currently enabled. ([OSDev Wiki][2])

---

# 1. What this chapter adds

We will add:

```text
arch/x86/
├── paging_asm.asm
├── paging.c
└── paging.h
```

We will modify:

```text
kernel/kmain.c
Makefile
```

After this chapter, the boot path becomes:

```text
GRUB
  ↓
boot.asm
  ↓
kernel_main()
  ↓
console
  ↓
GDT
  ↓
IDT
  ↓
PIC
  ↓
PMM
  ↓
paging_init()
  ↓
load CR3
  ↓
set CR0.PG
  ↓
kernel continues with paging enabled
```

We will also add a deliberate page-fault test target.

That matters because paging code that merely “does not crash” is not enough. We want evidence that:

```text
paging turns on
identity-mapped kernel memory still works
page faults reach our handler
page-fault diagnostics are useful
```

---

# 2. What paging gives us

Right now, without paging, addresses are effectively physical addresses.

If the kernel writes to:

```c
*(uint32_t *)0x00100000 = 123;
```

it writes to physical address `0x00100000`.

With paging enabled, the CPU treats `0x00100000` as a **virtual** address. It consults the page tables to discover which physical frame that virtual page maps to.

That indirection gives us the future ability to do things like:

```text
same physical page mapped at multiple virtual addresses
user programs isolated from the kernel
guard pages around stacks
copy-on-write memory
memory-mapped files
per-process address spaces
kernel heap expansion
```

For now, we are not using those powers yet. We are simply turning paging on safely.

---

# 3. The 32-bit paging structure

In ordinary 32-bit paging without PAE:

```text
virtual address bits 31..22 → page directory index
virtual address bits 21..12 → page table index
virtual address bits 11..0  → offset inside 4 KiB page
```

So the address:

```text
0x00123456
```

is split like this:

```text
directory index = 0
table index     = 0x123
offset          = 0x456
```

A page directory has 1024 entries.

Each page table has 1024 entries.

Each page table entry maps one 4 KiB page.

Therefore, one page table maps:

```text
1024 * 4096 = 4 MiB
```

If we identity-map the first 16 MiB, we need:

```text
16 MiB / 4 MiB = 4 page tables
```

That is what this chapter will do.

---

# 4. Why we are not building a higher-half kernel yet

Many hobby kernels eventually map themselves into the high virtual address range, commonly something like:

```text
0xC0000000 and above
```

That is called a **higher-half kernel**.

We are not doing that yet.

A higher-half kernel is valuable, but it makes the first paging transition more complex. You need linker-script changes, virtual-versus-physical address conversion, bootstrap mappings, and careful transition code.

This chapter’s goal is narrower:

```text
turn on paging
keep all current addresses valid
prove page faults work
then commit a clean checkpoint
```

We will move to a higher-half kernel later.

---

# 5. Add `arch/x86/paging.h`

```c
// arch/x86/paging.h
#ifndef TOYIX_ARCH_X86_PAGING_H
#define TOYIX_ARCH_X86_PAGING_H

#include <stdint.h>

#define X86_PAGE_SIZE 4096u

#define X86_PAGE_PRESENT  0x001u
#define X86_PAGE_WRITABLE 0x002u
#define X86_PAGE_USER     0x004u

void paging_init(void);
int paging_is_enabled(void);
void paging_test_identity_mapping(void);

#endif
```

## Why this interface is small

This is not our final virtual memory manager.

For now, the paging layer has only three public jobs:

```c
paging_init();
paging_is_enabled();
paging_test_identity_mapping();
```

Later, this will grow into a real virtual memory API:

```c
vmm_map_page();
vmm_unmap_page();
vmm_translate();
vmm_create_address_space();
vmm_switch_address_space();
```

But those functions need more design work. For this chapter, a simple identity map is enough.

---

# 6. Add `arch/x86/paging_asm.asm`

```asm
; arch/x86/paging_asm.asm
;
; Low-level paging control functions.
;
; C prototypes:
;   void paging_load_directory(uint32_t physical_addr);
;   void paging_enable_asm(void);
;   uint32_t paging_read_cr0(void);
;   uint32_t paging_read_cr2(void);
;   uint32_t paging_read_cr3(void);

BITS 32

global paging_load_directory
global paging_enable_asm
global paging_read_cr0
global paging_read_cr2
global paging_read_cr3

paging_load_directory:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

paging_enable_asm:
    mov eax, cr0
    or eax, 0x80000000       ; CR0.PG
    mov cr0, eax

    ; After enabling paging, execution continues at the next instruction.
    ; Because we are identity-mapped, this address is still valid.
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
```

## Why this is assembly

C has no standard way to read or write control registers.

Paging is controlled through CPU registers:

| Register | Use                                                  |
| -------- | ---------------------------------------------------- |
| `CR0`    | contains the paging-enable bit                       |
| `CR2`    | receives the faulting linear address on a page fault |
| `CR3`    | points to the current page directory                 |

The Intel manuals describe these control registers and the paging machinery as part of IA-32 system programming. ([Intel][3])

The important sequence is:

```asm
mov cr3, page_directory_physical_address
set CR0.PG
```

Because we use identity mapping, the instruction immediately after enabling paging is still reachable.

---

# 7. Add `arch/x86/paging.c`

```c
// arch/x86/paging.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/paging.h"
#include "kernel/console.h"
#include "kernel/panic.h"

#define PAGE_DIRECTORY_ENTRIES 1024u
#define PAGE_TABLE_ENTRIES     1024u

#define IDENTITY_MAP_MIB       16u
#define IDENTITY_TABLE_COUNT   (IDENTITY_MAP_MIB / 4u)

#define PAGE_FRAME_MASK        0xFFFFF000u

typedef uint32_t page_directory_entry_t;
typedef uint32_t page_table_entry_t;

static page_directory_entry_t page_directory[PAGE_DIRECTORY_ENTRIES]
    __attribute__((aligned(X86_PAGE_SIZE)));

static page_table_entry_t identity_tables[IDENTITY_TABLE_COUNT][PAGE_TABLE_ENTRIES]
    __attribute__((aligned(X86_PAGE_SIZE)));

static volatile uint32_t paging_test_word = 0x12345678u;

extern void paging_load_directory(uint32_t physical_addr);
extern void paging_enable_asm(void);
extern uint32_t paging_read_cr0(void);
extern uint32_t paging_read_cr2(void);
extern uint32_t paging_read_cr3(void);

static void zero_page_directory(void) {
    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; ++i) {
        page_directory[i] = 0;
    }
}

static void build_identity_tables(void) {
    for (uint32_t table = 0; table < IDENTITY_TABLE_COUNT; ++table) {
        for (uint32_t entry = 0; entry < PAGE_TABLE_ENTRIES; ++entry) {
            uint32_t physical_addr =
                ((table * PAGE_TABLE_ENTRIES) + entry) * X86_PAGE_SIZE;

            identity_tables[table][entry] =
                (physical_addr & PAGE_FRAME_MASK) |
                X86_PAGE_PRESENT |
                X86_PAGE_WRITABLE;
        }

        page_directory[table] =
            ((uint32_t)&identity_tables[table][0] & PAGE_FRAME_MASK) |
            X86_PAGE_PRESENT |
            X86_PAGE_WRITABLE;
    }
}

static void print_page_fault_error(uint32_t error_code) {
    console_write("Page fault error bits: ");

    if ((error_code & 0x01u) != 0) {
        console_write("present-protection ");
    } else {
        console_write("not-present ");
    }

    if ((error_code & 0x02u) != 0) {
        console_write("write ");
    } else {
        console_write("read ");
    }

    if ((error_code & 0x04u) != 0) {
        console_write("user ");
    } else {
        console_write("supervisor ");
    }

    if ((error_code & 0x08u) != 0) {
        console_write("reserved-bit ");
    }

    if ((error_code & 0x10u) != 0) {
        console_write("instruction-fetch ");
    }

    console_putc('\n');
}

static void page_fault_handler(interrupt_frame_t *frame) {
    uint32_t fault_addr = paging_read_cr2();

    console_writeln("");
    console_writeln("*** PAGE FAULT ***");

    console_write("Fault address CR2=");
    console_write_hex32(fault_addr);
    console_putc('\n');

    console_write("EIP=");
    console_write_hex32(frame->eip);
    console_write(" error=");
    console_write_hex32(frame->error_code);
    console_putc('\n');

    print_page_fault_error(frame->error_code);

    kernel_panic("page fault");
}

void paging_init(void) {
    console_writeln("Paging: building identity map");

    zero_page_directory();
    build_identity_tables();

    interrupt_register_handler(14, page_fault_handler);

    paging_load_directory((uint32_t)&page_directory[0]);
    paging_enable_asm();

    if (!paging_is_enabled()) {
        kernel_panic("paging enable bit did not stick");
    }

    console_write("Paging: enabled with identity map of first ");
    console_write_u32_dec(IDENTITY_MAP_MIB);
    console_writeln(" MiB");

    console_write("Paging: CR3=");
    console_write_hex32(paging_read_cr3());
    console_putc('\n');
}

int paging_is_enabled(void) {
    return (paging_read_cr0() & 0x80000000u) != 0;
}

void paging_test_identity_mapping(void) {
    uint32_t before = paging_test_word;

    paging_test_word = 0xCAFEBABEu;

    if (paging_test_word != 0xCAFEBABEu) {
        kernel_panic("paging identity test failed: write/read mismatch");
    }

    paging_test_word = before;

    console_writeln("Paging test: identity-mapped kernel data is readable/writable");
}
```

## Why the page tables are static

We could allocate page tables using `pmm_alloc_page()`.

But for the very first paging transition, static page tables are simpler and safer.

These arrays live inside the kernel image:

```c
static page_directory_entry_t page_directory[1024]
static page_table_entry_t identity_tables[4][1024]
```

Because Chapter 4’s PMM reserves the kernel image using:

```c
__kernel_start
__kernel_end
```

these page tables are automatically reserved and will not be handed out as free physical pages.

This is exactly the kind of simple dependency we want in early boot.

---

# 8. Why map the first 16 MiB?

The first 16 MiB includes:

```text
low legacy memory
VGA text memory at 0xB8000
kernel loaded at 1 MiB
kernel stack
GDT
IDT
static page directory and page tables
early boot data we still care about
```

For our current kernel, that is enough.

Could we map more? Yes.

Could we identity-map all 4 GiB? Also yes, but that would need 1024 page tables, or 4 MiB pages with PSE. We are avoiding both for now.

A small identity map gives us a clean first step. Later, we will replace it with a more intentional layout:

```text
0x00000000 - low guard/unmapped area
0x00100000 - kernel physical area
0xC0000000 - higher-half kernel virtual mapping
recursive page-table window
user process lower address space
```

But not yet.

---

# 9. Understanding the page-fault handler

Page faults are vector `14`.

Before enabling paging, we register:

```c
interrupt_register_handler(14, page_fault_handler);
```

This relies on `isr_handler()` dispatching registered exception handlers before it falls back to the generic CPU-exception dump:

```c
void isr_handler(interrupt_frame_t *frame) {
    if (interrupt_handlers[frame->interrupt_number] != NULL) {
        interrupt_handlers[frame->interrupt_number](frame);
        return;
    }

    /* generic exception reporting follows */
}
```

That matters because page faults are CPU exceptions, not hardware IRQs. Without this dispatch path, vector `14` would always use the generic exception handler and the paging-specific diagnostics would never run.

When a page fault occurs, the CPU stores the faulting linear address in `CR2`. Intel documents `CR2` as the page-fault linear-address register, and the page-fault exception includes an error code describing the access that faulted. ([Intel][3])

Our handler prints:

```text
faulting address
instruction pointer
error code
decoded error bits
```

For example:

```text
*** PAGE FAULT ***
Fault address CR2=0xC0000000
EIP=0x0010A123 error=0x00000000
Page fault error bits: not-present read supervisor
```

That tells us:

```text
the kernel tried to read virtual address 0xC0000000
the page was not present
the access came from supervisor/kernel mode
```

That is useful information.

---

# 10. Update `kernel/kmain.c`

Replace your current `kernel/kmain.c` with this version.

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
#include "drivers/input/keyboard.h"
#include "kernel/idle.h"
#include "kernel/console.h"
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
    console_writeln("Next stop: kernel heap on top of mapped pages.");

    kernel_idle();
}
```

## Why paging is initialized before timer and keyboard

Paging is a core CPU memory mode. Once it is enabled, every memory access goes through the translation tables.

It is better to turn it on before we initialize more drivers. That way, if later drivers depend on paging, they are born into the same memory model the kernel will keep using.

The order is now:

```text
GDT
IDT
PIC
PMM
Paging
PIT
Keyboard
Enable interrupts
```

That is a sensible early-kernel order.

---

# 11. Update `Makefile`

Add `arch/x86/paging.o` and `arch/x86/paging_asm.o`. In the current tree, the low-level assembly helper is named `paging_asm.asm`, so the object list includes:

```text
build/arch/x86/paging_asm.o
build/arch/x86/paging.o
```

Here is the full updated Makefile:

```make
# Makefile

SHELL := /bin/bash

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
    build/arch/x86/paging_asm.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/kernel/kmain.o \
    build/kernel/idle.o \
    build/kernel/console.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o

.PHONY: all clean iso run test test-exception test-page-fault

all: build/kernel.elf

build/arch/x86/paging.o: arch/x86/paging.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

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
	@mkdir -p build
	@rm -f build/test.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/test.log \
		-no-reboot \
		2>/dev/null || true
	grep -q "Toyix kernel alive" build/test.log
	grep -q "Boot protocol: Multiboot OK" build/test.log
	grep -q "PMM: parsing Multiboot memory map" build/test.log
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
	grep -q "Paging test: identity-mapped kernel data is readable/writable" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, IRQ, PMM, and paging smoke test passed."

test-exception:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_TEST_EXCEPTION
	@mkdir -p build
	@rm -f build/exception.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/exception.log \
		-no-reboot \
		2>/dev/null || true
	grep -q "Triggering test exception with UD2" build/exception.log
	grep -q "CPU EXCEPTION" build/exception.log
	grep -q "Invalid Opcode" build/exception.log
	grep -q "KERNEL PANIC" build/exception.log
	@echo "Exception handling test passed."

test-page-fault:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_PAGE_FAULT
	@mkdir -p build
	@rm -f build/pagefault.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/pagefault.log \
		-no-reboot \
		2>/dev/null || true
	grep -q "Paging: enabled with identity map of first 16 MiB" build/pagefault.log
	grep -q "Triggering test page fault at 0xC0000000" build/pagefault.log
	grep -q "PAGE FAULT" build/pagefault.log
	grep -q "Fault address CR2=0xC0000000" build/pagefault.log
	grep -q "KERNEL PANIC" build/pagefault.log
	@echo "Page fault test passed."

clean:
	rm -rf build
```

---

# 12. Current `tests/smoke.sh`

The current smoke script runs the normal boot test and the invalid-opcode exception test:

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception

echo "All smoke checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

Run the page-fault test directly when you want to validate paging diagnostics:

```bash
make test-page-fault
```

---

# 13. Expected normal boot output

Run:

```bash
make clean
make run
```

You should see something like:

```text
Toyix kernel alive
Boot protocol: Multiboot OK
Multiboot info at 0x0000XXXX
GDT: installed flat kernel code/data segments
IDT: installed CPU exception handlers
PIC: remapped IRQs to vectors 0x20-0x2F
PMM: parsing Multiboot memory map
...
PMM test: allocation/free sanity check passed
Paging: building identity map
Paging: enabled with identity map of first 16 MiB
Paging: CR3=0x001XXXXX
Paging test: identity-mapped kernel data is readable/writable
PIT: timer running at 0x00000064 Hz
Keyboard: IRQ1 handler installed
Interrupt hardware: configured
Interrupts: enabled
Timer: observed 3 ticks
Try typing in the QEMU window.
Next stop: kernel heap on top of mapped pages.
```

The exact CR3 address will vary.

The important lines are:

```text
Paging: enabled with identity map of first 16 MiB
Paging test: identity-mapped kernel data is readable/writable
Timer: observed 3 ticks
```

Those prove that paging enabled and the kernel continued running afterward.

---

# 14. Expected page-fault test output

Run:

```bash
make test-page-fault
```

Then inspect:

```bash
cat build/pagefault.log
```

You should see:

```text
Triggering test page fault at 0xC0000000...

*** PAGE FAULT ***
Fault address CR2=0xC0000000
EIP=0x001XXXXX error=0x00000000
Page fault error bits: not-present read supervisor

*** KERNEL PANIC ***
Reason: page fault
System halted.
```

That tells us the page-fault handler works.

Why does address `0xC0000000` fault?

Because this chapter maps only the first 16 MiB:

```text
mapped:
0x00000000 - 0x00FFFFFF

not mapped:
0x01000000 - 0xFFFFFFFF
```

So `0xC0000000` is intentionally unmapped.

---

# 15. Common failures

## Failure: instant reset after `Paging: building identity map`

This usually means a triple fault caused by a bad page-directory setup.

Check:

```c
page_directory[table] =
    ((uint32_t)&identity_tables[table][0] & PAGE_FRAME_MASK) |
    X86_PAGE_PRESENT |
    X86_PAGE_WRITABLE;
```

The page directory must point to the physical address of each page table.

Because we are still identity-mapped and loaded at physical 1 MiB, the C address is usable as the physical address at this stage.

Also verify that both arrays are 4 KiB aligned:

```c
__attribute__((aligned(X86_PAGE_SIZE)))
```

A page directory or page table that is not 4 KiB aligned will not work.

## Failure: page fault immediately after enabling paging

Likely causes:

```text
the current instruction address is not identity-mapped
the current stack is not identity-mapped
the GDT/IDT data is not identity-mapped
the VGA or serial driver data is not identity-mapped
```

For this tutorial kernel, mapping the first 16 MiB should cover those.

If your kernel image grows unusually large, temporarily increase:

```c
#define IDENTITY_MAP_MIB 16u
```

to:

```c
#define IDENTITY_MAP_MIB 32u
```

and update the table-count logic accordingly. Keep the value a multiple of 4 because each page table maps 4 MiB.

## Failure: `Fault address CR2` is not `0xC0000000`

If this happens during the deliberate page-fault test, the compiler may have optimized differently, or the fault may be happening earlier than the deliberate bad read.

Confirm this line still exists:

```c
volatile uint32_t *bad = (volatile uint32_t *)0xC0000000u;
```

The `volatile` matters because it forces an actual memory access.

## Failure: timer no longer ticks after paging

Likely causes:

```text
IDT not mapped
IRQ stubs not mapped
PIC/PIT driver state not mapped
stack not mapped
```

Again, the first 16 MiB identity map should cover the current kernel. If it does not, your kernel image or bootstrap stack may be outside that range.

---

# 16. What we have achieved

We now have this memory stack:

```text
Multiboot memory map
  ↓
physical memory manager
  ↓
static page directory and page tables
  ↓
paging enabled
  ↓
page fault handler
```

The kernel is still simple, but this is a major architectural milestone.

Before this chapter:

```text
addresses were physical
```

After this chapter:

```text
addresses are virtual, currently identity-mapped
```

That difference is foundational.

Even though identity mapping makes the addresses look unchanged, the CPU is now using the page-translation machinery. That means we can start introducing real virtual memory policy.

---

# 17. Important design limitation

This chapter uses one global page directory.

There are no processes yet, so that is fine.

Later, each process will need its own address space:

```text
kernel mappings shared
user mappings private per process
CR3 switched during context switch
```

That design will require a real VMM layer above this first paging layer.

Also, this chapter does not dynamically allocate page tables. It uses four static page tables. That is deliberate for the first paging step, but it is not enough for a general-purpose OS.

---

# 18. Commit this chapter

After the tests pass:

```bash
git status
git add .
git commit -m "Enable identity paging and add page fault diagnostics"
```

This is an important checkpoint. Do not proceed into heap allocation until paging tests pass.

---

# 19. Next chapter

The next chapter should build the first kernel heap:

```text
physical page allocator
  ↓
virtual memory mapping
  ↓
heap region
  ↓
kmalloc()
  ↓
kfree()
```

We will keep it simple at first:

```text
bump allocator for early heap
block headers
alignment support
free list
basic coalescing
```

The long-term goal is not merely “malloc in the kernel.” The goal is a swappable allocator interface:

```c
typedef struct kernel_allocator {
    void *(*alloc)(uint32_t size, uint32_t alignment);
    void (*free)(void *ptr);
} kernel_allocator_t;
```

That lets us start simple and later replace the allocator with something better without rewriting every subsystem that needs memory.

[1]: https://wiki.osdev.org/Paging "Paging"
[2]: https://wiki.osdev.org/Identity_Paging "Identity Paging"
[3]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel® 64 and IA-32 Architectures"

---

# Resources

- [Chapter 05 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_05)
- [OSDev Paging](https://wiki.osdev.org/Paging)
- [OSDev Identity Paging](https://wiki.osdev.org/Identity_Paging)
- [OSDev Page Tables](https://wiki.osdev.org/Page_Tables)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

That completes the fifth Toyix milestone: enabling identity paging and adding page-fault diagnostics.

Happy Coding!
