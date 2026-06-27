# Chapter 33 — Process Termination and Kill Checks

By the end of Chapter 32, Toyix could launch child processes, wait for them, and keep exited children around as zombies until the parent collected them.

For this chapter, we also extend the shell and syscall ABI just enough to inspect child state from user mode:

```text
SYS_GETPID
SYS_GETPPID
SYS_PROCINFO
runbg
jobs
wait PID
```

That gives the shell basic process ownership and job-state tracking, but it still leaves one obvious gap:

```text
there was no way to ask a child process to terminate
```

This chapter adds the first process termination mechanism:

```text
SYS_KILL
```

But we will **not** implement hard asynchronous thread destruction yet.

That is dangerous.

Instead, this chapter adds **cooperative kill**:

```text
parent shell
  ↓ SYS_KILL(child_pid)
kernel marks child kill_requested
  ↓
child reaches a syscall boundary
  ↓
kernel converts child into exited/zombie process
  ↓
parent wait collects status
```

After this chapter:

```text
ush> runbg counter victim
shell: runbg counter pid=4

ush> kill 4
shell: kill requested pid=4

ush> wait 4
shell: wait pid=4 name=counter code=128
```

Exit code `128` will mean:

```text
terminated by kill request
```

This is not Unix signal behavior yet. It is a safe teaching-kernel step toward signals and forced termination.

---
## 1. Why cooperative kill first?

Hard-killing a process safely requires answering hard questions:

```text
What if the process is inside the kernel?
What if it holds a mutex?
What if it is sleeping?
What if it is halfway through modifying process state?
What if its thread stack is active?
What if another subsystem holds a pointer to it?
```

A real kernel needs careful rules for all of that.

Toyix is not ready for arbitrary asynchronous destruction.

So we start with a safer rule:

```text
SYS_KILL marks a process for termination.
The target process exits at a syscall boundary.
```

That means termination occurs at a controlled point where the kernel already owns execution.

---

## 2. What this chapter adds

Modify:

```text
include/kernel/process.h
kernel/process.c
include/kernel/syscall.h
kernel/syscall.c
user/include/toyix_syscall.h
user/shell.c
kernel/program.c
Makefile
tests/smoke.sh
```

New syscall:

```text
SYS_KILL = 11
```

New process field:

```c
int kill_requested;
```

New shell command:

```text
kill PID
```

---

## 3. New behavior

A user process may kill only its own child.

So the shell can do:

```text
ush> runbg counter victim
ush> kill <counter_pid>
```

But it cannot kill unrelated processes.

Kernel monitor commands can still manage processes directly through kernel functions later, but the user syscall enforces parent/child ownership.

---

## 4. Exit code convention

For this chapter:

```text
128 = killed cooperatively
```

Add a named constant:

```c
#define PROCESS_KILLED_EXIT_CODE 128u
```

Later, if we add real Unix-like signals, we may use:

```text
128 + signal_number
```

For now, plain `128` is enough.

---

## 5. Update `include/kernel/process.h`

Add `kill_requested` to `process_t`.

Replace the struct with this version:

```c
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
    int kill_requested;

    uintptr_t user_code_base;
    uintptr_t user_entry;

    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
    uintptr_t user_initial_esp;

    struct process *next;
    struct process *prev;
} process_t;
```

Add these declarations near the other process helpers:

```c
#define PROCESS_KILLED_EXIT_CODE 128u

void process_request_kill(process_t *process);
int process_kill_requested(process_t *process);
int process_request_kill_child(uint32_t parent_pid, uint32_t child_pid);
```

The relevant section should look like:

```c
void process_set_parent(process_t *process, uint32_t parent_pid);
uint32_t process_parent_pid(process_t *process);
int process_is_child_of(process_t *process, uint32_t parent_pid);

void process_request_kill(process_t *process);
int process_kill_requested(process_t *process);
int process_request_kill_child(uint32_t parent_pid, uint32_t child_pid);

void process_start_user(process_t *process);
```

---

## 6. Initialize `kill_requested`

In `kernel/process.c`, inside `process_create_empty()`, initialize:

```c
process->kill_requested = 0;
```

The process initialization block should include:

```c
process->exit_code = 0xFFFFFFFFu;
process->exited = 0;
process->kill_requested = 0;
```

---

## 7. Add process kill helpers

Add these functions to `kernel/process.c`:

```c
void process_request_kill(process_t *process) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();

    if (!process->exited &&
        process->state != PROCESS_ZOMBIE &&
        process->state != PROCESS_DESTROYED) {
        process->kill_requested = 1;
    }

    irq_restore(flags);
}
```

```c
int process_kill_requested(process_t *process) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    int requested = process->kill_requested;
    irq_restore(flags);

    return requested;
}
```

```c
int process_request_kill_child(uint32_t parent_pid, uint32_t child_pid) {
    irq_flags_t flags = irq_save();

    process_t *cur = process_head;

    while (cur != 0) {
        if (cur->pid == child_pid &&
            cur->parent_pid == parent_pid) {
            if (!cur->exited &&
                cur->state != PROCESS_ZOMBIE &&
                cur->state != PROCESS_DESTROYED) {
                cur->kill_requested = 1;
                irq_restore(flags);
                return 0;
            }

            irq_restore(flags);
            return -1;
        }

        cur = cur->next;
    }

    irq_restore(flags);
    return -1;
}
```

This helper enforces the ownership rule:

```text
parent PID must match target's parent_pid
```

So userland cannot kill arbitrary processes.

---

## 8. Show kill status in `ps`

Update the process snapshot structure in `process_list()`.

Add:

```c
int kill_requested;
```

The snapshot becomes:

```c
typedef struct process_snapshot {
    uint32_t pid;
    uint32_t parent_pid;
    process_state_t state;
    uint32_t exit_code;
    int exited;
    int kill_requested;
    const char *name;
} process_snapshot_t;
```

When taking the snapshot, add:

```c
snapshots[count].kill_requested = cur->kill_requested;
```

Update the header:

```c
console_writeln("PID  PPID STATE     EXIT  KILL NAME");
```

Then print the kill flag before the name:

```c
if (snapshots[i].kill_requested) {
    console_write("yes  ");
} else {
    console_write("no   ");
}

console_writeln(snapshots[i].name);
```

The output becomes:

```text
PID  PPID STATE     EXIT  KILL NAME
4    2    running   -     yes  counter
```

This makes kill requests visible from the kernel monitor’s `ps`.

---

## 9. Update process destroy cleanup

In `process_destroy()`, before clearing the magic, clear:

```c
process->kill_requested = 0;
```

The end of the function should include:

```c
process->state = PROCESS_DESTROYED;
process->kill_requested = 0;
process->magic = 0;

kfree(process);
```

---

## 10. Update `include/kernel/syscall.h`

Add:

```c
#define SYS_KILL 11u
```

The syscall list becomes:

```c
#define SYS_PUTC     1u
#define SYS_EXIT     2u
#define SYS_WRITE    3u
#define SYS_SLEEP    4u
#define SYS_READ     5u
#define SYS_EXEC     6u
#define SYS_WAITPID  7u
#define SYS_GETPID   8u
#define SYS_GETPPID  9u
#define SYS_PROCINFO 10u
#define SYS_KILL     11u
```

No new ABI struct is needed for this chapter.

---

## 11. Update `user/include/toyix_syscall.h`

Add the syscall number:

```c
#define SYS_KILL     11u
```

Then add this wrapper after `toyix_procinfo()`:

```c
static inline toyix_i32 toyix_kill(toyix_u32 pid) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_KILL),
          "b"(pid)
        : "memory"
    );

    return result;
}
```

Return behavior:

```text
0   success
-1  failure
```

In raw register terms:

```text
EAX = 0
EAX = 0xFFFFFFFF
```

---

## 12. Add kernel `SYS_KILL`

In `kernel/syscall.c`, add:

```c
static void syscall_kill(interrupt_frame_t *frame) {
    uint32_t target_pid = frame->ebx;

    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (target_pid == 0 || target_pid == current->pid) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (process_request_kill_child(current->pid, target_pid) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = 0;
}
```

This enforces:

```text
must be a user process
cannot kill PID 0
cannot kill self
can kill only own child
```

---

## 13. Add kill-before-return handling

The kernel must turn a kill request into process exit at a safe point.

Add this helper in `kernel/syscall.c`:

```c
static int syscall_finish_or_kill(interrupt_frame_t *frame) {
    (void)frame;

    process_t *current = process_current();

    if (current == 0) {
        return 0;
    }

    if (!process_kill_requested(current)) {
        return 0;
    }

    process_exit_current(PROCESS_KILLED_EXIT_CODE);
    thread_exit();

    return 1;
}
```

Why return `int`?

It lets syscall cases do:

```c
if (syscall_finish_or_kill(frame)) {
    return;
}
```

`thread_exit()` should not return in normal operation, but returning a value keeps the control flow explicit.

---

## 14. Use kill check in `syscall_handler()`

For this first version, check kill requests:

```text
at syscall entry
after syscalls that may block or return to user
```

At the top of `syscall_handler()`, after reading the syscall number, add:

```c
uint32_t syscall_number = frame->eax;

if (syscall_number != SYS_EXIT &&
    syscall_number != SYS_WAITPID &&
    syscall_number != SYS_KILL) {
    if (syscall_finish_or_kill(frame)) {
        return;
    }
}
```

Why exclude some syscalls?

```text
SYS_EXIT    should be allowed to exit normally
SYS_WAITPID parent might be waiting; don't kill parent before it can collect
SYS_KILL    parent must be allowed to request child kill
```

Then, after syscall cases that block or return to user, call:

```c
syscall_finish_or_kill(frame);
```

The most important one is `SYS_SLEEP`.

Update the `SYS_SLEEP` case:

```c
case SYS_SLEEP: {
    uint32_t ticks = frame->ebx;
    thread_sleep_ticks(ticks);
    frame->eax = 0;
    syscall_finish_or_kill(frame);
    return;
}
```

This matters because `counter` sleeps between ticks. If the shell kills it while it sleeps, the process should exit when the sleep syscall completes, before returning to user mode.

For simple fast syscalls like `GETPID`, the entry check is enough for the next syscall boundary. But adding a finish check to common returning syscalls is fine:

```c
case SYS_WRITE:
    syscall_write(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_READ:
    syscall_read(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_GETPID:
    syscall_getpid(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_GETPPID:
    syscall_getppid(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_PROCINFO:
    syscall_procinfo(frame);
    syscall_finish_or_kill(frame);
    return;
```

Do not call it after `SYS_EXIT`, because `SYS_EXIT` already exits.

---

## 15. Important control-flow note

If `syscall_finish_or_kill()` calls:

```c
thread_exit();
```

the current thread becomes zombie and the scheduler switches away.

That means the syscall never returns to the killed user process.

The parent will later collect the child using:

```text
wait
```

and receive exit code:

```text
128
```

---

## 16. Update `syscall_handler()` case table

Add:

```c
case SYS_KILL:
    syscall_kill(frame);
    return;
```

The process-related syscall section now includes:

```c
case SYS_EXEC:
    syscall_exec(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_WAITPID:
    syscall_waitpid(frame);
    return;

case SYS_GETPID:
    syscall_getpid(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_GETPPID:
    syscall_getppid(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_PROCINFO:
    syscall_procinfo(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_KILL:
    syscall_kill(frame);
    return;

case SYS_EXIT: {
    uint32_t exit_code = frame->ebx;

    process_exit_current(exit_code);
    thread_exit();
    return;
}
```

For `SYS_EXEC`, the finish check applies to the calling process, not the new child.

That is fine.

---

## 17. Update `user/shell.c` help

Update the help text.

Replace:

```c
toyix_puts("commands: help, echo, args, run, runbg, jobs, wait, exit");
```

with:

```c
toyix_puts("commands: help, echo, args, run, runbg, jobs, wait, kill, exit");
```

---

## 18. Add shell `kill` command

Add this function to `user/shell.c`:

```c
static void cmd_kill(int argc, char **argv) {
    if (argc != 2) {
        toyix_puts("usage: kill PID");
        return;
    }

    toyix_i32 parsed = 0;

    if (!toyix_atoi(argv[1], &parsed) || parsed <= 0) {
        toyix_puts("kill: expected positive PID");
        return;
    }

    toyix_u32 pid = (toyix_u32)parsed;

    if (toyix_kill(pid) != 0) {
        toyix_printf("kill: failed for pid %u\n", pid);
        return;
    }

    toyix_printf("shell: kill requested pid=%u\n", pid);
}
```

Add the command branch before `exit`:

```c
if (toyix_streq(cmd_argv[0], "kill")) {
    cmd_kill(cmd_argc, cmd_argv);
    continue;
}
```

---

## 19. Shell jobs after kill

The existing `jobs` command should show the process as either:

```text
state=running
```

if it has not reached a syscall boundary yet, or:

```text
state=zombie code=128
```

after the cooperative kill is honored.

The shell’s `wait` command should then collect:

```text
shell: wait pid=4 name=counter code=128
```

---

## 20. Update shell test in `kernel/program.c`

We want to test kill deterministically.

Current shell test sequence likely includes:

```c
inject_text("runbg counter bg\n");
inject_text("jobs\n");

thread_sleep_ticks(10);

inject_text("jobs\n");
inject_text("wait\n");
inject_text("jobs\n");
```

Replace the background section with a kill-oriented sequence.

Use:

```c
inject_text("runbg counter victim\n");
inject_text("jobs\n");
```

Then sleep briefly so the shell has launched the job and the child has started:

```c
thread_sleep_ticks(2);
```

Now we need to send:

```text
kill <pid>
```

But the test does not know the PID as text.

To keep the test simple and deterministic, use the fact that earlier tests create predictable PIDs if your sequence is stable:

```text
pid=1  kernel background counter test
pid=2  shell
pid=3  shell foreground counter
pid=4  shell background counter victim
```

So inject:

```c
inject_text("kill 4\n");
```

Then allow the child to hit a syscall boundary:

```c
thread_sleep_ticks(10);
```

Then:

```c
inject_text("jobs\n");
inject_text("wait 4\n");
inject_text("jobs\n");
```

The full section becomes:

```c
inject_text("help\n");
inject_text("echo hello\n");
inject_text("run counter alpha beta\n");
inject_text("runbg counter victim\n");
inject_text("jobs\n");

thread_sleep_ticks(2);

inject_text("kill 4\n");

thread_sleep_ticks(10);

inject_text("jobs\n");
inject_text("wait 4\n");
inject_text("jobs\n");
inject_text("args\n");
inject_text("exit 7\n");
```

### Less brittle alternative

Hardcoding `4` is brittle.

A better future test is to add a shell command:

```text
killlast
waitlast
```

or make the shell support:

```text
kill %1
wait %1
```

But that is more shell work.

For this chapter, the PID sequence is acceptable if your boot tests are stable.

If your PIDs differ, update the injected PID or loosen the test by using an interactive manual check.

---

## 21. Expected shell output

The relevant section should look like:

```text
ush> runbg counter victim
Program: launching counter argc=2 ppid=2
Process: created pid=4 name=counter
shell: runbg counter pid=4

ush> jobs
shell jobs:
  pid=4 parent=2 name=counter state=running

ush> kill 4
shell: kill requested pid=4

counter: pid=4 ppid=2
counter: argc=2
counter: argv[0]=counter
counter: argv[1]=victim
counter: printf test A 0x1234 %
counter: tick 1
Syscall: process counter pid=4 exited code 128

ush> jobs
shell jobs:
  pid=4 parent=2 name=counter state=zombie code=128

ush> wait 4
Process: destroyed pid=4 ppid=2 name=counter
shell: wait pid=4 name=counter code=128

ush> jobs
shell jobs:
  none
```

Depending on timing, `counter` may print one or two ticks before the kill is honored.

That is expected because this is cooperative kill, not hard asynchronous termination.

---

## 22. Update Makefile greps

Update help grep:

```make
grep -q "commands: help, echo, args, run, runbg, jobs, wait, kill, exit" build/test.log
```

Add kill greps:

```make
grep -q "shell: kill requested pid=4" build/test.log
grep -q "exited code 128" build/test.log
grep -q "state=zombie code=128" build/test.log
grep -q "shell: wait pid=4 name=counter code=128" build/test.log
```

Update the old background job code grep if it expected `code=4` for that job.

The foreground `run counter alpha beta` should still exit with code `4`.

The killed background `counter victim` should exit with code `128`.

A useful shell block:

```make
	grep -q "commands: help, echo, args, run, runbg, jobs, wait, kill, exit" build/test.log
	grep -q "shell: run counter pid=" build/test.log
	grep -q "shell: counter exited code 4" build/test.log
	grep -q "shell: runbg counter pid=4" build/test.log
	grep -q "state=running" build/test.log
	grep -q "shell: kill requested pid=4" build/test.log
	grep -q "Syscall: process counter pid=4 exited code 128" build/test.log
	grep -q "state=zombie code=128" build/test.log
	grep -q "shell: wait pid=4 name=counter code=128" build/test.log
	grep -q "shell jobs:" build/test.log
	grep -q "  none" build/test.log
```

Update the final success message:

```make
    @echo "Boot, memory, heap, sync, monitor, cooperative kill, shell jobs, exec, and waitpid smoke test passed."
```

---

## 23. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 33 checks passed."
```

---

## 24. Interactive test

After boot:

```text
toyix> run shell
```

Inside shell:

```text
ush> runbg counter victim
shell: runbg counter pid=...
```

Then:

```text
ush> jobs
shell jobs:
  pid=... parent=... name=counter state=running
```

Then:

```text
ush> kill <pid>
shell: kill requested pid=<pid>
```

After a moment:

```text
ush> jobs
shell jobs:
  pid=... parent=... name=counter state=zombie code=128
```

Then:

```text
ush> wait <pid>
shell: wait pid=... name=counter code=128
```

Then:

```text
ush> jobs
shell jobs:
  none
```

---

## 25. Common failures

### Failure: `kill` says failed

Likely causes:

```text
target PID is not a child of the shell
PID was already reaped
PID is wrong in the automated test
process_request_kill_child() did not find target
```

For manual testing, use:

```text
jobs
```

to get the correct PID before running:

```text
kill PID
```

### Failure: process does not exit after kill

Remember: this is cooperative kill.

The target exits only when it reaches a syscall boundary.

For `counter`, that should happen quickly because it repeatedly calls:

```text
SYS_WRITE
SYS_SLEEP
```

If you later write a CPU-bound user program that never makes syscalls, this kill mechanism will not stop it yet.

That will require scheduler-return or timer-interrupt kill checks.

### Failure: process exits with code 4 instead of 128

That means the process finished normally before the kill request was honored.

In the automated test, reduce the delay before injecting `kill`, or make `counter` run longer.

A robust version is to change `counter` to count to 5 or 10 instead of 3, but that affects existing tests.

For this chapter, using `thread_sleep_ticks(2)` before kill should usually be enough.

### Failure: killed process remains `running`

Check `syscall_finish_or_kill()` and make sure it is called after `SYS_SLEEP`.

This is the key case for `counter`.

```c
case SYS_SLEEP: {
    uint32_t ticks = frame->ebx;
    thread_sleep_ticks(ticks);
    frame->eax = 0;
    syscall_finish_or_kill(frame);
    return;
}
```

### Failure: shell can kill unrelated process

Check `syscall_kill()`.

It must use:

```c
process_request_kill_child(current->pid, target_pid)
```

not unrestricted:

```c
process_find(target_pid)
```

### Failure: shell can kill itself

Check:

```c
if (target_pid == 0 || target_pid == current->pid) {
    frame->eax = 0xFFFFFFFFu;
    return;
}
```

For now, self-kill is denied.

Later, we may allow it.

---

## 26. What this chapter achieved

Before this chapter:

```text
shell could launch and wait for children
but could not request termination
```

After this chapter:

```text
SYS_KILL marks a child for cooperative termination
processes carry kill_requested state
syscall boundaries honor kill requests
shell has kill PID command
killed children become zombies with exit code 128
wait collects killed children normally
```

This is the first step toward signals.

---

## 27. Design limitations

This is not full kill/signal support.

Missing:

```text
hard asynchronous termination
timer-return kill checks
signal numbers
default signal actions
signal handlers
process groups
kill by process group
permission model beyond parent-child
waking killed sleeping processes immediately
```

Most importantly:

```text
CPU-bound user code that never makes syscalls will not be killed yet.
```

That is acceptable for this cooperative milestone.

A future chapter can add kill checks on interrupt return or scheduler return.

---
## Resources

- [Chapter source: Toyix repository](https://github.com/Monotoba/toyix)
- [Chapter release: Chapter_33](https://github.com/Monotoba/toyix/releases/tag/Chapter_33)

## Closure

Chapter 33 adds the first safe process-termination path to Toyix: a parent can request that its child exit, and the kernel honors that request at a controlled syscall boundary.

Happy Coding!
