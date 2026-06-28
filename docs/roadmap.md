# Toyix Roadmap

This roadmap tracks the direction of the Toyix series from the current published chapters through the planned text-based OS milestones ahead.

Toyix already has the early kernel foundation in place: paging, a heap, kernel threads, preemption, blocking primitives, terminal input, a kernel monitor, user-mode entry, fd-style syscalls, minimal processes, per-process address spaces, ELF loading, embedded compiled user programs, `argc`/`argv` startup, a program registry, process-table monitor commands, a shared userland runtime, a first interactive user-mode shell, parent-owned zombie child tracking, shell-visible background jobs, cooperative child termination, and a first read-only RAMFS/VFS path with named file reads, rewindable file descriptors, and basic path metadata from user mode.

The remaining work is no longer best described as a loose set of future themes. The planned chapters define a clearer path from the current embedded-program system to a text-based operating system that can later host a GUI and applications.

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
| 24 | User `argc`, `argv`, and a real initial stack |
| 25 | Embedded program registry and `run` monitor command |
| 26 | Process table, `ps`, `runbg`, and `wait PID` |
| 27 | A second user program for safe background execution |
| 28 | Pattern-based user-program build rules and a `USER_PROGRAMS` list |
| 29 | First userland runtime |
| 30 | First user-mode shell |
| 31 | `SYS_EXEC`, `SYS_WAITPID`, and shell-launched programs |
| 32 | Process ownership, waiting, and job state |
| 33 | Process termination and kill checks |
| 34 | First RAMFS and core file APIs |
| 35 | `SYS_SEEK` and rewindable file descriptors |
| 36 | `SYS_STAT` and file metadata |

## Planned Chapter Path

The roadmap now runs from Chapter 37 through Chapter 112.

### 1. User Shell and Process Control

These chapters turn the current embedded-user-program path into a real shell and a more Unix-like process model.

| Chapter | Topic |
| ------: | ----- |

### 2. File APIs and Program Lookup

These chapters add the first coherent VFS layer, RAMFS-backed program directories, and shell-visible command lookup.

| Chapter | Topic |
| ------: | ----- |
| 37 | Turning `/programs` into a Real Directory |
| 38 | Path-Based Program Launching with `/programs` |
| 39 | Shell Job References with `%1` |
| 40 | Dynamic `/programs` from the Program Registry |
| 41 | File Mode Bits and Executable Program Metadata |
| 42 | Shell `exec` Checks Executable Mode |
| 43 | Current Working Directories and Filesystem Launch Paths |
| 44 | `PATH` Search and Command Lookup |
| 45 | Shell Variables, Expansion, Prompt, and Status |
| 46 | Conditionals and Command Sequencing |
| 47 | Command History and Recall |

### 3. Writable RAMFS and Shell Redirection

These chapters make the shell materially useful for text-based work before persistent storage lands.

| Chapter | Topic |
| ------: | ----- |
| 48 | Writable RAMFS Files and `SYS_CREATE` |
| 49 | Redirection and Shell I/O Control |

### 4. Block Devices and ToyFS

These chapters build the persistent storage stack: block devices, a buffer cache, filesystem format, mounting, mutation, permissions, and larger files.

| Chapter | Topic |
| ------: | ----- |
| 50 | Block Device Abstraction |
| 51 | RAM Disk Block Device |
| 52 | Buffer Cache |
| 53 | Designing ToyFS |
| 54 | Formatting ToyFS |
| 55 | Mounting ToyFS |
| 56 | Reading the ToyFS Root Directory |
| 57 | ToyFS Name Lookup |
| 58 | Mounting ToyFS Under `/toyfs` |
| 59 | Reading Files from ToyFS |
| 60 | ToyFS Allocation Helpers |
| 61 | ToyFS Directory Insertion |
| 62 | Creating Files in ToyFS |
| 63 | Writing Files in ToyFS |
| 64 | Truncating ToyFS Files |
| 65 | Append Writes in ToyFS |
| 66 | Unlinking ToyFS Files |
| 67 | Making Directories in ToyFS |
| 68 | ToyFS Path Walking |
| 69 | Removing ToyFS Directories |
| 70 | Rename and ToyFS Checkpoint |
| 71 | Installing Embedded Programs into ToyFS |
| 72 | Reading ELF Files Through VFS |
| 73 | Loading Programs from ToyFS |
| 74 | Searching `/toyfs/bin` for Programs |
| 75 | Enforcing Executable Permissions |
| 76 | Adding `chmod` |
| 77 | Enforcing Read and Write Permissions |
| 78 | Directory Permissions and Permission Checkpoint |
| 79 | Designing Larger ToyFS Files |
| 80 | Reading ToyFS Indirect Blocks |
| 81 | Writing ToyFS Indirect Blocks |
| 82 | Truncation and Large-File Checkpoint |

### 5. Moving Core Utilities into Userland

These chapters shift the system from shell-heavy built-ins toward a real userland command set.

| Chapter | Topic |
| ------: | ----- |
| 83 | Moving `cat` into Userland |
| 84 | Moving `echo` and `touch` into Userland |
| 85 | Moving `stat` into Userland |
| 86 | Moving `ls` into Userland |
| 87 | Moving `mkdir`, `rm`, and `rmdir` into Userland |
| 88 | Moving `mv` and `chmod` into Userland |
| 89 | Designing Current Working Directories |
| 90 | Adding Process Current Working Directories |
| 91 | Resolving Relative Paths with CWD |
| 92 | `chdir`, `cd`, `pwd`, and CWD Checkpoint |
| 93 | Adding the `command` Built-in |
| 94 | Moving `pwd` into Userland and Userland/CWD Checkpoint |

### 6. Init, Services, and System Boot Policy

These chapters define the point where Toyix becomes a configurable text-based OS rather than a shell launched by hard-coded kernel behavior.

| Chapter | Topic |
| ------: | ----- |
| 95 | Designing the Toyix Init Process |
| 96 | Adding `/toyfs/bin/init` |
| 97 | Starting Init and Boot Handoff |
| 98 | Init, `/etc`, and Boot-Time Configuration |
| 99 | First Services and Controlled System Shutdown |
| 100 | TTYs, Sessions, and Terminal Control |
| 101 | Login, Users, and Process Credentials |
| 102 | Signals, Pipes, and Shell Pipelines |
| 103 | Process Spawning, Environment, and Service Supervision |
| 104 | PCI Discovery, IDE Disks, and a Persistent Root Filesystem |
| 105 | SATA, AHCI, and Persistent Disk Boot |
| 106 | Building a Usable Root Filesystem Layout |
| 107 | System Logging, Boot Scripts, and the First Usable Toyix |

### 7. Networking, Devices, and Pre-GUI System Readiness

These chapters push the text-based OS to the point where it is practical to administer, extend, and then begin GUI work.

| Chapter | Topic |
| ------: | ----- |
| 108 | First Networking: NIC Driver, Ethernet, IPv4, and Ping |
| 109 | Sockets, DNS, and Boot-Time Network Configuration |
| 110 | Basic Input and Legacy Device Support |
| 111 | USB Bring-Up and Removable Storage |
| 112 | Making Toyix Administrable from Userland |
| 113 | Packaging, Editing, and the Pre-GUI Toyix Checkpoint |

## Milestone Checkpoints

The planned chapters imply these concrete milestones:

| Milestone | Expected System State |
| --------- | --------------------- |
| After 36 | User shell exists, programs can launch other programs, kill child jobs, read named files from the first RAMFS, reposition file offsets, and query basic path metadata |
| After 49 | The shell supports practical text workflows with writable files and output redirection |
| After 82 | Toyix has a persistent filesystem with directories, permissions, mutation, and large-file support |
| After 94 | Core everyday commands run from userland with CWD-aware path handling |
| After 107 | Toyix boots through `init`, reads `/etc`, manages services, and behaves like a usable text-based operating system |
| After 113 | Toyix reaches the intended pre-GUI checkpoint: administrable, packageable, storage-backed, and ready for window-system work |

## Current Direction

The roadmap is now centered on reaching a usable text-based operating system after the first shell milestone and before any windowing system work begins.

That means the priorities after Chapter 36 are:

```text
user shell and process control
file and path semantics
persistent storage
userland utilities
init and /etc-driven system behavior
networking and device support
pre-GUI administration and packaging
```

The chapter plan already encodes that path. The main planning job now is to keep published chapters aligned with this sequence while preserving the implementation detail needed to make each checkpoint real.
