#include "toyix.h"

#define SHELL_LINE_MAX 96
#define SHELL_ARG_MAX  12

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p != '\0') {
        while (toyix_isspace(*p)) {
            ++p;
        }

        if (*p == '\0') {
            break;
        }

        if (argc >= max_args) {
            break;
        }

        argv[argc++] = p;

        while (*p != '\0' && !toyix_isspace(*p)) {
            ++p;
        }

        if (*p != '\0') {
            *p = '\0';
            ++p;
        }
    }

    return argc;
}

static void cmd_help(void) {
    toyix_puts("commands: help, echo, args, exit");
}

static void cmd_echo(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; ++i) {
        if (i > 1) {
            toyix_putchar(' ');
        }
        toyix_write_str(argv[i]);
    }

    toyix_putchar('\n');
}

static void cmd_args(int argc, char **argv) {
    int i;

    toyix_printf("argc=%d\n", argc);
    for (i = 0; i < argc; ++i) {
        toyix_printf("argv[%d]=%s\n", i, argv[i]);
    }
}

static int cmd_exit(int argc, char **argv, int *exit_requested) {
    toyix_i32 code = 0;

    if (argc > 2) {
        toyix_puts("usage: exit [CODE]");
        return 0;
    }

    if (argc == 2) {
        if (!toyix_atoi(argv[1], &code)) {
            toyix_puts("exit: expected numeric code");
            return 0;
        }
    }

    *exit_requested = 1;
    return (int)code;
}

static void print_startup_args(int argc, char **argv) {
    int i;

    toyix_printf("shell: startup argc=%d\n", argc);
    for (i = 0; i < argc; ++i) {
        toyix_printf("shell: argv[%d]=%s\n", i, argv[i]);
    }
}

int main(int argc, char **argv) {
    char line[SHELL_LINE_MAX];
    char *cmd_argv[SHELL_ARG_MAX];

    toyix_puts("shell: Toyix user shell");
    print_startup_args(argc, argv);

    for (;;) {
        int cmd_argc;

        toyix_write_str("ush> ");

        if (toyix_readline(line, sizeof(line)) < 0) {
            toyix_puts("shell: read failed");
            return 1;
        }

        cmd_argc = tokenize(line, cmd_argv, SHELL_ARG_MAX);
        if (cmd_argc == 0) {
            continue;
        }

        if (toyix_streq(cmd_argv[0], "help")) {
            cmd_help();
            continue;
        }

        if (toyix_streq(cmd_argv[0], "echo")) {
            cmd_echo(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "args")) {
            cmd_args(argc, argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "exit")) {
            int exit_requested = 0;
            int code = cmd_exit(cmd_argc, cmd_argv, &exit_requested);

            if (exit_requested) {
                return code;
            }

            continue;
        }

        toyix_printf("unknown command: %s\n", cmd_argv[0]);
        toyix_puts("type 'help'");
    }
}
