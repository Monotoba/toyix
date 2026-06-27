# Chapter 31 — `SYS_EXEC`, `SYS_WAITPID`, and Shell-Launched Programs

In Chapter 30, Toyix gained its first user-mode shell:

```text
ush> help
ush> echo hello
ush> args
ush> exit 7
```

But the shell could not launch programs yet.

The kernel monitor could run programs:

```text
toyix> run counter alpha beta
```

but the user shell could not.

This chapter adds the first user-facing process-control syscalls:

```text
SYS_EXEC
SYS_WAITPID
```

At first, `SYS_EXEC` still launches programs from the embedded program registry. We are not loading from a filesystem yet.

But the control path changes in an important way:

```text
user shell
  ↓ SYS_EXEC("counter", argv)
kernel program registry
  ↓
ELF loader
  ↓
new process
  ↓
return child PID to shell
  ↓ SYS_WAITPID(pid)
shell waits for child
```

After this chapter, the user shell can do:

```text
ush> run counter alpha beta
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: tick 1
counter: tick 2
counter: tick 3
shell: counter exited code 4
```

That is a major milestone: program launching moves from the kernel monitor into userland.

---

## 1. What this chapter adds

Modify:

```text
include/kernel/syscall.h
kernel/syscall.c
user/include/toyix_syscall.h
user/shell.c
kernel/program.c
Makefile
tests/smoke.sh
```

No new assembly is needed.

The syscall ABI becomes:

```text
SYS_READ       5
SYS_EXEC       6
SYS_WAITPID    7
```

We keep all previous syscalls.

---

## 2. New syscall ABI

### `SYS_EXEC`

```text
EAX = SYS_EXEC
EBX = user pointer to program name string
ECX = user pointer to argv pointer array
EDX = argc

returns:
  EAX = child PID on success
  EAX = 0xFFFFFFFF on error
```

Example userland call:

```c
const char *argv[] = {
    "counter",
    "alpha",
    "beta"
};

toyix_i32 pid = toyix_exec("counter", argv, 3);
```

### `SYS_WAITPID`

```text
EAX = SYS_WAITPID
EBX = pid
ECX = user pointer to uint32_t status

returns:
  EAX = 0 on success
  EAX = 0xFFFFFFFF on error
```

Example userland call:

```c
toyix_u32 status = 0;

if (toyix_waitpid(pid, &status) == 0) {
    toyix_printf("child exited code %u\n", status);
}
```

---

## 3. Important limitation

This is not Unix `execve()` yet.

Our `SYS_EXEC` does **not** replace the current process.

Instead, it spawns a new process and returns the child PID.

So this is closer to:

```text
spawn()
```

than true Unix `exec()`.

We will still call the syscall `SYS_EXEC` for now because it launches an executable program, but architecturally it is:

```text
embedded-program spawn
```

A later syscall set can separate these ideas more cleanly:

```text
SYS_SPAWN
SYS_EXECVE
SYS_WAITPID
```

For Toyix right now, this design keeps things simple and useful.

---

## 4. Update `include/kernel/syscall.h`

Replace it with this version:

```c
// include/kernel/syscall.h
#ifndef TOYIX_KERNEL_SYSCALL_H
#define TOYIX_KERNEL_SYSCALL_H

#include "arch/x86/interrupts.h"

#define FD_STDIN  0u
#define FD_STDOUT 1u
#define FD_STDERR 2u

#define SYS_PUTC    1u
#define SYS_EXIT    2u
#define SYS_WRITE   3u
#define SYS_SLEEP   4u
#define SYS_READ    5u
#define SYS_EXEC    6u
#define SYS_WAITPID 7u

void syscall_handler(interrupt_frame_t *frame);

#endif
```

The new syscall numbers are:

```c
#define SYS_EXEC    6u
#define SYS_WAITPID 7u
```

---

## 5. Update `user/include/toyix_syscall.h`

Replace it with this version:

```c
// user/include/toyix_syscall.h
#ifndef TOYIX_USER_SYSCALL_H
#define TOYIX_USER_SYSCALL_H

typedef unsigned int toyix_u32;
typedef int toyix_i32;

#define FD_STDIN  0u
#define FD_STDOUT 1u
#define FD_STDERR 2u

#define SYS_PUTC    1u
#define SYS_EXIT    2u
#define SYS_WRITE   3u
#define SYS_SLEEP   4u
#define SYS_READ    5u
#define SYS_EXEC    6u
#define SYS_WAITPID 7u

static inline toyix_i32 toyix_read(
    toyix_u32 fd,
    void *buffer,
    toyix_u32 length
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_READ),
          "b"(fd),
          "c"(buffer),
          "d"(length)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_write(
    toyix_u32 fd,
    const void *buffer,
    toyix_u32 length
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_WRITE),
          "b"(fd),
          "c"(buffer),
          "d"(length)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_sleep(toyix_u32 ticks) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_SLEEP),
          "b"(ticks)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_exec(
    const char *name,
    const char **argv,
    toyix_u32 argc
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_EXEC),
          "b"(name),
          "c"(argv),
          "d"(argc)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_waitpid(
    toyix_u32 pid,
    toyix_u32 *status
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_WAITPID),
          "b"(pid),
          "c"(status)
        : "memory"
    );

    return result;
}

static inline void toyix_exit(toyix_u32 code) {
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(SYS_EXIT),
          "b"(code)
        : "memory"
    );

    for (;;) {
        __asm__ volatile ("jmp .");
    }
}

#endif
```

The new user wrappers are:

```c
toyix_i32 toyix_exec(const char *name, const char **argv, toyix_u32 argc);
toyix_i32 toyix_waitpid(toyix_u32 pid, toyix_u32 *status);
```

---

## 6. Update `kernel/syscall.c` includes

Add:

```c
#include "kernel/program.h"
```

The top of `kernel/syscall.c` should now include:

```c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/syscall.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"
#include "kernel/usercopy.h"
```

---

## 7. Add syscall copy limits

Near the top of `kernel/syscall.c`, add:

```c
#define SYSCALL_RW_MAX 256u

#define SYSCALL_EXEC_MAX_ARGS    8u
#define SYSCALL_EXEC_MAX_NAME    32u
#define SYSCALL_EXEC_MAX_ARG_LEN 64u
```

`SYSCALL_RW_MAX` may already exist.

The new limits keep user-provided strings bounded:

```text
program name max: 31 chars + NUL
argument max:     63 chars + NUL
argument count:   8
```

This is intentionally small and easy to reason about.

---

## 8. Add a helper to copy user strings

Add this helper in `kernel/syscall.c`:

```c
static int syscall_copy_user_string(
    uintptr_t user_str,
    char *kernel_buffer,
    uint32_t kernel_buffer_size
) {
    if (user_str == 0 ||
        kernel_buffer == 0 ||
        kernel_buffer_size == 0) {
        return -1;
    }

    size_t length = 0;

    if (user_string_length(
            user_str,
            kernel_buffer_size,
            &length
        ) != USERCOPY_OK) {
        return -1;
    }

    if (length + 1u > kernel_buffer_size) {
        return -1;
    }

    if (copy_from_user(
            kernel_buffer,
            user_str,
            length + 1u
        ) != USERCOPY_OK) {
        return -1;
    }

    return 0;
}
```

This uses the `usercopy` layer created earlier.

The kernel never trusts a raw user pointer directly.

---

## 9. Add `SYS_EXEC` implementation

Add this function to `kernel/syscall.c`:

```c
static void syscall_exec(interrupt_frame_t *frame) {
    uintptr_t user_name = (uintptr_t)frame->ebx;
    uintptr_t user_argv = (uintptr_t)frame->ecx;
    uint32_t argc = frame->edx;

    char name[SYSCALL_EXEC_MAX_NAME];

    if (syscall_copy_user_string(
            user_name,
            name,
            sizeof(name)
        ) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    char arg_storage[SYSCALL_EXEC_MAX_ARGS][SYSCALL_EXEC_MAX_ARG_LEN];
    const char *kernel_argv[SYSCALL_EXEC_MAX_ARGS];

    if (argc == 0 || user_argv == 0) {
        argc = 1;
        kernel_argv[0] = name;
    } else {
        if (argc > SYSCALL_EXEC_MAX_ARGS) {
            frame->eax = 0xFFFFFFFFu;
            return;
        }

        for (uint32_t i = 0; i < argc; ++i) {
            uintptr_t user_arg_ptr = 0;

            if (copy_from_user(
                    &user_arg_ptr,
                    user_argv + i * sizeof(uintptr_t),
                    sizeof(uintptr_t)
                ) != USERCOPY_OK) {
                frame->eax = 0xFFFFFFFFu;
                return;
            }

            if (syscall_copy_user_string(
                    user_arg_ptr,
                    arg_storage[i],
                    SYSCALL_EXEC_MAX_ARG_LEN
                ) != 0) {
                frame->eax = 0xFFFFFFFFu;
                return;
            }

            kernel_argv[i] = arg_storage[i];
        }
    }

    process_t *child = 0;

    int rc = program_run_background(
        name,
        (int)argc,
        kernel_argv,
        &child
    );

    if (rc != 0 || child == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = child->pid;
}
```

### Why copy `argv` into kernel buffers?

The shell’s `argv` lives in the shell process address space.

The child process needs its own copy on its own user stack.

The flow is:

```text
shell argv strings
  ↓ copy_from_user()
kernel temporary buffers
  ↓ process_setup_arguments()
child user stack
```

The temporary kernel buffers only need to live until `program_run_background()` finishes, because `process_setup_arguments()` copies them into the child process.

---

## 10. Add `SYS_WAITPID` implementation

Add this function to `kernel/syscall.c`:

```c
static void syscall_waitpid(interrupt_frame_t *frame) {
    uint32_t pid = frame->ebx;
    uintptr_t user_status = (uintptr_t)frame->ecx;

    process_t *process = process_find(pid);

    if (process == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (process_current() == process) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    interrupts_enable();
    uint32_t status = process_wait(process);

    if (user_status != 0) {
        if (copy_to_user(
                user_status,
                &status,
                sizeof(status)
            ) != USERCOPY_OK) {
            frame->eax = 0xFFFFFFFFu;
            return;
        }
    }

    process_destroy(process);

    frame->eax = 0;
}
```

### Why destroy inside `SYS_WAITPID`?

For this first version:

```text
waitpid(pid)
  ↓
collect exit status
  ↓
destroy process object
  ↓
free address space
  ↓
remove from process table
```

That keeps the user shell simple.

Later, when we add parent/child relationships and more Unix-like zombie semantics, this can become more nuanced.

---

## 11. Update `syscall_handler()`

Add cases for the new syscalls:

```c
case SYS_EXEC:
    syscall_exec(frame);
    return;

case SYS_WAITPID:
    syscall_waitpid(frame);
    return;
```

The lower part of `syscall_handler()` should now look like this:

```c
        case SYS_SLEEP: {
            uint32_t ticks = frame->ebx;
            thread_sleep_ticks(ticks);
            frame->eax = 0;
            return;
        }

        case SYS_EXEC:
            syscall_exec(frame);
            return;

        case SYS_WAITPID:
            syscall_waitpid(frame);
            return;

        case SYS_EXIT: {
            uint32_t exit_code = frame->ebx;

            process_exit_current(exit_code);
            thread_exit();
            return;
        }
```

You can place `SYS_EXIT` before or after `SYS_EXEC`; the syscall numbers determine behavior, not source order.

---

## 12. Full syscall behavior after this chapter

The user shell can now do:

```text
SYS_EXEC("counter", argv, argc)
  ↓
kernel copies shell strings
  ↓
kernel finds embedded "counter"
  ↓
kernel loads ELF into new process
  ↓
kernel builds child argv stack
  ↓
kernel starts child
  ↓
returns child PID

SYS_WAITPID(pid, &status)
  ↓
kernel finds process
  ↓
kernel waits for exit
  ↓
kernel copies exit status to shell
  ↓
kernel destroys child
  ↓
returns success
```

That is our first userland process-control loop.

---

## 13. Update `user/shell.c`

Now we add a `run` built-in to the user shell.

Replace the help text:

```c
static void cmd_help(void) {
    toyix_puts("commands: help, echo, args, exit");
}
```

with:

```c
static void cmd_help(void) {
    toyix_puts("commands: help, echo, args, run, exit");
}
```

Add this helper:

```c
static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        toyix_puts("usage: run PROGRAM [ARGS...]");
        return;
    }

    const char *program_name = argv[1];

    int child_argc = argc - 1;
    const char **child_argv = (const char **)&argv[1];

    toyix_i32 pid = toyix_exec(
        program_name,
        child_argv,
        (toyix_u32)child_argc
    );

    if (pid < 0) {
        toyix_printf("run: failed to launch %s\n", program_name);
        return;
    }

    toyix_printf("shell: run %s pid=%d\n", program_name, pid);

    toyix_u32 status = 0;

    if (toyix_waitpid((toyix_u32)pid, &status) != 0) {
        toyix_printf("run: wait failed for pid %d\n", pid);
        return;
    }

    toyix_printf("shell: %s exited code %u\n", program_name, status);
}
```

Then add a command branch in `main()`:

```c
if (toyix_streq(cmd_argv[0], "run")) {
    cmd_run(cmd_argc, cmd_argv);
    continue;
}
```

Place it before `exit`.

The command dispatch section should now look like this:

```c
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

        if (toyix_streq(cmd_argv[0], "run")) {
            cmd_run(cmd_argc, cmd_argv);
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
```

---

## 14. Full updated `user/shell.c`

For convenience, here is the full file:

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
    toyix_puts("commands: help, echo, args, run, exit");
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

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        toyix_puts("usage: run PROGRAM [ARGS...]");
        return;
    }

    const char *program_name = argv[1];

    int child_argc = argc - 1;
    const char **child_argv = (const char **)&argv[1];

    toyix_i32 pid = toyix_exec(
        program_name,
        child_argv,
        (toyix_u32)child_argc
    );

    if (pid < 0) {
        toyix_printf("run: failed to launch %s\n", program_name);
        return;
    }

    toyix_printf("shell: run %s pid=%d\n", program_name, pid);

    toyix_u32 status = 0;

    if (toyix_waitpid((toyix_u32)pid, &status) != 0) {
        toyix_printf("run: wait failed for pid %d\n", pid);
        return;
    }

    toyix_printf("shell: %s exited code %u\n", program_name, status);
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

        if (toyix_streq(cmd_argv[0], "run")) {
            cmd_run(cmd_argc, cmd_argv);
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

---

## 15. Update shell test in `kernel/program.c`

In Chapter 31, the shell test injected:

```text
help
echo hello
args
exit 7
```

Now add:

```text
run counter alpha beta
```

before `args`.

Find this section:

```c
keyboard_debug_inject_char('a');
keyboard_debug_inject_char('r');
keyboard_debug_inject_char('g');
keyboard_debug_inject_char('s');
keyboard_debug_inject_char('\n');
```

Insert this before it:

```c
keyboard_debug_inject_char('r');
keyboard_debug_inject_char('u');
keyboard_debug_inject_char('n');
keyboard_debug_inject_char(' ');
keyboard_debug_inject_char('c');
keyboard_debug_inject_char('o');
keyboard_debug_inject_char('u');
keyboard_debug_inject_char('n');
keyboard_debug_inject_char('t');
keyboard_debug_inject_char('e');
keyboard_debug_inject_char('r');
keyboard_debug_inject_char(' ');
keyboard_debug_inject_char('a');
keyboard_debug_inject_char('l');
keyboard_debug_inject_char('p');
keyboard_debug_inject_char('h');
keyboard_debug_inject_char('a');
keyboard_debug_inject_char(' ');
keyboard_debug_inject_char('b');
keyboard_debug_inject_char('e');
keyboard_debug_inject_char('t');
keyboard_debug_inject_char('a');
keyboard_debug_inject_char('\n');
```

The shell test input sequence is now:

```text
help
echo hello
run counter alpha beta
args
exit 7
```

---

## 16. Why the shell test still launches shell with `program_run_background()`

The shell blocks waiting for input.

The kernel test must be able to inject input after the shell starts.

So the test flow remains:

```text
launch shell in background
  ↓
sleep
  ↓
inject help
  ↓
inject echo
  ↓
inject run counter
  ↓
inject args
  ↓
inject exit
  ↓
wait for shell
```

If we launched the shell in foreground, the kernel test would block before it could inject commands.

---

## 17. Expected shell test output

The shell section should now include:

```text
shell: Toyix user shell
shell: startup argc=3
shell: argv[0]=shell
shell: argv[1]=alpha
shell: argv[2]=beta
ush> help
commands: help, echo, args, run, exit
ush> echo hello
hello
ush> run counter alpha beta
Program: launching counter argc=3
Process: created pid=3 name=counter
shell: run counter pid=3
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: printf test A 0x1234 %
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=3 exited code 4
Process: destroyed pid=3 name=counter
shell: counter exited code 4
ush> args
argc=3
argv[0]=shell
argv[1]=alpha
argv[2]=beta
ush> exit 7
Syscall: process shell pid=2 exited code 7
```

The exact PIDs may differ.

Avoid overly strict PID greps if your test order changes.

---

## 18. Update Makefile greps

Update the shell help grep.

Replace:

```make
grep -q "commands: help, echo, args, exit" build/test.log
```

with:

```make
grep -q "commands: help, echo, args, run, exit" build/test.log
```

Add greps for the shell-launched counter:

```make
grep -q "shell: run counter pid=" build/test.log
grep -q "shell: counter exited code 4" build/test.log
```

Because `counter` now runs twice during the full program test — once as the background process-table test and once launched by the shell — most existing counter greps still pass.

Add a syscall grep for the new child process without assuming its PID:

```make
grep -q "Syscall: process counter pid=.*exited code 4" build/test.log
```

If your `grep` is basic grep, `.*` works as a basic regular expression.

The relevant shell block should now include:

```make
	grep -q "Program test: starting user shell test" build/test.log
	grep -q "Program: launching shell argc=3" build/test.log
	grep -q "shell: Toyix user shell" build/test.log
	grep -q "shell: startup argc=3" build/test.log
	grep -q "shell: argv\\[0\\]=shell" build/test.log
	grep -q "shell: argv\\[1\\]=alpha" build/test.log
	grep -q "shell: argv\\[2\\]=beta" build/test.log
	grep -q "commands: help, echo, args, run, exit" build/test.log
	grep -q "hello" build/test.log
	grep -q "shell: run counter pid=" build/test.log
	grep -q "shell: counter exited code 4" build/test.log
	grep -q "argc=3" build/test.log
	grep -q "argv\\[0\\]=shell" build/test.log
	grep -q "argv\\[1\\]=alpha" build/test.log
	grep -q "argv\\[2\\]=beta" build/test.log
	grep -q "Syscall: process shell pid=.*exited code 7" build/test.log
	grep -q "Process: destroyed pid=.*name=shell" build/test.log
	grep -q "Program test: user shell cleanup sanity check passed" build/test.log
```

Update the final test message:

```make
	@echo "Boot, memory, heap, sync, monitor, process table, user shell exec, and waitpid smoke test passed."
```

---

## 19. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 31 checks passed."
```

---

## 20. Expected full milestone output

The important new section is:

```text
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
commands: help, echo, args, run, exit
ush> echo hello
hello
ush> run counter alpha beta
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Program: launching counter argc=3
Thread: created counter id=...
Process: created pid=3 name=counter
shell: run counter pid=3
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: printf test A 0x1234 %
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=3 exited code 4
Threads: reaping zombie counter id=...
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=3 name=counter
shell: counter exited code 4
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

This proves the full userland control path:

```text
shell command parser
  ↓
SYS_EXEC
  ↓
kernel registry lookup
  ↓
ELF loader
  ↓
new process
  ↓
SYS_WAITPID
  ↓
child exit status returned to shell
```

---

## 21. Interactive test

After boot, try:

```text
toyix> run shell
```

Then inside the user shell:

```text
ush> run counter one two
```

Expected:

```text
shell: run counter pid=...
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=one
counter: argv[2]=two
counter: printf test A 0x1234 %
counter: tick 1
counter: tick 2
counter: tick 3
shell: counter exited code 4
```

Then:

```text
ush> exit 0
```

The kernel monitor should resume and print the shell process exit status.

---

## 22. Common failures

### Failure: `run counter` says failed to launch

Check the syscall path:

```text
shell toyix_exec()
  ↓
SYS_EXEC
  ↓
syscall_exec()
  ↓
program_run_background()
  ↓
program_find("counter")
```

Make sure `counter` is still registered in `kernel/program.c`.

### Failure: `SYS_EXEC` returns `0xFFFFFFFF`

Likely causes:

```text
program name pointer invalid
argv pointer invalid
argc exceeds SYSCALL_EXEC_MAX_ARGS
argument string exceeds SYSCALL_EXEC_MAX_ARG_LEN
program not found
ELF load failure
```

For the test command:

```text
run counter alpha beta
```

the shell passes:

```text
name = "counter"
argc = 3
argv[0] = "counter"
argv[1] = "alpha"
argv[2] = "beta"
```

All strings are well under the limits.

### Failure: shell hangs after `run counter`

Likely causes:

```text
child process never exits
SYS_WAITPID is waiting for wrong PID
process_exit_current() did not mark child exited
timer interrupts are not waking sleepers
```

Check:

```c
process->exited = 1;
process->state = PROCESS_EXITED;
```

inside `process_exit_current()`.

### Failure: shell exits but child remains in `ps`

`SYS_WAITPID` should call:

```c
process_destroy(process);
```

after collecting the exit code.

If it only waits but does not destroy, the child remains in the process table as exited.

### Failure: waiting on yourself panics

`process_wait()` already rejects self-wait.

`syscall_waitpid()` should check:

```c
if (process_current() == process) {
    frame->eax = 0xFFFFFFFFu;
    return;
}
```

Do not let a user process wait for itself.

### Failure: child argv strings are corrupted

The kernel must copy user argv strings before creating the child stack.

This is wrong:

```text
child argv points directly into shell memory
```

This is right:

```text
copy shell argv into kernel buffers
copy kernel buffers into child stack
```

The `syscall_exec()` implementation in this chapter does the right thing.

---

## 23. What this chapter achieved

Before this chapter:

```text
kernel monitor could launch programs
user shell could not
```

After this chapter:

```text
user shell can launch embedded programs
user shell can wait for exit status
child processes are loaded through the ELF loader
child processes get argc/argv
child process cleanup happens through waitpid
```

Toyix now has its first userland process-control loop.

This is a very important operating-system milestone.

---

## 24. Design limitations

This is still minimal.

Limitations:

```text
SYS_EXEC launches embedded registry programs only
no filesystem path lookup
no true exec-replace-current-process behavior
no parent/child ownership enforcement
any process can wait any PID
no nonblocking wait
no background user-shell jobs
no environment variables
no file descriptor inheritance model
no kill/signal syscall
```

But the architecture is now ready for those features.

The path is clear:

```text
embedded exec
  ↓
filesystem exec
  ↓
shell launches programs by path
  ↓
file descriptors and working directories
```

---

## Resources

- Chapter source: [Toyix repository](https://github.com/Monotoba/toyix)
- Chapter release: [Chapter_31](https://github.com/Monotoba/toyix/releases/tag/Chapter_31)

## Closure

Chapter 31 gives the user-mode shell its first real process-control path. Toyix can now launch embedded programs from ring 3, wait for their exit status, and clean them up through a user-facing syscall flow.

Happy Coding!
