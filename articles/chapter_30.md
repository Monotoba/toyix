# Chapter 30 — First User-Mode Shell

In Chapter 29, we added the first shared userland runtime.

This chapter extends that runtime with formatted output and shell-oriented helpers, then uses it to build the first real long-running user-mode program:

```c
toyix_printf("counter: argv[%d]=%s\n", i, argv[i]);
```

This first shell will run entirely in ring 3. It will read lines from stdin, parse commands, and execute a few built-in commands.

It will **not** launch other programs yet, because we do not have a user-facing `exec` syscall. That comes later.

For now, the shell supports:

```text
help
echo TEXT...
args
exit [CODE]
```

The milestone output will include:

```text
shell: Toyix user shell
ush> help
commands: help, echo, args, exit
ush> echo hello from shell
hello from shell
ush> args
argc=3
argv[0]=shell
argv[1]=alpha
argv[2]=beta
ush> exit 7
Syscall: process shell pid=... exited code 7
```

---

## 1. What this chapter adds

Add:

```text
user/
└── shell.c
```

Modify:

```text
user/include/toyix.h
user/lib/toyix.c
kernel/program.c
Makefile
tests/smoke.sh
```

No kernel syscall changes are required.

The new shell still uses the existing syscalls:

```text
SYS_READ
SYS_WRITE
SYS_EXIT
```

---

## 2. Update `user/include/toyix.h`

Add a few small userland helpers.

Replace the header with this version:

```c
#ifndef TOYIX_USER_TOYIX_H
#define TOYIX_USER_TOYIX_H

#include "toyix_syscall.h"

typedef unsigned int toyix_size_t;
typedef __builtin_va_list toyix_va_list;

toyix_size_t toyix_strlen(const char *text);

void toyix_putchar(char ch);
void toyix_write_str(const char *text);
void toyix_puts(const char *text);

void toyix_write_uint(toyix_u32 value);
void toyix_write_int(toyix_i32 value);
void toyix_write_hex(toyix_u32 value);

int toyix_streq(const char *a, const char *b);
int toyix_strcmp(const char *a, const char *b);
int toyix_isspace(char ch);

int toyix_atoi(const char *text, toyix_i32 *out_value);

toyix_i32 toyix_readline(char *buffer, toyix_u32 size);

#define toyix_va_start(ap, last) __builtin_va_start(ap, last)
#define toyix_va_end(ap) __builtin_va_end(ap)
#define toyix_va_arg(ap, type) __builtin_va_arg(ap, type)

void toyix_vprintf(const char *format, toyix_va_list ap);
void toyix_printf(const char *format, ...);

#endif
```

The new helpers are:

```c
int toyix_strcmp(const char *a, const char *b);
int toyix_isspace(char ch);
int toyix_atoi(const char *text, toyix_i32 *out_value);
toyix_i32 toyix_readline(char *buffer, toyix_u32 size);
```

`toyix_readline()` wraps `toyix_read()` and adds a terminating `'\0'`.

---

## 3. Update `user/lib/toyix.c`

Add these functions to the existing Chapter 29 runtime file.

The runtime also needs `toyix_write_hex()`, `toyix_vprintf()`, and `toyix_printf()` so the shell can format output without libc.

```c
int toyix_strcmp(const char *a, const char *b) {
    if (a == 0 && b == 0) {
        return 0;
    }

    if (a == 0) {
        return -1;
    }

    if (b == 0) {
        return 1;
    }

    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return (int)((unsigned char)*a) -
                   (int)((unsigned char)*b);
        }

        a++;
        b++;
    }

    return (int)((unsigned char)*a) -
           (int)((unsigned char)*b);
}

int toyix_isspace(char ch) {
    return ch == ' ' ||
           ch == '\t' ||
           ch == '\r' ||
           ch == '\n';
}

int toyix_atoi(const char *text, toyix_i32 *out_value) {
    if (text == 0 || out_value == 0) {
        return 0;
    }

    int negative = 0;
    toyix_i32 value = 0;

    while (toyix_isspace(*text)) {
        text++;
    }

    if (*text == '-') {
        negative = 1;
        text++;
    } else if (*text == '+') {
        text++;
    }

    if (*text < '0' || *text > '9') {
        return 0;
    }

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text++;
    }

    while (toyix_isspace(*text)) {
        text++;
    }

    if (*text != '\0') {
        return 0;
    }

    if (negative) {
        value = -value;
    }

    *out_value = value;
    return 1;
}

toyix_i32 toyix_readline(char *buffer, toyix_u32 size) {
    if (buffer == 0 || size == 0) {
        return -1;
    }

    if (size == 1) {
        buffer[0] = '\0';
        return 0;
    }

    toyix_i32 got = toyix_read(FD_STDIN, buffer, size - 1u);

    if (got < 0) {
        buffer[0] = '\0';
        return got;
    }

    if ((toyix_u32)got >= size) {
        got = (toyix_i32)(size - 1u);
    }

    buffer[got] = '\0';
    return got;
}
```

Why `toyix_readline()` matters:

```text
SYS_READ returns bytes
C string code wants '\0'
```

The kernel terminal layer already consumes the newline. `toyix_readline()` simply makes the returned line safe for C string parsing.

---

## 4. Add `user/shell.c`

```c
// user/shell.c
#include "toyix.h"

#define SHELL_LINE_MAX 96
#define SHELL_ARG_MAX  12

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p != '\0') {
        while (toyix_isspace(*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        if (argc >= max_args) {
            break;
        }

        argv[argc++] = p;

        while (*p != '\0' && !toyix_isspace(*p)) {
            p++;
        }

        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }

    return argc;
}

static void cmd_help(void) {
    toyix_puts("commands: help, echo, args, exit");
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            toyix_putchar(' ');
        }

        toyix_write_str(argv[i]);
    }

    toyix_putchar('\n');
}

static void cmd_args(int argc, char **argv) {
    toyix_printf("argc=%d\n", argc);

    for (int i = 0; i < argc; ++i) {
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
    return code;
}

static void print_startup_args(int argc, char **argv) {
    toyix_printf("shell: startup argc=%d\n", argc);

    for (int i = 0; i < argc; ++i) {
        toyix_printf("shell: argv[%d]=%s\n", i, argv[i]);
    }
}

int main(int argc, char **argv) {
    char line[SHELL_LINE_MAX];
    char *cmd_argv[SHELL_ARG_MAX];

    toyix_puts("shell: Toyix user shell");
    print_startup_args(argc, argv);

    for (;;) {
        toyix_write_str("ush> ");

        toyix_i32 got = toyix_readline(line, sizeof(line));

        if (got < 0) {
            toyix_puts("shell: read failed");
            return 1;
        }

        int cmd_argc = tokenize(line, cmd_argv, SHELL_ARG_MAX);

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
```

This shell is intentionally small.

It does not support quotes, pipes, redirects, variables, history, or launching other programs.

It proves the important thing:

```text
a normal user-mode C program can run an interactive command loop
```

---

## 5. Update `Makefile`

Add `shell` to the user program list.

Change:

```make
USER_PROGRAMS := demo counter
```

to:

```make
USER_PROGRAMS := demo counter shell
```

Because Chapter 28 added pattern rules, that is enough to build:

```text
build/user/shell.o
build/user/shell.elf
build/user/shell.map
build/user/shell_elf_blob.o
```

The kernel `OBJS` list already includes:

```make
$(USER_BLOBS)
```

so `shell_elf_blob.o` will automatically be linked into the kernel.

---

## 6. Update `kernel/program.c`

Declare the embedded shell symbols.

Near the existing declarations:

```c
DECLARE_EMBEDDED_PROGRAM(demo);
DECLARE_EMBEDDED_PROGRAM(counter);
```

add:

```c
DECLARE_EMBEDDED_PROGRAM(shell);
```

Then add the registry entry:

```c
EMBEDDED_PROGRAM(
    shell,
    "shell",
    "interactive user-mode shell"
)
```

The registry should now look like:

```c
static const embedded_program_t programs[] = {
    EMBEDDED_PROGRAM(
        demo,
        "demo",
        "interactive stdin/stdout demo"
    ),
    EMBEDDED_PROGRAM(
        counter,
        "counter",
        "background-safe counter demo"
    ),
    EMBEDDED_PROGRAM(
        shell,
        "shell",
        "interactive user-mode shell"
    )
};
```

The registry should now report:

```text
Program registry: registered 3 embedded program(s)
```

---

## 7. Add a shell test

We can test the shell automatically by launching it in the foreground and injecting commands.

Update `program_test_once()` in `kernel/program.c`.

Keep the existing background counter test, then add a shell test after it.

A good final version is:

```c
void program_test_once(void) {
    console_writeln("Program test: starting background counter test");

    static const char *counter_argv[] = {
        "counter",
        "alpha",
        "beta"
    };

    process_t *counter = 0;

    int rc = program_run_background(
        "counter",
        3,
        counter_argv,
        &counter
    );

    if (rc != 0 || counter == 0) {
        kernel_panic("program test could not launch counter");
    }

    uint32_t counter_pid = counter->pid;

    console_write("Program test: background pid=");
    console_write_u32_dec(counter_pid);
    console_putc('\n');

    process_list();

    process_t *found = process_find(counter_pid);

    if (found != counter) {
        kernel_panic("program test could not find background process by PID");
    }

    uint32_t counter_exit = process_wait(counter);

    if (counter_exit != 4) {
        kernel_panic("program test received wrong counter exit code");
    }

    process_list();

    process_destroy(counter);

    console_writeln("Program test: background counter cleanup sanity check passed");

    console_writeln("Program test: starting user shell test");

    static const char *shell_argv[] = {
        "shell",
        "alpha",
        "beta"
    };

    process_t *shell = 0;

    rc = program_run_background(
        "shell",
        3,
        shell_argv,
        &shell
    );

    if (rc != 0 || shell == 0) {
        kernel_panic("program test could not launch shell");
    }

    /*
     * Let the shell start, print its banner and prompt, then feed commands.
     */
    thread_sleep_ticks(2);

    keyboard_debug_inject_char('h');
    keyboard_debug_inject_char('e');
    keyboard_debug_inject_char('l');
    keyboard_debug_inject_char('p');
    keyboard_debug_inject_char('\n');

    keyboard_debug_inject_char('e');
    keyboard_debug_inject_char('c');
    keyboard_debug_inject_char('h');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char(' ');
    keyboard_debug_inject_char('h');
    keyboard_debug_inject_char('e');
    keyboard_debug_inject_char('l');
    keyboard_debug_inject_char('l');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('\n');

    keyboard_debug_inject_char('a');
    keyboard_debug_inject_char('r');
    keyboard_debug_inject_char('g');
    keyboard_debug_inject_char('s');
    keyboard_debug_inject_char('\n');

    keyboard_debug_inject_char('e');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char(' ');
    keyboard_debug_inject_char('7');
    keyboard_debug_inject_char('\n');

    uint32_t shell_exit = process_wait(shell);

    if (shell_exit != 7) {
        kernel_panic("program test received wrong shell exit code");
    }

    process_destroy(shell);

    console_writeln("Program test: user shell cleanup sanity check passed");
}
```

Because this uses `keyboard_debug_inject_char()`, make sure `kernel/program.c` includes:

```c
#include "drivers/input/keyboard.h"
#include "kernel/thread.h"
```

The top include list should include:

```c
#include <stddef.h>
#include <stdint.h>
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/elf_loader.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/string.h"
#include "kernel/thread.h"
```

---

## 8. Why use `program_run_background()` for the shell test?

If we used `program_run_foreground()`, the kernel test would block immediately inside `process_wait()` before it had a chance to inject input.

So the test does:

```text
launch shell in background
  ↓
sleep briefly
  ↓
inject commands
  ↓
wait for shell
```

That lets the shell block on `SYS_READ`, then receive the injected command lines.

---

## 9. Update test greps

Update the registry count:

```make
grep -q "Program registry: registered 3 embedded program(s)" build/test.log
```

Add the shell program listing:

```make
grep -q "shell - interactive user-mode shell" build/test.log
```

Keep the counter greps, and add shell greps:

```make
grep -q "Program test: starting user shell test" build/test.log
grep -q "Program: launching shell argc=3" build/test.log
grep -q "Process: created pid=2 name=shell" build/test.log
grep -q "shell: Toyix user shell" build/test.log
grep -q "shell: startup argc=3" build/test.log
grep -q "shell: argv\\[0\\]=shell" build/test.log
grep -q "shell: argv\\[1\\]=alpha" build/test.log
grep -q "shell: argv\\[2\\]=beta" build/test.log
grep -q "commands: help, echo, args, exit" build/test.log
grep -q "hello" build/test.log
grep -q "argc=3" build/test.log
grep -q "argv\\[0\\]=shell" build/test.log
grep -q "argv\\[1\\]=alpha" build/test.log
grep -q "argv\\[2\\]=beta" build/test.log
grep -q "Syscall: process shell pid=2 exited code 7" build/test.log
grep -q "Process: destroyed pid=2 name=shell" build/test.log
grep -q "Program test: user shell cleanup sanity check passed" build/test.log
```

The updated final success message can be:

```make
@echo "Boot, memory, heap, sync, monitor, process table, user printf, and shell smoke test passed."
```

---

## 10. Expected output

The relevant section should look like this:

```text
Program registry: registered 3 embedded program(s)
Embedded programs:
  demo - interactive stdin/stdout demo
  counter - background-safe counter demo
  shell - interactive user-mode shell
...
Program test: starting user shell test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Program: launching shell argc=3
Thread: created shell id=...
Process: created pid=2 name=shell
shell: Toyix user shell
shell: startup argc=3
shell: argv[0]=shell
shell: argv[1]=alpha
shell: argv[2]=beta
ush> help
commands: help, echo, args, exit
ush> echo hello
hello
ush> args
argc=3
argv[0]=shell
argv[1]=alpha
argv[2]=beta
ush> exit 7
Syscall: process shell pid=2 exited code 7
Threads: reaping zombie shell id=...
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=2 name=shell
Program test: user shell cleanup sanity check passed
```

The exact PID may differ if earlier tests create more processes. If your test greps assume `pid=2`, keep the boot sequence stable or loosen the grep to avoid PID brittleness.

---

## 11. Interactive test

After boot, try:

```text
toyix> run shell one two
```

Expected:

```text
shell: Toyix user shell
shell: startup argc=3
shell: argv[0]=shell
shell: argv[1]=one
shell: argv[2]=two
ush>
```

Then:

```text
ush> help
commands: help, echo, args, exit

ush> echo hello user mode
hello user mode

ush> args
argc=3
argv[0]=shell
argv[1]=one
argv[2]=two

ush> exit 5
Syscall: process shell pid=... exited code 5
run: shell exited code 5
toyix>
```

That demonstrates a nested command environment:

```text
kernel monitor
  launches
user shell
  accepts commands
  exits
kernel monitor resumes
```

---

## 12. Common failures

### Failure: `shell` does not appear in `programs`

Check both places:

```make
USER_PROGRAMS := demo counter shell
```

and:

```c
DECLARE_EMBEDDED_PROGRAM(shell);
```

plus the registry entry:

```c
EMBEDDED_PROGRAM(
    shell,
    "shell",
    "interactive user-mode shell"
)
```

The build system embeds the program. The registry exposes it.

Both are required.

### Failure: undefined `user_shell_elf_start`

Check that the pattern rule produced:

```text
build/user/shell_elf_blob.o
```

Then inspect it:

```bash
i686-elf-nm build/user/shell_elf_blob.o
```

You should see:

```text
user_shell_elf_start
user_shell_elf_end
```

### Failure: shell hangs during automated test

Most likely the injected `exit 7\n` was not delivered.

Check that every command ends with:

```c
keyboard_debug_inject_char('\n');
```

The shell is line-oriented. Without newline, `toyix_readline()` will not return.

### Failure: `args` prints command arguments instead of startup arguments

In this design, `args` intentionally prints the shell process startup arguments:

```c
cmd_args(argc, argv);
```

where `argc` and `argv` are from `main()`.

If you want `args` to print the parsed command tokens instead, call:

```c
cmd_args(cmd_argc, cmd_argv);
```

For this chapter, startup arguments are the more useful test because they prove the kernel-built initial stack still works.

### Failure: `exit -1` gives a strange code

Process exit codes are currently stored as `uint32_t`.

A negative shell exit value becomes a large unsigned value.

That is acceptable for now. Later we can define exit status semantics more carefully.

---

## 13. What this chapter achieved

Before this chapter:

```text
user programs were one-shot utilities
```

After this chapter:

```text
Toyix can run a long-lived interactive user-mode shell
```

The shell proves:

```text
ring-3 command loop
line input
user-space tokenization
argc/argv startup state
formatted output
clean process exit
monitor resumes after foreground run
```

This is a major userland milestone.

---

## 14. Design limitations

This shell is intentionally tiny.

It does not support:

```text
quoted strings
escaping
pipes
redirection
environment variables
current directory
launching programs
background jobs
history
line editing
tab completion
```

Most importantly, it cannot yet run other programs.

That needs kernel syscalls such as:

```text
SYS_EXEC
SYS_WAITPID
SYS_PS or process info syscall
```

or a filesystem-backed exec path later.

But the user-mode shell is now alive.

---

## Resources

- Chapter source: [Toyix repository](https://github.com/Monotoba/toyix)
- Chapter release: [Chapter_30](https://github.com/Monotoba/toyix/releases/tag/Chapter_30)

## Closure

Chapter 30 gives Toyix its first real long-running user-mode shell. The kernel can now launch an interactive ring-3 command loop, feed it input, and return cleanly to the monitor when the shell exits.

Happy Coding!
