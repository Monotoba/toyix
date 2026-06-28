# Chapter 36 — `SYS_STAT` and File Metadata

In Chapter 35, Toyix gained seekable file descriptors:

```text
SYS_OPEN
SYS_READ
SYS_SEEK
SYS_CLOSE
```

The shell could now test file offsets:

```text
ush> seektest /README
seektest: first read: Toyix RA
seektest: rewind read: Toyix RA
seektest: skip read: RAMFS
```

But userland still cannot ask basic questions about a path:

```text
Does this file exist?
How large is it?
What type of object is it?
```

This chapter adds:

```text
SYS_STAT
```

and a shell command:

```text
stat PATH
```

After this chapter:

```text
ush> stat /README
stat: path=/README type=file size=101

ush> stat /missing
stat: could not stat /missing
```

This gives Toyix the first file metadata syscall and prepares us for directories and `ls`.

---

## 1. What this chapter adds

Modify:

```text
include/kernel/vfs.h
kernel/vfs.c
include/kernel/syscall.h
kernel/syscall.c
user/include/toyix_syscall.h
user/shell.c
Makefile
tests/smoke.sh
```

New syscall:

```text
SYS_STAT = 15
```

New ABI structure:

```c
typedef struct toyix_stat {
    uint32_t type;
    uint32_t size;
} toyix_stat_t;
```

New file type constants:

```text
TOYIX_FILE_REGULAR   1
TOYIX_FILE_DIRECTORY 2
```

For now, RAMFS only returns regular files.

Directories come later.

---

## 2. Why `stat` matters

Right now, `cat` discovers whether a file exists by trying to open it.

That works, but a shell also needs metadata commands:

```text
stat /README
ls /
exec /bin/program
```

Those need a way to ask the filesystem about a path without necessarily opening it for reading.

`SYS_STAT` is that first metadata syscall.

---

## 3. Add VFS stat types

Update `include/kernel/vfs.h`.

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

#define VFS_NODE_REGULAR   1u
#define VFS_NODE_DIRECTORY 2u

typedef struct vfs_file vfs_file_t;

typedef struct vfs_stat {
    uint32_t type;
    uint32_t size;
} vfs_stat_t;

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

int vfs_stat(const char *path, vfs_stat_t *out_stat);

void vfs_close(vfs_file_t *file);

void vfs_test_once(void);

#endif
```

New pieces:

```c
#define VFS_NODE_REGULAR   1u
#define VFS_NODE_DIRECTORY 2u
```

and:

```c
typedef struct vfs_stat {
    uint32_t type;
    uint32_t size;
} vfs_stat_t;
```

The VFS has its own internal stat structure.

The syscall layer will copy this into the user ABI structure.

---

## 4. Update RAMFS node type

In `kernel/vfs.c`, update the RAMFS node structure.

Replace:

```c
typedef struct ramfs_node {
    const char *path;
    const uint8_t *data;
    uint32_t size;
} ramfs_node_t;
```

with:

```c
typedef struct ramfs_node {
    const char *path;
    uint32_t type;
    const uint8_t *data;
    uint32_t size;
} ramfs_node_t;
```

Then update the static node table.

Replace:

```c
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
```

with:

```c
static const ramfs_node_t ramfs_nodes[] = {
    {
        .path = "/README",
        .type = VFS_NODE_REGULAR,
        .data = readme_text,
        .size = sizeof(readme_text) - 1u
    },
    {
        .path = "/programs",
        .type = VFS_NODE_REGULAR,
        .data = programs_text,
        .size = sizeof(programs_text) - 1u
    }
};
```

Both current nodes are regular files.

---

## 5. Add `vfs_stat()`

Add this to `kernel/vfs.c` after `vfs_size()`:

```c
int vfs_stat(const char *path, vfs_stat_t *out_stat) {
    if (path == 0 || out_stat == 0) {
        return VFS_ERR_INVALID;
    }

    const ramfs_node_t *node = ramfs_find(path);

    if (node == 0) {
        return VFS_ERR_NOT_FOUND;
    }

    out_stat->type = node->type;
    out_stat->size = node->size;

    return VFS_OK;
}
```

This is path-based metadata.

It does not open the file.

It simply looks up the RAMFS node and returns:

```text
type
size
```

---

## 6. Expand `vfs_test_once()`

Add a stat test near the beginning of `vfs_test_once()`.

After opening `/README`, insert:

```c
vfs_stat_t stat;

if (vfs_stat("/README", &stat) != VFS_OK) {
    kernel_panic("VFS test could not stat /README");
}

if (stat.type != VFS_NODE_REGULAR || stat.size == 0) {
    kernel_panic("VFS test received invalid /README stat");
}

console_write("VFS test: /README size=");
console_write_u32_dec(stat.size);
console_writeln(" type=file");
```

The full beginning of `vfs_test_once()` should look like:

```c
void vfs_test_once(void) {
    console_writeln("VFS test: starting RAMFS open/read/seek/stat/close test");

    vfs_stat_t stat;

    if (vfs_stat("/README", &stat) != VFS_OK) {
        kernel_panic("VFS test could not stat /README");
    }

    if (stat.type != VFS_NODE_REGULAR || stat.size == 0) {
        kernel_panic("VFS test received invalid /README stat");
    }

    console_write("VFS test: /README size=");
    console_write_u32_dec(stat.size);
    console_writeln(" type=file");

    vfs_file_t *file = 0;

    if (vfs_open("/README", &file) != VFS_OK || file == 0) {
        kernel_panic("VFS test could not open /README");
    }

    /*
     * Existing seek/read tests continue here.
     */
```

Also update the final success line from Chapter 35:

```c
console_writeln("VFS test: RAMFS seek sanity check passed");
```

to:

```c
console_writeln("VFS test: RAMFS stat/seek sanity check passed");
```

---

## 7. Update syscall constants

Update both:

```text
include/kernel/syscall.h
user/include/toyix_syscall.h
```

Add:

```c
#define SYS_STAT     15u
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
#define SYS_STAT     15u
```

Add file type constants to both headers:

```c
#define TOYIX_FILE_REGULAR   1u
#define TOYIX_FILE_DIRECTORY 2u
```

---

## 8. Add stat ABI structure

In `include/kernel/syscall.h`, add:

```c
typedef struct toyix_stat {
    uint32_t type;
    uint32_t size;
} toyix_stat_t;
```

Place it near `toyix_procinfo_t`.

The ABI structure section should now contain:

```c
typedef struct toyix_procinfo {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint32_t exit_code;
    uint32_t exited;
} toyix_procinfo_t;

typedef struct toyix_stat {
    uint32_t type;
    uint32_t size;
} toyix_stat_t;
```

In `user/include/toyix_syscall.h`, add the same structure using user integer types:

```c
typedef struct toyix_stat {
    toyix_u32 type;
    toyix_u32 size;
} toyix_stat_t;
```

Place it after `toyix_procinfo_t`.

---

## 9. Add user `toyix_stat()` wrapper

In `user/include/toyix_syscall.h`, add:

```c
static inline toyix_i32 toyix_stat(
    const char *path,
    toyix_stat_t *stat
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_STAT),
          "b"(path),
          "c"(stat)
        : "memory"
    );

    return result;
}
```

Return behavior:

```text
0   success
-1  error
```

The metadata is written into the user-provided `toyix_stat_t`.

---

## 10. Add kernel `SYS_STAT`

In `kernel/syscall.c`, add this helper:

```c
static uint32_t syscall_vfs_type_to_abi(uint32_t vfs_type) {
    switch (vfs_type) {
        case VFS_NODE_REGULAR:
            return TOYIX_FILE_REGULAR;

        case VFS_NODE_DIRECTORY:
            return TOYIX_FILE_DIRECTORY;

        default:
            return 0;
    }
}
```

Then add:

```c
static void syscall_stat(interrupt_frame_t *frame) {
    uintptr_t user_path = (uintptr_t)frame->ebx;
    uintptr_t user_stat = (uintptr_t)frame->ecx;

    if (user_path == 0 || user_stat == 0) {
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

    vfs_stat_t vfs_stat_result;

    if (vfs_stat(path, &vfs_stat_result) != VFS_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    toyix_stat_t user_result;

    user_result.type = syscall_vfs_type_to_abi(vfs_stat_result.type);
    user_result.size = vfs_stat_result.size;

    if (user_result.type == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (copy_to_user(
            user_stat,
            &user_result,
            sizeof(user_result)
        ) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = 0;
}
```

This does:

```text
copy user path
  ↓
query VFS metadata
  ↓
convert VFS type to public ABI type
  ↓
copy result to user
```

---

## 11. Update syscall handler

Add:

```c
case SYS_STAT:
    syscall_stat(frame);
    syscall_finish_or_kill(frame);
    return;
```

Place it near the other file syscalls:

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

case SYS_STAT:
    syscall_stat(frame);
    syscall_finish_or_kill(frame);
    return;
```

---

## 12. Add shell file-type helper

In `user/shell.c`, add:

```c
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
```

Place it near the existing `proc_state_name()` helper.

---

## 13. Add shell `stat` command

Update help text.

Replace:

```c
toyix_puts("commands: help, echo, args, cat, seektest, run, runbg, jobs, wait, kill, exit");
```

with:

```c
toyix_puts("commands: help, echo, args, cat, seektest, stat, run, runbg, jobs, wait, kill, exit");
```

Now add:

```c
static void cmd_stat(int argc, char **argv) {
    if (argc != 2) {
        toyix_puts("usage: stat PATH");
        return;
    }

    toyix_stat_t stat;

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
```

Then add the dispatch branch after `seektest`:

```c
if (toyix_streq(cmd_argv[0], "stat")) {
    cmd_stat(cmd_argc, cmd_argv);
    continue;
}
```

The file command section should look like:

```c
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
```

---

## 14. Update shell test input

In `kernel/program.c`, add `stat` commands after the existing `cat` commands.

Change:

```c
inject_text("cat /README\n");
inject_text("cat /programs\n");
inject_text("seektest /README\n");
inject_text("run counter alpha beta\n");
```

to:

```c
inject_text("cat /README\n");
inject_text("cat /programs\n");
inject_text("stat /README\n");
inject_text("stat /programs\n");
inject_text("stat /missing\n");
inject_text("seektest /README\n");
inject_text("run counter alpha beta\n");
```

This proves:

```text
stat succeeds for /README
stat succeeds for /programs
stat fails cleanly for /missing
shell continues after stat failures
```

---

## 15. Expected shell output

The shell test should now include:

```text
ush> stat /README
stat: path=/README type=file size=101

ush> stat /programs
stat: path=/programs type=file size=24

ush> stat /missing
stat: could not stat /missing
```

The exact sizes may differ if you edited the RAMFS strings.

Therefore, tests should grep the stable prefixes:

```text
stat: path=/README type=file size=
stat: path=/programs type=file size=
stat: could not stat /missing
```

---

## 16. Update Makefile greps

Update the VFS success grep.

Replace:

```make
grep -q "VFS test: RAMFS seek sanity check passed" build/test.log
```

with:

```make
grep -q "VFS test: RAMFS stat/seek sanity check passed" build/test.log
```

Add VFS stat grep:

```make
grep -q "VFS test: /README size=" build/test.log
grep -q "type=file" build/test.log
```

Update shell help grep:

```make
grep -q "commands: help, echo, args, cat, seektest, stat, run, runbg, jobs, wait, kill, exit" build/test.log
```

Add stat command greps:

```make
grep -q "stat: path=/README type=file size=" build/test.log
grep -q "stat: path=/programs type=file size=" build/test.log
grep -q "stat: could not stat /missing" build/test.log
```

A useful VFS/shell block:

```make
	grep -q "VFS: initialized RAMFS with 2 file(s)" build/test.log
	grep -q "VFS test: /README size=" build/test.log
	grep -q "VFS test: first bytes: Toyix RA" build/test.log
	grep -q "VFS test: rewind bytes: Toyix RA" build/test.log
	grep -q "VFS test: seek bytes: RAMFS" build/test.log
	grep -q "VFS test: RAMFS stat/seek sanity check passed" build/test.log
	grep -q "commands: help, echo, args, cat, seektest, stat, run, runbg, jobs, wait, kill, exit" build/test.log
	grep -q "Toyix RAMFS" build/test.log
	grep -q "This file lives inside the kernel image." build/test.log
	grep -q "The first filesystem is read-only and memory-backed." build/test.log
	grep -q "stat: path=/README type=file size=" build/test.log
	grep -q "stat: path=/programs type=file size=" build/test.log
	grep -q "stat: could not stat /missing" build/test.log
	grep -q "seektest: first read: Toyix RA" build/test.log
	grep -q "seektest: rewind read: Toyix RA" build/test.log
	grep -q "seektest: skip read: RAMFS" build/test.log
```

Update final success message:

```make
	@echo "Boot, memory, heap, VFS stat/seek, RAMFS, cat, process control, and shell jobs smoke test passed."
```

---

## 17. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 36 checks passed."
```

---

## 18. Interactive test

After boot:

```text
toyix> run shell
```

Inside shell:

```text
ush> stat /README
```

Expected:

```text
stat: path=/README type=file size=...
```

Then:

```text
ush> stat /programs
```

Expected:

```text
stat: path=/programs type=file size=...
```

Try a missing path:

```text
ush> stat /missing
```

Expected:

```text
stat: could not stat /missing
```

Then make sure existing file commands still work:

```text
ush> cat /README
ush> seektest /README
```

---

## 19. Common failures

### Failure: `stat /README` returns error

Check:

```text
SYS_STAT number matches in kernel and user headers
toyix_stat wrapper passes path in EBX and stat pointer in ECX
syscall_stat copies user path correctly
vfs_stat uses ramfs_find(path)
path is exactly /README
```

The path is case-sensitive.

```text
/README
```

works.

```text
/readme
```

does not.

---

### Failure: stat writes garbage into user memory

Check that the user and kernel ABI structs match exactly.

Kernel:

```c
typedef struct toyix_stat {
    uint32_t type;
    uint32_t size;
} toyix_stat_t;
```

User:

```c
typedef struct toyix_stat {
    toyix_u32 type;
    toyix_u32 size;
} toyix_stat_t;
```

Both fields must be 32-bit and in the same order.

---

### Failure: size is zero for real files

Check RAMFS node initialization:

```c
.size = sizeof(readme_text) - 1u
```

Do not use:

```c
sizeof(readme_text)
```

unless you intentionally want to include the trailing null byte.

---

### Failure: type prints as unknown

Check the mapping:

```c
VFS_NODE_REGULAR
  ↓
TOYIX_FILE_REGULAR
```

The syscall layer should call:

```c
syscall_vfs_type_to_abi()
```

and user shell should call:

```c
file_type_name()
```

---

### Failure: `cat` works but `stat` fails

That usually means `vfs_open()` has a correct lookup path, but `vfs_stat()` is not using the same lookup helper.

Both should use:

```c
ramfs_find(path)
```

---

### Failure: `stat /missing` crashes

Missing files should return:

```c
VFS_ERR_NOT_FOUND
```

from `vfs_stat()`.

Then `syscall_stat()` should return:

```text
0xFFFFFFFF
```

to userland.

The shell should print:

```text
stat: could not stat /missing
```

---

## 20. What this chapter achieved

Before this chapter:

```text
userland could open/read/seek/close files
but could not query metadata
```

After this chapter:

```text
VFS can stat paths
SYS_STAT exposes file metadata to userland
shell can run stat PATH
missing paths fail cleanly
file type and size are visible from ring 3
```

This is another key filesystem milestone.

---

## 21. Design limitations

Still missing:

```text
directories
directory stat behavior
permissions
timestamps
inode numbers
device IDs
block counts
links
path normalization
relative paths
current working directory
```

This stat structure is intentionally tiny:

```c
type
size
```

That is enough for the next milestone:

```text
directories and ls
```

---

## Next Chapter

Now we are ready to turn RAMFS from a flat table of file paths into something that can represent directories.

The next chapter should add:

```text
directory nodes
root directory /
SYS_READDIR or SYS_GETDENTS
shell ls PATH
```

A first directory ABI can be simple:

```c
typedef struct toyix_dirent {
    uint32_t type;
    char name[32];
};
```

Then the shell can do:

```text
ush> ls /
README
programs
```

After that, `/programs` can become a real directory, and later filesystem-backed `exec` can use paths.

---

## Resources

- [Chapter source: Toyix repository](https://github.com/Monotoba/toyix)
- [Chapter release: Chapter_36](https://github.com/Monotoba/toyix/releases/tag/Chapter_36)

## Closure

Chapter 36 gives Toyix its first filesystem metadata syscall, which is the step that makes later directory handling and shell-visible file inspection possible.

Happy Coding!
