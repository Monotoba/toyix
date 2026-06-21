# Chapter 3 — Hardware Interrupts: PIC, PIT Timer, and Keyboard Input

In Chapter 2 we taught the CPU how to call our code when something goes wrong:

```text
CPU exception → ISR stub → C exception handler → panic
```

Now we will teach the machine how to call our code when **hardware wants attention**:

```text
timer chip → IRQ0 → PIC → CPU interrupt vector 32 → timer handler
keyboard controller → IRQ1 → PIC → CPU interrupt vector 33 → keyboard handler
```

This is the first step toward a living kernel. A timer interrupt eventually gives us scheduling. Keyboard input eventually gives us a kernel monitor and shell.

We are still using classic PC-compatible hardware:

| Device                         | Purpose                                            |
| ------------------------------ | -------------------------------------------------- |
| 8259A-compatible PIC           | Routes hardware IRQ lines to CPU interrupt vectors |
| 8253/8254-compatible PIT       | Periodic timer source                              |
| PS/2-style keyboard controller | Early keyboard scancode input                      |

This is intentionally old-school. Modern systems use APIC/IOAPIC/x2APIC, HPET, LAPIC timers, ACPI tables, USB HID stacks, and UEFI. But QEMU and most PC-compatible boot environments still make the legacy path available, and it is much easier to learn first. Intel’s architecture manuals remain the primary reference for IA-32 interrupt behavior, while the 8259A and 8254 datasheets describe the legacy interrupt controller and timer chips we are programming here. ([Intel][1])

---

# 1. What changes in this chapter

We will add:

```text
arch/x86/
├── irq.asm
├── pic.c
├── pic.h
├── pit.c
└── pit.h

drivers/input/
├── keyboard.c
└── keyboard.h

kernel/
├── idle.c
└── idle.h
```

We will modify:

```text
arch/x86/idt.c
arch/x86/interrupts.c
arch/x86/interrupts.h
kernel/kmain.c
Makefile
tests/smoke.sh
```

The new boot path becomes:

```text
GRUB
  ↓
boot.asm
  ↓
kernel_main()
  ↓
console init
  ↓
GDT init
  ↓
IDT init
  ↓
PIC remap
  ↓
PIT timer init
  ↓
keyboard init
  ↓
enable interrupts
  ↓
verify timer interrupts
  ↓
kernel_idle()
  ↓
HLT until the next timer, keyboard, or hardware interrupt
```

The final idle step matters. A fatal halt routine normally executes `cli` and then
halts forever. That is correct after a panic, but it is not correct for normal
kernel operation because `cli` prevents IRQ0 and IRQ1 from reaching the CPU.
This chapter therefore gives normal idling and fatal halting separate functions.

---

# 2. Why the PIC must be remapped

The original IBM PC interrupt layout conflicts with CPU exceptions in protected mode.

CPU exceptions use vectors `0` through `31`.

The legacy PIC, by default, historically maps IRQs into low interrupt vectors that overlap with those exception numbers. That is bad. We want hardware IRQs to live somewhere else.

The common protected-mode mapping is:

```text
IRQ0  → vector 32 / 0x20
IRQ1  → vector 33 / 0x21
...
IRQ15 → vector 47 / 0x2F
```

So this chapter remaps the master/slave PIC pair to:

```text
master PIC base = 0x20
slave PIC base  = 0x28
```

The 8259A handles eight interrupt inputs, and PC-compatible systems use two cascaded PICs for fifteen usable hardware IRQ lines, with the slave connected through the master’s IRQ2 line. The 8259A datasheet describes its initialization command words and operational command words, which are exactly what we send in `pic.c`. ([MIT CSAIL PDOS][2])

---

# 3. Update `arch/x86/interrupts.h`

Replace the previous file with this expanded version:

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

typedef void (*interrupt_handler_t)(interrupt_frame_t *frame);

void isr_handler(interrupt_frame_t *frame);
void irq_handler(interrupt_frame_t *frame);

int interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);

void interrupts_enable(void);
void interrupts_disable(void);
void interrupts_wait(void);

#endif
```

## What changed

Previously, this header only supported CPU exceptions.

Now it supports a general handler registry:

```c
int interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);
```

That lets subsystems attach handlers to vectors:

```c
interrupt_register_handler(32, pit_irq_handler);
interrupt_register_handler(33, keyboard_irq_handler);
```

This is another swappable-parts pattern. The IDT and assembly stubs do not know what a timer is. The timer driver registers itself.

---

# 4. Update `arch/x86/interrupts.c`

Replace the previous file with this:

```c
// arch/x86/interrupts.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/pic.h"
#include "kernel/console.h"
#include "kernel/panic.h"

static interrupt_handler_t interrupt_handlers[256];

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

int interrupt_register_handler(uint8_t vector, interrupt_handler_t handler) {
    interrupt_handlers[vector] = handler;
    return 0;
}

void interrupts_enable(void) {
    __asm__ volatile ("sti");
}

void interrupts_disable(void) {
    __asm__ volatile ("cli");
}

void interrupts_wait(void) {
    __asm__ volatile ("hlt");
}

static void print_register(const char *name, uint32_t value) {
    console_write(name);
    console_write("=");
    console_write_hex32(value);
    console_putc(' ');
}

void isr_handler(interrupt_frame_t *frame) {
    if (frame == NULL) {
        kernel_panic("null exception frame");
    }

    if (frame->interrupt_number < 256 &&
        interrupt_handlers[frame->interrupt_number] != NULL) {
        interrupt_handlers[frame->interrupt_number](frame);
        return;
    }

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

void irq_handler(interrupt_frame_t *frame) {
    if (frame == NULL) {
        kernel_panic("null IRQ frame");
    }

    uint32_t vector = frame->interrupt_number;

    if (vector >= 32 && vector <= 47) {
        if (interrupt_handlers[vector] != NULL) {
            interrupt_handlers[vector](frame);
        }

        pic_send_eoi((uint8_t)(vector - 32));
        return;
    }

    console_write("Unexpected IRQ vector ");
    console_write_hex32(vector);
    console_putc('\n');
}
```

## Why exceptions and IRQs are separate

Exceptions are usually synchronous. They happen because the currently executing instruction did something wrong or important:

```text
divide by zero
invalid opcode
page fault
general protection fault
```

IRQs are asynchronous. They happen because external hardware wants service:

```text
timer tick
keyboard event
disk controller event
network event
serial port event
```

They share the same saved-register frame shape, but we dispatch them differently.

Exceptions currently panic unless specially handled.

IRQs call their driver handler and then send **EOI**, meaning **End Of Interrupt**, to the PIC.

---

# 5. Add `arch/x86/irq.asm`

```asm
; arch/x86/irq.asm
;
; Hardware IRQ stubs for remapped PIC vectors 32..47.
;
; IRQ0  -> vector 32
; IRQ1  -> vector 33
; ...
; IRQ15 -> vector 47
;
; Hardware IRQs do not push CPU error codes, so every IRQ stub pushes
; a fake error code of 0, followed by the interrupt vector number.

BITS 32

extern irq_handler

%macro IRQ_STUB 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

IRQ_STUB 0,  32
IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

global irq_common_stub
irq_common_stub:
    pusha

    xor eax, eax
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa

    add esp, 8

    iretd
```

## Why this mirrors `isr.asm`

The IRQ path deliberately uses the same frame shape as the exception path.

That means C code can receive one type:

```c
interrupt_frame_t *frame
```

instead of separate frame structs for exceptions and IRQs.

This will matter later when we add:

```text
syscalls
preemption
scheduler entry
signal delivery
debug traps
```

A clean trap frame is one of the foundations of a clean kernel.

---

# 6. Add `arch/x86/pic.h`

```c
// arch/x86/pic.h
#ifndef TOYIX_ARCH_X86_PIC_H
#define TOYIX_ARCH_X86_PIC_H

#include <stdint.h>

#define PIC_REMAP_MASTER_OFFSET 0x20u
#define PIC_REMAP_SLAVE_OFFSET  0x28u

void pic_init(void);
void pic_send_eoi(uint8_t irq_line);
void pic_set_mask(uint8_t irq_line);
void pic_clear_mask(uint8_t irq_line);

#endif
```

---

# 7. Add `arch/x86/pic.c`

```c
// arch/x86/pic.c
#include <stdint.h>
#include "arch/x86/io.h"
#include "arch/x86/pic.h"
#include "kernel/console.h"

#define PIC1_COMMAND 0x20u
#define PIC1_DATA    0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA    0xA1u

#define PIC_EOI      0x20u

#define ICW1_ICW4    0x01u
#define ICW1_INIT    0x10u
#define ICW4_8086    0x01u

void pic_send_eoi(uint8_t irq_line) {
    if (irq_line >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;

    if (irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line = (uint8_t)(irq_line - 8);
    }

    value = inb(port);
    value = (uint8_t)(value | (1u << irq_line));
    outb(port, value);
}

void pic_clear_mask(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;
    uint8_t original_irq = irq_line;

    if (irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line = (uint8_t)(irq_line - 8);
    }

    value = inb(port);
    value = (uint8_t)(value & ~(1u << irq_line));
    outb(port, value);

    /*
     * If any slave IRQ is unmasked, the master's cascade line IRQ2 must
     * also be unmasked or the slave interrupt can never reach the CPU.
     */
    if (original_irq >= 8) {
        value = inb(PIC1_DATA);
        value = (uint8_t)(value & ~(1u << 2));
        outb(PIC1_DATA, value);
    }
}

void pic_init(void) {
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);

    /*
     * Begin PIC initialization sequence.
     */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /*
     * ICW2: vector offsets.
     *
     * Master IRQs become vectors 0x20..0x27.
     * Slave IRQs become vectors  0x28..0x2F.
     */
    outb(PIC1_DATA, PIC_REMAP_MASTER_OFFSET);
    io_wait();
    outb(PIC2_DATA, PIC_REMAP_SLAVE_OFFSET);
    io_wait();

    /*
     * ICW3: tell master there is a slave on IRQ2;
     * tell slave its cascade identity is 2.
     */
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    /*
     * ICW4: 8086/88 mode.
     */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /*
     * Start conservative: restore existing masks, then mask everything.
     * Individual drivers will unmask their own IRQ lines.
     */
    (void)master_mask;
    (void)slave_mask;

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    console_writeln("PIC: remapped IRQs to vectors 0x20-0x2F");
}
```

## The PIC initialization sequence must be exact

The initialization command words are positional. The PIC does not receive a
field name with each byte; it interprets each data-port write according to the
current initialization step:

```text
ICW1  command-port write: begin initialization
ICW2  first data-port write: interrupt-vector offset
ICW3  second data-port write: master/slave cascade wiring
ICW4  third data-port write: operating mode
```

Two details are especially important:

```c
#define ICW1_ICW4 0x01u
```

`ICW1_ICW4` is bit 0, not `0x20`. It tells the PIC that an ICW4 byte will
follow.

Also, write the ICW2 offsets exactly once. Repeating these writes is not a
harmless duplicate:

```c
outb(PIC1_DATA, PIC_REMAP_MASTER_OFFSET);
outb(PIC2_DATA, PIC_REMAP_SLAVE_OFFSET);
```

If they are written a second time, the PIC consumes the repeated values as
ICW3 and corrupts the cascade configuration. The correct order is exactly:

```text
ICW1 → ICW2 → ICW3 → ICW4
```

Finally, keep I/O port variables as `uint16_t`. x86 I/O port addresses are
16-bit values even though the legacy PIC ports happen to fit in eight bits.

## Why we mask everything at first

An operating system should not accept interrupts until it has handlers ready.

So `pic_init()` remaps the PIC and then masks every IRQ line:

```text
IRQ0 masked
IRQ1 masked
...
IRQ15 masked
```

Then individual drivers do this:

```c
pic_clear_mask(0);  // timer
pic_clear_mask(1);  // keyboard
```

This is safer than enabling all hardware interrupts and hoping every vector has a working handler.

---

# 8. Update `arch/x86/io.h`

Add `io_wait()` to the bottom of the file.

```c
// arch/x86/io.h
#ifndef TOYIX_ARCH_X86_IO_H
#define TOYIX_ARCH_X86_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
```

## Why `io_wait()` exists

Old PC hardware sometimes needed a tiny delay between port writes.

Writing to port `0x80` is a traditional delay technique in low-level x86 PC code. In QEMU it is mostly harmless. On real hardware, this is a legacy compatibility trick.

Later, we may replace this with a more explicit platform delay abstraction.

---

# 9. Add `arch/x86/pit.h`

```c
// arch/x86/pit.h
#ifndef TOYIX_ARCH_X86_PIT_H
#define TOYIX_ARCH_X86_PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency_hz);
uint32_t pit_get_ticks(void);
void pit_wait_ticks(uint32_t ticks);

#endif
```

---

# 10. Add `arch/x86/pit.c`

```c
// arch/x86/pit.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "kernel/console.h"

#define PIT_CHANNEL0_PORT 0x40u
#define PIT_COMMAND_PORT  0x43u
#define PIT_INPUT_HZ      1193182u

static volatile uint32_t pit_ticks;

static void pit_irq_handler(interrupt_frame_t *frame) {
    (void)frame;
    pit_ticks++;
}

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }

    uint32_t divisor = PIT_INPUT_HZ / frequency_hz;

    if (divisor == 0) {
        divisor = 1;
    }

    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    pit_ticks = 0;

    interrupt_register_handler(32, pit_irq_handler);

    /*
     * Command byte 0x36:
     *
     * channel 0
     * access mode: low byte then high byte
     * operating mode 3: square wave generator
     * binary counter
     */
    outb(PIT_COMMAND_PORT, 0x36);
    outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));

    pic_clear_mask(0);

    console_write("PIT: timer running at ");
    console_write_hex32(frequency_hz);
    console_writeln(" Hz");
}

uint32_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_wait_ticks(uint32_t ticks) {
    uint32_t start = pit_get_ticks();

    while ((uint32_t)(pit_get_ticks() - start) < ticks) {
        interrupts_wait();
    }
}
```

## What the PIT is doing

The 8254 programmable interval timer provides independent counters that can be programmed for timing functions. In the PC-compatible legacy layout, channel 0 is commonly used as the periodic system timer interrupt source. ([Stanford Center for Stanford Stories][3])

We program channel 0 using command byte:

```text
0x36
```

That means:

```text
channel 0
low byte then high byte
mode 3 square wave
binary counting
```

The divisor calculation is:

```text
divisor = 1193182 / requested_frequency
```

So for `100 Hz`:

```text
1193182 / 100 ≈ 11931
```

Then the PIT sends IRQ0 about 100 times per second.

Our handler does only this:

```c
pit_ticks++;
```

That is deliberate. Interrupt handlers should be short. Later, the timer interrupt may drive scheduler accounting and preemption, but it should still avoid doing heavy work directly inside the interrupt handler.

---

# 11. Add `drivers/input/keyboard.h`

```c
// drivers/input/keyboard.h
#ifndef TOYIX_DRIVERS_INPUT_KEYBOARD_H
#define TOYIX_DRIVERS_INPUT_KEYBOARD_H

void keyboard_init(void);

#endif
```

---

# 12. Add `drivers/input/keyboard.c`

```c
// drivers/input/keyboard.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/pic.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"

#define KEYBOARD_DATA_PORT 0x60u

static const char scancode_set1_ascii[128] = {
    [0x01] = 0,      // Escape
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',

    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',

    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',

    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' '
};

static void keyboard_irq_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    /*
     * In set 1, the high bit usually marks a key release.
     * For this first driver, we ignore releases and only echo presses.
     */
    if ((scancode & 0x80u) != 0) {
        return;
    }

    if (scancode < 128) {
        char ch = scancode_set1_ascii[scancode];

        if (ch != 0) {
            console_putc(ch);
        }
    }
}

void keyboard_init(void) {
    interrupt_register_handler(33, keyboard_irq_handler);
    pic_clear_mask(1);

    console_writeln("Keyboard: IRQ1 handler installed");
}
```

## What this keyboard driver does and does not do

This is a deliberately primitive driver.

It does:

```text
read one scancode from port 0x60
ignore key releases
translate some US keyboard scancodes to ASCII
echo printable characters
```

It does not yet handle:

```text
Shift
Ctrl
Alt
Caps Lock
extended scancodes
keyboard LEDs
non-US layouts
command queueing
input buffering
line editing
```

That is fine. This chapter is about proving that IRQ1 reaches the kernel.

Later, this should become two layers:

```text
low-level PS/2 keyboard driver
  ↓
keyboard event layer
  ↓
terminal/input subsystem
```

The low-level driver should not eventually write directly to the console. For now, echoing characters is a useful proof.

---

# 13. Update `arch/x86/idt.c`

Replace the previous `idt.c` with this version. The main change is that it now installs vectors `32` through `47` for hardware IRQs.

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

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

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

    void (*irq_stubs[16])(void) = {
        irq0,  irq1,  irq2,  irq3,
        irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11,
        irq12, irq13, irq14, irq15
    };

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base = (uint32_t)&idt[0];

    for (uint16_t i = 0; i < 256; ++i) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    /*
     * 0x8E:
     *   present
     *   ring 0
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

    for (uint8_t i = 0; i < 16; ++i) {
        idt_set_gate(
            (uint8_t)(32 + i),
            (uint32_t)irq_stubs[i],
            X86_KERNEL_CODE_SELECTOR,
            0x8Eu
        );
    }

    idt_load((uint32_t)&idt_ptr);

    console_writeln("IDT: installed CPU exceptions and hardware IRQ gates");
}
```

## What changed conceptually

The IDT now has two important regions:

```text
0x00-0x1F  CPU exceptions
0x20-0x2F  remapped hardware IRQs
```

The CPU does not inherently know that vector `32` is the timer. That is our convention after PIC remapping.

---

# 14. Add `kernel/idle.h`

Normal idling and fatal halting are different kernel operations.

`kernel_halt()` remains the terminal path used by panic handling. It disables
interrupts and never resumes. `kernel_idle()` is the normal running-kernel path.
It sleeps until an interrupt occurs, handles that interrupt, and then goes back
to sleep.

Add:

```c
// kernel/idle.h
#ifndef TOYIX_KERNEL_IDLE_H
#define TOYIX_KERNEL_IDLE_H

/*
 * Enter the normal kernel idle loop.
 *
 * Interrupts must already be enabled. The processor sleeps between hardware
 * interrupts while continuing to service timer, keyboard, and other IRQs.
 */
_Noreturn void kernel_idle(void);

#endif
```

The `_Noreturn` declaration documents that control never returns to
`kernel_main()`.

---

# 15. Add `kernel/idle.c`

```c
// kernel/idle.c
#include "arch/x86/interrupts.h"
#include "kernel/idle.h"

_Noreturn void kernel_idle(void) {
    for (;;) {
        interrupts_wait();
    }
}
```

`interrupts_wait()` executes `hlt`, but it does not execute `cli`. With the
interrupt flag still set, the CPU wakes when IRQ0, IRQ1, or another enabled
interrupt arrives.

The layering is intentional:

```text
kernel_idle()          kernel policy: idle forever
    ↓
interrupts_wait()      architecture abstraction: wait once
    ↓
x86 HLT               processor instruction
```

Do not move `kernel_idle()` into `kernel/panic.c`, and do not remove `cli` from
`kernel_halt()`. A panic must be able to stop the machine without permitting
more device handlers to run against potentially inconsistent kernel state.

The two paths should remain:

```text
normal operation: kernel_idle() → HLT → wake on interrupt → HLT again
fatal operation:  kernel_halt() → CLI → HLT forever
```

---

# 16. Update `kernel/kmain.c`

Replace `kernel/kmain.c` with this:

```c
// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/idle.h"

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
    pic_init();

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
    console_writeln("Next stop: physical memory map and page allocator.");

    kernel_idle();
}
```

## Why interrupts are enabled late

Notice the order:

```c
gdt_init();
idt_init();
pic_init();

pit_init(100);
keyboard_init();

interrupts_enable();
```

That is intentional.

We do not enable interrupts until:

1. The GDT is valid.
2. The IDT is valid.
3. The PIC is remapped.
4. The timer handler is registered.
5. The keyboard handler is registered.
6. The relevant IRQ lines are unmasked.

Then, and only then, we execute `sti` through:

```c
interrupts_enable();
```

The `sti` instruction sets the CPU interrupt-enable flag, allowing maskable
hardware interrupts to be delivered. Intel’s IA-32 manuals describe interrupt
and exception handling, including interrupt-flag behavior and interrupt gates.
([Intel][1])

## Why `kernel_halt()` was wrong here

A typical fatal halt implementation looks like this:

```c
_Noreturn void kernel_halt(void) {
    __asm__ volatile ("cli");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
```

That is correct for a panic. It is incorrect at the end of a normally running
kernel because `cli` disables the timer and keyboard interrupt path. The screen
may print “Try typing,” but IRQ1 can no longer be delivered.

`kernel_main()` must therefore enter `kernel_idle()`, not `kernel_halt()`.

---

# 17. Update `Makefile`

Replace your Makefile with this version, or carefully merge the object-list
change. The important addition is `build/kernel/idle.o`.

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
    build/kernel/idle.o \
    build/kernel/panic.o \
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
	grep -q "GDT: installed flat kernel code/data segments" build/test.log
	grep -q "IDT: installed CPU exceptions and hardware IRQ gates" build/test.log
	grep -q "PIC: remapped IRQs to vectors 0x20-0x2F" build/test.log
	grep -q "PIT: timer running at" build/test.log
	grep -q "Keyboard: IRQ1 handler installed" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot and IRQ smoke test passed."

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

# 18. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception

echo "All Chapter 3 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

The automated smoke test proves that the timer interrupt path is active because
`pit_wait_ticks(3)` cannot complete unless IRQ0 reaches the kernel. Keyboard
input is still tested manually in this chapter.

---

# 19. Expected output

Normal run:

```bash
make clean
make run
```

You should see:

```text
Toyix kernel alive
Boot protocol: Multiboot OK
Multiboot info at 0xXXXXXXXX
GDT: installed flat kernel code/data segments
IDT: installed CPU exceptions and hardware IRQ gates
PIC: remapped IRQs to vectors 0x20-0x2F
PIT: timer running at 0x00000064 Hz
Keyboard: IRQ1 handler installed
Interrupt hardware: configured
Interrupts: enabled
Timer: observed 3 ticks
Try typing in the QEMU window.
Next stop: physical memory map and page allocator.
```

The frequency prints as hexadecimal because our console layer only has
`console_write_hex32()` so far:

```text
0x00000064 = 100 decimal
```

Unlike the earlier fatal-halt version, QEMU must remain running after the final
message. The CPU spends most of its time in `hlt`, but timer and keyboard
interrupts continue waking it.

---

# 20. What to test manually

Run:

```bash
make run
```

Click inside the QEMU window and type letters and numbers such as:

```text
abc123
```

You should see those characters appear on the kernel console.

This proves:

```text
keyboard event
  ↓
legacy keyboard controller
  ↓
IRQ1
  ↓
PIC
  ↓
CPU vector 33
  ↓
irq1 assembly stub
  ↓
irq_handler()
  ↓
keyboard_irq_handler()
  ↓
console_putc()
```

It also proves that `kernel_idle()` preserved the CPU interrupt-enable flag.

Use QEMU’s configured release-key combination when you need to return keyboard
and mouse control to the host. With the default GTK display this is commonly
`Ctrl+Alt+G`.

---

# 21. Common failures

## Failure: kernel hangs at `pit_wait_ticks(3)`

Likely causes:

```text
interrupts not enabled
PIC IRQ0 still masked
IDT vector 32 not installed
PIT not programmed correctly
IRQ common stub stack layout wrong
EOI not sent
PIC initialization command sequence malformed
```

Check these lines:

```c
pic_clear_mask(0);
interrupt_register_handler(32, pit_irq_handler);
interrupts_enable();
```

Confirm that `irq0` is installed at vector `32` in `idt.c`.

Also verify that the PIC uses:

```c
#define ICW1_ICW4 0x01u
```

and that ICW2 is written exactly once before ICW3.

## Failure: timer works once, then stops

Likely cause:

```text
missing EOI
```

The PIC will not continue delivering interrupts correctly unless the handler
acknowledges the interrupt.

This must happen after IRQ handling:

```c
pic_send_eoi((uint8_t)(vector - 32));
```

## Failure: prompt appears, but the keyboard does nothing

First confirm that the QEMU window has input focus.

Then check that `kernel_main()` ends with:

```c
kernel_idle();
```

and not:

```c
kernel_halt();
```

`kernel_halt()` executes `cli`, which disables IRQ1. Printing a prompt before
calling it does not leave the keyboard active.

Other possible causes are:

```text
IRQ1 still masked
keyboard handler not registered at vector 33
wrong keyboard data port
QEMU window not focused
```

For QEMU, its emulated PS/2-style keyboard path should work even when the host
computer uses a physical USB keyboard.

## Failure: `implicit declaration of function 'interrupt_wait'`

The architecture helper is named:

```c
interrupts_wait();
```

with the plural word `interrupts`. Also ensure that the implementation of
`kernel_idle()` exists only in `kernel/idle.c`. Leaving another copy in
`kernel/panic.c` will either produce compilation problems or a multiple-definition
linker error.

## Failure: typing causes weird characters

Expected for now.

We only handle a small subset of scancode set 1 and ignore Shift state. A real
keyboard layer needs press/release state, modifier tracking, layout translation,
extended-scancode handling, and input buffering.

---

# 22. Why this chapter matters

We now have four major kernel mechanisms:

```text
exceptions
hardware IRQs
driver registration
interruptible idle operation
```

That means the OS can react to the outside world while avoiding a wasteful busy
loop.

The timer will eventually drive:

```text
uptime
sleep
timeouts
scheduler ticks
preemption
profiling
```

The keyboard path will give us an early kernel monitor, something like:

```text
toyix> mem
toyix> ticks
toyix> reboot
toyix> help
```

The new idle boundary is equally important. Later, `kernel_idle()` can become a
scheduler idle task without changing the low-level `interrupts_wait()` primitive.

---

# 23. Commit this chapter

Once both automated tests pass and manual typing works:

```bash
git status
git add arch/x86 drivers/input kernel/idle.c kernel/idle.h \
        kernel/kmain.c Makefile tests/smoke.sh articles/chapter_03.md
git commit -m "Add hardware IRQs and interruptible kernel idle loop"
```

That commit message records both the interrupt subsystem and the normal idle
behavior required to keep keyboard IRQs active.

---

# 24. Next chapter

The next chapter should add the first memory-awareness layer:

```text
read Multiboot memory information
print the memory map
identify usable RAM regions
reserve kernel image memory
build a physical page allocator
```

That is the next big transition.

Right now the kernel can boot, receive interrupts, and idle safely, but it
cannot allocate memory. Once we have a physical page allocator, we can build:

```text
kernel heap
paging
higher-half kernel
process address spaces
filesystem cache
driver buffers
```

The next practical milestone is:

```text
kernel knows which physical pages are usable
kernel can allocate and free 4 KiB pages
```

[1]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel® 64 and IA-32 Architectures"
[2]: https://pdos.csail.mit.edu/6.828/2010/readings/hardware/8259A.pdf "8259A Programmable Interrupt Controller"
[3]: https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf "8254 Programmable Interval Timer"

---

# Resources

- [Chapter 03 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_03)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Intel 8259A Programmable Interrupt Controller datasheet](https://pdos.csail.mit.edu/6.828/2010/readings/hardware/8259A.pdf)
- [Intel 8254 Programmable Interval Timer datasheet](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf)
- [OSDev 8259 PIC](https://wiki.osdev.org/8259_PIC)
- [OSDev Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer)
- [OSDev PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)

That completes the third Toyix milestone: hardware IRQs, timer ticks, keyboard input, and an interruptible idle loop.

Happy Coding!
