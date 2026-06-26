# Chapter 25 — Embedded Program Registry and `run` Monitor Command

In Chapter 24, the kernel could run a compiled user ELF with real `argc` and `argv`:

```text
compiled user ELF
  ↓
ELF loader
  ↓
process address space
  ↓
initial user stack
  ↓
main(argc, argv)
```

But the process test still launched that program directly from kernel test code.

This chapter adds the first **exec-like program launcher**.

We still do not have a filesystem, so programs are embedded in the kernel image. But instead of hardcoding one test path, we will add a small program registry:

```text
embedded ELF blob
  ↓
program registry
  ↓
monitor command: run demo alpha beta
  ↓
ELF loader
  ↓
process_start_user()
  ↓
process_wait()
  ↓
process_destroy()
```

After this chapter, the monitor can do:

```text
toyix> programs
toyix> run demo alpha beta
```

The milestone output becomes:

```text
Program registry: registered 1 embedded program(s)
Program test: starting embedded program run test
Program: launching demo argc=3
Process: initial stack argc=3
Process: created pid=1 name=demo
argc=3
argv[0]=demo
argv[1]=alpha
argv[2]=beta
user> toyix
echo: toyix
Syscall: process demo pid=1 exited code 9
Process: destroyed pid=1 name=demo
Program test: embedded ELF program run cleanup sanity check passed
```

---

# 1. What this chapter adds

Add:

```text
include/kernel/
└── program.h

kernel/
└── program.c
```

Modify:

```text
include/kernel/process.h
kernel/process.c
kernel/monitor.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

The key idea is to move this knowledge out of `process.c`:

```text
where embedded programs live
which program names exist
how to launch one by name
```

That belongs in a program registry layer.

---

# 2. New layer

The new layering is:

```text
monitor.c
  parses command line

program.c
  finds embedded program by name
  creates process
  sets argc/argv
  starts process
  waits/destroys for foreground run

elf_loader.c
  validates ELF
  maps PT_LOAD segments
  sets entry and stack

process.c
  owns process lifecycle

thread.c
  schedules execution
```

This is an important separation.

The monitor should not know where the ELF bytes are stored.

The ELF loader should not know about monitor commands.

The process layer should not know which named programs exist.

---

# 3. Update `include/kernel/process.h`

Remove this declaration if it is still present:

```c
void process_test_once(void);
```

`process.c` should no longer own the user-program test. The new registry layer will own it.

The bottom of `process.h` should now look like this:

```c
uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

#endif
```

---

# 4. Clean up `kernel/process.c`

Remove the old compiled ELF test from `process.c`.

Delete these declarations if present:

```c
extern const uint8_t user_demo_elf_start[];
extern const uint8_t user_demo_elf_end[];
```

Delete the old:

```c
void process_test_once(void)
```

The process layer should now only contain process lifecycle logic:

```text
create empty process
map user memory
copy/zero user memory
setup argv stack
start process
exit process
wait process
destroy process
```

It should not know that a program named `demo` exists.

---

# 5. Add `include/kernel/program.h`

```c
// include/kernel/program.h
#ifndef TOYIX_KERNEL_PROGRAM_H
#define TOYIX_KERNEL_PROGRAM_H

#include <stdint.h>
#include "kernel/process.h"

typedef struct embedded_program {
    const char *name;
    const char *description;

    const uint8_t *image_start;
    const uint8_t *image_end;
} embedded_program_t;

void program_registry_init(void);

const embedded_program_t *program_find(const char *name);
void program_list(void);

process_t *program_create_process(
    const char *name,
    int argc,
    const char **argv
);

int program_run_foreground(
    const char *name,
    int argc,
    const char **argv,
    uint32_t *exit_code_out
);

void program_test_once(void);

#endif
```

## Why `image_start` and `image_end`?

The embedded ELF comes from linker symbols created by `objcopy`.

The size is:

```c
image_end - image_start
```

This avoids hardcoding the ELF size.

---

# 6. Add `kernel/program.c`

```c
// kernel/program.c
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

extern const uint8_t user_demo_elf_start[];
extern const uint8_t user_demo_elf_end[];

static const embedded_program_t programs[] = {
    {
        .name = "demo",
        .description = "compiled user-mode demo program",
        .image_start = user_demo_elf_start,
        .image_end = user_demo_elf_end
    }
};

static const uint32_t program_count =
    sizeof(programs) / sizeof(programs[0]);

void program_registry_init(void) {
    console_write("Program registry: registered ");
    console_write_u32_dec(program_count);
    console_writeln(" embedded program(s)");
}

const embedded_program_t *program_find(const char *name) {
    if (name == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < program_count; ++i) {
        if (kstrcmp(programs[i].name, name) == 0) {
            return &programs[i];
        }
    }

    return 0;
}

void program_list(void) {
    console_writeln("Embedded programs:");

    for (uint32_t i = 0; i < program_count; ++i) {
        console_write("  ");
        console_write(programs[i].name);
        console_write(" - ");
        console_writeln(programs[i].description);
    }
}

static uint32_t program_image_size(const embedded_program_t *program) {
    if (program == 0 ||
        program->image_start == 0 ||
        program->image_end == 0 ||
        program->image_end <= program->image_start) {
        return 0;
    }

    return (uint32_t)(program->image_end - program->image_start);
}

process_t *program_create_process(
    const char *name,
    int argc,
    const char **argv
) {
    const embedded_program_t *program = program_find(name);

    if (program == 0) {
        return 0;
    }

    uint32_t image_size = program_image_size(program);

    if (image_size == 0) {
        kernel_panic("embedded program image is empty");
    }

    process_t *process = elf_create_process_suspended(
        program->name,
        program->image_start,
        image_size
    );

    const char *default_argv[] = {
        program->name
    };

    if (argc <= 0 || argv == 0) {
        argc = 1;
        argv = default_argv;
    }

    if (process_setup_arguments(process, argc, argv) != 0) {
        kernel_panic("program argument setup failed");
    }

    console_write("Program: launching ");
    console_write(program->name);
    console_write(" argc=");
    console_write_u32_dec((uint32_t)argc);
    console_putc('\n');

    process_start_user(process);

    return process;
}

int program_run_foreground(
    const char *name,
    int argc,
    const char **argv,
    uint32_t *exit_code_out
) {
    process_t *process = program_create_process(name, argc, argv);

    if (process == 0) {
        return -1;
    }

    uint32_t exit_code = process_wait(process);

    process_destroy(process);

    if (exit_code_out != 0) {
        *exit_code_out = exit_code;
    }

    return 0;
}

void program_test_once(void) {
    console_writeln("Program test: starting embedded program run test");

    static const char *argv[] = {
        "demo",
        "alpha",
        "beta"
    };

    process_t *process = program_create_process(
        "demo",
        3,
        argv
    );

    if (process == 0) {
        kernel_panic("program test could not launch demo");
    }

    /*
     * Let the user process start, print argc/argv, print its prompt,
     * and block in SYS_READ.
     */
    thread_sleep_ticks(2);

    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('y');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('\n');

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("program test received wrong exit code");
    }

    process_destroy(process);

    console_writeln("Program test: embedded ELF program run cleanup sanity check passed");
}
```

---

# 7. Why `program_run_foreground()` waits and destroys

This is our first foreground execution model.

A monitor command like:

```text
run demo alpha beta
```

does this:

```text
create process
  ↓
start process
  ↓
wait for exit
  ↓
destroy process
  ↓
print exit code
```

That is simple, predictable, and useful.

Later, we can add background execution:

```text
runbg demo alpha beta
```

or job control:

```text
jobs
kill PID
wait PID
```

But foreground execution is the right first step.

---

# 8. Update `kernel/monitor.c`

Add:

```c
#include "kernel/program.h"
```

Add prototypes:

```c
static int cmd_programs(int argc, char **argv);
static int cmd_run(int argc, char **argv);
```

Add two command-table entries:

```c
{
    .name = "programs",
    .usage = "programs",
    .help = "list embedded user programs",
    .handler = cmd_programs
},
{
    .name = "run",
    .usage = "run PROGRAM [ARGS...]",
    .help = "run an embedded user program in the foreground",
    .handler = cmd_run
}
```

For example, the table section should include:

```c
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
        .name = "programs",
        .usage = "programs",
        .help = "list embedded user programs",
        .handler = cmd_programs
    },
    {
        .name = "run",
        .usage = "run PROGRAM [ARGS...]",
        .help = "run an embedded user program in the foreground",
        .handler = cmd_run
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
```

Now add the command handlers.

```c
static int cmd_programs(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: programs");
        return 1;
    }

    program_list();
    return 1;
}

static int cmd_run(int argc, char **argv) {
    if (argc < 2) {
        console_writeln("usage: run PROGRAM [ARGS...]");
        return 1;
    }

    const char *program_name = argv[1];

    /*
     * Convert:
     *
     *   run demo alpha beta
     *
     * into user argv:
     *
     *   argv[0] = demo
     *   argv[1] = alpha
     *   argv[2] = beta
     */
    int child_argc = argc - 1;
    const char **child_argv = (const char **)&argv[1];

    uint32_t exit_code = 0xFFFFFFFFu;

    int rc = program_run_foreground(
        program_name,
        child_argc,
        child_argv,
        &exit_code
    );

    if (rc != 0) {
        console_write("run: unknown program ");
        console_writeln(program_name);
        console_writeln("type 'programs' to list available programs");
        return 1;
    }

    console_write("run: ");
    console_write(program_name);
    console_write(" exited code ");
    console_write_u32_dec(exit_code);
    console_putc('\n');

    return 1;
}
```

Finally, update `monitor_test_once()`.

Add:

```c
monitor_execute_command("programs");
monitor_execute_command("run");
```

The updated test can look like this:

```c
void monitor_test_once(void) {
    console_writeln("Monitor test: starting command table test");

    monitor_execute_command("help ticks");
    monitor_execute_command("ticks");
    monitor_execute_command("programs");
    monitor_execute_command("run");
    monitor_execute_command("echo monitor ok");
    monitor_execute_command("unknown-test-command");

    console_writeln("Monitor test: command table sanity check passed");
}
```

Do **not** call:

```text
run demo
```

inside `monitor_test_once()`, because the demo program waits for terminal input. The dedicated `program_test_once()` handles that safely with injected input.

---

# 9. Update `kernel/kmain.c`

Add:

```c
#include "kernel/program.h"
```

After process initialization, initialize the program registry:

```c
threading_init();
process_init_system();
program_registry_init();
thread_test_once();
```

Later, replace:

```c
process_test_once();
```

with:

```c
program_test_once();
```

The relevant flow should now look like this:

```c
threading_init();
process_init_system();
program_registry_init();
thread_test_once();
```

And later:

```c
terminal_init();
terminal_test_once();

monitor_init();
monitor_test_once();

program_test_once();

monitor_start();
```

The registry must be initialized before the monitor test, because `monitor_test_once()` now calls:

```text
programs
```

---

# 10. Update `Makefile`

Add:

```text
build/kernel/program.o
```

to the object list.

The relevant object list section becomes:

```make
OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/address_space.o \
    build/kernel/elf_loader.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/process.o \
    build/kernel/program.o \
    build/kernel/sync.o \
    build/kernel/syscall.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o \
    build/user/demo_elf_blob.o
```

---

# 11. Update test greps

Replace the Chapter 24 process-test greps:

```make
grep -q "Process test: starting compiled ELF32 argv user program test" build/test.log
grep -q "Process test: compiled ELF32 argv cleanup sanity check passed" build/test.log
```

with:

```make
grep -q "Program registry: registered 1 embedded program(s)" build/test.log
grep -q "Program test: starting embedded program run test" build/test.log
grep -q "Program: launching demo argc=3" build/test.log
grep -q "Process test" build/test.log
```

Actually, do **not** keep that last placeholder. Use exact program-test greps instead:

```make
grep -q "Program registry: registered 1 embedded program(s)" build/test.log
grep -q "Embedded programs:" build/test.log
grep -q "demo - compiled user-mode demo program" build/test.log
grep -q "Program test: starting embedded program run test" build/test.log
grep -q "Program: launching demo argc=3" build/test.log
grep -q "Process: created pid=1 name=demo" build/test.log
grep -q "argc=3" build/test.log
grep -q "argv\\[0\\]=demo" build/test.log
grep -q "argv\\[1\\]=alpha" build/test.log
grep -q "argv\\[2\\]=beta" build/test.log
grep -q "echo: toyix" build/test.log
grep -q "Syscall: process demo pid=1 exited code 9" build/test.log
grep -q "Address space: destroyed process page directory" build/test.log
grep -q "Process: destroyed pid=1 name=demo" build/test.log
grep -q "Program test: embedded ELF program run cleanup sanity check passed" build/test.log
```

The full process/program block should look like this:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Program registry: registered 1 embedded program(s)" build/test.log
	grep -q "Embedded programs:" build/test.log
	grep -q "demo - compiled user-mode demo program" build/test.log
	grep -q "Program test: starting embedded program run test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000" build/test.log
	grep -q "ELF32: entry=0x40100000" build/test.log
	grep -q "Process: initial stack argc=3" build/test.log
	grep -q "Program: launching demo argc=3" build/test.log
	grep -q "Process: created pid=1 name=demo" build/test.log
	grep -q "argc=3" build/test.log
	grep -q "argv\\[0\\]=demo" build/test.log
	grep -q "argv\\[1\\]=alpha" build/test.log
	grep -q "argv\\[2\\]=beta" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process demo pid=1 exited code 9" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=demo" build/test.log
	grep -q "Program test: embedded ELF program run cleanup sanity check passed" build/test.log
```

Update the final test message:

```make
	@echo "Boot, memory, heap, sync, monitor, program registry, and run command smoke test passed."
```

---

# 12. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 25 checks passed."
```

---

# 13. Expected boot output

A successful boot should include:

```text
Program registry: registered 1 embedded program(s)
...
Monitor test: starting command table test
Embedded programs:
  demo - compiled user-mode demo program
usage: run PROGRAM [ARGS...]
...
Program test: starting embedded program run test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Program: launching demo argc=3
Thread: created demo id=...
Process: created pid=1 name=demo
argc=3
argv[0]=demo
argv[1]=alpha
argv[2]=beta
user> toyix
echo: toyix
Syscall: process demo pid=1 exited code 9
Threads: reaping zombie demo id=...
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=1 name=demo
Program test: embedded ELF program run cleanup sanity check passed
```

After boot, the interactive monitor should support:

```text
toyix> programs
Embedded programs:
  demo - compiled user-mode demo program

toyix> run demo one two
argc=3
argv[0]=demo
argv[1]=one
argv[2]=two
user> hello
echo: hello
Syscall: process demo pid=... exited code 9
run: demo exited code 9
```

---

# 14. Common failures

## Failure: `programs` command says nothing

Check that `program_registry_init()` is called before `monitor_test_once()` and before `monitor_start()`.

Expected order:

```c
process_init_system();
program_registry_init();
```

## Failure: undefined `user_demo_elf_start`

The embedded ELF symbols should still come from the Chapter 23 `objcopy` rule:

```make
--redefine-sym _binary_build_user_demo_elf_start=user_demo_elf_start
--redefine-sym _binary_build_user_demo_elf_end=user_demo_elf_end
```

Check:

```bash
i686-elf-nm build/user/demo_elf_blob.o
```

You should see:

```text
user_demo_elf_start
user_demo_elf_end
```

## Failure: `run demo alpha beta` starts but `argv[0]` is wrong

The monitor must pass `argv[1]` as the child’s `argv[0]`.

For:

```text
run demo alpha beta
```

monitor tokens are:

```text
argv[0] = run
argv[1] = demo
argv[2] = alpha
argv[3] = beta
```

The child should receive:

```c
const char **child_argv = (const char **)&argv[1];
int child_argc = argc - 1;
```

So the child sees:

```text
argv[0] = demo
argv[1] = alpha
argv[2] = beta
```

## Failure: monitor hangs after `run demo`

That is expected if the user program is waiting for input.

The demo program prints:

```text
user>
```

then blocks in:

```c
toyix_read(FD_STDIN, buffer, sizeof(buffer));
```

Type a line and press Enter.

## Failure: automated test hangs

The automated `program_test_once()` must inject input:

```c
keyboard_debug_inject_char('t');
keyboard_debug_inject_char('o');
keyboard_debug_inject_char('y');
keyboard_debug_inject_char('i');
keyboard_debug_inject_char('x');
keyboard_debug_inject_char('\n');
```

If the newline is missing, `SYS_READ` will never complete.

## Failure: process exits but memory is not destroyed

`program_run_foreground()` and `program_test_once()` must both call:

```c
process_wait(process);
process_destroy(process);
```

Foreground process execution owns cleanup.

---

# 15. What this chapter achieved

Before this chapter:

```text
kernel test code directly launched one embedded ELF
```

After this chapter:

```text
program registry
  ↓
program lookup by name
  ↓
monitor run command
  ↓
argv passed from monitor tokens
  ↓
foreground process execution
  ↓
exit code reported
  ↓
process cleanup
```

This is our first `exec`-like path.

Not a real filesystem-backed `exec()` yet, but the shape is now visible.

---

# 16. Design limitations

The program registry is still simple:

```text
only embedded programs
only one demo program
no filesystem
no PATH lookup
no background execution
no process list
no waitpid command
no kill command
no file descriptor inheritance
```

But it gives the monitor a real way to launch user programs.

That is a major usability improvement.

---

# 17. Next chapter

The next practical step is a **process table and PID-aware monitor commands**.

Add:

```text
global process list
PID allocation tracking
process lookup by PID
ps command
wait PID
runbg command
kill placeholder
```

That moves us from:

```text
foreground-only monitor execution
```

toward:

```text
multiple live user processes
monitor can inspect and manage them
```

The first useful commands would be:

```text
toyix> ps
toyix> runbg demo alpha beta
toyix> wait 3
```

---

# 18. Resources

- [Chapter 25 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_25)
- [Chapter 25 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_25)
- [GNU objcopy documentation](https://sourceware.org/binutils/docs/binutils/objcopy.html)

---

# 19. Closure

Chapter 25 gives Toyix its first named program launcher. The kernel now keeps an embedded program registry, the monitor can list available programs and run one in the foreground, and user-program startup flows through a cleaner boundary where the process layer owns lifecycle mechanics while the program layer owns lookup and launch policy.

Happy Coding!
