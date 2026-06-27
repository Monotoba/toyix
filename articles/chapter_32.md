# Chapter 32 — Process Ownership, Waiting, and Job State

In Chapter 32, the user shell gained:

```text
SYS_EXEC
SYS_WAITPID
```

That let a user process launch another user process:

```text
ush> run counter alpha beta
```

The flow worked:

```text
shell
  ↓ SYS_EXEC
counter process starts
  ↓ SYS_WAITPID
shell waits and receives exit code
```

But the process model was too loose.

Any process could wait for any PID.

This chapter tightens that up.

We will add:

```text
parent PID tracking
child ownership checks
zombie process state
waitpid only for child processes
orphan reparenting to kernel PID 0
```

After this chapter, a child process belongs to the process that launched it:

```text
shell pid=2
  ↓ SYS_EXEC counter
counter pid=3, ppid=2
```

Then:

```text
SYS_WAITPID(3)
```

is allowed from the shell, but not from unrelated processes.

This is a major step toward a Unix-like process lifecycle.

---

## 1. Current problem

Right now, `SYS_WAITPID` does roughly this:

```text
find process by PID
wait for it
destroy it
return exit code
```

But it does not ask:

```text
who owns this process?
```

That means any user process could wait for any other process.

That is wrong.

We want this rule:

```text
A user process may wait only for its own children.
```

For kernel-side monitor commands and kernel tests, we will keep direct kernel wait functions available.

---

## 2. New process lifecycle

Before this chapter:

```text
running
  ↓ exit
exited
  ↓ wait/destroy
destroyed
```

After this chapter:

```text
running
  ↓ exit
zombie
  ↓ parent waitpid
destroyed
```

A zombie process is:

```text
not running
has an exit code
still has a process table entry
waiting for parent to collect it
```

That gives the parent a chance to retrieve the child’s exit status.

---

## 3. Process ownership model

We will add:

```c
uint32_t parent_pid;
```

to `process_t`.

Rules:

```text
kernel-created processes have parent_pid = 0
user-created processes inherit parent_pid = current process PID
only a parent can SYS_WAITPID its child
destroying a parent reparents its children to PID 0
```

For now, PID 0 means:

```text
kernel / no user parent
```

We do not have a real PID 1 init process yet.

Later, we can introduce:

```text
init process
orphan adoption
system process tree
```

---

## 4. Update `include/kernel/process.h`

Replace the process state enum and process struct with this version.

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
    PROCESS_ZOMBIE,
    PROCESS_DESTROYED
} process_state_t;

typedef struct process {
    uint32_t magic;
    uint32_t pid;
    uint32_t parent_pid;

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

void process_set_parent(process_t *process, uint32_t parent_pid);
uint32_t process_parent_pid(process_t *process);
int process_is_child_of(process_t *process, uint32_t parent_pid);

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

Important changes:

```c
PROCESS_ZOMBIE
```

replaces the old `PROCESS_EXITED` state.

And:

```c
uint32_t parent_pid;
```

is now part of every process.

---

## 5. Update process globals in `kernel/process.c`

The existing process-table globals can stay mostly the same.

Near the top of `kernel/process.c`, make sure you still have:

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

No new global is required for parent tracking.

The parent PID lives inside each `process_t`.

---

## 6. Update `process_state_name()`

Replace the old version with:

```c
const char *process_state_name(process_state_t state) {
    switch (state) {
        case PROCESS_NEW:
            return "new";

        case PROCESS_RUNNING:
            return "running";

        case PROCESS_ZOMBIE:
            return "zombie";

        case PROCESS_DESTROYED:
            return "destroyed";

        default:
            return "unknown";
    }
}
```

Now `ps` can show:

```text
zombie
```

instead of only:

```text
exited
```

---

## 7. Initialize `parent_pid` in `process_create_empty()`

Inside `process_create_empty()`, after assigning the PID, initialize:

```c
process->parent_pid = 0;
```

The beginning of the initialized fields should look like:

```c
process->magic = PROCESS_MAGIC;
process->pid = next_pid++;
process->parent_pid = 0;

process->name = name;
process->state = PROCESS_NEW;
```

For now, every process starts as parentless.

The launcher layer will set the parent when appropriate.

---

## 8. Add parent helper functions

Add these to `kernel/process.c`:

```c
void process_set_parent(process_t *process, uint32_t parent_pid) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    process->parent_pid = parent_pid;
    irq_restore(flags);
}

uint32_t process_parent_pid(process_t *process) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    uint32_t parent_pid = process->parent_pid;
    irq_restore(flags);

    return parent_pid;
}

int process_is_child_of(process_t *process, uint32_t parent_pid) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    int result = process->parent_pid == parent_pid;
    irq_restore(flags);

    return result;
}
```

These small helpers keep parent ownership logic out of raw structure access.

---

## 9. Add orphan reparenting

When a parent process is destroyed, any children it still owns should not point to a dead parent PID.

For now, reparent them to PID 0.

Add this helper:

```c
static void process_reparent_children(uint32_t old_parent_pid, uint32_t new_parent_pid) {
    process_t *cur = process_head;

    while (cur != 0) {
        if (cur->parent_pid == old_parent_pid) {
            cur->parent_pid = new_parent_pid;
        }

        cur = cur->next;
    }
}
```

This helper assumes interrupts are already disabled by the caller.

We will call it from `process_destroy()` while holding the process-table lock.

---

## 10. Update `process_exit_current()`

Find the part where the process marks itself exited.

Replace:

```c
process->state = PROCESS_EXITED;
```

with:

```c
process->state = PROCESS_ZOMBIE;
```

The relevant block should become:

```c
process->exit_code = exit_code;
process->exited = 1;
process->state = PROCESS_ZOMBIE;

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

A process that calls `SYS_EXIT` is no longer merely “exited.”

It is now:

```text
zombie until collected
```

---

## 11. Update `process_destroy()`

`process_destroy()` must now:

```text
verify process is exited/zombie
reparent children
remove process from table
destroy address space
free process object
```

Replace it with this version:

```c
void process_destroy(process_t *process) {
    validate_live_process(process);

    if (!process->exited && process->state != PROCESS_ZOMBIE) {
        kernel_panic("process_destroy called on running process");
    }

    uint32_t pid = process->pid;
    uint32_t parent_pid = process->parent_pid;
    const char *name = process->name;

    irq_flags_t flags = irq_save();

    process_reparent_children(pid, 0);
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
    console_write(" ppid=");
    console_write_u32_dec(parent_pid);
    console_write(" name=");
    console_writeln(name);
}
```

The destroy log now includes the parent PID:

```text
Process: destroyed pid=3 ppid=2 name=counter
```

That makes ownership visible during tests.

---

## 12. Update `process_list()`

Add parent PID to the process table output.

Update the snapshot structure:

```c
typedef struct process_snapshot {
    uint32_t pid;
    uint32_t parent_pid;
    process_state_t state;
    uint32_t exit_code;
    int exited;
    const char *name;
} process_snapshot_t;
```

When taking the snapshot, add:

```c
snapshots[count].parent_pid = cur->parent_pid;
```

Then replace the header:

```c
console_writeln("PID  STATE     EXIT  NAME");
```

with:

```c
console_writeln("PID  PPID STATE     EXIT  NAME");
```

A simple full version:

```c
typedef struct process_snapshot {
    uint32_t pid;
    uint32_t parent_pid;
    process_state_t state;
    uint32_t exit_code;
    int exited;
    const char *name;
} process_snapshot_t;

static void print_padded_u32(uint32_t value, uint32_t width) {
    console_write_u32_dec(value);

    uint32_t digits = 1;
    uint32_t tmp = value;

    while (tmp >= 10) {
        tmp /= 10;
        digits++;
    }

    while (digits < width) {
        console_putc(' ');
        digits++;
    }
}

void process_list(void) {
    process_snapshot_t snapshots[PROCESS_SNAPSHOT_MAX];
    uint32_t count = 0;

    irq_flags_t flags = irq_save();

    process_t *cur = process_head;

    while (cur != 0 && count < PROCESS_SNAPSHOT_MAX) {
        snapshots[count].pid = cur->pid;
        snapshots[count].parent_pid = cur->parent_pid;
        snapshots[count].state = cur->state;
        snapshots[count].exit_code = cur->exit_code;
        snapshots[count].exited = cur->exited;
        snapshots[count].name = cur->name;
        count++;

        cur = cur->next;
    }

    uint32_t total = process_count;

    irq_restore(flags);

    console_writeln("PID  PPID STATE     EXIT  NAME");

    for (uint32_t i = 0; i < count; ++i) {
        print_padded_u32(snapshots[i].pid, 5);
        print_padded_u32(snapshots[i].parent_pid, 5);

        const char *state = process_state_name(snapshots[i].state);

        console_write(state);

        uint32_t state_len = (uint32_t)kstrlen(state);

        while (state_len < 10) {
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

The output now looks like:

```text
PID  PPID STATE     EXIT  NAME
1    0    running   -     counter
2    0    running   -     shell
3    2    zombie    4     counter
```

---

## 13. Update `program_create_process()` to assign parent PID

In `kernel/program.c`, update `program_create_process()`.

After creating the process:

```c
process_t *process = elf_create_process_suspended(
    program->name,
    program->image_start,
    image_size
);
```

add:

```c
process_t *parent = process_current();

if (parent != 0) {
    process_set_parent(process, parent->pid);
}
```

The relevant portion should become:

```c
process_t *process = elf_create_process_suspended(
    program->name,
    program->image_start,
    image_size
);

process_t *parent = process_current();

if (parent != 0) {
    process_set_parent(process, parent->pid);
}
```

Why this works:

```text
kernel monitor launches program
  process_current() == NULL
  child parent_pid = 0

user shell launches program through SYS_EXEC
  process_current() == shell
  child parent_pid = shell PID
```

That gives us correct parent ownership without adding extra parameters to the program launcher.

---

## 14. Update launch logging

Still in `program_create_process()`, expand the launch log.

Replace:

```c
console_write("Program: launching ");
console_write(program->name);
console_write(" argc=");
console_write_u32_dec((uint32_t)argc);
console_putc('\n');
```

with:

```c
console_write("Program: launching ");
console_write(program->name);
console_write(" argc=");
console_write_u32_dec((uint32_t)argc);
console_write(" ppid=");
console_write_u32_dec(process_parent_pid(process));
console_putc('\n');
```

Now launches show:

```text
Program: launching counter argc=3 ppid=2
```

when launched by the shell.

Kernel-launched programs show:

```text
Program: launching counter argc=3 ppid=0
```

---

## 15. Update `SYS_WAITPID`

Now enforce parent-child ownership.

In `kernel/syscall.c`, replace `syscall_waitpid()` with this version:

```c
static void syscall_waitpid(interrupt_frame_t *frame) {
    uint32_t pid = frame->ebx;
    uintptr_t user_status = (uintptr_t)frame->ecx;

    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    process_t *process = process_find(pid);

    if (process == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (process == current) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (!process_is_child_of(process, current->pid)) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

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

This is the key safety change.

Before:

```text
any user process could wait any PID
```

After:

```text
only parent can wait child PID
```

---

## 16. Optional: add a debug message for denied wait

For early debugging, you may want this in the ownership failure path:

```c
console_write("SYS_WAITPID: pid ");
console_write_u32_dec(pid);
console_write(" is not child of pid ");
console_write_u32_dec(current->pid);
console_putc('\n');
```

But do not leave it permanently if you want quieter logs.

The clean version simply returns:

```text
0xFFFFFFFF
```

to userland.

---

## 17. Update user shell error text

In `user/shell.c`, improve the wait failure message.

Replace:

```c
toyix_printf("run: wait failed for pid %d\n", pid);
```

with:

```c
toyix_printf("run: wait failed for pid %d\n", pid);
toyix_puts("run: process may not be a child or may not exist");
```

The full part remains:

```c
if (toyix_waitpid((toyix_u32)pid, &status) != 0) {
    toyix_printf("run: wait failed for pid %d\n", pid);
    toyix_puts("run: process may not be a child or may not exist");
    return;
}
```

This message will become useful as process ownership rules grow stricter.

---

## 18. Update `program_test_once()`

The existing test already has two useful cases:

```text
kernel launches counter
shell launches counter
```

After this chapter:

```text
kernel-launched counter has ppid=0
shell-launched counter has ppid=shell pid
```

We should make that visible.

In the background counter test, the launch should now print:

```text
Program: launching counter argc=3 ppid=0
```

In the shell test, shell-launched counter should print something like:

```text
Program: launching counter argc=3 ppid=2
```

No major structural change is needed.

However, after the background counter exits, `process_list()` should now show:

```text
zombie
```

before `process_destroy(counter)`.

That proves the new zombie state.

The existing sequence:

```c
uint32_t counter_exit = process_wait(counter);

if (counter_exit != 4) {
    kernel_panic("program test received wrong counter exit code");
}

process_list();

process_destroy(counter);
```

is perfect.

Now `process_list()` should show the counter as:

```text
zombie
```

after `process_wait()` returns and before destroy.

---

## 19. Expected `ps` output during tests

During the background counter test:

```text
PID  PPID STATE     EXIT  NAME
1    0    running   -     counter
```

After the counter exits but before it is destroyed:

```text
PID  PPID STATE     EXIT  NAME
1    0    zombie    4     counter
```

During the shell test, when the shell launches counter:

```text
Program: launching shell argc=3 ppid=0
Process: created pid=2 name=shell
...
ush> run counter alpha beta
Program: launching counter argc=3 ppid=2
Process: created pid=3 name=counter
```

Then `SYS_WAITPID` from shell is allowed because:

```text
counter.parent_pid == shell.pid
```

---

## 20. Update Makefile greps

Update the process table header grep.

Replace:

```make
grep -q "PID  STATE" build/test.log
```

with:

```make
grep -q "PID  PPID STATE" build/test.log
```

Update launch greps.

Replace:

```make
grep -q "Program: launching counter argc=3" build/test.log
```

with:

```make
grep -q "Program: launching counter argc=3 ppid=0" build/test.log
```

But note: counter is now launched twice:

```text
first by kernel test, ppid=0
second by shell, ppid=<shell pid>
```

So also add:

```make
grep -q "Program: launching counter argc=3 ppid=" build/test.log
```

For the shell-launched child, avoid hardcoding the parent PID unless your test order is stable.

Add zombie grep:

```make
grep -q "zombie" build/test.log
```

Update shell-launched counter greps:

```make
grep -q "shell: run counter pid=" build/test.log
grep -q "shell: counter exited code 4" build/test.log
```

Update process destroy greps because the log now includes `ppid`.

Replace:

```make
grep -q "Process: destroyed pid=1 name=counter" build/test.log
```

with:

```make
grep -q "Process: destroyed pid=.*name=counter" build/test.log
```

If your Makefile uses regular `grep`, the basic regular expression works.

For stricter matching, use:

```make
grep -E -q "Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=counter" build/test.log
```

A useful process/program block now includes:

```make
	grep -q "Program registry: registered 3 embedded program(s)" build/test.log
	grep -q "demo - interactive stdin/stdout demo" build/test.log
	grep -q "counter - background-safe counter demo" build/test.log
	grep -q "shell - interactive user-mode shell" build/test.log
	grep -q "Program test: starting background counter test" build/test.log
	grep -q "Program: launching counter argc=3 ppid=0" build/test.log
	grep -q "Process: created pid=1 name=counter" build/test.log
	grep -q "Program test: background pid=1" build/test.log
	grep -q "PID  PPID STATE" build/test.log
	grep -q "zombie" build/test.log
	grep -q "counter: argc=3" build/test.log
	grep -q "counter: argv\\[0\\]=counter" build/test.log
	grep -q "counter: argv\\[1\\]=alpha" build/test.log
	grep -q "counter: argv\\[2\\]=beta" build/test.log
	grep -q "counter: printf test A 0x1234 %" build/test.log
	grep -q "counter: tick 1" build/test.log
	grep -q "counter: tick 2" build/test.log
	grep -q "counter: tick 3" build/test.log
	grep -q "Syscall: process counter pid=.*exited code 4" build/test.log
	grep -E -q "Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=counter" build/test.log
	grep -q "Program test: background counter cleanup sanity check passed" build/test.log
	grep -q "Program test: starting user shell test" build/test.log
	grep -q "Program: launching shell argc=3 ppid=0" build/test.log
	grep -q "shell: Toyix user shell" build/test.log
	grep -q "commands: help, echo, args, run, exit" build/test.log
	grep -q "shell: run counter pid=" build/test.log
	grep -q "shell: counter exited code 4" build/test.log
	grep -q "Syscall: process shell pid=.*exited code 7" build/test.log
	grep -E -q "Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=shell" build/test.log
	grep -q "Program test: user shell cleanup sanity check passed" build/test.log
```

Update the final test message:

```make
	@echo "Boot, memory, heap, sync, monitor, process ownership, zombies, exec, and waitpid smoke test passed."
```

---

## 21. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 32 checks passed."
```

---

## 22. Expected milestone output

The relevant parts should look like this:

```text
Program test: starting background counter test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Program: launching counter argc=3 ppid=0
Thread: created counter id=...
Process: created pid=1 name=counter
Program test: background pid=1
PID  PPID STATE     EXIT  NAME
1    0    running   -     counter
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: printf test A 0x1234 %
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=1 exited code 4
Threads: reaping zombie counter id=...
PID  PPID STATE     EXIT  NAME
1    0    zombie    4     counter
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=1 ppid=0 name=counter
Program test: background counter cleanup sanity check passed
```

Then the shell-owned child path:

```text
Program test: starting user shell test
Program: launching shell argc=3 ppid=0
Process: created pid=2 name=shell
shell: Toyix user shell
shell: startup argc=3
shell: argv[0]=shell
shell: argv[1]=alpha
shell: argv[2]=beta
ush> run counter alpha beta
Program: launching counter argc=3 ppid=2
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
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=3 ppid=2 name=counter
shell: counter exited code 4
ush> exit 7
Syscall: process shell pid=2 exited code 7
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=2 ppid=0 name=shell
Program test: user shell cleanup sanity check passed
```

This proves:

```text
kernel-launched process has ppid=0
shell-launched process has ppid=shell_pid
child becomes zombie on exit
parent waitpid collects and destroys child
```

---

## 23. Interactive test

After boot:

```text
toyix> run shell
```

Inside the shell:

```text
ush> run counter one two
```

Expected:

```text
Program: launching counter argc=3 ppid=<shell pid>
shell: run counter pid=<counter pid>
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=one
counter: argv[2]=two
counter: printf test A 0x1234 %
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=<counter pid> exited code 4
Process: destroyed pid=<counter pid> ppid=<shell pid> name=counter
shell: counter exited code 4
```

Then:

```text
ush> exit 0
```

The kernel monitor resumes.

---

## 24. Manual ownership test idea

Later, once we add more shell commands, we can test an ownership failure directly:

```text
shell A starts child
shell B tries to wait shell A's child
waitpid fails
```

We do not have multiple interactive terminals yet, so this chapter does not automate that.

For now, the logic is enforced in `SYS_WAITPID`:

```c
if (!process_is_child_of(process, current->pid)) {
    frame->eax = 0xFFFFFFFFu;
    return;
}
```

That is the important boundary.

---

## 25. Common failures

### Failure: shell can no longer wait for counter

Check that `program_create_process()` sets the parent PID using `process_current()`.

This must happen while the syscall is still executing in the shell’s context:

```c
process_t *parent = process_current();

if (parent != 0) {
    process_set_parent(process, parent->pid);
}
```

If this is missing, shell-launched counter will have:

```text
ppid=0
```

and `SYS_WAITPID` from the shell will fail.

### Failure: kernel monitor `wait PID` stops working

The kernel monitor command does not use `SYS_WAITPID`.

It should still call the kernel function directly:

```c
process_wait(process);
process_destroy(process);
```

Do not apply user parent checks to kernel monitor wait commands.

The parent check belongs in:

```c
syscall_waitpid()
```

not in the low-level `process_wait()` function.

### Failure: `ps` still says `exited`

Check `process_state_name()` and `process_exit_current()`.

The state should become:

```c
PROCESS_ZOMBIE
```

when the process exits.

### Failure: destroyed process log grep fails

The log now includes parent PID:

```text
Process: destroyed pid=3 ppid=2 name=counter
```

Update greps that expected:

```text
Process: destroyed pid=3 name=counter
```

### Failure: children still show dead parent PID

Check `process_destroy()`.

It must call:

```c
process_reparent_children(pid, 0);
```

before removing and freeing the parent.

### Failure: process table corruption

`process_reparent_children()` must run while interrupts are disabled, because it walks the process table.

The pattern should be:

```c
irq_flags_t flags = irq_save();

process_reparent_children(pid, 0);
process_table_remove(process);

irq_restore(flags);
```

---

## 26. What this chapter achieved

Before this chapter:

```text
processes had no parent ownership
any process could wait any PID
exited process state was vague
```

After this chapter:

```text
each process has a parent PID
SYS_EXEC children belong to the calling process
SYS_WAITPID only waits for child processes
exited children become zombies
waitpid collects status and destroys child
parent destruction reparents remaining children to PID 0
```

This is a major process-management correction.

The shell is no longer just launching programs.

It is launching and collecting its own children.

---

## 27. Design limitations

Still missing:

```text
real PID 1 init process
child lists
reference counting process objects
wait for any child
nonblocking wait
parent death notification
orphan adoption by init
process groups
sessions
signals
kill
job control
```

Also, the process table still returns raw pointers:

```c
process_t *process_find(uint32_t pid);
```

That is acceptable for this teaching kernel, but a stronger kernel would need safer lifetime rules.

Eventually we should add:

```text
process table lock
process reference count
process_get()
process_put()
```

For now, the model is good enough for a single-CPU teaching OS.

---

## Resources

- Chapter source: [Toyix repository](https://github.com/Monotoba/toyix)
- Chapter release: [Chapter_32](https://github.com/Monotoba/toyix/releases/tag/Chapter_32)

## Closure

Chapter 32 gives Toyix a more coherent process lifecycle. Child ownership is now explicit, exited children become zombies until collected, and `SYS_WAITPID` is restricted to real parent-child relationships.

Happy Coding!
