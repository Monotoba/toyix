# Chapter 1 — The Smallest Useful Kernel

## 1. What we are building

By the end of this chapter, the machine will boot into our own kernel and print something like:

```text
Toyix kernel alive
Boot protocol: Multiboot OK
Multiboot info at 0x0012A000
Console drivers: serial + VGA text
Next stop: GDT, IDT, memory map, heap.
```

It will print to both:

1. **VGA text memory** at `0xB8000`, so you see text in the emulator window.
2. **COM1 serial output**, so our tests can capture the boot log automatically.

That second part matters. A kernel that merely “looks right” in QEMU is hard to test. A kernel that writes to serial can be checked by scripts.

---

# 2. Why we are using GRUB first

There are two classic ways to start an OS project.

One path is to write a 512-byte boot sector immediately. That teaches BIOS boot mechanics, but it also forces you to solve disk loading, memory layout, real-mode limitations, and protected-mode switching before you even have a kernel.

The other path is to let a real bootloader load your kernel and then focus on kernel design. We’ll use that path first.

GRUB can load kernels that include a **Multiboot header**. For the original Multiboot format, the kernel image contains magic value `0x1BADB002`, flags, and a checksum chosen so the magic and flags sum to zero modulo 32 bits. ([GNU][2]) OSDev’s Bare Bones tutorial uses this approach because GRUB handles the early bootloader work and enters a 32-bit environment suitable for a small starter kernel. ([OSDev Wiki][1])

Later, once the kernel has a clean internal shape, we can replace GRUB with our own bootloader. That is the first example of our “swappable parts” philosophy.

---

# 3. The architecture of our first kernel

We will organize the project like this:

```text
toyix/
├── Makefile
├── linker.ld
├── grub.cfg
├── arch/
│   └── x86/
│       ├── boot.asm
│       └── io.h
├── include/
│   └── kernel/
│       └── console.h
├── kernel/
│   ├── console.c
│   ├── kmain.c
│   └── lib/
│       └── mem.c
├── drivers/
│   └── console/
│       ├── serial.c
│       └── vga_text.c
└── tests/
    └── smoke.sh
```

This is intentionally more structured than the smallest possible “Hello, kernel” demo.

A tiny demo often has only three files: assembly, C, and linker script. That boots, but it teaches poor habits. We want the kernel core to talk to a **console abstraction**, not directly to VGA or serial hardware. That way, VGA text, serial, framebuffer, graphical console, and log buffer can all become replaceable console drivers.

The key design rule is:

> The kernel core should depend on interfaces, not on hardware details.

That principle will repeat throughout the OS.

---

# 4. Hosted C versus freestanding C

Normal C programs run in a **hosted environment**. They have an operating system beneath them. They can call `printf`, `malloc`, `fopen`, `exit`, and so on.

A kernel is different. A kernel is the environment. There is no libc unless you provide one. GCC describes an OS kernel as an example of a **freestanding environment**, and says `-ffreestanding` tells GCC not to assume the usual hosted C library behavior. GCC also notes that kernel-style freestanding code may still need its own `memcpy`, `memmove`, `memset`, and `memcmp`. ([GCC][3])

So our kernel will not call `printf`.

Instead, we write our own tiny console layer.

---

# 5. Source listing: `arch/x86/boot.asm`

This is the first code that runs inside our kernel image.

```asm
; arch/x86/boot.asm
;
; This file is the bridge between the bootloader and our C kernel.
;
; GRUB loads our ELF kernel, finds the Multiboot header, switches to the
; expected 32-bit environment, and jumps to _start.
;
; At entry:
;   EAX = Multiboot magic value
;   EBX = pointer to Multiboot information structure
;
; We create a stack, then call kernel_main(magic, info_ptr).

BITS 32

global _start
extern kernel_main

MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x00000003        ; bit 0: align modules, bit 1: request memory info
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

section .bss
align 16

stack_bottom:
    resb 16384                    ; 16 KiB bootstrap stack
stack_top:

section .text
align 16

_start:
    ; x86 stacks grow downward. Setting ESP to stack_top gives C a usable stack.
    mov esp, stack_top

    ; C uses cdecl on i386. Arguments are pushed right-to-left.
    ; kernel_main(uint32_t magic, uint32_t info_ptr)
    push ebx
    push eax
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
```

## What this file does

The `.multiboot` section is not executable code. It is a signature GRUB scans for. Without it, GRUB does not know that our ELF file is intended to be booted as a kernel.

The `_start` label is our real entry point. C functions expect a stack. A bootloader does not promise to give us a C-friendly stack, so we reserve 16 KiB in `.bss` and load `ESP` with the top of that region.

Then we pass two values into C:

```c
kernel_main(multiboot_magic, multiboot_info_pointer);
```

This matters later. The Multiboot information structure can tell us about memory size, modules, boot device, command line, and memory maps. In this chapter we only print its address.

---

# 6. Source listing: `linker.ld`

The linker script tells the linker where the kernel lives in memory and how to arrange sections.

```ld
/* linker.ld
 *
 * The linker script controls the physical layout of the kernel image.
 *
 * We place the kernel at 1 MiB. This is traditional for simple x86 kernels:
 * it avoids the low memory area used by BIOS data structures and bootloader
 * scratch space.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

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
}
```

## Why the linker script matters

In normal Linux user-space programs, the OS loader decides where your program goes. In a kernel, you are designing the loader contract.

The important line is:

```ld
. = 1M;
```

That says: link this kernel as though it starts at physical address `0x00100000`.

The `.multiboot` section is deliberately first. GRUB must find the Multiboot header near the start of the image. OSDev notes that the Multiboot header must appear early enough for GRUB to find it. ([OSDev Wiki][1])

---

# 7. Source listing: `include/kernel/console.h`

This is our first real subsystem interface.

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

#endif
```

## Why this is written as an interface

The kernel core should not care whether output goes to VGA, serial, a framebuffer, a log ring, or a remote debug stub.

Each console driver provides:

```c
void init(void);
void putc(char c);
```

The kernel registers whatever drivers it wants. The console layer then fans output to all registered drivers.

This is the beginning of a Linux-like modular design. Linux has far more sophisticated driver models, but the idea is similar: the core uses abstractions; hardware-specific code lives behind operations tables.

---

# 8. Source listing: `kernel/console.c`

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
```

## What this gives us

This file gives the kernel one stable way to speak:

```c
console_writeln("hello");
```

The kernel does not know how serial works. It does not know how VGA works. It just emits characters.

Later, we can add:

```text
drivers/console/framebuffer.c
drivers/console/log_buffer.c
drivers/console/usb_debug.c
```

without rewriting `kernel/kmain.c`.

---

# 9. Source listing: `arch/x86/io.h`

The serial driver needs x86 I/O port access. C has no standard concept of I/O ports, so we use inline assembly.

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

#endif
```

## Why this is architecture-specific

This file belongs under `arch/x86/` because I/O ports are an x86 concept. ARM, RISC-V, 68k, and other machines use different hardware-access models.

That gives us another important pattern:

```text
arch/x86/io.h
arch/riscv/io.h
arch/arm/io.h
```

The kernel core should not fill up with architecture-specific inline assembly.

---

# 10. Source listing: `drivers/console/serial.c`

```c
// drivers/console/serial.c
#include <stdint.h>
#include "kernel/console.h"
#include "arch/x86/io.h"

#define COM1 0x3F8

static int serial_transmit_ready(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB: divisor access
    outb(COM1 + 0, 0x03);    // Divisor low byte: 38400 baud
    outb(COM1 + 1, 0x00);    // Divisor high byte
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear it, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }

    for (uint32_t timeout = 0; timeout < 100000; ++timeout) {
        if (serial_transmit_ready()) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

const console_driver_t serial_console_driver = {
    .name = "serial",
    .init = serial_init,
    .putc = serial_putc
};
```

## Why serial output matters

Serial output is not glamorous, but it is one of the best early kernel tools.

VGA output is useful for humans. Serial output is useful for tests, logs, and emulator automation.

When QEMU runs with:

```bash
-serial stdio
```

characters written to COM1 appear on the host terminal. That means a script can boot the kernel and check whether the expected line appears.

That is our first kernel test.

---

# 11. Source listing: `drivers/console/vga_text.c`

```c
// drivers/console/vga_text.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14,
    VGA_WHITE = 15
};

static volatile uint16_t *const vga_buffer = (volatile uint16_t *)0xB8000;

static size_t row;
static size_t column;
static uint8_t color;

static uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t)(fg | (bg << 4));
}

static uint16_t vga_entry(unsigned char ch, uint8_t entry_color) {
    return (uint16_t)ch | ((uint16_t)entry_color << 8);
}

static void vga_clear_row(size_t y) {
    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', color);
    }
}

static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga_buffer[(y - 1) * VGA_WIDTH + x] =
                vga_buffer[y * VGA_WIDTH + x];
        }
    }

    vga_clear_row(VGA_HEIGHT - 1);
    row = VGA_HEIGHT - 1;
}

static void vga_newline(void) {
    column = 0;
    ++row;

    if (row >= VGA_HEIGHT) {
        vga_scroll();
    }
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_newline();
        return;
    }

    vga_buffer[row * VGA_WIDTH + column] =
        vga_entry((unsigned char)c, color);

    ++column;

    if (column >= VGA_WIDTH) {
        vga_newline();
    }
}

static void vga_init(void) {
    row = 0;
    column = 0;
    color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        vga_clear_row(y);
    }
}

const console_driver_t vga_text_console_driver = {
    .name = "vga_text",
    .init = vga_init,
    .putc = vga_putc
};
```

## What VGA text mode is doing

Classic VGA text mode maps screen characters into memory at physical address:

```text
0xB8000
```

Each screen cell is two bytes:

```text
byte 0: ASCII character
byte 1: foreground/background color attribute
```

So writing a 16-bit value into VGA memory displays a character.

This is crude, but perfect for early kernel work. Later, we can replace this with a framebuffer console.

---

# 12. Source listing: `kernel/lib/mem.c`

```c
// kernel/lib/mem.c
#include <stddef.h>

void *memset(void *dest, int value, size_t count) {
    unsigned char *d = (unsigned char *)dest;

    for (size_t i = 0; i < count; ++i) {
        d[i] = (unsigned char)value;
    }

    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || count == 0) {
        return dest;
    }

    if (d < s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }

    return dest;
}

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;

    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}
```

## Why we provide these now

Even if we do not explicitly call `memcpy` or `memset`, the compiler may emit calls to them when optimizing C code.

A hosted C program would get these from libc.

A kernel does not.

So we provide simple versions early.

---

# 13. Source listing: `kernel/kmain.c`

```c
// kernel/kmain.c
#include <stdint.h>
#include "kernel/console.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

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

    console_writeln("Console drivers: serial + VGA text");
    console_writeln("Next stop: GDT, IDT, memory map, heap.");

    halt_forever();
}
```

## What `kernel_main` is

This is not `main`.

There is no operating system to call `main`.

Our assembly entry point calls `kernel_main` directly after setting up the stack. That makes `kernel_main` the first C function in the OS.

Notice what it does first:

```c
console_register(&serial_console_driver);
console_register(&vga_text_console_driver);
console_init_all();
```

That is the first “swappable subsystem” pattern.

---

# 14. Source listing: `grub.cfg`

```cfg
set timeout=0
set default=0

menuentry "Toyix" {
    multiboot /boot/kernel.elf
    boot
}
```

## What this does

This tells GRUB:

```text
Load /boot/kernel.elf as a Multiboot kernel.
```

GRUB does the disk reading. GRUB loads the ELF. GRUB jumps to `_start`.

Our kernel does not yet understand disks, filesystems, or boot media.

That is acceptable. Kernel development is layered. The first win is control.

---

# 15. Source listing: `Makefile`

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
          -I.

LDFLAGS := -T linker.ld \
           -ffreestanding \
           -O2 \
           -nostdlib \
           -lgcc

OBJS := \
    build/arch/x86/boot.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o

.PHONY: all clean iso run test

all: build/kernel.elf

build/arch/x86/boot.o: arch/x86/boot.asm
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
	@echo "Smoke test passed."

clean:
	rm -rf build
```

## Important toolchain note

Use an `i686-elf` cross-compiler for serious OS work. OSDev explicitly warns that the host Linux compiler is not the right compiler for kernel development, even if it can emit ELF, because it targets Linux user-space assumptions rather than your OS. ([OSDev Wiki][1])

On Ubuntu, you will usually install these packages:

```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso mtools
```

You will still need an `i686-elf-gcc` cross-compiler. We should treat that as its own setup chapter because doing it correctly matters.

---

# 16. Source listing: `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test

echo "All smoke checks passed."
```

Make it executable:

```bash
chmod +x tests/smoke.sh
```

Run it:

```bash
./tests/smoke.sh
```

## What this test proves

This test proves three things:

1. The kernel builds.
2. GRUB recognizes it as a Multiboot kernel.
3. QEMU boots it far enough for the kernel to print known serial output.

That is not a complete OS test. It is a boot smoke test. But it is exactly the kind of test you want early.

Every time we add a subsystem, we should keep this test passing.

---

# 17. Build and run

From the `toyix/` directory:

```bash
make iso
make run
```

For test mode:

```bash
make test
```

For version control:

```bash
git init
git add .
git commit -m "Boot minimal Multiboot kernel with swappable console drivers"
```

Commit early. Kernel development breaks easily. Small commits let you bisect regressions.

---

# 18. What we have achieved

At this point, we have:

```text
GRUB
  ↓
Multiboot kernel image
  ↓
arch/x86/boot.asm
  ↓
kernel_main()
  ↓
console abstraction
  ↓
serial driver + VGA text driver
```

This is small, but it is already shaped like a real kernel project.

We separated:

| Concern                       | File                         |
| ----------------------------- | ---------------------------- |
| Boot entry                    | `arch/x86/boot.asm`          |
| Memory layout                 | `linker.ld`                  |
| Kernel core                   | `kernel/kmain.c`             |
| Console interface             | `include/kernel/console.h`   |
| Console multiplexer           | `kernel/console.c`           |
| Serial hardware               | `drivers/console/serial.c`   |
| VGA hardware                  | `drivers/console/vga_text.c` |
| Freestanding memory functions | `kernel/lib/mem.c`           |

That separation is more important than the amount of code.

---

# 19. What “swappable OS parts” will mean in this project

We will use a table-driven, interface-driven style.

For example, console drivers already look like this:

```c
typedef struct console_driver {
    const char *name;
    void (*init)(void);
    void (*putc)(char c);
} console_driver_t;
```

Later we will use similar patterns for:

```text
memory_allocator_t
physical_memory_manager_t
virtual_memory_manager_t
block_device_t
filesystem_t
scheduler_class_t
clock_source_t
irq_controller_t
syscall_table_t
executable_loader_t
```

This lets the OS grow without turning into one tangled file.

A few future examples:

```c
kernel_set_allocator(&bitmap_allocator);
kernel_set_allocator(&buddy_allocator);

vfs_mount("/", &initramfs_fs);
vfs_mount("/disk", &fat32_fs);

scheduler_set_class(&round_robin_scheduler);
scheduler_set_class(&priority_scheduler);
```

That does not mean every subsystem should be runtime-hot-swappable on day one. It means the kernel should be designed so implementations can be replaced without rewriting the whole kernel.

---

# 20. The chapter roadmap

A good path from here is:

| Chapter | Goal                                                 |
| ------- | ---------------------------------------------------- |
| 1       | Bootable kernel, serial/VGA console, test harness    |
| 2       | Correct cross-compiler setup and reproducible build  |
| 3       | GDT, IDT, exceptions, panic screen                   |
| 4       | PIC/APIC basics, timer interrupt, keyboard interrupt |
| 5       | Physical memory map from Multiboot                   |
| 6       | Page allocator                                       |
| 7       | Paging and higher-half kernel                        |
| 8       | Kernel heap: `kmalloc` / `kfree`                     |
| 9       | Cooperative threads                                  |
| 10      | Preemptive scheduler                                 |
| 11      | VFS abstraction                                      |
| 12      | Initramfs                                            |
| 13      | ELF loader                                           |
| 14      | User mode ring 3                                     |
| 15      | Syscalls                                             |
| 16      | Shell process                                        |
| 17      | Block devices                                        |
| 18      | FAT-like filesystem                                  |
| 19      | Loadable kernel modules                              |
| 20      | Replace GRUB with our own bootloader                 |

The next technical milestone should be **GDT + IDT + exception handling**. Until we can catch faults cleanly, debugging the kernel will be painful.

[1]: https://wiki.osdev.org/Bare_Bones "Bare Bones - OSDev Wiki"
[2]: https://www.gnu.org/software/grub/manual/multiboot/html_node/Header-magic-fields.html "Multiboot Specification version 0.6.96: Header magic fields"
[3]: https://gcc.gnu.org/onlinedocs/gcc/Standards.html "Standards (Using the GNU Compiler Collection (GCC))"

---

# Resources

- [Chapter 01 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_01)
- [OSDev Bare Bones tutorial](https://wiki.osdev.org/Bare_Bones)
- [GNU GRUB Multiboot Specification](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- [GNU Multiboot header magic fields](https://www.gnu.org/software/grub/manual/multiboot/html_node/Header-magic-fields.html)
- [GCC language standards and freestanding environments](https://gcc.gnu.org/onlinedocs/gcc/Standards.html)

That completes the first Toyix milestone: a bootable kernel with serial output, VGA text output, and an automated smoke test.

Happy Coding!
