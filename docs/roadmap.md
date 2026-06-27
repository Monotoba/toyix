# Toyix Roadmap

This roadmap tracks the direction of the Toyix series against the current combined chapter plan now staged in `articles/normalized/`.

Toyix already has the early kernel foundation in place: paging, a heap, kernel threads, preemption, blocking primitives, terminal input, a kernel monitor, user-mode entry, fd-style syscalls, minimal processes, per-process address spaces, ELF loading, embedded compiled user programs, `argc`/`argv` startup, a program registry, process-table monitor commands, a shared userland runtime, and a first interactive user-mode shell.

The remaining work is no longer best described as a loose set of future themes. The normalized chapters define a clearer path from the current embedded-program system to a text-based operating system that can later host a GUI and applications.

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

## Normalized Chapter Path

The combined roadmap now runs from Chapter 31 through Chapter 111.

### 1. User Shell and Process Control

These chapters turn the current embedded-user-program path into a real shell and a more Unix-like process model.

| Chapter | Topic |
| ------: | ----- |
| 31 | `SYS_EXEC`, `SYS_WAITPID`, and Shell-Launched Programs |
| 32 | Process Ownership, Waiting, and Job State |
| 33 | Process Termination and Kill Checks |

### 2. File APIs and Program Lookup

These chapters add the first coherent VFS layer, RAMFS-backed program directories, and shell-visible command lookup.

| Chapter | Topic |
| ------: | ----- |
| 34 | First RAMFS and Core File APIs |
| 35 | Turning `/programs` into a Real Directory |
| 36 | Path-Based Program Launching with `/programs` |
| 37 | Shell Job References with `%1` |
| 38 | Dynamic `/programs` from the Program Registry |
| 39 | File Mode Bits and Executable Program Metadata |
| 40 | Shell `exec` Checks Executable Mode |
| 41 | Current Working Directories and Filesystem Launch Paths |
| 42 | `PATH` Search and Command Lookup |
| 43 | Shell Variables, Expansion, Prompt, and Status |
| 44 | Conditionals and Command Sequencing |
| 45 | Command History and Recall |

### 3. Writable RAMFS and Shell Redirection

These chapters make the shell materially useful for text-based work before persistent storage lands.

| Chapter | Topic |
| ------: | ----- |
| 46 | Writable RAMFS Files and `SYS_CREATE` |
| 47 | Redirection and Shell I/O Control |

### 4. Block Devices and ToyFS

These chapters build the persistent storage stack: block devices, a buffer cache, filesystem format, mounting, mutation, permissions, and larger files.

| Chapter | Topic |
| ------: | ----- |
| 48 | Block Device Abstraction |
| 49 | RAM Disk Block Device |
| 50 | Buffer Cache |
| 51 | Designing ToyFS |
| 52 | Formatting ToyFS |
| 53 | Mounting ToyFS |
| 54 | Reading the ToyFS Root Directory |
| 55 | ToyFS Name Lookup |
| 56 | Mounting ToyFS Under `/toyfs` |
| 57 | Reading Files from ToyFS |
| 58 | ToyFS Allocation Helpers |
| 59 | ToyFS Directory Insertion |
| 60 | Creating Files in ToyFS |
| 61 | Writing Files in ToyFS |
| 62 | Truncating ToyFS Files |
| 63 | Append Writes in ToyFS |
| 64 | Unlinking ToyFS Files |
| 65 | Making Directories in ToyFS |
| 66 | ToyFS Path Walking |
| 67 | Removing ToyFS Directories |
| 68 | Rename and ToyFS Checkpoint |
| 69 | Installing Embedded Programs into ToyFS |
| 70 | Reading ELF Files Through VFS |
| 71 | Loading Programs from ToyFS |
| 72 | Searching `/toyfs/bin` for Programs |
| 73 | Enforcing Executable Permissions |
| 74 | Adding `chmod` |
| 75 | Enforcing Read and Write Permissions |
| 76 | Directory Permissions and Permission Checkpoint |
| 77 | Designing Larger ToyFS Files |
| 78 | Reading ToyFS Indirect Blocks |
| 79 | Writing ToyFS Indirect Blocks |
| 80 | Truncation and Large-File Checkpoint |

### 5. Moving Core Utilities into Userland

These chapters shift the system from shell-heavy built-ins toward a real userland command set.

| Chapter | Topic |
| ------: | ----- |
| 81 | Moving `cat` into Userland |
| 82 | Moving `echo` and `touch` into Userland |
| 83 | Moving `stat` into Userland |
| 84 | Moving `ls` into Userland |
| 85 | Moving `mkdir`, `rm`, and `rmdir` into Userland |
| 86 | Moving `mv` and `chmod` into Userland |
| 87 | Designing Current Working Directories |
| 88 | Adding Process Current Working Directories |
| 89 | Resolving Relative Paths with CWD |
| 90 | `chdir`, `cd`, `pwd`, and CWD Checkpoint |
| 91 | Adding the `command` Built-in |
| 92 | Moving `pwd` into Userland and Userland/CWD Checkpoint |

### 6. Init, Services, and System Boot Policy

These chapters define the point where Toyix becomes a configurable text-based OS rather than a shell launched by hard-coded kernel behavior.

| Chapter | Topic |
| ------: | ----- |
| 93 | Designing the Toyix Init Process |
| 94 | Adding `/toyfs/bin/init` |
| 95 | Starting Init and Boot Handoff |
| 96 | Init, `/etc`, and Boot-Time Configuration |
| 97 | First Services and Controlled System Shutdown |
| 98 | TTYs, Sessions, and Terminal Control |
| 99 | Login, Users, and Process Credentials |
| 100 | Signals, Pipes, and Shell Pipelines |
| 101 | Process Spawning, Environment, and Service Supervision |
| 102 | PCI Discovery, IDE Disks, and a Persistent Root Filesystem |
| 103 | SATA, AHCI, and Persistent Disk Boot |
| 104 | Building a Usable Root Filesystem Layout |
| 105 | System Logging, Boot Scripts, and the First Usable Toyix |

### 7. Networking, Devices, and Pre-GUI System Readiness

These chapters push the text-based OS to the point where it is practical to administer, extend, and then begin GUI work.

| Chapter | Topic |
| ------: | ----- |
| 106 | First Networking: NIC Driver, Ethernet, IPv4, and Ping |
| 107 | Sockets, DNS, and Boot-Time Network Configuration |
| 108 | Basic Input and Legacy Device Support |
| 109 | USB Bring-Up and Removable Storage |
| 110 | Making Toyix Administrable from Userland |
| 111 | Packaging, Editing, and the Pre-GUI Toyix Checkpoint |

## Milestone Checkpoints

The normalized chapters imply these concrete milestones:

| Milestone | Expected System State |
| --------- | --------------------- |
| After 33 | User shell exists, programs can launch other programs, and process ownership/wait semantics are coherent |
| After 47 | The shell supports practical text workflows with writable files and output redirection |
| After 80 | Toyix has a persistent filesystem with directories, permissions, mutation, and large-file support |
| After 92 | Core everyday commands run from userland with CWD-aware path handling |
| After 105 | Toyix boots through `init`, reads `/etc`, manages services, and behaves like a usable text-based operating system |
| After 111 | Toyix reaches the intended pre-GUI checkpoint: administrable, packageable, storage-backed, and ready for window-system work |

## Current Direction

The roadmap is now centered on reaching a usable text-based operating system after the first shell milestone and before any windowing system work begins.

That means the priorities after Chapter 29 are:

```text
user shell and process control
file and path semantics
persistent storage
userland utilities
init and /etc-driven system behavior
networking and device support
pre-GUI administration and packaging
```

The combined chapter plan already encodes that path. The main planning job now is to keep published chapters aligned with this sequence while preserving the implementation detail needed to make each checkpoint real.
