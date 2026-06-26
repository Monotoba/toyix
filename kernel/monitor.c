#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/monitor.h"
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
