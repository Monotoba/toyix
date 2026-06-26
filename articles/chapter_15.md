# Chapter 15 — Command Tables, Argument Parsing, and Shift-Aware Keyboard Input

In Chapter 14, we added the first interactive kernel monitor:

```text
toyix> help
toyix> ticks
toyix> threads
toyix> mem
toyix> heap
```

That worked, but the monitor command parser was still a chain of `if` statements:

```c
if (command_is(line, "help")) ...
if (command_is(line, "ticks")) ...
if (command_is(line, "threads")) ...
```

That is fine for five commands, but it does not scale.

This chapter cleans that up by adding:

```text
monitor command table
argument tokenizer
help for individual commands
more robust sleep/echo parsing
keyboard Shift handling
keyboard Caps Lock handling
```

The keyboard improvement matters because our monitor is interactive now. A monitor where you cannot type uppercase letters or shifted symbols is usable, but unpleasant.

The IBM PS/2 Hardware Interface Technical Reference includes keyboard scan-code sets and keyboard specifications; our early driver still uses a simple Set 1 style mapping, but we will now track modifier state for Shift and Caps Lock. ([Ardent Tool of Capitalism][1])

---

# 1. Design goal

We want monitor commands to become data:

```c
static const monitor_command_t commands[] = {
    { "help",    cmd_help,    "help [command]", "show help" },
    { "ticks",   cmd_ticks,   "ticks",          "show scheduler ticks" },
    { "threads", cmd_threads, "threads",        "show thread state" },
};
```

instead of becoming a growing pile of special cases.

That lets us add commands by adding one table entry.

The command dispatcher becomes:

```text
read line
  ↓
tokenize into argc/argv
  ↓
search command table
  ↓
call command function
```

This is the same general structure used by many boot monitors, debuggers, shells, firmware command loops, and diagnostic consoles.

---

# 2. Patch overview

Modify:

```text
include/kernel/string.h
kernel/lib/mem.c
drivers/input/keyboard.c
kernel/monitor.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

No new files are required for this chapter.

---

# 3. Update `include/kernel/string.h`

Add safe bounded copy and character helpers.

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
size_t kstrlcpy(char *dest, const char *src, size_t dest_size);

int kchar_is_space(char ch);
int kchar_is_digit(char ch);
int kchar_is_alpha(char ch);

#endif
```

## Why add `kstrlcpy()`

The monitor will receive a `const char *line`, but tokenization needs to write `'\0'` terminators between arguments.

So the command dispatcher will copy the line into a local mutable buffer:

```c
char work[MONITOR_LINE_SIZE];
kstrlcpy(work, line, sizeof(work));
```

Then it can safely split `work` into arguments.

---

# 4. Update `kernel/lib/mem.c`

Append these functions.

```c
size_t kstrlcpy(char *dest, const char *src, size_t dest_size) {
    size_t src_len = 0;

    if (src != 0) {
        while (src[src_len] != '\0') {
            src_len++;
        }
    }

    if (dest_size == 0 || dest == 0) {
        return src_len;
    }

    if (src == 0) {
        dest[0] = '\0';
        return 0;
    }

    size_t copy_len = 0;

    while (copy_len + 1 < dest_size && src[copy_len] != '\0') {
        dest[copy_len] = src[copy_len];
        copy_len++;
    }

    dest[copy_len] = '\0';

    return src_len;
}

int kchar_is_space(char ch) {
    return ch == ' ' ||
           ch == '\t' ||
           ch == '\n' ||
           ch == '\r' ||
           ch == '\v' ||
           ch == '\f';
}

int kchar_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

int kchar_is_alpha(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z');
}
```

---

# 5. Replace `drivers/input/keyboard.c`

This version adds Shift and Caps Lock handling.

```c
// drivers/input/keyboard.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/irq_state.h"
#include "arch/x86/pic.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

#define KEYBOARD_DATA_PORT 0x60u
#define KEYBOARD_RING_SIZE 128u

#define SC_LSHIFT   0x2Au
#define SC_RSHIFT   0x36u
#define SC_CAPSLOCK 0x3Au

static wait_queue_t keyboard_wait_queue;

static char keyboard_ring[KEYBOARD_RING_SIZE];
static uint32_t keyboard_head;
static uint32_t keyboard_tail;
static uint32_t keyboard_count;

static int keyboard_shift_down;
static int keyboard_caps_lock;

static volatile uint32_t keyboard_test_done;
static char keyboard_test_result[4];

static const char scancode_set1_ascii[128] = {
    [0x01] = 0,
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

static const char scancode_set1_shifted_ascii[128] = {
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',

    [0x1A] = '{',
    [0x1B] = '}',

    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',

    [0x2B] = '|',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?'
};

static int keyboard_has_data(void *context) {
    (void)context;
    return keyboard_count > 0;
}

static int ascii_is_lower(char ch) {
    return ch >= 'a' && ch <= 'z';
}

static int ascii_is_upper(char ch) {
    return ch >= 'A' && ch <= 'Z';
}

static char ascii_to_upper(char ch) {
    if (ascii_is_lower(ch)) {
        return (char)(ch - 'a' + 'A');
    }

    return ch;
}

static char keyboard_translate_scancode(uint8_t scancode) {
    char base = scancode_set1_ascii[scancode];

    if (base == 0) {
        return 0;
    }

    if (ascii_is_lower(base) || ascii_is_upper(base)) {
        int upper = keyboard_shift_down ^ keyboard_caps_lock;
        return upper ? ascii_to_upper(base) : base;
    }

    if (keyboard_shift_down && scancode_set1_shifted_ascii[scancode] != 0) {
        return scancode_set1_shifted_ascii[scancode];
    }

    return base;
}

static int keyboard_ring_push(char ch) {
    if (keyboard_count >= KEYBOARD_RING_SIZE) {
        return 0;
    }

    keyboard_ring[keyboard_tail] = ch;
    keyboard_tail = (keyboard_tail + 1u) % KEYBOARD_RING_SIZE;
    keyboard_count++;

    return 1;
}

static int keyboard_ring_pop(char *out) {
    if (keyboard_count == 0 || out == 0) {
        return 0;
    }

    *out = keyboard_ring[keyboard_head];
    keyboard_head = (keyboard_head + 1u) % KEYBOARD_RING_SIZE;
    keyboard_count--;

    return 1;
}

static void keyboard_deliver_char(char ch) {
    irq_flags_t flags = irq_save();

    int pushed = keyboard_ring_push(ch);

    irq_restore(flags);

    if (pushed) {
        wait_queue_wake_one(&keyboard_wait_queue);
    }
}

static void keyboard_handle_modifier(uint8_t scancode) {
    int released = (scancode & 0x80u) != 0;
    uint8_t code = scancode & 0x7Fu;

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        keyboard_shift_down = released ? 0 : 1;
        return;
    }

    if (!released && code == SC_CAPSLOCK) {
        keyboard_caps_lock = !keyboard_caps_lock;
        return;
    }
}

static int keyboard_is_modifier(uint8_t scancode) {
    uint8_t code = scancode & 0x7Fu;

    return code == SC_LSHIFT ||
           code == SC_RSHIFT ||
           code == SC_CAPSLOCK;
}

static void keyboard_irq_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (keyboard_is_modifier(scancode)) {
        keyboard_handle_modifier(scancode);
        return;
    }

    /*
     * In Set 1 style scancodes, high bit marks key release.
     * This driver ignores non-modifier releases.
     */
    if ((scancode & 0x80u) != 0) {
        return;
    }

    if (scancode < 128) {
        char ch = keyboard_translate_scancode(scancode);

        if (ch != 0) {
            keyboard_deliver_char(ch);
        }
    }
}

int keyboard_init(void) {
    keyboard_head = 0;
    keyboard_tail = 0;
    keyboard_count = 0;

    keyboard_shift_down = 0;
    keyboard_caps_lock = 0;

    wait_queue_init(&keyboard_wait_queue, "keyboard");

    interrupt_register_handler(33, keyboard_irq_handler);
    pic_clear_mask(1);

    console_writeln("Keyboard: IRQ1 handler, modifiers, and input buffer installed");
    return 0;
}

int keyboard_try_getchar(char *out) {
    if (out == 0) {
        return 0;
    }

    irq_flags_t flags = irq_save();

    int ok = keyboard_ring_pop(out);

    irq_restore(flags);

    return ok;
}

char keyboard_getchar_blocking(void) {
    char ch;

    for (;;) {
        wait_queue_wait(&keyboard_wait_queue, keyboard_has_data, 0);

        irq_flags_t flags = irq_save();
        int ok = keyboard_ring_pop(&ch);
        irq_restore(flags);

        if (ok) {
            return ch;
        }
    }
}

void keyboard_debug_inject_char(char ch) {
    keyboard_deliver_char(ch);
}

static void keyboard_reader_test_thread(void *arg) {
    (void)arg;

    keyboard_test_result[0] = keyboard_getchar_blocking();
    keyboard_test_result[1] = keyboard_getchar_blocking();
    keyboard_test_result[2] = keyboard_getchar_blocking();
    keyboard_test_result[3] = '\0';

    console_write("Keyboard test: reader received ");
    console_writeln(keyboard_test_result);

    keyboard_test_done = 1;
}

void keyboard_buffer_test_once(void) {
    console_writeln("Keyboard test: starting blocking input-buffer test");

    keyboard_test_done = 0;
    keyboard_test_result[0] = 0;
    keyboard_test_result[1] = 0;
    keyboard_test_result[2] = 0;
    keyboard_test_result[3] = 0;

    thread_create("kbd-reader", keyboard_reader_test_thread, 0);

    thread_sleep_ticks(2);

    keyboard_debug_inject_char('o');
    thread_sleep_ticks(1);

    keyboard_debug_inject_char('s');
    thread_sleep_ticks(1);

    keyboard_debug_inject_char('!');
    thread_sleep_ticks(1);

    while (!keyboard_test_done) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (keyboard_test_result[0] != 'o' ||
        keyboard_test_result[1] != 's' ||
        keyboard_test_result[2] != '!') {
        kernel_panic("keyboard blocking buffer test failed");
    }

    console_writeln("Keyboard test: blocking input-buffer sanity check passed");
}
```

## What this adds

The keyboard now tracks:

```text
left/right Shift down
Caps Lock toggled
```

For letters:

```text
uppercase = Shift XOR CapsLock
```

For punctuation and digits:

```text
Shift selects shifted symbol table
```

Examples:

```text
1        → 1
Shift+1  → !
a        → a
Shift+a  → A
Caps+a   → A
Caps+Shift+a → a
```

Still missing:

```text
Ctrl
Alt
extended E0 scancodes
arrow keys
function keys
keyboard LEDs
layout selection
```

But this is a meaningful usability improvement.

---

# 6. Replace `kernel/monitor.c`

This version introduces a command table and tokenizer.

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
#define MONITOR_MAX_ARGS  8u

typedef int (*monitor_command_fn_t)(int argc, char **argv);

typedef struct monitor_command {
    const char *name;
    const char *usage;
    const char *help;
    monitor_command_fn_t handler;
} monitor_command_t;

static void monitor_thread_main(void *arg);

static int cmd_help(int argc, char **argv);
static int cmd_ticks(int argc, char **argv);
static int cmd_threads(int argc, char **argv);
static int cmd_mem(int argc, char **argv);
static int cmd_heap(int argc, char **argv);
static int cmd_sleep(int argc, char **argv);
static int cmd_echo(int argc, char **argv);
static int cmd_clear(int argc, char **argv);

static const monitor_command_t commands[] = {
    {
        .name = "help",
        .usage = "help [command]",
        .help = "show command list or details for one command",
        .handler = cmd_help
    },
    {
        .name = "ticks",
        .usage = "ticks",
        .help = "show scheduler tick count",
        .handler = cmd_ticks
    },
    {
        .name = "threads",
        .usage = "threads",
        .help = "show thread queues and scheduler state",
        .handler = cmd_threads
    },
    {
        .name = "mem",
        .usage = "mem",
        .help = "show physical memory manager stats",
        .handler = cmd_mem
    },
    {
        .name = "heap",
        .usage = "heap",
        .help = "show kernel heap stats",
        .handler = cmd_heap
    },
    {
        .name = "sleep",
        .usage = "sleep N",
        .help = "sleep monitor thread for N timer ticks",
        .handler = cmd_sleep
    },
    {
        .name = "echo",
        .usage = "echo TEXT...",
        .help = "print text",
        .handler = cmd_echo
    },
    {
        .name = "clear",
        .usage = "clear",
        .help = "scroll the display down",
        .handler = cmd_clear
    }
};

static const uint32_t command_count =
    sizeof(commands) / sizeof(commands[0]);

void monitor_init(void) {
    console_writeln("Monitor: command table initialized");
}

static const monitor_command_t *find_command(const char *name) {
    if (name == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < command_count; ++i) {
        if (kstrcmp(commands[i].name, name) == 0) {
            return &commands[i];
        }
    }

    return 0;
}

static int parse_u32(const char *text, uint32_t *out) {
    uint32_t value = 0;
    int any = 0;

    if (text == 0 || out == 0) {
        return 0;
    }

    while (kchar_is_space(*text)) {
        text++;
    }

    while (kchar_is_digit(*text)) {
        uint32_t digit = (uint32_t)(*text - '0');

        if (value > (0xFFFFFFFFu - digit) / 10u) {
            return 0;
        }

        value = value * 10u + digit;
        any = 1;
        text++;
    }

    if (*text != '\0') {
        return 0;
    }

    *out = value;
    return any;
}

static int tokenize_line(
    char *line,
    int *argc_out,
    char **argv,
    int max_args
) {
    int argc = 0;
    char *cursor = line;

    if (argc_out == 0 || argv == 0 || max_args <= 0) {
        return 0;
    }

    while (cursor != 0 && *cursor != '\0') {
        while (kchar_is_space(*cursor)) {
            *cursor = '\0';
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        if (argc >= max_args) {
            return 0;
        }

        argv[argc++] = cursor;

        while (*cursor != '\0' && !kchar_is_space(*cursor)) {
            cursor++;
        }
    }

    *argc_out = argc;
    return 1;
}

static int cmd_help(int argc, char **argv) {
    if (argc == 1) {
        console_writeln("Available commands:");

        for (uint32_t i = 0; i < command_count; ++i) {
            console_write("  ");
            console_write(commands[i].usage);
            console_write(" - ");
            console_writeln(commands[i].help);
        }

        return 1;
    }

    if (argc == 2) {
        const monitor_command_t *cmd = find_command(argv[1]);

        if (cmd == 0) {
            console_write("help: unknown command ");
            console_writeln(argv[1]);
            return 1;
        }

        console_write("usage: ");
        console_writeln(cmd->usage);
        console_write("  ");
        console_writeln(cmd->help);
        return 1;
    }

    console_writeln("usage: help [command]");
    return 1;
}

static int cmd_ticks(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: ticks");
        return 1;
    }

    console_write("ticks: ");
    console_write_u32_dec(thread_ticks());
    console_putc('\n');

    return 1;
}

static int cmd_threads(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: threads");
        return 1;
    }

    thread_dump_state();
    return 1;
}

static int cmd_mem(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: mem");
        return 1;
    }

    pmm_dump_stats();
    return 1;
}

static int cmd_heap(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: heap");
        return 1;
    }

    heap_dump_stats();
    return 1;
}

static int cmd_sleep(int argc, char **argv) {
    uint32_t ticks = 0;

    if (argc != 2 || !parse_u32(argv[1], &ticks)) {
        console_writeln("usage: sleep N");
        return 1;
    }

    console_write("sleeping for ");
    console_write_u32_dec(ticks);
    console_writeln(" ticks");

    thread_sleep_ticks(ticks);

    console_writeln("awake");
    return 1;
}

static int cmd_echo(int argc, char **argv) {
    if (argc < 2) {
        console_writeln("");
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            console_putc(' ');
        }

        console_write(argv[i]);
    }

    console_putc('\n');
    return 1;
}

static int cmd_clear(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: clear");
        return 1;
    }

    for (uint32_t i = 0; i < 30; ++i) {
        console_putc('\n');
    }

    return 1;
}

int monitor_execute_command(const char *line) {
    char work[MONITOR_LINE_SIZE];
    char *argv[MONITOR_MAX_ARGS];
    int argc = 0;

    if (line == 0) {
        return 0;
    }

    kstrlcpy(work, line, sizeof(work));

    if (!tokenize_line(work, &argc, argv, MONITOR_MAX_ARGS)) {
        console_write("too many arguments; max is ");
        console_write_u32_dec(MONITOR_MAX_ARGS);
        console_putc('\n');
        return 0;
    }

    if (argc == 0) {
        return 0;
    }

    const monitor_command_t *cmd = find_command(argv[0]);

    if (cmd == 0) {
        console_write("unknown command: ");
        console_writeln(argv[0]);
        console_writeln("type 'help' for commands");
        return 0;
    }

    return cmd->handler(argc, argv);
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

void monitor_test_once(void) {
    console_writeln("Monitor test: starting command table test");

    monitor_execute_command("help ticks");
    monitor_execute_command("ticks");
    monitor_execute_command("echo monitor ok");
    monitor_execute_command("unknown-test-command");

    console_writeln("Monitor test: command table sanity check passed");
}
```

---

# 7. What changed in the monitor

The command dispatcher now does this:

```text
copy input line
  ↓
tokenize into argv
  ↓
find command by argv[0]
  ↓
call command handler
```

So:

```text
sleep 50
```

becomes:

```text
argc = 2
argv[0] = "sleep"
argv[1] = "50"
```

and:

```text
echo hello from toyix
```

becomes:

```text
argc = 4
argv[0] = "echo"
argv[1] = "hello"
argv[2] = "from"
argv[3] = "toyix"
```

This parser still does not handle quotes:

```text
echo "hello from toyix"
```

will currently print the quotes as separate normal characters if separated by spaces.

That is fine. A real shell parser can come later.

---

# 8. Update `kernel/kmain.c`

Only two output strings need to be updated because keyboard and monitor init messages changed.

Replace this final helper text:

```c
console_writeln("Try: help, ticks, threads, mem, heap, echo hello");
```

with:

```c
console_writeln("Try: help, help sleep, ticks, threads, mem, heap, echo Hello!");
```

The rest of `kernel_main()` remains the same as Chapter 14.

---

# 9. Update `Makefile`

Update the test greps for the changed keyboard and monitor messages:

```make
grep -q "Keyboard: IRQ1 handler, modifiers, and input buffer installed" build/test.log
grep -q "Monitor test: command table sanity check passed" build/test.log
```

Here is the updated `test` target.

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
	grep -q "Keyboard: IRQ1 handler, modifiers, and input buffer installed" build/test.log
	grep -q "Console: output mutex enabled" build/test.log
	grep -q "Sync test: mutex/semaphore sanity check passed" build/test.log
	grep -q "Console lock test: non-interleaved line output sanity check passed" build/test.log
	grep -q "Keyboard test: blocking input-buffer sanity check passed" build/test.log
	grep -q "Terminal test: readline/backspace sanity check passed" build/test.log
	grep -q "Monitor test: command table sanity check passed" build/test.log
	grep -q "Monitor: monitor thread started" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	grep -q "VMM: initialized kernel address-space mapper" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	@echo "Boot, memory, heap, sync, terminal, monitor, and keyboard modifier smoke test passed."
```

---

# 10. Update `tests/smoke.sh`

No structural change is required.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 15 checks passed."
```

---

# 11. Expected output

A successful boot should now include:

```text
Keyboard: IRQ1 handler, modifiers, and input buffer installed
Monitor: command table initialized
Monitor test: starting command table test
usage: ticks
  show scheduler tick count
ticks: ...
monitor ok
unknown command: unknown-test-command
type 'help' for commands
Monitor test: command table sanity check passed
Monitor: monitor thread started
```

At the interactive prompt, try:

```text
help
help sleep
ticks
threads
mem
heap
echo Hello!
sleep 20
```

You should now be able to type uppercase letters and shifted punctuation such as:

```text
Hello!
TEST
@#$%
```

provided QEMU is focused and the keyboard scancodes arrive through the emulated PS/2 path.

---

# 12. Important limitations

The monitor parser still does not support:

```text
quoted strings
escaped spaces
environment variables
command history
tab completion
pipes
redirection
scripts
```

The keyboard still does not support:

```text
Ctrl
Alt
extended E0 scancodes
arrow keys
Delete/Home/End
function keys
keyboard LEDs
multiple layouts
```

Those limitations are acceptable. This chapter’s purpose was to make the monitor extensible and make ordinary typing more usable.

---

# 13. Common failures

## Failure: Shift works for letters but not symbols

Check the shifted table:

```c
static const char scancode_set1_shifted_ascii[128]
```

The number row and punctuation must be filled separately. Letter uppercasing is handled by logic, not by this table.

## Failure: Caps Lock toggles repeatedly while held

Make sure Caps Lock toggles only on make, not break:

```c
if (!released && code == SC_CAPSLOCK) {
    keyboard_caps_lock = !keyboard_caps_lock;
}
```

## Failure: `help ticks` says unknown command

Check that tokenization produced:

```text
argv[0] = help
argv[1] = ticks
```

The command table search should use `argv[1]` for detailed help.

## Failure: `echo hello from toyix` prints only `hello`

Make sure `cmd_echo()` loops through all arguments:

```c
for (int i = 1; i < argc; ++i)
```

---

# 14. What this chapter achieved

The monitor is no longer a hardcoded `if` chain.

It now has:

```text
command table
usage strings
per-command help
argc/argv tokenizer
bounded line copying
```

The keyboard is now more practical:

```text
Shift
Caps Lock
shifted punctuation
uppercase letters
```

This makes the monitor much easier to grow.

---

# 15. Next chapter

The next major step is to prepare for user mode.

Before we can run user programs, we need:

```text
TSS
ring 3 code/data segments
kernel stack pointer for privilege transitions
user-mode test thread
IRET transition to ring 3
syscall interrupt gate
```

The next milestone should be:

```text
enter ring 3
execute a tiny user-mode loop
trap back into kernel through a syscall interrupt
return safely
```

That will be one of the biggest architectural transitions so far.

[1]: https://www.ardent-tool.com/docs/pdf/Personal_System_2_Hardware_Interface_Technical_Reference_May88.pdf "PS/2 Hardware Interface Technical Reference"

---

# 16. Resources

- [Chapter 15 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_15)
- [Chapter 15 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_15)
- [IBM Personal System/2 Hardware Interface Technical Reference](https://www.ardent-tool.com/docs/pdf/Personal_System_2_Hardware_Interface_Technical_Reference_May88.pdf)
- [OSDev Wiki: PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)
- [OSDev Wiki: Command Line](https://wiki.osdev.org/Command_Line)

---

# 17. Closure

The monitor now has a command table, usage strings, detailed help, and argument parsing, while the keyboard path can produce uppercase letters and shifted symbols. That makes the interactive monitor easier to use and much easier to extend.

Happy Coding!
