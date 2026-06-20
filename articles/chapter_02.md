# Chapter 2 — GDT, IDT, and Surviving Your First Kernel Crash

In Chapter 1, we got control of the machine:

```text
GRUB → boot.asm → kernel_main() → serial/VGA console
```

Now we need something more important than printing text: we need the CPU to call **our code** when something goes wrong.

Right now, if the kernel executes an invalid instruction, divides by zero, touches unmapped memory later, or faults during setup, the machine may reset, hang, or triple-fault. That gives us almost no evidence.

This chapter adds:

```text
GDT  — Global Descriptor Table
IDT  — Interrupt Descriptor Table
ISR  — Interrupt Service Routine stubs
panic path
exception reporting
deliberate exception test
```

The goal is not yet hardware IRQs, keyboard input, or timer interrupts. The goal is simpler and more foundational:

> When the CPU raises an exception, our kernel should catch it, print useful information, and halt cleanly.

The Intel IA-32 manuals describe descriptor tables, protected-mode memory management, and interrupt/exception handling as part of the architecture’s operating-system support environment. Intel’s current manual page is the authoritative source for the architecture; OSDev’s GDT and IDT pages are useful working references for hobby OS implementation details. ([Intel][1])

---

# 1. Why we need a GDT

The **GDT**, or Global Descriptor Table, tells the CPU what memory segments exist and what permissions they have.

In old x86 programming, segmentation could be used heavily. In a modern flat 32-bit hobby kernel, we usually set up a simple model:

```text
selector 0x00 → null segment
selector 0x08 → kernel code segment
selector 0x10 → kernel data segment
```

Both code and data cover the whole 32-bit address space. This is called a **flat memory model**.

That may sound pointless, but it is not optional. Protected-mode x86 still uses segment selectors in `CS`, `DS`, `SS`, and the other segment registers. Even if we later rely mostly on paging for real memory protection, the CPU still needs valid segment descriptors. OSDev’s GDT reference explains descriptor fields such as base, limit, access byte, size flag, and granularity flag. ([OSDev Wiki][2])

For now:

```text
code segment:
    base  = 0
    limit = 4 GiB-ish
    ring  = 0
    type  = execute/read

data segment:
    base  = 0
    limit = 4 GiB-ish
    ring  = 0
    type  = read/write
```

This is not yet user/kernel isolation. That comes later when we add ring 3 and paging.

---

# 2. Why we need an IDT

The **IDT**, or Interrupt Descriptor Table, tells the CPU what function to call for each interrupt or exception vector.

When the CPU raises an exception, it indexes into the IDT using the vector number. OSDev’s interrupt tutorial summarizes this relationship directly: when an interrupt fires, the CPU uses the vector as an index into the IDT and reads the entry to decide what handler to call. ([OSDev Wiki][3])

Some important CPU exception vectors are:

| Vector | Exception                |
| -----: | ------------------------ |
|      0 | Divide Error             |
|      3 | Breakpoint               |
|      6 | Invalid Opcode           |
|      8 | Double Fault             |
|     13 | General Protection Fault |
|     14 | Page Fault               |
|     17 | Alignment Check          |

In this chapter we will install handlers for vectors `0` through `31`.

Later, hardware IRQs will usually be mapped starting at vector `32` after remapping the legacy PIC. We are not doing that yet.

---

# 3. Patch overview

Starting from the Chapter 1 tree, add these files:

```text
arch/x86/
├── gdt.c
├── gdt.h
├── gdt_flush.asm
├── idt.c
├── idt.h
├── interrupts.c
├── interrupts.h
└── isr.asm

include/kernel/
└── panic.h

kernel/
└── panic.c
```

Modify:

```text
kernel/kmain.c
Makefile
tests/smoke.sh
```

After this chapter, our boot path becomes:

```text
GRUB
  ↓
boot.asm
  ↓
kernel_main()
  ↓
console init
  ↓
GDT install
  ↓
IDT install
  ↓
exception-safe kernel execution
```

---

# 4. Add `include/kernel/panic.h`

```c
// include/kernel/panic.h
#ifndef TOYIX_KERNEL_PANIC_H
#define TOYIX_KERNEL_PANIC_H

void kernel_halt(void) __attribute__((noreturn));
void kernel_panic(const char *message) __attribute__((noreturn));

#endif
```

## Why this exists

A kernel needs a final emergency path.

In user-space, a crashing program can return to the operating system. In kernel-space, there is nothing beneath us to return to. If we detect an unrecoverable condition, the safest early behavior is:

```text
print diagnostic
disable interrupts
halt forever
```

Later, `kernel_panic()` can grow into a richer crash screen that prints register dumps, stack traces, CPU number, process name, and loaded module list.

For now, it is intentionally simple.

---

# 5. Add `kernel/panic.c`

```c
// kernel/panic.c
#include "kernel/console.h"
#include "kernel/panic.h"

void kernel_halt(void) {
    __asm__ volatile ("cli");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_panic(const char *message) {
    console_writeln("");
    console_writeln("*** KERNEL PANIC ***");

    if (message != 0) {
        console_write("Reason: ");
        console_writeln(message);
    }

    console_writeln("System halted.");
    kernel_halt();
}
```

## What `cli` and `hlt` do

`cli` clears the interrupt flag, disabling maskable hardware interrupts.

`hlt` tells the CPU to stop executing instructions until the next external interrupt. Since we have disabled interrupts, this becomes a stable low-power halt loop in an emulator.

This is much better than falling off the end of a kernel function and executing random memory.

---

# 6. Add `arch/x86/gdt.h`

```c
// arch/x86/gdt.h
#ifndef TOYIX_ARCH_X86_GDT_H
#define TOYIX_ARCH_X86_GDT_H

#include <stdint.h>

#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_KERNEL_DATA_SELECTOR 0x10u

void gdt_init(void);

#endif
```

## Why selectors are constants

A segment selector is not a pointer. It is an index-like value loaded into segment registers.

Our GDT layout will be:

```text
GDT entry 0 → null descriptor
GDT entry 1 → kernel code descriptor
GDT entry 2 → kernel data descriptor
```

Each GDT entry is 8 bytes, so:

```text
entry 1 selector = 1 * 8 = 0x08
entry 2 selector = 2 * 8 = 0x10
```

These constants are used by the GDT loader and by the IDT, because interrupt gates need to know which code segment selector to use.

---

# 7. Add `arch/x86/gdt.c`

```c
// arch/x86/gdt.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/console.h"

typedef struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct gdt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_pointer_t;

static gdt_entry_t gdt[3];
static gdt_pointer_t gdt_ptr;

extern void gdt_flush(uint32_t gdt_pointer_addr);

static void gdt_set_entry(
    int index,
    uint32_t base,
    uint32_t limit,
    uint8_t access,
    uint8_t granularity
) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFFu);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFFu);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFFu);

    gdt[index].limit_low = (uint16_t)(limit & 0xFFFFu);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0Fu);

    gdt[index].granularity |= (uint8_t)(granularity & 0xF0u);
    gdt[index].access = access;
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base = (uint32_t)&gdt[0];

    /*
     * Entry 0: mandatory null descriptor.
     *
     * A null selector is invalid. Keeping entry 0 empty helps catch bugs
     * where a segment register accidentally receives selector 0.
     */
    gdt_set_entry(0, 0, 0, 0, 0);

    /*
     * Entry 1: kernel code segment.
     *
     * base  = 0
     * limit = 0xFFFFF with 4 KiB granularity, giving a flat 4 GiB range
     *
     * access 0x9A:
     *   present
     *   ring 0
     *   code segment
     *   executable
     *   readable
     *
     * granularity 0xCF:
     *   high limit bits = 0xF
     *   4 KiB granularity
     *   32-bit protected-mode segment
     */
    gdt_set_entry(1, 0, 0xFFFFFu, 0x9Au, 0xCFu);

    /*
     * Entry 2: kernel data segment.
     *
     * access 0x92:
     *   present
     *   ring 0
     *   data segment
     *   writable
     */
    gdt_set_entry(2, 0, 0xFFFFFu, 0x92u, 0xCFu);

    gdt_flush((uint32_t)&gdt_ptr);

    console_writeln("GDT: installed flat kernel code/data segments");
}
```

## What the packed structs are doing

The CPU expects the GDT pointer and GDT entries to have exact binary layouts.

C compilers are allowed to insert padding inside structs unless told not to. That would be fatal here.

So we use:

```c
__attribute__((packed))
```

That tells GCC not to add padding bytes.

This is one of the places where kernel C is not “normal application C.” We are not merely expressing abstract data structures. We are constructing binary tables consumed directly by the CPU.

---

# 8. Add `arch/x86/gdt_flush.asm`

```asm
; arch/x86/gdt_flush.asm
;
; Loads the GDT and reloads all segment registers.
;
; C prototype:
;   void gdt_flush(uint32_t gdt_pointer_addr);

BITS 32

global gdt_flush

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    ; 0x10 is our kernel data selector.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reloading CS requires a far jump.
    ; 0x08 is our kernel code selector.
    jmp 0x08:.reload_cs

.reload_cs:
    ret
```

## Why this cannot be done cleanly in C

C has no standard syntax for:

```text
lgdt
load segment registers
perform far jump to reload CS
```

So this belongs in assembly.

The important detail is that loading a new GDT is not enough. The CPU has hidden cached descriptor state associated with segment registers. We reload the segment registers after `lgdt` so the CPU uses the descriptors we just installed.

Reloading `CS` is special. You cannot simply write:

```asm
mov cs, ax
```

Instead, we use a far jump:

```asm
jmp 0x08:.reload_cs
```

That loads `CS` with selector `0x08` and resumes execution at `.reload_cs`.

---

# 9. Add `arch/x86/idt.h`

```c
// arch/x86/idt.h
#ifndef TOYIX_ARCH_X86_IDT_H
#define TOYIX_ARCH_X86_IDT_H

void idt_init(void);

#endif
```

The IDT setup has one public job:

```c
idt_init();
```

The rest should stay private to the x86 architecture layer.

---

# 10. Add `arch/x86/interrupts.h`

```c
// arch/x86/interrupts.h
#ifndef TOYIX_ARCH_X86_INTERRUPTS_H
#define TOYIX_ARCH_X86_INTERRUPTS_H

#include <stdint.h>

typedef struct interrupt_frame {
    uint32_t ds;

    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t original_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;

    uint32_t interrupt_number;
    uint32_t error_code;

    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} interrupt_frame_t;

void isr_handler(interrupt_frame_t *frame);

#endif
```

## Why this struct must match assembly

The CPU pushes part of the interrupt frame automatically.

Our assembly ISR stub pushes additional values.

Then it passes a pointer to C.

Therefore, this C struct is a treaty between assembly and C. If the order is wrong, the handler will print nonsense or crash.

The layout we will create is:

```text
ds
edi
esi
ebp
original_esp
ebx
edx
ecx
eax
interrupt_number
error_code
eip
cs
eflags
```

Later, when we support user mode, the CPU may also push user `ESP` and `SS` during ring transitions. We are not handling that yet.

---

# 11. Add `arch/x86/isr.asm`

```asm
; arch/x86/isr.asm
;
; Exception stubs for CPU vectors 0..31.
;
; Some CPU exceptions push an error code automatically.
; Others do not.
;
; To make the C handler simple, every stub creates the same shape:
;
;   interrupt_number
;   error_code
;   CPU-pushed return frame
;
; Then isr_common_stub saves general registers and calls isr_handler().

BITS 32

extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common_stub
%endmacro

ISR_NOERR 0     ; Divide Error
ISR_NOERR 1     ; Debug
ISR_NOERR 2     ; Non-Maskable Interrupt
ISR_NOERR 3     ; Breakpoint
ISR_NOERR 4     ; Overflow
ISR_NOERR 5     ; Bound Range Exceeded
ISR_NOERR 6     ; Invalid Opcode
ISR_NOERR 7     ; Device Not Available
ISR_ERR   8     ; Double Fault
ISR_NOERR 9     ; Coprocessor Segment Overrun / reserved on newer CPUs
ISR_ERR   10    ; Invalid TSS
ISR_ERR   11    ; Segment Not Present
ISR_ERR   12    ; Stack-Segment Fault
ISR_ERR   13    ; General Protection Fault
ISR_ERR   14    ; Page Fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; x87 Floating-Point Exception
ISR_ERR   17    ; Alignment Check
ISR_NOERR 18    ; Machine Check
ISR_NOERR 19    ; SIMD Floating-Point Exception
ISR_NOERR 20    ; Virtualization Exception or reserved, depending on CPU
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

global isr_common_stub
isr_common_stub:
    ; Save general-purpose registers.
    pusha

    ; Save current data segment selector.
    xor eax, eax
    mov ax, ds
    push eax

    ; Load kernel data selector into data segment registers.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Pass pointer to interrupt_frame_t.
    push esp
    call isr_handler
    add esp, 4

    ; Restore original data segment selector.
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Restore general-purpose registers.
    popa

    ; Drop interrupt_number and error_code.
    add esp, 8

    iretd
```

## Why some stubs are different

Some exceptions push an error code. For example:

```text
#GP — General Protection Fault
#PF — Page Fault
#DF — Double Fault
```

Others do not:

```text
#DE — Divide Error
#BP — Breakpoint
#UD — Invalid Opcode
```

We want the C handler to receive one uniform frame. So for exceptions without a CPU-provided error code, we push a fake zero error code.

For exceptions with a CPU-provided error code, we only push the interrupt number.

That way, by the time we reach `isr_common_stub`, the stack always contains:

```text
interrupt_number
error_code
eip
cs
eflags
```

Then `pusha` and the segment-save code add the rest.

---

# 12. Add `arch/x86/idt.c`

```c
// arch/x86/idt.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "kernel/console.h"

typedef struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  always_zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed)) idt_entry_t;

typedef struct idt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_pointer_t;

static idt_entry_t idt[256];
static idt_pointer_t idt_ptr;

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

static void idt_load(uint32_t idt_pointer_addr) {
    __asm__ volatile ("lidt (%0)" : : "r"(idt_pointer_addr));
}

static void idt_set_gate(
    uint8_t vector,
    uint32_t handler_addr,
    uint16_t selector,
    uint8_t flags
) {
    idt[vector].base_low = (uint16_t)(handler_addr & 0xFFFFu);
    idt[vector].selector = selector;
    idt[vector].always_zero = 0;
    idt[vector].flags = flags;
    idt[vector].base_high = (uint16_t)((handler_addr >> 16) & 0xFFFFu);
}

void idt_init(void) {
    void (*exception_stubs[32])(void) = {
        isr0,  isr1,  isr2,  isr3,
        isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11,
        isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19,
        isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27,
        isr28, isr29, isr30, isr31
    };

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base = (uint32_t)&idt[0];

    for (uint16_t i = 0; i < 256; ++i) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    /*
     * 0x8E means:
     *   present
     *   descriptor privilege level 0
     *   32-bit interrupt gate
     */
    for (uint8_t i = 0; i < 32; ++i) {
        idt_set_gate(
            i,
            (uint32_t)exception_stubs[i],
            X86_KERNEL_CODE_SELECTOR,
            0x8Eu
        );
    }

    idt_load((uint32_t)&idt_ptr);

    console_writeln("IDT: installed CPU exception handlers");
}
```

## Why IDT entries point to assembly, not directly to C

The CPU does not call C functions.

It transfers control to an address with a very specific stack layout. A normal C function expects to be called by another C function using the compiler’s ABI.

So each IDT entry points to an assembly stub. The assembly stub:

1. Normalizes the error-code layout.
2. Saves registers.
3. Loads known-good kernel data segments.
4. Calls the C handler.
5. Restores state.
6. Executes `iretd`.

The C handler gets a clean struct and can do readable diagnostic work.

---

# 13. Add `arch/x86/interrupts.c`

```c
// arch/x86/interrupts.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "kernel/console.h"
#include "kernel/panic.h"

static const char *const exception_names[32] = {
    "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

static void print_register(const char *name, uint32_t value) {
    console_write(name);
    console_write("=");
    console_write_hex32(value);
    console_putc(' ');
}

void isr_handler(interrupt_frame_t *frame) {
    console_writeln("");
    console_writeln("*** CPU EXCEPTION ***");

    console_write("Vector: ");
    console_write_hex32(frame->interrupt_number);

    if (frame->interrupt_number < 32) {
        console_write(" (");
        console_write(exception_names[frame->interrupt_number]);
        console_write(")");
    }

    console_putc('\n');

    console_write("Error code: ");
    console_write_hex32(frame->error_code);
    console_putc('\n');

    print_register("EAX", frame->eax);
    print_register("EBX", frame->ebx);
    print_register("ECX", frame->ecx);
    print_register("EDX", frame->edx);
    console_putc('\n');

    print_register("ESI", frame->esi);
    print_register("EDI", frame->edi);
    print_register("EBP", frame->ebp);
    console_putc('\n');

    print_register("EIP", frame->eip);
    print_register("CS", frame->cs);
    print_register("EFLAGS", frame->eflags);
    console_putc('\n');

    if (frame->interrupt_number == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        console_write("Page fault address CR2=");
        console_write_hex32(cr2);
        console_putc('\n');
    }

    kernel_panic("unhandled CPU exception");
}
```

## What this gives us

If the kernel executes an illegal instruction, the handler prints something like:

```text
*** CPU EXCEPTION ***
Vector: 0x00000006 (Invalid Opcode)
Error code: 0x00000000
EAX=0x00000000 EBX=0x0012A000 ECX=0x00000000 EDX=0x00000000
ESI=0x00000000 EDI=0x00000000 EBP=0x00103FF0
EIP=0x00100ABC CS=0x00000008 EFLAGS=0x00000202

*** KERNEL PANIC ***
Reason: unhandled CPU exception
System halted.
```

That is a major debugging improvement.

Later, we will route exceptions through a more general trap system:

```text
CPU exception
  ↓
trap frame
  ↓
kernel trap dispatcher
  ↓
page fault handler / debugger / panic / signal delivery
```

But this chapter’s handler is enough to stop silent death.

---

# 14. Replace `kernel/kmain.c`

Replace your Chapter 1 `kernel/kmain.c` with this:

```c
// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "kernel/console.h"
#include "kernel/panic.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

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
    }

    console_write("Multiboot info at ");
    console_write_hex32(multiboot_info_addr);
    console_putc('\n');

    gdt_init();
    idt_init();

    console_writeln("Descriptor tables: ready");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    console_writeln("Kernel survived early CPU setup.");
    console_writeln("Next stop: PIC remap, timer IRQ, keyboard IRQ.");

    kernel_halt();
}
```

## What changed

The kernel now does this:

```c
gdt_init();
idt_init();
```

Only after console initialization.

That order matters. If GDT or IDT setup fails, we want the ability to print how far we got.

The optional block:

```c
#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    __asm__ volatile ("ud2");
#endif
```

is a deliberate crash test.

`UD2` is an x86 instruction specifically intended to raise an invalid-opcode exception. That gives us a clean way to verify that vector `6` reaches our handler.

We do not enable it in normal builds. We enable it only in the exception test target.

---

# 15. Update `Makefile`

Replace the Chapter 1 Makefile with this version, or carefully merge the changes.

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
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/panic.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o

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
	grep -q "GDT: installed flat kernel code/data segments" build/test.log
	grep -q "IDT: installed CPU exception handlers" build/test.log
	grep -q "Kernel survived early CPU setup." build/test.log
	@echo "Boot smoke test passed."

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

## What changed in the build

We added:

```make
CFLAGS_EXTRA
```

That lets us compile special test builds without editing source files.

This target:

```make
test-exception
```

builds a kernel with:

```text
-DTOYIX_TRIGGER_TEST_EXCEPTION
```

Then the kernel deliberately executes `UD2`, the invalid-opcode test instruction.

That should prove:

```text
IDT installed
vector 6 reached our ISR stub
ISR stub reached C
C handler printed diagnostics
panic path halted cleanly
```

This is a real kernel test, not merely a “did it compile?” check.

---

# 16. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception

echo "All Chapter 2 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 17. Build and run manually

Normal boot:

```bash
make clean
make run
```

You should see something like:

```text
Toyix kernel alive
Boot protocol: Multiboot OK
Multiboot info at 0xXXXXXXXX
GDT: installed flat kernel code/data segments
IDT: installed CPU exception handlers
Descriptor tables: ready
Kernel survived early CPU setup.
Next stop: PIC remap, timer IRQ, keyboard IRQ.
```

Exception test:

```bash
make test-exception
```

Expected result:

```text
Exception handling test passed.
```

You can inspect the captured output:

```bash
cat build/exception.log
```

---

# 18. Debugging common failures

## Failure: QEMU resets immediately

This usually means a triple fault.

Likely causes:

```text
bad GDT pointer
bad IDT pointer
wrong selector
bad far jump
bad ISR stack cleanup
missing packed attribute
```

Start by checking that these are exact:

```c
#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_KERNEL_DATA_SELECTOR 0x10u
```

and that `gdt_flush.asm` uses:

```asm
mov ax, 0x10
jmp 0x08:.reload_cs
```

## Failure: exception test hangs but prints nothing

Likely causes:

```text
IDT not loaded
IDT entry has wrong handler address
IDT entry selector is wrong
serial output broke
QEMU serial output not enabled
```

Run with:

```bash
make run
```

and see whether normal console output appears.

## Failure: invalid opcode reports the wrong vector

Likely cause:

```text
stack layout mismatch between isr.asm and interrupt_frame_t
```

The order of pushes in assembly must match the C struct exactly.

The most important part is this:

```asm
push dword 0
push dword %1
```

for no-error-code exceptions, and this:

```asm
push dword %1
```

for error-code exceptions.

Then the common stub does:

```asm
pusha
push eax   ; saved DS
push esp
call isr_handler
```

That is why the C struct begins with:

```c
uint32_t ds;
uint32_t edi;
uint32_t esi;
...
```

## Failure: linker complains about missing ISR symbols

Check that `arch/x86/isr.asm` is in `OBJS`:

```make
build/arch/x86/isr.o
```

and that your generic assembly rule exists:

```make
build/%.o: %.asm
```

---

# 19. Why this chapter matters structurally

We now have the first pieces of a serious kernel architecture:

```text
arch/x86/gdt.c          CPU segmentation setup
arch/x86/idt.c          CPU interrupt table setup
arch/x86/isr.asm        low-level interrupt entry code
arch/x86/interrupts.c   high-level exception handling
kernel/panic.c          fatal kernel stop path
```

That split is intentional.

The assembly file handles what C cannot express cleanly.

The architecture layer handles x86-specific tables.

The kernel layer handles generic panic behavior.

This is the design style we want long-term:

```text
hardware-specific mechanism below
portable kernel policy above
```

Later, on another architecture, we would not have an x86 GDT at all. But we would still have a kernel panic path, a console layer, and a trap/exception abstraction.

---

# 20. Commit the working chapter

Once both tests pass:

```bash
git status
git add .
git commit -m "Install GDT and IDT with CPU exception handling"
```

A good kernel history should look like a lab notebook. Each commit should represent one tested step.

---

# 21. What comes next

The next chapter should add the first real hardware interrupts:

```text
PIC remapping
IRQ stubs
PIT timer interrupt
keyboard interrupt
interrupt enable/disable discipline
```

That will move us from:

```text
exceptions only
```

to:

```text
exceptions + hardware IRQs
```

Once we have timer interrupts, we can build toward scheduling. Once we have keyboard input, we can build toward a primitive kernel monitor.

[1]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html?utm_source=chatgpt.com "Manuals for Intel® 64 and IA-32 Architectures"
[2]: https://wiki.osdev.org/Global_Descriptor_Table?utm_source=chatgpt.com "Global Descriptor Table"
[3]: https://wiki.osdev.org/Interrupts_Tutorial?utm_source=chatgpt.com "Interrupts Tutorial"

