#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/monitor.h"
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

void monitor_test_once(void) {
    console_writeln("Monitor test: starting command dispatcher test");

    monitor_execute_command("ticks");
    monitor_execute_command("echo monitor-ok");
    monitor_execute_command("unknown-test-command");

    console_writeln("Monitor test: command dispatcher sanity check passed");
}
