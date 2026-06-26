# Toyix Roadmap

This roadmap tracks the direction of the Toyix series after Chapter 23. It is a living plan, not a promise that every future chapter title or boundary will stay fixed.

Toyix has moved beyond early bootstrapping. The current system already has paging, a heap, kernel threads, preemption, blocking primitives, keyboard and terminal input, a kernel monitor, ring-3 entry, fd-style syscalls, minimal processes, per-process address spaces with `CR3` switching, process teardown with user-page reclamation, an initial ELF32 loader for user programs, and a compiled user C demo that is embedded into the kernel image.

The next goal is to make user startup look more like a real process environment, then build enough process, file, and terminal infrastructure to support a small shell.

## Completed Chapters

| Chapter | Topic |
| ------: | ----- |
| 1 | Booting a freestanding Multiboot kernel |
| 2 | Descriptor tables and CPU exceptions |
| 3 | Hardware IRQs, timer ticks, and keyboard input |
| 4 | Multiboot memory maps and physical page allocation |
| 5 | Turning on paging |
| 6 | Building the first kernel heap |
| 7 | A real virtual memory mapping layer |
| 8 | Moving the heap onto virtual memory |
| 9 | Cooperative multitasking and kernel threads |
| 10 | Timer-driven preemptive multitasking |
| 11 | Blocking primitives, sleep queues, and scheduler hygiene |
| 12 | Wait queues and blocking keyboard input |
| 13 | Mutexes, semaphores, and a console lock |
| 14 | Terminal line discipline and a kernel monitor |
| 15 | Command tables, argument parsing, and shift-aware keyboard input |
| 16 | Entering user mode and returning through syscalls |
| 17 | Minimal processes, user memory copying, and more robust syscalls |
| 18 | File-descriptor syscalls and a tiny user-mode console program |
| 19 | Per-process address spaces and `CR3` switching |
| 20 | A tiny executable format and user program loader |
| 21 | Process teardown and address-space cleanup |
| 22 | First ELF32 loader milestone |
| 23 | Building a real user C program and embedding its ELF |

## Near-Term Plan

These chapters are the most likely next path because they build directly on Chapter 23.

| Chapter | Planned Topic |
| ------: | ------------- |
| 24 | User stack setup, `argc`, and `argv` startup handoff |
| 25 | Program image abstraction and embedded program registry |
| 26 | User program entry metadata and separate code/data/BSS regions |
| 27 | Process table lookup, process listing, and `ps`-style monitor output |
| 28 | Parent/child process relationships and exit status storage |
| 29 | `wait`/`waitpid`-style syscall support |
| 30 | Safer user fault handling that kills a process instead of panicking |

## Medium-Term Plan

After Toyix can load and manage small user programs, the series should move toward files and a user shell.

| Phase | Focus |
| ----: | ----- |
| 1 | ELF32 loader: program headers, user entry point, and user C programs |
| 2 | Basic userland runtime: startup code, syscall wrappers, `printf`, and small utilities |
| 3 | VFS foundation: file objects, paths, `open`, `read`, `write`, `close`, and `stat` |
| 4 | Read-only RAMFS: embedded files, directories, `cat`, `ls`, and program lookup |
| 5 | First user-mode shell: command dispatch, foreground programs, status codes, and simple variables |
| 6 | File descriptor inheritance and redirection: stdin/stdout/stderr, `>`, `>>`, `<`, and `2>` |
| 7 | Writable RAMFS: create, truncate, append, delete, and directory mutation |
| 8 | Block device layer: RAM disk first, then a buffer cache and block I/O tests |
| 9 | ToyFS: a small persistent filesystem with superblock, bitmaps, inodes, directories, and file data |
| 10 | Booting from filesystem programs: `/bin`, `/sbin/init`, shell startup, and installed utilities |

## Long-Term Plan

These areas are intentionally broader. They may split into many chapters as the system grows.

| Area | Goals |
| ---- | ----- |
| User memory | `brk`/`sbrk`, user heap, guard pages, read-only text mappings, demand allocation, and eventually copy-on-write |
| Devices | Character devices, `/dev`, console and keyboard devices, device registration, and blocking device reads |
| TTY | Canonical/raw modes, echo control, EOF and interrupt characters, foreground process groups, and shell line editing |
| Signals | `SIGKILL`, `SIGTERM`, `SIGCHLD`, `SIGSEGV`, `SIGINT`, default actions, masks, and delivery to user mode |
| Pipes and IPC | `pipe()`, blocking pipe reads/writes, EOF behavior, shell pipelines, and IPC tests |
| Networking | QEMU network setup, Ethernet, ARP, IPv4, ICMP, UDP, sockets, DNS, and a small TCP path |
| Multi-user support | User/group IDs, credentials, ownership, permissions, login, sessions, home directories, and a superuser model |
| Security | Kernel/user isolation review, syscall permission checks, path permission checks, descriptor permissions, and security tests |
| System services | `/sbin/init`, service startup, shutdown/reboot flow, filesystem sync, logging, and recovery shell |
| User applications | `cp`, `mv`, `rm`, `mkdir`, `touch`, `hexdump`, `grep`, pager/editor prototypes, and startup scripts |

## Current Direction

The most important architectural gap after Chapter 23 is the lack of a real process startup ABI.

The kernel now loads a real compiled user ELF, but the startup contract is still minimal: no `argc`, no `argv`, and no initial user stack layout beyond a blank mapped stack. The next stage should make startup look more like a real process environment while keeping the current loader path stable:

```text
argc
argv
user stack layout
crt0 argument handoff
kernel-supplied process arguments
shell-ready user entry
```

Once that exists, Toyix can move from a single embedded demo toward richer user programs, process management, a shell, and filesystem-backed execution.
