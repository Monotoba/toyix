# Chapter 35 — `SYS_SEEK` and Rewindable File Descriptors

In Chapter 34, Toyix gained its first tiny file layer:

```text
RAMFS
  ↓
VFS-like file objects
  ↓
per-process file descriptors
  ↓
SYS_OPEN
SYS_READ
SYS_CLOSE
  ↓
shell cat PATH
```

The shell could now do:

```text
ush> cat /README
Toyix RAMFS
This file lives inside the kernel image.
The first filesystem is read-only and memory-backed.
```

But file descriptors were strictly forward-only.

Once a file was read, its offset moved forward. There was no way to rewind, skip, or query position.

This chapter adds:

```text
SYS_SEEK
```

After this chapter, userland can do:

```c
fd = toyix_open("/README", 0);
toyix_read(fd, buffer, 8);
toyix_seek(fd, 0, TOYIX_SEEK_SET);
toyix_read(fd, buffer, 8);
toyix_close(fd);
```

And the shell gains a small test command:

```text
ush> seektest /README
seektest: first read: Toyix RA
seektest: rewind read: Toyix RA
seektest: skip read: RAMFS
```

---

# 1. What this chapter adds

Modify:

```text
include/kernel/vfs.h
kernel/vfs.c
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
SYS_SEEK = 14
```

New seek modes:

```text
TOYIX_SEEK_SET = 0
TOYIX_SEEK_CUR = 1
TOYIX_SEEK_END = 2
```

The new syscall ABI:

```text
EAX = SYS_SEEK
EBX = fd
ECX = signed offset
EDX = whence

returns:
  EAX = new offset on success
  EAX = 0xFFFFFFFF on error
```

---

# 2. Seek behavior

Toyix seek supports three modes:

```text
TOYIX_SEEK_SET
  new_offset = offset

TOYIX_SEEK_CUR
  new_offset = current_offset + offset

TOYIX_SEEK_END
  new_offset = file_size + offset
```

For now:

```text
seeking before start is an error
seeking past EOF is allowed
reading past EOF returns 0 bytes
```

Allowing seek past EOF is normal and useful. Since RAMFS is read-only, it does not create holes or grow files.

---

# 3. Update `include/kernel/vfs.h`

Replace it with this version:

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

#define TOYIX_SEEK_SET 0u
#define TOYIX_SEEK_CUR 1u
#define TOYIX_SEEK_END 2u

typedef struct vfs_file vfs_file_t;

void vfs_init(void);

int vfs_open(const char *path, vfs_file_t **out_file);

int vfs_read(
    vfs_file_t *file,
    void *buffer,
    uint32_t length,
    uint32_t *out_read
);

int vfs_seek(
    vfs_file_t *file,
    int32_t offset,
    uint32_t whence,
    uint32_t *out_position
);

uint32_t vfs_tell(vfs_file_t *file);
uint32_t vfs_size(vfs_file_t *file);

void vfs_close(vfs_file_t *file);

void vfs_test_once(void);

#endif
```

New declarations:

```c
int vfs_seek(
    vfs_file_t *file,
    int32_t offset,
    uint32_t whence,
    uint32_t *out_position
);

uint32_t vfs_tell(vfs_file_t *file);
uint32_t vfs_size(vfs_file_t *file);
```

---

# 4. Update `kernel/vfs.c`

Add `vfs_seek()`, `vfs_tell()`, and `vfs_size()` after `vfs_read()`.

```c
int vfs_seek(
    vfs_file_t *file,
    int32_t offset,
    uint32_t whence,
    uint32_t *out_position
) {
    if (file == 0 || out_position == 0) {
        return VFS_ERR_INVALID;
    }

    int64_t base = 0;

    switch (whence) {
        case TOYIX_SEEK_SET:
            base = 0;
            break;

        case TOYIX_SEEK_CUR:
            base = (int64_t)file->offset;
            break;

        case TOYIX_SEEK_END:
            base = (int64_t)file->node->size;
            break;

        default:
            return VFS_ERR_INVALID;
    }

    int64_t next = base + (int64_t)offset;

    if (next < 0) {
        return VFS_ERR_INVALID;
    }

    if (next > 0xFFFFFFFFLL) {
        return VFS_ERR_INVALID;
    }

    file->offset = (uint32_t)next;
    *out_position = file->offset;

    return VFS_OK;
}

uint32_t vfs_tell(vfs_file_t *file) {
    if (file == 0) {
        return 0;
    }

    return file->offset;
}

uint32_t vfs_size(vfs_file_t *file) {
    if (file == 0 || file->node == 0) {
        return 0;
    }

    return file->node->size;
}
```

Why use `int64_t` internally?

Because this expression can overflow if done in 32-bit arithmetic:

```c
current_offset + signed_offset
```

Using `int64_t` makes the boundary checks straightforward.

---

# 5. Expand `vfs_test_once()`

Replace `vfs_test_once()` with this version:

```c
void vfs_test_once(void) {
    console_writeln("VFS test: starting RAMFS open/read/seek/close test");

    vfs_file_t *file = 0;

    if (vfs_open("/README", &file) != VFS_OK || file == 0) {
        kernel_panic("VFS test could not open /README");
    }

    char buffer[16];
    uint32_t got = 0;

    if (vfs_read(file, buffer, 8u, &got) != VFS_OK || got != 8u) {
        kernel_panic("VFS test could not read first bytes");
    }

    buffer[got] = '\0';

    console_write("VFS test: first bytes: ");
    console_writeln(buffer);

    uint32_t pos = 0;

    if (vfs_seek(file, 0, TOYIX_SEEK_SET, &pos) != VFS_OK || pos != 0) {
        kernel_panic("VFS test could not rewind");
    }

    if (vfs_read(file, buffer, 8u, &got) != VFS_OK || got != 8u) {
        kernel_panic("VFS test could not reread first bytes");
    }

    buffer[got] = '\0';

    console_write("VFS test: rewind bytes: ");
    console_writeln(buffer);

    if (vfs_seek(file, 6, TOYIX_SEEK_SET, &pos) != VFS_OK || pos != 6u) {
        kernel_panic("VFS test could not seek to offset 6");
    }

    if (vfs_read(file, buffer, 5u, &got) != VFS_OK || got != 5u) {
        kernel_panic("VFS test could not read after seek");
    }

    buffer[got] = '\0';

    console_write("VFS test: seek bytes: ");
    console_writeln(buffer);

    vfs_close(file);

    console_writeln("VFS test: RAMFS seek sanity check passed");
}
```

Expected lines:

```text
VFS test: first bytes: Toyix RA
VFS test: rewind bytes: Toyix RA
VFS test: seek bytes: RAMFS
VFS test: RAMFS seek sanity check passed
```

---

# 6. Update syscall numbers

Update both:

```text
include/kernel/syscall.h
user/include/toyix_syscall.h
```

Add:

```c
#define SYS_SEEK     14u
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
#define SYS_SEEK     14u
```

Also add the seek constants to both headers:

```c
#define TOYIX_SEEK_SET 0u
#define TOYIX_SEEK_CUR 1u
#define TOYIX_SEEK_END 2u
```

For the kernel header, keep them near the syscall ABI constants.

For the user header, put them near `TOYIX_WNOHANG`.

---

# 7. Add user `toyix_seek()` wrapper

In `user/include/toyix_syscall.h`, add:

```c
static inline toyix_i32 toyix_seek(
    toyix_u32 fd,
    toyix_i32 offset,
    toyix_u32 whence
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_SEEK),
          "b"(fd),
          "c"(offset),
          "d"(whence)
        : "memory"
    );

    return result;
}
```

Return behavior:

```text
>= 0   new file offset
< 0    error
```

Since errors return `0xFFFFFFFF`, userland sees that as `-1`.

---

# 8. Add kernel `SYS_SEEK`

In `kernel/syscall.c`, add:

```c
static void syscall_seek(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    int32_t offset = (int32_t)frame->ecx;
    uint32_t whence = frame->edx;

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

    uint32_t position = 0;

    if (vfs_seek(file, offset, whence, &position) != VFS_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = position;
}
```

Then add a case in `syscall_handler()`:

```c
case SYS_SEEK:
    syscall_seek(frame);
    syscall_finish_or_kill(frame);
    return;
```

Place it near `SYS_OPEN` and `SYS_CLOSE`:

```c
case SYS_OPEN:
    syscall_open(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_CLOSE:
    syscall_close(frame);
    syscall_finish_or_kill(frame);
    return;

case SYS_SEEK:
    syscall_seek(frame);
    syscall_finish_or_kill(frame);
    return;
```

---

# 9. Why seeking stdin is unsupported

`SYS_SEEK` uses:

```c
process_fd_get(current, fd)
```

That only works for file descriptors `>= 3`.

Therefore:

```text
toyix_seek(0, ...)
toyix_seek(1, ...)
toyix_seek(2, ...)
```

returns error.

That is fine.

For now:

```text
stdin is terminal input
stdout/stderr are console output
only RAMFS file descriptors are seekable
```

---

# 10. Add shell `seektest` command

Update `user/shell.c`.

First, update help text.

Replace:

```c
toyix_puts("commands: help, echo, args, cat, run, runbg, jobs, wait, kill, exit");
```

with:

```c
toyix_puts("commands: help, echo, args, cat, seektest, run, runbg, jobs, wait, kill, exit");
```

Now add this helper:

```c
static void print_chunk(const char *label, const char *buffer, toyix_i32 got) {
    toyix_printf("%s", label);

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_putchar('\n');
}
```

Add the command:

```c
static void cmd_seektest(int argc, char **argv) {
    if (argc != 2) {
        toyix_puts("usage: seektest PATH");
        return;
    }

    toyix_i32 fd = toyix_open(argv[1], 0);

    if (fd < 0) {
        toyix_printf("seektest: could not open %s\n", argv[1]);
        return;
    }

    char buffer[16];

    toyix_i32 got = toyix_read((toyix_u32)fd, buffer, 8);

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

    got = toyix_read((toyix_u32)fd, buffer, 8);

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

    got = toyix_read((toyix_u32)fd, buffer, 5);

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

    got = toyix_read((toyix_u32)fd, buffer, 5);

    if (got < 0) {
        toyix_puts("seektest: end-relative read failed");
        toyix_close((toyix_u32)fd);
        return;
    }

    print_chunk("seektest: tail read: ", buffer, got);

    toyix_close((toyix_u32)fd);
}
```

Then add the command dispatch branch after `cat`:

```c
if (toyix_streq(cmd_argv[0], "seektest")) {
    cmd_seektest(cmd_argc, cmd_argv);
    continue;
}
```

The shell dispatch section should include:

```c
if (toyix_streq(cmd_argv[0], "cat")) {
    cmd_cat(cmd_argc, cmd_argv);
    continue;
}

if (toyix_streq(cmd_argv[0], "seektest")) {
    cmd_seektest(cmd_argc, cmd_argv);
    continue;
}

if (toyix_streq(cmd_argv[0], "run")) {
    cmd_run(cmd_argc, cmd_argv);
    continue;
}
```

---

# 11. Expected `seektest /README` output

For the `/README` text:

```text
Toyix RAMFS
This file lives inside the kernel image.
The first filesystem is read-only and memory-backed.
```

Expected shell output:

```text
ush> seektest /README
seektest: first read: Toyix RA
seektest: rewind read: Toyix RA
seektest: skip read: RAMFS
seektest: tail read: ked.
```

Why `ked.`?

The file ends with:

```text
memory-backed.
```

The last five bytes are:

```text
ked.\n
```

Depending on how the console displays the newline, this may appear as:

```text
seektest: tail read: ked.
```

with the cursor moving to the next line.

For less fragile tests, grep only the first three seektest lines.

---

# 12. Update shell test input

In `kernel/program.c`, add the seek test after the `cat` commands.

Current section:

```c
inject_text("cat /README\n");
inject_text("cat /programs\n");
inject_text("run counter alpha beta\n");
```

Change to:

```c
inject_text("cat /README\n");
inject_text("cat /programs\n");
inject_text("seektest /README\n");
inject_text("run counter alpha beta\n");
```

This proves that file descriptors support read offsets and seeking from userland.

---

# 13. Update Makefile greps

Update the VFS test grep.

Replace:

```make
grep -q "VFS test: RAMFS sanity check passed" build/test.log
```

with:

```make
grep -q "VFS test: RAMFS seek sanity check passed" build/test.log
```

Add seek output greps:

```make
grep -q "VFS test: first bytes: Toyix RA" build/test.log
grep -q "VFS test: rewind bytes: Toyix RA" build/test.log
grep -q "VFS test: seek bytes: RAMFS" build/test.log
grep -q "seektest: first read: Toyix RA" build/test.log
grep -q "seektest: rewind read: Toyix RA" build/test.log
grep -q "seektest: skip read: RAMFS" build/test.log
```

Update shell help grep:

```make
grep -q "commands: help, echo, args, cat, seektest, run, runbg, jobs, wait, kill, exit" build/test.log
```

A useful VFS/shell block:

```make
	grep -q "VFS: initialized RAMFS with 2 file(s)" build/test.log
	grep -q "VFS test: first bytes: Toyix RA" build/test.log
	grep -q "VFS test: rewind bytes: Toyix RA" build/test.log
	grep -q "VFS test: seek bytes: RAMFS" build/test.log
	grep -q "VFS test: RAMFS seek sanity check passed" build/test.log
	grep -q "commands: help, echo, args, cat, seektest, run, runbg, jobs, wait, kill, exit" build/test.log
	grep -q "Toyix RAMFS" build/test.log
	grep -q "This file lives inside the kernel image." build/test.log
	grep -q "The first filesystem is read-only and memory-backed." build/test.log
	grep -q "seektest: first read: Toyix RA" build/test.log
	grep -q "seektest: rewind read: Toyix RA" build/test.log
	grep -q "seektest: skip read: RAMFS" build/test.log
```

Update final success message:

```make
	@echo "Boot, memory, heap, VFS seek, RAMFS, cat, process control, and shell jobs smoke test passed."
```

---

# 14. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 35 checks passed."
```

---

# 15. Interactive test

After boot:

```text
toyix> run shell
```

Inside shell:

```text
ush> seektest /README
```

Expected:

```text
seektest: first read: Toyix RA
seektest: rewind read: Toyix RA
seektest: skip read: RAMFS
seektest: tail read: ked.
```

Try seeking through `/programs`:

```text
ush> seektest /programs
```

Expected first bytes:

```text
seektest: first read: demo
cou
```

Because `/programs` starts with:

```text
demo
counter
shell
spin
```

The first eight bytes include a newline.

---

# 16. Common failures

## Failure: `seektest` says seek failed

Check that the fd passed to `toyix_seek()` is the open file descriptor returned by `toyix_open()`.

Only fds `>= 3` are seekable.

This should fail:

```c
toyix_seek(FD_STDIN, 0, TOYIX_SEEK_SET);
```

This should work:

```c
toyix_i32 fd = toyix_open("/README", 0);
toyix_seek(fd, 0, TOYIX_SEEK_SET);
```

## Failure: rewind read is empty

Check that `vfs_seek()` actually updates:

```c
file->offset = (uint32_t)next;
```

If the offset remains at EOF, the next read returns zero.

## Failure: second open starts at previous offset

Each `vfs_open()` must allocate a fresh file object:

```c
file->offset = 0;
```

The RAMFS node is shared. The VFS file object is per open.

## Failure: negative seek behaves strangely

Do not do seek arithmetic in unsigned values.

This is wrong:

```c
uint32_t next = file->offset + offset;
```

Use signed wide arithmetic:

```c
int64_t base = ...
int64_t next = base + (int64_t)offset;
```

Then reject `next < 0`.

## Failure: seek past EOF fails

For this chapter, seeking past EOF should be allowed.

Only reading after EOF returns zero.

So this should succeed:

```c
toyix_seek(fd, 100000, TOYIX_SEEK_SET);
```

and this should return zero bytes:

```c
toyix_read(fd, buffer, sizeof(buffer));
```

## Failure: `SYS_SEEK` returns `-1` for large valid positions

The syscall returns the new offset in `EAX`.

Userland sees it as signed `toyix_i32`.

That means offsets above `0x7FFFFFFF` would look negative.

For tiny RAMFS files, that does not matter.

Later, a better ABI should return errors separately from unsigned offsets.

---

# 17. What this chapter achieved

Before this chapter:

```text
Toyix files were forward-only read streams
```

After this chapter:

```text
VFS file objects have seekable offsets
SYS_SEEK supports SET/CUR/END
userland can rewind files
userland can seek relative to EOF
shell has seektest PATH
```

This makes Toyix file descriptors more realistic.

---

# 18. Design limitations

Still missing:

```text
stat
directory listing
write
truncate
append
permissions
mount points
path normalization
relative paths
current working directory
large file offsets
separate errno-style error reporting
```

The next step should be:

```text
SYS_STAT
```

Then the shell can do:

```text
ush> stat /README
type=file
size=101
```

After that:

```text
directory support
```

So `/programs` can become a real directory instead of a plain text file.

---

# 19. Next chapter

Next we should add:

```text
SYS_STAT
```

The ABI can be:

```c
typedef struct toyix_stat {
    uint32_t type;
    uint32_t size;
};
```

With types:

```text
TOYIX_FILE_REGULAR
TOYIX_FILE_DIRECTORY
```

At first, RAMFS only returns regular files.

The shell can then implement:

```text
ush> stat /README
type=file
size=101
```

That sets up the next major step: real directory entries and `ls`.

---

## Resources

- [Chapter source: Toyix repository](https://github.com/Monotoba/toyix)
- [Chapter release: Chapter_35](https://github.com/Monotoba/toyix/releases/tag/Chapter_35)

## Closure

Chapter 35 extends the first RAMFS-backed file layer with seekable file descriptors, giving both the kernel and userland their first rewindable named-file access path.

Happy Coding!
