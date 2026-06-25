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

            continue;
        }
    }
}

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

    thread_sleep_ticks(2);

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
