# Chapter 34 — First RAMFS and Core File APIs

By the end of Chapter 33, Toyix could launch child processes, inspect child state from the shell, and request cooperative termination:

```text
ush> runbg counter victim
ush> jobs
ush> kill 4
ush> wait 4
shell: wait pid=4 name=counter code=128
```

At this point, Toyix can:

```text
boot
schedule threads
run user processes
load embedded ELF programs
track parent/child ownership
wait for children
inspect process state
kill child processes cooperatively
```

The next major subsystem is files.

This chapter adds the first tiny filesystem path:

```text
RAMFS
  ↓
VFS-like file interface
  ↓
SYS_OPEN
SYS_READ
SYS_CLOSE
  ↓
user shell cat PATH
```

After this chapter, the user shell can do:

```text
ush> cat /README
Toyix RAMFS
This file lives inside the kernel image.

ush> cat /programs
demo
counter
shell
```

This is not a disk filesystem yet.

It is a tiny read-only in-memory filesystem that gives us the kernel/user ABI shape for future real filesystems.

---
## 1. What this chapter adds

Add:

```text
include/kernel/vfs.h
kernel/vfs.c
```

Modify:

```text
include/kernel/process.h
kernel/process.c
include/kernel/syscall.h
kernel/syscall.c
user/include/toyix_syscall.h
user/shell.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

New syscalls:

```text
SYS_OPEN   12
SYS_CLOSE  13
```

Existing syscall extended:

```text
SYS_READ
```

It already reads from stdin when `fd == 0`.

Now it can also read file descriptors returned by `SYS_OPEN`.

---

## 2. Filesystem design for this chapter

We will intentionally keep the first filesystem tiny.

Supported:

```text
open path
read bytes
close fd
read-only files
exact path matching
per-process file descriptors
```

Not supported yet:

```text
directories
write
seek
create
delete
permissions
mount points
device files
relative paths
current working directory
```

The first filesystem contains two files:

```text
/README
/programs
```

---

## 3. File descriptor model

Toyix already has these special descriptors:

```text
0 = stdin
1 = stdout
2 = stderr
```

This chapter adds process-owned file descriptors starting at:

```text
3
```

So a user program can do:

```c
toyix_i32 fd = toyix_open("/README", 0);
toyix_read(fd, buffer, sizeof(buffer));
toyix_close(fd);
```

Each process gets its own small descriptor table.

For now:

```text
maximum open files per process = 16
```

Descriptors `0`, `1`, and `2` are reserved.

Descriptors `3` through `15` can refer to RAMFS files.

---

## 4. Add `include/kernel/vfs.h`

```c
// include/kernel/vfs.h
#ifndef TOYIX_KERNEL_VFS_H
#define TOYIX_KERNEL_VFS_H

#include <stdint.h>

#define VFS_OK 0
#define VFS_ERR_NOT_FOUND       -1
#define VFS_ERR_INVALID         -2
#define VFS_ERR_NO_MEMORY       -3
#define VFS_ERR_NOT_SUPPORTED   -4

typedef struct vfs_file vfs_file_t;

void vfs_init(void);

int vfs_open(const char *path, vfs_file_t **out_file);
int vfs_read(
    vfs_file_t *file,
    void *buffer,
    uint32_t length,
    uint32_t *out_read
);
void vfs_close(vfs_file_t *file);

void vfs_test_once(void);

#endif
```

The rest of the kernel does not need to know how a file is represented internally.

It only sees:

```c
vfs_file_t *
```

That gives us room to swap the implementation later.

---

## 5. Add `kernel/vfs.c`

```c
// kernel/vfs.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/vfs.h"

typedef struct ramfs_node {
    const char *path;
    const uint8_t *data;
    uint32_t size;
} ramfs_node_t;

struct vfs_file {
    const ramfs_node_t *node;
    uint32_t offset;
};

static const uint8_t readme_text[] =
    "Toyix RAMFS\n"
    "This file lives inside the kernel image.\n"
    "The first filesystem is read-only and memory-backed.\n";

static const uint8_t programs_text[] =
    "demo\n"
    "counter\n"
    "shell\n";

static const ramfs_node_t ramfs_nodes[] = {
    {
        .path = "/README",
        .data = readme_text,
        .size = sizeof(readme_text) - 1u
    },
    {
        .path = "/programs",
        .data = programs_text,
        .size = sizeof(programs_text) - 1u
    }
};

static const uint32_t ramfs_node_count =
    sizeof(ramfs_nodes) / sizeof(ramfs_nodes[0]);

void vfs_init(void) {
    console_write("VFS: initialized RAMFS with ");
    console_write_u32_dec(ramfs_node_count);
    console_writeln(" file(s)");
}

static const ramfs_node_t *ramfs_find(const char *path) {
    if (path == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < ramfs_node_count; ++i) {
        if (kstrcmp(path, ramfs_nodes[i].path) == 0) {
            return &ramfs_nodes[i];
        }
    }

    return 0;
}

int vfs_open(const char *path, vfs_file_t **out_file) {
    if (path == 0 || out_file == 0) {
        return VFS_ERR_INVALID;
    }

    const ramfs_node_t *node = ramfs_find(path);

    if (node == 0) {
        return VFS_ERR_NOT_FOUND;
    }

    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));

    if (file == 0) {
        return VFS_ERR_NO_MEMORY;
    }

    file->node = node;
    file->offset = 0;

    *out_file = file;
    return VFS_OK;
}

int vfs_read(
    vfs_file_t *file,
    void *buffer,
    uint32_t length,
    uint32_t *out_read
) {
    if (file == 0 || buffer == 0 || out_read == 0) {
        return VFS_ERR_INVALID;
    }

    *out_read = 0;

    if (length == 0) {
        return VFS_OK;
    }

    if (file->offset >= file->node->size) {
        return VFS_OK;
    }

    uint32_t remaining = file->node->size - file->offset;
    uint32_t to_copy = length;

    if (to_copy > remaining) {
        to_copy = remaining;
    }

    memcpy(buffer, file->node->data + file->offset, to_copy);

    file->offset += to_copy;
    *out_read = to_copy;

    return VFS_OK;
}

void vfs_close(vfs_file_t *file) {
    if (file == 0) {
        return;
    }

    kfree(file);
}

void vfs_test_once(void) {
    console_writeln("VFS test: starting RAMFS open/read/close test");

    vfs_file_t *file = 0;

    if (vfs_open("/README", &file) != VFS_OK || file == 0) {
        kernel_panic("VFS test could not open /README");
    }

    char buffer[16];
    uint32_t got = 0;

    if (vfs_read(file, buffer, sizeof(buffer) - 1u, &got) != VFS_OK) {
        kernel_panic("VFS test could not read /README");
    }

    buffer[got] = '\0';

    console_write("VFS test: first bytes: ");
    console_writeln(buffer);

    vfs_close(file);

    console_writeln("VFS test: RAMFS sanity check passed");
}
```

This is deliberately simple.

The RAMFS “directory” is just a static table:

```c
static const ramfs_node_t ramfs_nodes[] = { ... };
```

Opening a file creates a tiny heap-allocated `vfs_file_t` with its own read offset.

That means two opens of the same file have independent offsets.

---

## 6. Add file descriptor table to `process_t`

Update `include/kernel/process.h`.

Near the top, after includes, add:

```c
struct vfs_file;

#define PROCESS_MAX_FDS 16u
#define PROCESS_FIRST_FILE_FD 3u

typedef struct process_fd {
    int used;
    struct vfs_file *file;
} process_fd_t;
```

Then add this field to `process_t`:

```c
process_fd_t fds[PROCESS_MAX_FDS];
```

A good placement is near the process runtime fields:

```c
uint32_t exit_code;
int exited;
int kill_requested;

process_fd_t fds[PROCESS_MAX_FDS];
```

Now add these declarations:

```c
int process_fd_install(process_t *process, struct vfs_file *file);
struct vfs_file *process_fd_get(process_t *process, uint32_t fd);
int process_fd_close(process_t *process, uint32_t fd);
void process_close_all_files(process_t *process);
```

The file descriptor section of `process.h` should look like:

```c
int process_fd_install(process_t *process, struct vfs_file *file);
struct vfs_file *process_fd_get(process_t *process, uint32_t fd);
int process_fd_close(process_t *process, uint32_t fd);
void process_close_all_files(process_t *process);
```

---

## 7. Initialize file descriptors in `process_create_empty()`

In `kernel/process.c`, include:

```c
#include "kernel/vfs.h"
```

Inside `process_create_empty()`, after basic fields are initialized, add:

```c
for (uint32_t i = 0; i < PROCESS_MAX_FDS; ++i) {
    process->fds[i].used = 0;
    process->fds[i].file = 0;
}
```

Descriptors `0`, `1`, and `2` are reserved by convention, but they do not need entries in this table because stdin/stdout/stderr are still handled directly by syscall code.

---

## 8. Add process FD helpers

Add these to `kernel/process.c`:

```c
int process_fd_install(process_t *process, struct vfs_file *file) {
    validate_live_process(process);

    if (file == 0) {
        return -1;
    }

    irq_flags_t flags = irq_save();

    for (uint32_t fd = PROCESS_FIRST_FILE_FD;
         fd < PROCESS_MAX_FDS;
         ++fd) {
        if (!process->fds[fd].used) {
            process->fds[fd].used = 1;
            process->fds[fd].file = file;

            irq_restore(flags);
            return (int)fd;
        }
    }

    irq_restore(flags);
    return -1;
}
```

```c
struct vfs_file *process_fd_get(process_t *process, uint32_t fd) {
    validate_live_process(process);

    if (fd >= PROCESS_MAX_FDS || fd < PROCESS_FIRST_FILE_FD) {
        return 0;
    }

    irq_flags_t flags = irq_save();

    struct vfs_file *file = 0;

    if (process->fds[fd].used) {
        file = process->fds[fd].file;
    }

    irq_restore(flags);

    return file;
}
```

```c
int process_fd_close(process_t *process, uint32_t fd) {
    validate_live_process(process);

    if (fd >= PROCESS_MAX_FDS || fd < PROCESS_FIRST_FILE_FD) {
        return -1;
    }

    irq_flags_t flags = irq_save();

    struct vfs_file *file = 0;

    if (process->fds[fd].used) {
        file = process->fds[fd].file;
        process->fds[fd].used = 0;
        process->fds[fd].file = 0;
    }

    irq_restore(flags);

    if (file == 0) {
        return -1;
    }

    vfs_close(file);
    return 0;
}
```

```c
void process_close_all_files(process_t *process) {
    validate_live_process(process);

    struct vfs_file *to_close[PROCESS_MAX_FDS];

    for (uint32_t i = 0; i < PROCESS_MAX_FDS; ++i) {
        to_close[i] = 0;
    }

    irq_flags_t flags = irq_save();

    for (uint32_t fd = PROCESS_FIRST_FILE_FD;
         fd < PROCESS_MAX_FDS;
         ++fd) {
        if (process->fds[fd].used) {
            to_close[fd] = process->fds[fd].file;
            process->fds[fd].used = 0;
            process->fds[fd].file = 0;
        }
    }

    irq_restore(flags);

    for (uint32_t fd = PROCESS_FIRST_FILE_FD;
         fd < PROCESS_MAX_FDS;
         ++fd) {
        if (to_close[fd] != 0) {
            vfs_close(to_close[fd]);
        }
    }
}
```

Why collect files into a local array first?

Because `vfs_close()` may call `kfree()`.

It is cleaner not to call heap functions while holding the process table interrupt lock.

---

## 9. Close files on process destroy

In `process_destroy()`, before destroying the address space, add:

```c
process_close_all_files(process);
```

The relevant section should become:

```c
irq_flags_t flags = irq_save();

process_reparent_children(pid, 0);
process_table_remove(process);

irq_restore(flags);

process_close_all_files(process);

if (process->address_space != 0) {
    address_space_destroy(process->address_space);
    process->address_space = 0;
}
```

This prevents leaked open RAMFS file handles.

---

## 10. Update syscall numbers

Update `include/kernel/syscall.h`.

Add:

```c
#define SYS_OPEN     12u
#define SYS_CLOSE    13u
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
#define SYS_OPEN     12u
#define SYS_CLOSE    13u
```

Update `user/include/toyix_syscall.h` with the same numbers.

---

## 11. Add user syscall wrappers

In `user/include/toyix_syscall.h`, add after `toyix_kill()`:

```c
static inline toyix_i32 toyix_open(
    const char *path,
    toyix_u32 flags
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_OPEN),
          "b"(path),
          "c"(flags)
        : "memory"
    );

    return result;
}
```

```c
static inline toyix_i32 toyix_close(toyix_u32 fd) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_CLOSE),
          "b"(fd)
        : "memory"
    );

    return result;
}
```

For now:

```text
flags must be 0
```

because the RAMFS is read-only.

---

## 12. Add syscall copy limit for paths

In `kernel/syscall.c`, add:

```c
#define SYSCALL_PATH_MAX 64u
```

near the other syscall limits.

We will reuse the existing helper:

```c
syscall_copy_user_string()
```

from the earlier `SYS_EXEC` chapter.

---

## 13. Add `SYS_OPEN`

In `kernel/syscall.c`, include:

```c
#include "kernel/vfs.h"
```

Then add:

```c
static void syscall_open(interrupt_frame_t *frame) {
    uintptr_t user_path = (uintptr_t)frame->ebx;
    uint32_t flags = frame->ecx;

    if (flags != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    char path[SYSCALL_PATH_MAX];

    if (syscall_copy_user_string(
            user_path,
            path,
            sizeof(path)
        ) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    vfs_file_t *file = 0;

    if (vfs_open(path, &file) != VFS_OK || file == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    int fd = process_fd_install(current, file);

    if (fd < 0) {
        vfs_close(file);
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = (uint32_t)fd;
}
```

This path is:

```text
copy user path
  ↓
open VFS file
  ↓
install in current process fd table
  ↓
return fd
```

---

## 14. Extend `SYS_READ`

The existing `SYS_READ` probably has logic like:

```text
if fd == stdin:
    terminal_readline()
else:
    error
```

Change it so:

```text
fd == 0      read from terminal
fd >= 3      read from process file descriptor
otherwise    error
```

Add a helper:

```c
static void syscall_read_file(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    uintptr_t user_buffer = (uintptr_t)frame->ecx;
    uint32_t length = frame->edx;

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    if (user_buffer == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    vfs_file_t *file = process_fd_get(current, fd);

    if (file == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    uint8_t kernel_buffer[SYSCALL_RW_MAX];
    uint32_t got = 0;

    if (vfs_read(file, kernel_buffer, length, &got) != VFS_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (got > 0) {
        if (copy_to_user(
                user_buffer,
                kernel_buffer,
                got
            ) != USERCOPY_OK) {
            frame->eax = 0xFFFFFFFFu;
            return;
        }
    }

    frame->eax = got;
}
```

Then update your existing `syscall_read()` to dispatch:

```c
static void syscall_read(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;

    if (fd == FD_STDIN) {
        syscall_read_stdin(frame);
        return;
    }

    if (fd >= PROCESS_FIRST_FILE_FD) {
        syscall_read_file(frame);
        return;
    }

    frame->eax = 0xFFFFFFFFu;
}
```

If your existing `syscall_read()` is not split into helpers, refactor it now:

```c
static void syscall_read_stdin(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    uintptr_t user_buffer = (uintptr_t)frame->ecx;
    uint32_t length = frame->edx;

    if (fd != FD_STDIN || user_buffer == 0 || length == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    char kernel_buffer[SYSCALL_RW_MAX + 1u];

    toyix_memset_not_available;
}
```

Do **not** literally add `toyix_memset_not_available`.

Instead, keep your current stdin implementation from Chapter 18 and move it into `syscall_read_stdin()`.

The important change is only that file descriptors `>= 3` now use `syscall_read_file()`.

---

## 15. Add `SYS_CLOSE`

Add this to `kernel/syscall.c`:

```c
static void syscall_close(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;

    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (process_fd_close(current, fd) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = 0;
}
```

Closing stdin/stdout/stderr is not supported yet.

So:

```text
close(0)
close(1)
close(2)
```

returns error.

---

## 16. Update syscall handler

Add cases:

```c
case SYS_OPEN:
    syscall_open(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_CLOSE:
    syscall_close(frame);
    syscall_finish_or_kill(frame);
    return;
```

The process/syscall section now includes:

```c
case SYS_PROCINFO:
    syscall_procinfo(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_KILL:
    syscall_kill(frame);
    return;

case SYS_OPEN:
    syscall_open(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_CLOSE:
    syscall_close(frame);
    syscall_finish_or_kill(frame);
    return;
```

Keep `SYS_READ` using the existing case, but make sure it calls the updated `syscall_read()`.

---

## 17. Initialize VFS in `kernel/kmain.c`

Include:

```c
#include "kernel/vfs.h"
```

Call `vfs_init()` after the heap is initialized and before program tests.

A good placement is after:

```c
heap_init(4);
heap_test_once();
```

Add:

```c
vfs_init();
vfs_test_once();
```

So the boot path includes:

```c
heap_init(4);
heap_test_once();

vfs_init();
vfs_test_once();

threading_init();
process_init_system();
program_registry_init();
```

Why after heap?

Because `vfs_open()` allocates file handles with `kmalloc()`.

The VFS test calls `vfs_open()`, so the heap must already exist.

---

## 18. Update `Makefile`

Add:

```make
build/kernel/vfs.o
```

to `OBJS`.

For example:

```make
OBJS := \
    build/arch/x86/boot.o \
    ...
    build/kernel/vfs.o \
    build/kernel/program.o \
    ...
```

The exact placement does not matter as long as it is linked.

---

## 19. Add shell `cat` command

Update `user/shell.c`.

First, update help text.

Replace:

```c
toyix_puts("commands: help, echo, args, run, runbg, jobs, wait, kill, exit");
```

with:

```c
toyix_puts("commands: help, echo, args, cat, run, runbg, jobs, wait, kill, exit");
```

Add this function:

```c
static void cmd_cat(int argc, char **argv) {
    if (argc != 2) {
        toyix_puts("usage: cat PATH");
        return;
    }

    toyix_i32 fd = toyix_open(argv[1], 0);

    if (fd < 0) {
        toyix_printf("cat: could not open %s\n", argv[1]);
        return;
    }

    char buffer[64];

    for (;;) {
        toyix_i32 got = toyix_read(
            (toyix_u32)fd,
            buffer,
            sizeof(buffer)
        );

        if (got < 0) {
            toyix_puts("cat: read error");
            break;
        }

        if (got == 0) {
            break;
        }

        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_close((toyix_u32)fd);
}
```

Then add the command branch before `run`:

```c
if (toyix_streq(cmd_argv[0], "cat")) {
    cmd_cat(cmd_argc, cmd_argv);
    continue;
}
```

The command dispatch section should now include:

```c
if (toyix_streq(cmd_argv[0], "args")) {
    cmd_args(argc, argv);
    continue;
}

if (toyix_streq(cmd_argv[0], "cat")) {
    cmd_cat(cmd_argc, cmd_argv);
    continue;
}

if (toyix_streq(cmd_argv[0], "run")) {
    cmd_run(cmd_argc, cmd_argv);
    continue;
}
```

---

## 20. Update shell test input

In `kernel/program.c`, add `cat` commands to the injected shell test.

Current beginning:

```c
inject_text("help\n");
inject_text("echo hello\n");
inject_text("run counter alpha beta\n");
```

Change it to:

```c
inject_text("help\n");
inject_text("echo hello\n");
inject_text("cat /README\n");
inject_text("cat /programs\n");
inject_text("run counter alpha beta\n");
```

This proves:

```text
shell can open /README
shell can read file contents
shell can close file
shell can continue running afterward
```

---

## 21. Expected shell output

The shell test should now include:

```text
ush> cat /README
Toyix RAMFS
This file lives inside the kernel image.
The first filesystem is read-only and memory-backed.

ush> cat /programs
demo
counter
shell
```

Then the existing process-control tests continue:

```text
ush> run counter alpha beta
...
ush> runbg counter victim
...
```

---

## 22. Update Makefile greps

Add VFS boot greps:

```make
grep -q "VFS: initialized RAMFS with 2 file(s)" build/test.log
grep -q "VFS test: RAMFS sanity check passed" build/test.log
```

Update shell help grep:

```make
grep -q "commands: help, echo, args, cat, run, runbg, jobs, wait, kill, exit" build/test.log
```

Add file content greps:

```make
grep -q "Toyix RAMFS" build/test.log
grep -q "This file lives inside the kernel image." build/test.log
grep -q "The first filesystem is read-only and memory-backed." build/test.log
grep -q "demo" build/test.log
grep -q "counter" build/test.log
grep -q "shell" build/test.log
```

Because those program names also appear elsewhere, the `/programs` greps are not very specific, but they still help.

A useful shell block now includes:

```make
	grep -q "VFS: initialized RAMFS with 2 file(s)" build/test.log
	grep -q "VFS test: RAMFS sanity check passed" build/test.log
	grep -q "commands: help, echo, args, cat, run, runbg, jobs, wait, kill, exit" build/test.log
	grep -q "Toyix RAMFS" build/test.log
	grep -q "This file lives inside the kernel image." build/test.log
	grep -q "The first filesystem is read-only and memory-backed." build/test.log
	grep -q "shell: run counter pid=" build/test.log
	grep -q "shell: counter exited code 4" build/test.log
	grep -q "shell: runbg counter pid=" build/test.log
	grep -q "state=zombie code=128" build/test.log
	grep -q "name=counter code=128" build/test.log
	grep -q "shell jobs:" build/test.log
	grep -q "  none" build/test.log
```

Update final success message:

```make
	@echo "Boot, memory, heap, VFS, RAMFS, cat, process control, and shell jobs smoke test passed."
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

echo "All Chapter 34 checks passed."
```

---

## 24. Interactive test

After boot:

```text
toyix> run shell
```

Inside shell:

```text
ush> cat /README
```

Expected:

```text
Toyix RAMFS
This file lives inside the kernel image.
The first filesystem is read-only and memory-backed.
```

Then:

```text
ush> cat /programs
```

Expected:

```text
demo
counter
shell
```

Try an invalid file:

```text
ush> cat /missing
```

Expected:

```text
cat: could not open /missing
```

Then confirm the shell still works:

```text
ush> run counter filetest
```

Expected:

```text
counter: argv[1]=filetest
...
shell: counter exited code 4
```

---

## 25. Common failures

### Failure: `cat /README` says open failed

Check:

```text
VFS initialized after heap
/README path matches exactly
SYS_OPEN copies user path correctly
process_fd_install returns fd >= 3
```

The path is case-sensitive:

```text
/README
```

not:

```text
/readme
```

### Failure: `cat` prints only part of the file

That is normal if it then continues printing the rest.

`cat` reads in chunks:

```c
char buffer[64];
```

It loops until `toyix_read()` returns `0`.

If it prints only one chunk and stops too early, check that `vfs_read()` updates:

```c
file->offset += to_copy;
```

and returns `0` only at EOF.

### Failure: second `cat /README` prints nothing

That means file offsets are shared globally.

Each `vfs_open()` should allocate a new `vfs_file_t` with:

```c
file->offset = 0;
```

Do not store the offset in the RAMFS node itself.

### Failure: open file handles leak

Make sure `process_destroy()` calls:

```c
process_close_all_files(process);
```

Also make sure `cmd_cat()` calls:

```c
toyix_close(fd);
```

after reading.

### Failure: `SYS_READ` from stdin broke

Keep stdin handling separate:

```text
fd == 0      terminal read
fd >= 3      file read
```

Do not route `fd == 0` through the process FD table.

### Failure: `close(3)` succeeds, then `read(3)` still works

`process_fd_close()` must clear:

```c
process->fds[fd].used = 0;
process->fds[fd].file = 0;
```

before closing.

### Failure: kernel panic in `process_close_all_files`

Avoid calling `vfs_close()` while holding the interrupt lock.

Use the two-phase close pattern:

```text
copy file pointers into local array while locked
clear fd table while locked
unlock
vfs_close each file
```

---

## 26. What this chapter achieved

Before this chapter:

```text
all user-visible content came from embedded programs
no open/read/close file abstraction existed
```

After this chapter:

```text
Toyix has a tiny RAMFS
Toyix has a VFS-like file handle abstraction
processes own file descriptors
SYS_OPEN returns fd >= 3
SYS_READ works on stdin and files
SYS_CLOSE releases file handles
shell can cat files
```

This is a major operating-system milestone.

Even though RAMFS is tiny, the user/kernel shape is now in place.

---

## 27. Design limitations

This first file layer is intentionally small.

Missing:

```text
write
seek
directories
stat
file permissions
mount points
device files
current working directory
relative paths
filesystem-backed exec
file descriptor inheritance
dup
pipe
```

Also, RAMFS files are compiled into the kernel image.

That is fine for now.

The goal was not a complete filesystem.

The goal was to create the first clean path:

```text
user program
  ↓ open/read/close syscalls
kernel fd table
  ↓
VFS file object
  ↓
RAMFS file data
```

---

## Resources

- [Chapter source: Toyix repository](https://github.com/Monotoba/toyix)
- [Chapter release: Chapter_34](https://github.com/Monotoba/toyix/releases/tag/Chapter_34)

## Closure

Chapter 34 gives Toyix its first real file interface: a tiny RAMFS, per-process file descriptors, and enough shell support to read named files instead of relying on hardcoded kernel-only paths.

Happy Coding!
