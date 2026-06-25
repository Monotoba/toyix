# Chapter 14 — Terminal Line Discipline and a Kernel Monitor

At this point our kernel has enough machinery to become interactive:

```text
keyboard IRQ
  ↓
keyboard ring buffer
  ↓
blocking wait queue
  ↓
thread scheduler
  ↓
console output with mutex
```

Now we will build two new layers:

```text
terminal line discipline
  ↓
kernel monitor
```

The **terminal line discipline** turns raw keyboard characters into editable input lines. It handles:

```text
blocking until a line is available
echoing typed characters
backspace
newline
fixed-size command buffer
```

The **kernel monitor** is a small command loop:

```text
toyix> help
toyix> ticks
toyix> threads
toyix> mem
toyix> heap
toyix> sleep 50
toyix> echo hello
```

This is not user space yet. It is still a kernel thread. But it gives us the first interactive control surface for inspecting the OS while it runs.

---

# 1. Layering goal

The new structure should look like this:

```text
PS/2 keyboard IRQ driver
  ↓
keyboard character ring buffer
  ↓
keyboard_getchar_blocking()
  ↓
terminal_readline()
  ↓
monitor command loop
```

The keyboard driver should not know about commands.

The terminal should not know about memory maps or thread queues.

The monitor should not know about scancodes.

That separation is what will let us replace parts later.

---

# 2. Patch overview

Add:

```text
include/kernel/
├── monitor.h
└── terminal.h

kernel/
├── monitor.c
└── terminal.c
```

Modify:

```text
include/kernel/string.h
kernel/lib/mem.c
drivers/console/vga_text.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

We will also add basic string helpers because command parsing needs them.

---

# 3. Update `include/kernel/string.h`

Replace the existing file with this expanded version.

```c
// include/kernel/string.h
#ifndef TOYIX_KERNEL_STRING_H
#define TOYIX_KERNEL_STRING_H

#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *left, const void *right, size_t count);

size_t kstrlen(const char *text);
int kstrcmp(const char *left, const char *right);
int kstrncmp(const char *left, const char *right, size_t count);
char *kstrcpy(char *dest, const char *src);

#endif
```

## Why not use the standard names?

We can safely provide `memcpy`, `memset`, and friends because GCC may emit calls to them in freestanding mode.

For string helpers, I am using `kstrlen`, `kstrcmp`, and `kstrcpy` to make it clear these are our kernel versions, not libc.

Later, we may build a more complete kernel libc layer, but for now these names keep intent explicit.

---

# 4. Update `kernel/lib/mem.c`

Append these functions to the existing file.

```c
size_t kstrlen(const char *text) {
    size_t length = 0;

    if (text == 0) {
        return 0;
    }

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

int kstrcmp(const char *left, const char *right) {
    if (left == 0 && right == 0) {
        return 0;
    }

    if (left == 0) {
        return -1;
    }

    if (right == 0) {
        return 1;
    }

    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return (unsigned char)*left - (unsigned char)*right;
        }

        left++;
        right++;
    }

    return (unsigned char)*left - (unsigned char)*right;
}

int kstrncmp(const char *left, const char *right, size_t count) {
    if (count == 0) {
        return 0;
    }

    if (left == 0 && right == 0) {
        return 0;
    }

    if (left == 0) {
        return -1;
    }

    if (right == 0) {
        return 1;
    }

    for (size_t i = 0; i < count; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];

        if (a != b) {
            return (int)a - (int)b;
        }

        if (a == '\0') {
            return 0;
        }
    }

    return 0;
}

char *kstrcpy(char *dest, const char *src) {
    char *original = dest;

    if (dest == 0) {
        return 0;
    }

    if (src == 0) {
        dest[0] = '\0';
        return dest;
    }

    while (*src != '\0') {
        *dest++ = *src++;
    }

    *dest = '\0';

    return original;
}
```

---

# 5. Update `drivers/console/vga_text.c` for backspace

The terminal needs backspace support.

Find `vga_putc()` and replace it with this version.

```c
static void vga_backspace(void) {
    if (column > 0) {
        column--;
        vga_buffer[row * VGA_WIDTH + column] = vga_entry(' ', color);
        return;
    }

    if (row > 0) {
        row--;
        column = VGA_WIDTH - 1;
        vga_buffer[row * VGA_WIDTH + column] = vga_entry(' ', color);
    }
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_newline();
        return;
    }

    if (c == '\b') {
        vga_backspace();
        return;
    }

    vga_buffer[row * VGA_WIDTH + column] =
        vga_entry((unsigned char)c, color);

    ++column;

    if (column >= VGA_WIDTH) {
        vga_newline();
    }
}
```

## Why this matters

Without VGA backspace support, the terminal can remove characters from the input buffer, but the screen would not visually update correctly.

The terminal will echo backspace as:

```text
\b \b
```

That sequence means:

```text
move cursor left
overwrite old character with a space
move cursor left again
```

On serial terminals this is a common convention. On VGA, our new backspace handler makes it work well enough for the early monitor.

---

# 6. Add `include/kernel/terminal.h`

```c
// include/kernel/terminal.h
#ifndef TOYIX_KERNEL_TERMINAL_H
#define TOYIX_KERNEL_TERMINAL_H

#include <stddef.h>

void terminal_init(void);

size_t terminal_readline(char *buffer, size_t buffer_size);

void terminal_test_once(void);

#endif
```

---

# 7. Add `kernel/terminal.c`

```c
// kernel/terminal.c
#include <stddef.h>
#include <stdint.h>
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"

#define TERMINAL_MAX_TEST_LINE 32u

static volatile uint32_t terminal_test_done;
static char terminal_test_line[TERMINAL_MAX_TEST_LINE];

void terminal_init(void) {
    console_writeln("Terminal: line discipline initialized");
}

static int terminal_is_printable(char ch) {
    return ch >= 32 && ch <= 126;
}

size_t terminal_readline(char *buffer, size_t buffer_size) {
    if (buffer == 0 || buffer_size == 0) {
        return 0;
    }

    size_t length = 0;
    buffer[0] = '\0';

    for (;;) {
        char ch = keyboard_getchar_blocking();

        if (ch == '\r') {
            ch = '\n';
        }

        if (ch == '\n') {
            console_putc('\n');
            buffer[length] = '\0';
            return length;
        }

        if (ch == '\b' || ch == 127) {
            if (length > 0) {
                length--;
                buffer[length] = '\0';

                console_write("\b \b");
            }

            continue;
        }

        if (terminal_is_printable(ch)) {
            if (length + 1 < buffer_size) {
                buffer[length++] = ch;
                buffer[length] = '\0';
                console_putc(ch);
            }

            /*
             * If the line is full, ignore extra printable characters for now.
             * Later we can add a bell, status message, or horizontal editing.
             */
            continue;
        }

        /*
         * Ignore other control characters in the first terminal version.
         */
    }
}

/* ------------------------------------------------------------------------- */
/* Test                                                                       */
/* ------------------------------------------------------------------------- */

static void terminal_reader_test_thread(void *arg) {
    (void)arg;

    terminal_readline(terminal_test_line, sizeof(terminal_test_line));

    console_write("Terminal test: reader got line ");
    console_writeln(terminal_test_line);

    terminal_test_done = 1;
}

void terminal_test_once(void) {
    console_writeln("Terminal test: starting readline test");

    terminal_test_done = 0;
    terminal_test_line[0] = '\0';

    thread_create("term-reader", terminal_reader_test_thread, 0);

    /*
     * Let the reader block waiting for input.
     */
    thread_sleep_ticks(2);

    /*
     * Input sequence:
     *
     *   a b c backspace D newline
     *
     * Expected line:
     *
     *   abD
     */
    keyboard_debug_inject_char('a');
    keyboard_debug_inject_char('b');
    keyboard_debug_inject_char('c');
    keyboard_debug_inject_char('\b');
    keyboard_debug_inject_char('D');
    keyboard_debug_inject_char('\n');

    while (!terminal_test_done) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (kstrcmp(terminal_test_line, "abD") != 0) {
        kernel_panic("terminal readline test failed");
    }

    console_writeln("Terminal test: readline/backspace sanity check passed");
}
```

## What the terminal layer does

The terminal reads one character at a time from:

```c
keyboard_getchar_blocking()
```

That means the terminal thread sleeps when there is no input.

It handles only the basics:

```text
printable ASCII
newline
backspace
fixed buffer limit
```

It does not yet handle:

```text
arrow keys
history
Ctrl+C
Ctrl+D
tab completion
quoted arguments
UTF-8
terminal modes
```

That is fine. We are building a kernel monitor, not a full shell.

---

# 8. Add `include/kernel/monitor.h`

```c
// include/kernel/monitor.h
#ifndef TOYIX_KERNEL_MONITOR_H
#define TOYIX_KERNEL_MONITOR_H

void monitor_init(void);
void monitor_start(void);

int monitor_execute_command(const char *line);

void monitor_test_once(void);

#endif
```

---

# 9. Add `kernel/monitor.c`

```c
// kernel/monitor.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/monitor.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"

#define MONITOR_LINE_SIZE 128u

static void monitor_thread_main(void *arg);

void monitor_init(void) {
    console_writeln("Monitor: command dispatcher initialized");
}

static const char *skip_spaces(const char *text) {
    while (text != 0 && *text == ' ') {
        text++;
    }

    return text;
}

static int parse_u32(const char *text, uint32_t *out) {
    uint32_t value = 0;
    int any = 0;

    if (text == 0 || out == 0) {
        return 0;
    }

    text = skip_spaces(text);

    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text - '0');

        if (value > (0xFFFFFFFFu - digit) / 10u) {
            return 0;
        }

        value = value * 10u + digit;
        any = 1;
        text++;
    }

    *out = value;
    return any;
}

static int command_is(const char *line, const char *command) {
    return kstrcmp(line, command) == 0;
}

static int command_starts_with(const char *line, const char *prefix) {
    return kstrncmp(line, prefix, kstrlen(prefix)) == 0;
}

static void monitor_help(void) {
    console_writeln("Available commands:");
    console_writeln("  help       - show this help");
    console_writeln("  ticks      - show scheduler tick count");
    console_writeln("  threads    - show thread queues and scheduler state");
    console_writeln("  mem        - show physical memory manager stats");
    console_writeln("  heap       - show kernel heap stats");
    console_writeln("  sleep N    - sleep monitor thread for N ticks");
    console_writeln("  echo TEXT  - print TEXT");
    console_writeln("  clear      - scroll the screen down");
}

static void monitor_clear(void) {
    for (uint32_t i = 0; i < 30; ++i) {
        console_putc('\n');
    }
}

int monitor_execute_command(const char *line) {
    if (line == 0) {
        return 0;
    }

    line = skip_spaces(line);

    if (*line == '\0') {
        return 0;
    }

    if (command_is(line, "help")) {
        monitor_help();
        return 1;
    }

    if (command_is(line, "ticks")) {
        console_write("ticks: ");
        console_write_u32_dec(thread_ticks());
        console_putc('\n');
        return 1;
    }

    if (command_is(line, "threads")) {
        thread_dump_state();
        return 1;
    }

    if (command_is(line, "mem")) {
        pmm_dump_stats();
        return 1;
    }

    if (command_is(line, "heap")) {
        heap_dump_stats();
        return 1;
    }

    if (command_starts_with(line, "echo ")) {
        const char *message = line + 5;
        console_writeln(message);
        return 1;
    }

    if (command_starts_with(line, "sleep ")) {
        uint32_t ticks = 0;

        if (!parse_u32(line + 6, &ticks)) {
            console_writeln("sleep: expected decimal tick count");
            return 1;
        }

        console_write("sleeping for ");
        console_write_u32_dec(ticks);
        console_writeln(" ticks");

        thread_sleep_ticks(ticks);

        console_writeln("awake");
        return 1;
    }

    if (command_is(line, "clear")) {
        monitor_clear();
        return 1;
    }

    console_write("unknown command: ");
    console_writeln(line);
    console_writeln("type 'help' for commands");

    return 0;
}

static void monitor_thread_main(void *arg) {
    (void)arg;

    char line[MONITOR_LINE_SIZE];

    console_writeln("");
    console_writeln("Toyix kernel monitor ready.");
    console_writeln("Type 'help' for commands.");

    for (;;) {
        console_write("toyix> ");
        terminal_readline(line, sizeof(line));
        monitor_execute_command(line);
    }
}

void monitor_start(void) {
    thread_create("monitor", monitor_thread_main, 0);
    console_writeln("Monitor: monitor thread started");
}

/* ------------------------------------------------------------------------- */
/* Test                                                                       */
/* ------------------------------------------------------------------------- */

void monitor_test_once(void) {
    console_writeln("Monitor test: starting command dispatcher test");

    monitor_execute_command("ticks");
    monitor_execute_command("echo monitor-ok");
    monitor_execute_command("unknown-test-command");

    console_writeln("Monitor test: command dispatcher sanity check passed");
}
```

## Why the monitor runs in its own thread

The monitor blocks waiting for keyboard input.

That should not stop the rest of the kernel. By running it as a normal kernel thread, we get the behavior we want:

```text
monitor waits for input
  ↓
monitor thread blocks
  ↓
idle thread or other kernel threads run
  ↓
keyboard input arrives
  ↓
monitor wakes and processes command
```

This is the same model we will later use for device servers, shell processes, and user-space programs.

---

# 10. Update `kernel/kmain.c`

Add:

```c
#include "kernel/monitor.h"
#include "kernel/terminal.h"
```

Then initialize and test the terminal and monitor after keyboard and sync tests.

Here is the full updated `kernel/kmain.c`.

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
#include "kernel/heap.h"
#include "kernel/monitor.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/sync.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"
#include "kernel/vmem.h"

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

    vmem_init();
    vmem_test_once();

    heap_init(4);
    heap_test_once();

    threading_init();
    thread_test_once();

#ifdef TOYIX_TRIGGER_PAGE_FAULT
    console_writeln("Triggering test page fault at 0xC0000000...");
    volatile uint32_t *bad = (volatile uint32_t *)0xC0000000u;
    uint32_t value = *bad;
    (void)value;
#endif

    pit_init(100);
    keyboard_init();

    thread_preemption_init(2);

    console_writeln("Interrupt hardware: configured");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    interrupts_enable();

    console_writeln("Interrupts: enabled");

    console_locking_init();

    sync_test_once();
    console_lock_test_once();

    thread_preemption_test_prepare();
    thread_preemption_test_wait();

    thread_sleep_test_once();
    keyboard_buffer_test_once();

    terminal_init();
    terminal_test_once();

    monitor_init();
    monitor_test_once();
    monitor_start();

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Interactive kernel monitor is running.");
    console_writeln("Try: help, ticks, threads, mem, heap, echo hello");

    kernel_idle();
}
```

## Why `monitor_start()` happens near the end

The monitor should start after the boot tests have finished.

If we start it too early, it competes for input with test threads. Once the boot tests are done, the monitor owns interactive input.

---

# 11. Update `Makefile`

Add:

```text
build/kernel/monitor.o
build/kernel/terminal.o
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
    build/arch/x86/paging_asm.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/idle.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/sync.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the `test` target greps:

```make
test: iso
	@mkdir -p build
	@rm -f build/test.log
	@timeout 10s $(QEMU) \
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
	grep -q "Heap: initialized virtual heap with 4 page(s)" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	grep -q "Heap test: VMM-backed allocation/free sanity check passed" build/test.log
	grep -q "Threads: blocking scheduler initialized" build/test.log
	grep -q "Thread test: worker A step 0" build/test.log
	grep -q "Thread test: worker B step 0" build/test.log
	grep -q "Thread test: completed software-yield multitasking test" build/test.log
	grep -q "Threads: preemption enabled, slice ticks=2" build/test.log
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
	grep -q "Sleep test: blocking sleep sanity check passed" build/test.log
	grep -q "Keyboard: IRQ1 handler and input buffer installed" build/test.log
	grep -q "Console: output mutex enabled" build/test.log
	grep -q "Sync test: mutex/semaphore sanity check passed" build/test.log
	grep -q "Console lock test: non-interleaved line output sanity check passed" build/test.log
	grep -q "Keyboard test: blocking input-buffer sanity check passed" build/test.log
	grep -q "Terminal test: readline/backspace sanity check passed" build/test.log
	grep -q "Monitor test: command dispatcher sanity check passed" build/test.log
	grep -q "Monitor: monitor thread started" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	grep -q "VMM: initialized kernel address-space mapper" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	@echo "Boot, memory, heap, sync, terminal, and monitor smoke test passed."
```

---

# 12. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 14 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 13. Expected output

A successful boot should include:

```text
Terminal: line discipline initialized
Terminal test: starting readline test
abD
Terminal test: reader got line abD
Terminal test: readline/backspace sanity check passed
Monitor: command dispatcher initialized
Monitor test: starting command dispatcher test
ticks: ...
monitor-ok
unknown command: unknown-test-command
type 'help' for commands
Monitor test: command dispatcher sanity check passed
Monitor: monitor thread started

Toyix kernel monitor ready.
Type 'help' for commands.
toyix> Timer: observed 3 ticks
Interactive kernel monitor is running.
Try: help, ticks, threads, mem, heap, echo hello
```

The `toyix>` prompt means the monitor thread is blocked waiting for keyboard input. Because the monitor runs as a normal thread, the prompt may appear before the final boot-status lines, or on the same serial line as `Timer: observed 3 ticks`.

In QEMU, click the window and try:

```text
help
ticks
threads
mem
heap
echo hello from toyix
sleep 50
```

Because our keyboard driver still lacks Shift handling, stick to lowercase letters, digits, spaces, backspace, and Enter for now.

---

# 14. Common failures

## Failure: monitor prompt appears but typing does nothing

Likely causes:

```text
QEMU window is not focused
keyboard IRQ1 is not unmasked
keyboard driver is not installed
monitor thread is not running
keyboard_getchar_blocking() is sleeping but never woken
```

Check for:

```text
Keyboard: IRQ1 handler and input buffer installed
Monitor: monitor thread started
```

## Failure: terminal test hangs

Likely causes:

```text
terminal reader did not block correctly
keyboard_debug_inject_char() did not wake keyboard wait queue
wait queue lost wakeup
reader thread never scheduled
```

The test injects:

```text
a b c backspace D newline
```

and expects:

```text
abD
```

If it hangs, inspect the keyboard wait queue and `wait_queue_wait()` path.

## Failure: backspace removes text internally but not visually

Check that `vga_text.c` handles:

```c
if (c == '\b') {
    vga_backspace();
    return;
}
```

Also check that terminal backspace echo uses:

```c
console_write("\b \b");
```

## Failure: command parsing fails

The monitor parser is intentionally simple.

Commands must match exactly:

```text
help
ticks
threads
mem
heap
clear
```

Commands with arguments require a space:

```text
echo hello
sleep 10
```

No quoting, escaping, tabs, or uppercase handling yet.

---

# 15. What this chapter achieved

We now have an interactive kernel:

```text
keyboard IRQ
  ↓
blocking keyboard buffer
  ↓
terminal readline
  ↓
kernel monitor thread
  ↓
inspection commands
```

This is a major usability milestone.

Before this chapter, the kernel ran tests and idled.

After this chapter, you can ask the live kernel what it knows:

```text
toyix> ticks
toyix> threads
toyix> mem
toyix> heap
```

That is the beginning of a real operator/debugging interface.

---

# 16. Design limitations

The terminal is still primitive:

```text
no command history
no arrow keys
no Shift/Caps support
no tab completion
no line kill
no Ctrl+C
no terminal modes
no UTF-8
```

The monitor is also primitive:

```text
no argument tokenizer
no command table yet
no permissions
no scripting
no user/kernel boundary
```

But the structure is right. We can improve the terminal and monitor without rewriting the keyboard driver, scheduler, or console layer.

---

# 17. Next chapter

The next useful chapter is to clean up the monitor and terminal into reusable infrastructure:

```text
command table
argument parser
terminal history
Shift handling in keyboard driver
basic line editing
monitor command registration
```

After that, we can start moving toward user mode:

```text
TSS
ring 3 segments
user stack
IRET transition to user mode
syscall interrupt
first user task
```

---

# 18. Resources

- [Chapter 14 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_14)
- [Chapter 14 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_14)
- [OSDev Wiki: Command Line](https://wiki.osdev.org/Command_Line)
- [OSDev Wiki: Text UI](https://wiki.osdev.org/Text_UI)
- [OSDev Wiki: PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)

---

# 19. Closure

The kernel now has a blocking terminal line discipline and a monitor thread that accepts live commands. That gives Toyix its first interactive inspection surface and a practical place to grow debugging and operator tools.

Happy Coding!
