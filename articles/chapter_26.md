# Chapter 26 — Process Table, `ps`, `runbg`, and `wait PID`

In Chapter 25, the monitor gained an embedded program registry and a foreground `run` command:

```text
toyix> programs
toyix> run demo alpha beta
```

That gave us an `exec`-like path, but only in foreground mode:

```text
launch process
  ↓
wait immediately
  ↓
destroy process
```

This chapter adds the next process-management layer:

```text
global process table
PID lookup
ps command
runbg command
wait PID command
```

After this chapter, the monitor will support:

```text
toyix> ps
toyix> runbg demo alpha beta
toyix> wait 3
```

The kernel still does not have job control, signals, or a filesystem, but it now has the first version of process management.

---

# 1. New process model

Before this chapter:

```text
process exists only while caller holds process_t *
```

After this chapter:

```text
process_t objects are linked into a global process table
```

The lifecycle becomes:

```text
process_create_empty()
  ↓
insert into process table
  ↓
load program
  ↓
start user thread
  ↓
process exits
  ↓
state becomes PROCESS_EXITED
  ↓
wait PID collects exit code
  ↓
process_destroy()
  ↓
remove from process table
  ↓
free address space and process object
```

This gives the monitor something to inspect.

---

# 2. New commands

We will add:

```text
ps
```

List active and exited-but-not-yet-collected processes.

```text
runbg PROGRAM [ARGS...]
```

Start a program but return to the monitor immediately.

```text
wait PID
```

Wait for a process to exit, print its exit code, and destroy it.

Example:

```text
toyix> runbg demo alpha beta
runbg: started demo pid=2

toyix> ps
PID  STATE    EXIT  NAME
2    running  -     demo

toyix> wait 2
wait: pid 2 exited code 9
```

Important limitation: our demo program reads from the shared terminal. If you run it in the background, it may still wait for keyboard input. That is okay for now, but it means interactive background programs can compete with the monitor for input.

---

# 3. Patch overview

Modify:

```text
include/kernel/process.h
kernel/process.c
include/kernel/program.h
kernel/program.c
kernel/monitor.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

No new assembly is needed.

---

# 4. Update `include/kernel/process.h`

Replace it with this version.

```c
// include/kernel/process.h
#ifndef TOYIX_KERNEL_PROCESS_H
#define TOYIX_KERNEL_PROCESS_H

#include <stdint.h>
#include "kernel/address_space.h"

struct thread;

typedef enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_EXITED,
    PROCESS_DESTROYED
} process_state_t;

typedef struct process {
    uint32_t magic;
    uint32_t pid;

    const char *name;
    process_state_t state;

    address_space_t *address_space;

    struct thread *main_thread;

    uint32_t exit_code;
    int exited;

    uintptr_t user_code_base;
    uintptr_t user_entry;

    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
    uintptr_t user_initial_esp;

    struct process *next;
    struct process *prev;
} process_t;

void process_init_system(void);

process_t *process_create_empty(const char *name);

int process_map_user_region(
    process_t *process,
    uintptr_t virtual_addr,
    uint32_t size_bytes
);

int process_copy_to_user_init(
    process_t *process,
    uintptr_t user_dest,
    const void *kernel_src,
    uint32_t size
);

int process_zero_user_init(
    process_t *process,
    uintptr_t user_dest,
    uint32_t size
);

void process_set_user_entry(process_t *process, uintptr_t entry);

void process_set_user_stack(
    process_t *process,
    uintptr_t stack_base,
    uintptr_t stack_top
);

int process_setup_arguments(
    process_t *process,
    int argc,
    const char **argv
);

void process_start_user(process_t *process);

process_t *process_current(void);

void process_exit_current(uint32_t exit_code);

process_t *process_find(uint32_t pid);
void process_list(void);
const char *process_state_name(process_state_t state);

uint32_t process_wait(process_t *process);
void process_destroy(process_t *process);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

#endif
```

The new fields are:

```c
struct process *next;
struct process *prev;
```

The new APIs are:

```c
process_t *process_find(uint32_t pid);
void process_list(void);
const char *process_state_name(process_state_t state);
```

---

# 5. Update globals in `kernel/process.c`

Near the top of `kernel/process.c`, update the global state.

```c
#define PROCESS_MAGIC 0x50524F43u

#define PROCESS_SNAPSHOT_MAX 32u

static uint32_t next_pid;
static volatile uint32_t last_exit_code;
static volatile int last_exit_seen;

static process_t *process_head;
static process_t *process_tail;
static uint32_t process_count;

static void user_process_thread_entry(void *arg);
```

The process table is a simple doubly linked list.

That is enough for now.

---

# 6. Add process-table helpers in `kernel/process.c`

Add these helpers after `validate_live_process()`.

```c
const char *process_state_name(process_state_t state) {
    switch (state) {
        case PROCESS_NEW:
            return "new";

        case PROCESS_RUNNING:
            return "running";

        case PROCESS_EXITED:
            return "exited";

        case PROCESS_DESTROYED:
            return "destroyed";

        default:
            return "unknown";
    }
}

static void process_table_insert(process_t *process) {
    validate_process(process);

    process->next = 0;
    process->prev = process_tail;

    if (process_tail != 0) {
        process_tail->next = process;
    } else {
        process_head = process;
    }

    process_tail = process;
    process_count++;
}

static void process_table_remove(process_t *process) {
    validate_process(process);

    if (process->prev != 0) {
        process->prev->next = process->next;
    } else {
        process_head = process->next;
    }

    if (process->next != 0) {
        process->next->prev = process->prev;
    } else {
        process_tail = process->prev;
    }

    process->next = 0;
    process->prev = 0;

    if (process_count > 0) {
        process_count--;
    }
}
```

These helpers are internal to `process.c`.

---

# 7. Update `process_init_system()`

Replace the existing version with this one.

```c
void process_init_system(void) {
    next_pid = 1;
    last_exit_code = 0xFFFFFFFFu;
    last_exit_seen = 0;

    process_head = 0;
    process_tail = 0;
    process_count = 0;

    console_writeln("Process: process table initialized");
}
```

---

# 8. Update `process_create_empty()`

Inside `process_create_empty()`, initialize the list fields:

```c
process->next = 0;
process->prev = 0;
```

Then insert the process into the process table.

The end of `process_create_empty()` should look like this:

```c
process->user_code_base = 0;
process->user_entry = 0;
process->user_stack_base = 0;
process->user_stack_top = 0;
process->user_initial_esp = 0;

process->next = 0;
process->prev = 0;

irq_flags_t flags = irq_save();
process_table_insert(process);
irq_restore(flags);

return process;
```

Now every created process appears in the global process table.

---

# 9. Add `process_find()` and `process_list()`

Add these after `process_current()` or near the other process-table functions.

```c
process_t *process_find(uint32_t pid) {
    irq_flags_t flags = irq_save();

    process_t *cur = process_head;

    while (cur != 0) {
        if (cur->pid == pid) {
            irq_restore(flags);
            return cur;
        }

        cur = cur->next;
    }

    irq_restore(flags);
    return 0;
}
```

Now add a snapshot-based `process_list()`.

We avoid holding interrupts disabled while printing.

```c
typedef struct process_snapshot {
    uint32_t pid;
    process_state_t state;
    uint32_t exit_code;
    int exited;
    const char *name;
} process_snapshot_t;

void process_list(void) {
    process_snapshot_t snapshots[PROCESS_SNAPSHOT_MAX];
    uint32_t count = 0;

    irq_flags_t flags = irq_save();

    process_t *cur = process_head;

    while (cur != 0 && count < PROCESS_SNAPSHOT_MAX) {
        snapshots[count].pid = cur->pid;
        snapshots[count].state = cur->state;
        snapshots[count].exit_code = cur->exit_code;
        snapshots[count].exited = cur->exited;
        snapshots[count].name = cur->name;
        count++;

        cur = cur->next;
    }

    uint32_t total = process_count;

    irq_restore(flags);

    console_writeln("PID  STATE     EXIT  NAME");

    for (uint32_t i = 0; i < count; ++i) {
        console_write_u32_dec(snapshots[i].pid);

        if (snapshots[i].pid < 10) {
            console_write("    ");
        } else if (snapshots[i].pid < 100) {
            console_write("   ");
        } else {
            console_write("  ");
        }

        const char *state = process_state_name(snapshots[i].state);

        console_write(state);

        /*
         * Crude spacing for the early monitor.
         */
        uint32_t state_len = (uint32_t)kstrlen(state);

        while (state_len < 9) {
            console_putc(' ');
            state_len++;
        }

        if (snapshots[i].exited) {
            console_write_u32_dec(snapshots[i].exit_code);
        } else {
            console_putc('-');
        }

        console_write("     ");
        console_writeln(snapshots[i].name);
    }

    if (total > count) {
        console_write("ps: truncated process list at ");
        console_write_u32_dec(PROCESS_SNAPSHOT_MAX);
        console_writeln(" entries");
    }
}
```

This gives us a simple `ps` backend.

---

# 10. Update `process_exit_current()`

The existing function already marks the process exited.

Make sure the process state is set:

```c
process->exit_code = exit_code;
process->exited = 1;
process->state = PROCESS_EXITED;
```

The full process branch should remain like this:

```c
process->exit_code = exit_code;
process->exited = 1;
process->state = PROCESS_EXITED;

last_exit_code = exit_code;
last_exit_seen = 1;

console_write("Syscall: process ");
console_write(process->name);
console_write(" pid=");
console_write_u32_dec(process->pid);
console_write(" exited code ");
console_write_u32_dec(exit_code);
console_putc('\n');
```

No major change is needed here.

---

# 11. Update `process_destroy()`

`process_destroy()` must remove the process from the global table before freeing it.

Replace the top portion of `process_destroy()` with this version.

```c
void process_destroy(process_t *process) {
    validate_live_process(process);

    if (!process->exited) {
        kernel_panic("process_destroy called on running process");
    }

    uint32_t pid = process->pid;
    const char *name = process->name;

    irq_flags_t flags = irq_save();
    process_table_remove(process);
    irq_restore(flags);

    if (process->address_space != 0) {
        address_space_destroy(process->address_space);
        process->address_space = 0;
    }

    process->state = PROCESS_DESTROYED;
    process->magic = 0;

    kfree(process);

    console_write("Process: destroyed pid=");
    console_write_u32_dec(pid);
    console_write(" name=");
    console_writeln(name);
}
```

Now destroyed processes disappear from `ps`.

---

# 12. Update `include/kernel/program.h`

Replace it with this version.

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

int program_run_background(
    const char *name,
    int argc,
    const char **argv,
    process_t **process_out
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

The new API is:

```c
int program_run_background(...);
```

---

# 13. Update `kernel/program.c`

Keep most of the Chapter 25 file, but replace the run helpers.

`program_create_process()` remains the common creator.

Make sure it still starts the process and returns the pointer:

```c
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
```

Now add `program_run_background()`:

```c
int program_run_background(
    const char *name,
    int argc,
    const char **argv,
    process_t **process_out
) {
    process_t *process = program_create_process(name, argc, argv);

    if (process == 0) {
        return -1;
    }

    if (process_out != 0) {
        *process_out = process;
    }

    return 0;
}
```

Then update `program_run_foreground()` to use the background helper:

```c
int program_run_foreground(
    const char *name,
    int argc,
    const char **argv,
    uint32_t *exit_code_out
) {
    process_t *process = 0;

    int rc = program_run_background(
        name,
        argc,
        argv,
        &process
    );

    if (rc != 0 || process == 0) {
        return -1;
    }

    uint32_t exit_code = process_wait(process);

    process_destroy(process);

    if (exit_code_out != 0) {
        *exit_code_out = exit_code;
    }

    return 0;
}
```

---

# 14. Update `program_test_once()`

The test should now explicitly exercise background launch, process table visibility, wait, and destroy.

Replace `program_test_once()` with this version.

```c
void program_test_once(void) {
    console_writeln("Program test: starting background process table test");

    static const char *argv[] = {
        "demo",
        "alpha",
        "beta"
    };

    process_t *process = 0;

    int rc = program_run_background(
        "demo",
        3,
        argv,
        &process
    );

    if (rc != 0 || process == 0) {
        kernel_panic("program test could not launch demo");
    }

    uint32_t pid = process->pid;

    console_write("Program test: background pid=");
    console_write_u32_dec(pid);
    console_putc('\n');

    process_list();

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

    process_t *found = process_find(pid);

    if (found != process) {
        kernel_panic("program test could not find background process by PID");
    }

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("program test received wrong exit code");
    }

    process_list();

    process_destroy(process);

    console_writeln("Program test: background process table cleanup sanity check passed");
}
```

This test proves:

```text
program can launch in background
process is listed by ps backend
process can be found by PID
process can be waited
exited process remains in table until destroyed
destroy removes it and frees memory
```

---

# 15. Update `kernel/monitor.c`

Add:

```c
#include "kernel/process.h"
```

You already included `kernel/program.h` in Chapter 25.

Add prototypes:

```c
static int cmd_ps(int argc, char **argv);
static int cmd_runbg(int argc, char **argv);
static int cmd_wait(int argc, char **argv);
```

Add command-table entries.

```c
{
    .name = "ps",
    .usage = "ps",
    .help = "list processes",
    .handler = cmd_ps
},
{
    .name = "runbg",
    .usage = "runbg PROGRAM [ARGS...]",
    .help = "run an embedded user program in the background",
    .handler = cmd_runbg
},
{
    .name = "wait",
    .usage = "wait PID",
    .help = "wait for a process and collect its exit code",
    .handler = cmd_wait
},
```

A good command-table order is:

```text
help
ticks
threads
ps
mem
heap
programs
run
runbg
wait
sleep
echo
clear
```

Now add the command handlers.

```c
static int cmd_ps(int argc, char **argv) {
    (void)argv;

    if (argc != 1) {
        console_writeln("usage: ps");
        return 1;
    }

    process_list();
    return 1;
}
```

```c
static int cmd_runbg(int argc, char **argv) {
    if (argc < 2) {
        console_writeln("usage: runbg PROGRAM [ARGS...]");
        return 1;
    }

    const char *program_name = argv[1];

    int child_argc = argc - 1;
    const char **child_argv = (const char **)&argv[1];

    process_t *process = 0;

    int rc = program_run_background(
        program_name,
        child_argc,
        child_argv,
        &process
    );

    if (rc != 0 || process == 0) {
        console_write("runbg: unknown program ");
        console_writeln(program_name);
        console_writeln("type 'programs' to list available programs");
        return 1;
    }

    console_write("runbg: started ");
    console_write(program_name);
    console_write(" pid=");
    console_write_u32_dec(process->pid);
    console_putc('\n');

    return 1;
}
```

For `wait`, we need to parse a PID.

`monitor.c` already has `parse_u32()` from earlier chapters. Reuse it.

```c
static int cmd_wait(int argc, char **argv) {
    if (argc != 2) {
        console_writeln("usage: wait PID");
        return 1;
    }

    uint32_t pid = 0;

    if (!parse_u32(argv[1], &pid)) {
        console_writeln("wait: expected decimal PID");
        return 1;
    }

    process_t *process = process_find(pid);

    if (process == 0) {
        console_write("wait: no such PID ");
        console_write_u32_dec(pid);
        console_putc('\n');
        return 1;
    }

    uint32_t exit_code = process_wait(process);

    console_write("wait: pid ");
    console_write_u32_dec(pid);
    console_write(" exited code ");
    console_write_u32_dec(exit_code);
    console_putc('\n');

    process_destroy(process);

    return 1;
}
```

Finally, update `monitor_test_once()` to exercise the command parser without launching an input-blocking process:

```c
void monitor_test_once(void) {
    console_writeln("Monitor test: starting command table test");

    monitor_execute_command("help ticks");
    monitor_execute_command("ticks");
    monitor_execute_command("ps");
    monitor_execute_command("programs");
    monitor_execute_command("run");
    monitor_execute_command("runbg");
    monitor_execute_command("wait");
    monitor_execute_command("echo monitor ok");
    monitor_execute_command("unknown-test-command");

    console_writeln("Monitor test: command table sanity check passed");
}
```

This tests that the commands exist and produce usage output. The dedicated `program_test_once()` does the real process launch.

---

# 16. Update monitor help text

Once this chapter is applied, `help` should list:

```text
ps
programs
run
runbg
wait
```

Example:

```text
toyix> help runbg
usage: runbg PROGRAM [ARGS...]
  run an embedded user program in the background
```

---

# 17. Update `kernel/kmain.c`

The high-level flow remains mostly the same.

Make sure the initialization order is still:

```c
threading_init();
process_init_system();
program_registry_init();
thread_test_once();
```

And make sure the test section still calls:

```c
program_test_once();
```

No other `kmain.c` changes are required.

---

# 18. Update `Makefile` object list

No new object is needed if `kernel/program.o` was already added in Chapter 25.

If not, make sure this is present:

```text
build/kernel/program.o
```

---

# 19. Update test greps

Replace Chapter 25’s program-test lines:

```make
grep -q "Program test: starting embedded program run test" build/test.log
grep -q "Program test: embedded ELF program run cleanup sanity check passed" build/test.log
```

with:

```make
grep -q "Program test: starting background process table test" build/test.log
grep -q "Program test: background pid=1" build/test.log
grep -q "PID  STATE" build/test.log
grep -q "Program test: background process table cleanup sanity check passed" build/test.log
```

Also add greps for the new command usage lines from `monitor_test_once()`:

```make
grep -q "usage: runbg PROGRAM" build/test.log
grep -q "usage: wait PID" build/test.log
```

The process/program block should now include:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Program registry: registered 1 embedded program(s)" build/test.log
	grep -q "Embedded programs:" build/test.log
	grep -q "demo - compiled user-mode demo program" build/test.log
	grep -q "usage: runbg PROGRAM" build/test.log
	grep -q "usage: wait PID" build/test.log
	grep -q "Program test: starting background process table test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000" build/test.log
	grep -q "ELF32: entry=0x40100000" build/test.log
	grep -q "Process: initial stack argc=3" build/test.log
	grep -q "Program: launching demo argc=3" build/test.log
	grep -q "Process: created pid=1 name=demo" build/test.log
	grep -q "Program test: background pid=1" build/test.log
	grep -q "PID  STATE" build/test.log
	grep -q "argc=3" build/test.log
	grep -q "argv\\[0\\]=demo" build/test.log
	grep -q "argv\\[1\\]=alpha" build/test.log
	grep -q "argv\\[2\\]=beta" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process demo pid=1 exited code 9" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=demo" build/test.log
	grep -q "Program test: background process table cleanup sanity check passed" build/test.log
```

Update the final success line:

```make
	@echo "Boot, memory, heap, sync, monitor, process table, and runbg/wait smoke test passed."
```

---

# 20. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 26 checks passed."
```

---

# 21. Expected boot output

A successful boot should include:

```text
Process: process table initialized
Program registry: registered 1 embedded program(s)
...
Monitor test: starting command table test
PID  STATE     EXIT  NAME
Embedded programs:
  demo - compiled user-mode demo program
usage: run PROGRAM [ARGS...]
usage: runbg PROGRAM [ARGS...]
usage: wait PID
...
Program test: starting background process table test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Program: launching demo argc=3
Thread: created demo id=...
Process: created pid=1 name=demo
Program test: background pid=1
PID  STATE     EXIT  NAME
1    running   -     demo
argc=3
argv[0]=demo
argv[1]=alpha
argv[2]=beta
user> toyix
echo: toyix
Syscall: process demo pid=1 exited code 9
Threads: reaping zombie demo id=...
PID  STATE     EXIT  NAME
1    exited    9     demo
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=1 name=demo
Program test: background process table cleanup sanity check passed
```

After boot, try:

```text
toyix> ps
toyix> programs
toyix> run demo one two
toyix> runbg demo one two
toyix> ps
toyix> wait 2
```

---

# 22. Common failures

## Failure: `ps` shows nothing while background process should exist

Check that `process_create_empty()` inserts the process into the table:

```c
process_table_insert(process);
```

Also check that `process_destroy()` is not being called immediately after `runbg`.

Foreground `run` destroys the process after waiting.

Background `runbg` must not.

## Failure: process table corrupts after destroy

Check that `process_table_remove()` handles all four cases:

```text
only node
head node
tail node
middle node
```

The code should update both `next` and `prev`.

## Failure: `wait PID` crashes

Likely causes:

```text
process_find() returned a stale pointer
process was already destroyed
wait called on invalid PID
```

For now, the monitor is single-threaded enough that this is manageable. Later, a real process table should use references or locks.

## Failure: `runbg demo` seems to freeze input

The demo program reads from stdin.

If you run it in the background, it can consume terminal input that you expected the monitor to receive.

This is not a bug in the process table. It is a limitation of our current terminal model.

Later we need:

```text
per-process controlling terminal behavior
background job input rules
noninteractive demo program
pipes or /dev/null-style input
```

For now, `run demo` is better for interactive programs, while `runbg` is mainly a process-management milestone.

## Failure: process exits but remains in `ps`

That is expected until you collect it.

The process lifecycle is now:

```text
running
  ↓
exited
  ↓ wait PID
destroyed and removed
```

An exited process remains visible so the monitor can collect its exit code.

---

# 23. What this chapter achieved

Before this chapter:

```text
run demo
  ↓
foreground only
  ↓
process disappears immediately after wait/destroy
```

After this chapter:

```text
process table
PID lookup
ps
runbg
wait PID
foreground and background launch paths
exited process collection
```

This is the first recognizable process-management interface in the kernel.

---

# 24. Design limitations

Still missing:

```text
process table locking with real references
waitpid semantics
parent/child ownership
orphan handling
zombie process state separate from exited process object
kill
signals
background terminal discipline
multiple embedded programs
filesystem-backed exec
```

But the structure is now in place.

We can launch, inspect, wait for, and collect processes by PID.

---

# 25. Next chapter

The next useful step is to add a second user program that does **not** read from stdin.

That gives `runbg` something safe to launch.

For example:

```text
counter
  prints argv
  prints tick messages
  sleeps between messages
  exits
```

Then the monitor can safely do:

```text
toyix> runbg counter A B
toyix> ps
toyix> wait 2
```

---

# 26. Resources

- [Chapter 26 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_26)
- [Chapter 26 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_26)
- [GNU ld linker script documentation](https://sourceware.org/binutils/docs/ld/Scripts.html)

---

# 27. Closure

Chapter 26 gives Toyix its first recognizable process-management interface. The kernel now tracks live processes in a global table, the monitor can list them, background-launch a user program, wait on a PID, and collect exit codes explicitly instead of only through a foreground helper path.

Happy Coding!
