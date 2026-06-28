#include "toyix.h"

#define SHELL_LINE_MAX 96
#define SHELL_ARG_MAX  12
#define SHELL_JOB_MAX  8
#define SHELL_JOB_NAME_MAX 32

typedef struct shell_job {
    toyix_u32 pid;
    int active;
    char name[SHELL_JOB_NAME_MAX];
} shell_job_t;

static shell_job_t shell_jobs[SHELL_JOB_MAX];

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

static void copy_job_name(char *dest, const char *src, int dest_size) {
    int i = 0;

    if (dest == 0 || src == 0 || dest_size <= 0) {
        return;
    }

    while (i + 1 < dest_size && src[i] != '\0') {
        dest[i] = src[i];
        ++i;
    }

    dest[i] = '\0';
}

static int shell_find_job_index(toyix_u32 pid) {
    for (int i = 0; i < SHELL_JOB_MAX; ++i) {
        if (shell_jobs[i].active && shell_jobs[i].pid == pid) {
            return i;
        }
    }

    return -1;
}

static int shell_alloc_job_slot(void) {
    for (int i = 0; i < SHELL_JOB_MAX; ++i) {
        if (!shell_jobs[i].active) {
            return i;
        }
    }

    return -1;
}

static void shell_track_job(toyix_u32 pid, const char *name) {
    int index = shell_find_job_index(pid);

    if (index < 0) {
        index = shell_alloc_job_slot();
    }

    if (index < 0) {
        return;
    }

    shell_jobs[index].pid = pid;
    shell_jobs[index].active = 1;
    copy_job_name(shell_jobs[index].name, name, SHELL_JOB_NAME_MAX);
}

static void shell_forget_job(toyix_u32 pid) {
    int index = shell_find_job_index(pid);

    if (index < 0) {
        return;
    }

    shell_jobs[index].pid = 0;
    shell_jobs[index].active = 0;
    shell_jobs[index].name[0] = '\0';
}

static const char *shell_job_name(toyix_u32 pid) {
    int index = shell_find_job_index(pid);

    if (index < 0 || shell_jobs[index].name[0] == '\0') {
        return "unknown";
    }

    return shell_jobs[index].name;
}

static const char *process_state_name(toyix_u32 state) {
    switch (state) {
        case TOYIX_PROCESS_NEW:
            return "new";
        case TOYIX_PROCESS_RUNNING:
            return "running";
        case TOYIX_PROCESS_ZOMBIE:
            return "zombie";
        case TOYIX_PROCESS_DESTROYED:
            return "destroyed";
        default:
            return "unknown";
    }
}

static const char *file_type_name(toyix_u32 type) {
    switch (type) {
        case TOYIX_FILE_REGULAR:
            return "file";
        case TOYIX_FILE_DIRECTORY:
            return "directory";
        default:
            return "unknown";
    }
}

static void cmd_help(void) {
    toyix_puts("commands: help, echo, args, cat, seektest, stat, run, runbg, jobs, wait, kill, exit");
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

static void cmd_cat(int argc, char **argv) {
    char buffer[64];
    toyix_i32 fd = 0;

    if (argc != 2) {
        toyix_puts("usage: cat PATH");
        return;
    }

    fd = toyix_open(argv[1], 0);
    if (fd < 0) {
        toyix_printf("cat: could not open %s\n", argv[1]);
        return;
    }

    for (;;) {
        toyix_i32 got = toyix_read((toyix_u32)fd, buffer, sizeof(buffer));

        if (got < 0) {
            toyix_puts("cat: read failed");
            break;
        }

        if (got == 0) {
            break;
        }

        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    if (toyix_close((toyix_u32)fd) != 0) {
        toyix_puts("cat: close failed");
    }
}

static void print_chunk(const char *label, const char *buffer, toyix_i32 got) {
    toyix_printf("%s", label);

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_putchar('\n');
}

static void cmd_seektest(int argc, char **argv) {
    char buffer[16];

    if (argc != 2) {
        toyix_puts("usage: seektest PATH");
        return;
    }

    toyix_i32 fd = toyix_open(argv[1], 0);

    if (fd < 0) {
        toyix_printf("seektest: could not open %s\n", argv[1]);
        return;
    }

    toyix_i32 got = toyix_read((toyix_u32)fd, buffer, 8u);

    if (got < 0) {
        toyix_puts("seektest: first read failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    print_chunk("seektest: first read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, 0, TOYIX_SEEK_SET) < 0) {
        toyix_puts("seektest: rewind failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    got = toyix_read((toyix_u32)fd, buffer, 8u);

    if (got < 0) {
        toyix_puts("seektest: rewind read failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    print_chunk("seektest: rewind read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, 6, TOYIX_SEEK_SET) < 0) {
        toyix_puts("seektest: skip seek failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    got = toyix_read((toyix_u32)fd, buffer, 5u);

    if (got < 0) {
        toyix_puts("seektest: skip read failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    print_chunk("seektest: skip read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, -5, TOYIX_SEEK_END) < 0) {
        toyix_puts("seektest: end-relative seek failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    got = toyix_read((toyix_u32)fd, buffer, 5u);

    if (got < 0) {
        toyix_puts("seektest: end-relative read failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    print_chunk("seektest: tail read: ", buffer, got);

    toyix_close((toyix_u32)fd);
}

static void cmd_stat(int argc, char **argv) {
    toyix_stat_t stat;

    if (argc != 2) {
        toyix_puts("usage: stat PATH");
        return;
    }

    if (toyix_stat(argv[1], &stat) != 0) {
        toyix_printf("stat: could not stat %s\n", argv[1]);
        return;
    }

    toyix_printf(
        "stat: path=%s type=%s size=%u\n",
        argv[1],
        file_type_name(stat.type),
        stat.size
    );
}

static void cmd_run(int argc, char **argv) {
    const char *program_name;
    int child_argc;
    const char **child_argv;
    toyix_i32 pid;
    toyix_u32 status = 0;

    if (argc < 2) {
        toyix_puts("usage: run PROGRAM [ARGS...]");
        return;
    }

    program_name = argv[1];
    child_argc = argc - 1;
    child_argv = (const char **)&argv[1];

    pid = toyix_exec(program_name, child_argv, (toyix_u32)child_argc);
    if (pid < 0) {
        toyix_printf("run: failed to launch %s\n", program_name);
        return;
    }

    toyix_printf("shell: run %s pid=%d\n", program_name, pid);

    if (toyix_waitpid((toyix_u32)pid, &status) != 0) {
        toyix_printf("run: wait failed for pid %d\n", pid);
        toyix_puts("run: process may not be a child or may not exist");
        return;
    }

    toyix_printf("shell: %s exited code %u\n", program_name, status);
}

static void cmd_runbg(int argc, char **argv) {
    const char *program_name;
    int child_argc;
    const char **child_argv;
    toyix_i32 pid;

    if (argc < 2) {
        toyix_puts("usage: runbg PROGRAM [ARGS...]");
        return;
    }

    if (shell_alloc_job_slot() < 0) {
        toyix_puts("runbg: job table full");
        return;
    }

    program_name = argv[1];
    child_argc = argc - 1;
    child_argv = (const char **)&argv[1];

    pid = toyix_exec(program_name, child_argv, (toyix_u32)child_argc);
    if (pid < 0) {
        toyix_printf("runbg: failed to launch %s\n", program_name);
        return;
    }

    shell_track_job((toyix_u32)pid, program_name);
    toyix_printf("shell: runbg %s pid=%d\n", program_name, pid);
}

static void cmd_jobs(void) {
    int found = 0;

    toyix_puts("shell jobs:");

    for (int i = 0; i < SHELL_JOB_MAX; ++i) {
        toyix_procinfo_t info;

        if (!shell_jobs[i].active) {
            continue;
        }

        if (toyix_procinfo(shell_jobs[i].pid, &info) != 0) {
            shell_forget_job(shell_jobs[i].pid);
            continue;
        }

        found = 1;
        toyix_printf(
            "  pid=%u parent=%u name=%s state=%s",
            info.pid,
            info.parent_pid,
            shell_jobs[i].name,
            process_state_name(info.state)
        );

        if (info.exited) {
            toyix_printf(" code=%u", info.exit_code);
        }

        if (info.kill_requested) {
            toyix_write_str(" kill=yes");
        }

        toyix_putchar('\n');
    }

    if (!found) {
        toyix_puts("  none");
    }
}

static void cmd_wait(int argc, char **argv) {
    toyix_i32 parsed = 0;
    toyix_u32 pid = 0;
    toyix_u32 status = 0;

    if (argc != 2) {
        toyix_puts("usage: wait PID");
        return;
    }

    if (!toyix_atoi(argv[1], &parsed) || parsed <= 0) {
        toyix_puts("wait: expected positive PID");
        return;
    }

    pid = (toyix_u32)parsed;

    if (toyix_waitpid(pid, &status) != 0) {
        toyix_printf("wait: failed for pid %u\n", pid);
        return;
    }

    toyix_printf(
        "shell: wait pid=%u name=%s code=%u\n",
        pid,
        shell_job_name(pid),
        status
    );

    shell_forget_job(pid);
}

static void cmd_kill(int argc, char **argv) {
    toyix_i32 parsed = 0;
    toyix_u32 pid = 0;

    if (argc != 2) {
        toyix_puts("usage: kill PID");
        return;
    }

    if (!toyix_atoi(argv[1], &parsed) || parsed <= 0) {
        toyix_puts("kill: expected positive PID");
        return;
    }

    pid = (toyix_u32)parsed;

    if (toyix_kill(pid) != 0) {
        toyix_printf("kill: failed for pid %u\n", pid);
        return;
    }

    toyix_printf("shell: kill requested pid=%u\n", pid);
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

        if (toyix_streq(cmd_argv[0], "cat")) {
            cmd_cat(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "seektest")) {
            cmd_seektest(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "stat")) {
            cmd_stat(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "run")) {
            cmd_run(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "runbg")) {
            cmd_runbg(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "jobs")) {
            cmd_jobs();
            continue;
        }

        if (toyix_streq(cmd_argv[0], "wait")) {
            cmd_wait(cmd_argc, cmd_argv);
            continue;
        }

        if (toyix_streq(cmd_argv[0], "kill")) {
            cmd_kill(cmd_argc, cmd_argv);
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
