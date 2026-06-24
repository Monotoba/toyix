## 20. Series Roadmap

When this series began, I sketched a short roadmap of roughly twenty chapters. That was enough to describe the *idea* of the project, but it no longer reflects the actual scope of what we are building.

Toyix is not just a “boot and print a message” tutorial.

The goal is to build a small but usable Linux-style operating system with:

```text
kernel initialization
interrupts and exceptions
memory management
paging
kernel heap
threads and scheduling
user mode
system calls
processes
ELF loading
a shell
a VFS layer
a real filesystem
block devices
I/O subsystem
terminal/TTY support
networking
user applications
multi-user support
permissions
security boundaries
```

This roadmap is not a rigid promise that every chapter title will remain exactly the same. As the operating system grows, some topics may split into multiple chapters and others may merge. The first eleven chapters already compressed the original bootstrapping, interrupt, and early memory-management plan into a faster bring-up path. This updated roadmap uses the actual Chapter 1-11 sequence as the baseline and then maps the next major steps from there.

---

### Phase 1 — Completed Bring-Up

| Chapter | Topic                                                        |
| ------: | ------------------------------------------------------------ |
|       1 | Smallest useful Multiboot kernel with serial/VGA output      |
|       2 | GDT, IDT, CPU exceptions, and panic handling                 |
|       3 | PIC, PIT timer, keyboard IRQs, and interruptible idle loop   |
|       4 | Multiboot memory map and physical page allocator             |
|       5 | Identity paging and page-fault diagnostics                   |
|       6 | Early kernel heap with `kmalloc`, `kcalloc`, and `kfree`     |
|       7 | Virtual memory map/unmap/translate layer                     |

---

### Phase 2 — Memory Management Maturation

| Chapter | Topic                                                   |
| ------: | ------------------------------------------------------- |
|       8 | Move the kernel heap onto VMM-backed virtual pages      |
|       9 | Cooperative kernel threads with a software context switch |
|      10 | Timer-driven preemptive multitasking                     |
|      11 | Blocking primitives, sleep queues, and zombie reaping   |
|      12 | Wait queues and lock-protected wakeups                   |

---

### Phase 3 — Console Input and Early Kernel Services

| Chapter | Topic                         |
| ------: | ----------------------------- |
|      13 | Keyboard scancode decoding    |
|      14 | Shift-aware keyboard input    |
|      15 | Basic input buffering         |
|      16 | Terminal line discipline      |
|      17 | Early kernel monitor          |
|      18 | Kernel command table          |
|      19 | Argument parsing in monitor   |
|      20 | Console locking and log polish |

---

### Phase 4 — Threads and Scheduling

| Chapter | Topic                           |
| ------: | ------------------------------- |
|      21 | Scheduler locking and critical sections |
|      22 | Wait queues                     |
|      23 | Mutexes                         |
|      24 | Semaphores                      |
|      25 | Scheduler hygiene and debugging  |
|      26 | Thread priority policy           |
|      27 | Load balancing basics            |
|      28 | Scheduler instrumentation        |
|      29 | CPU affinity basics              |
|      30 | Fairness policy                  |
|      31 | Lock contention profiling        |

---

### Phase 5 — User Mode and System Calls

| Chapter | Topic                                                         |
| ------: | ------------------------------------------------------------- |
|      32 | Entering user mode                                            |
|      33 | First `int 0x80` syscall                                      |
|      34 | User pointer validation                                       |
|      35 | `SYS_WRITE`, `SYS_EXIT`, and `SYS_SLEEP`                      |
|      36 | Process structure                                             |
|      37 | User stacks                                                   |
|      38 | Per-process address spaces                                    |
|      39 | Returning from syscalls safely                                |
|      40 | Killing faulty user processes instead of panicking the kernel |

---

### Phase 6 — Programs and ELF Loading

| Chapter | Topic                        |
| ------: | ---------------------------- |
|      41 | First tiny executable format |
|      42 | Loading a toy executable     |
|      43 | ELF32 loader introduction    |
|      44 | Loading ELF program headers  |
|      45 | Building user C programs     |
|      46 | User startup code and `crt0` |
|      47 | Passing `argc` and `argv`    |
|      48 | Embedded program registry    |
|      49 | Running named user programs  |
|      50 | Userland library foundation  |
|      51 | Minimal `printf`             |
|      52 | Process exit status          |

---

### Phase 7 — Process Management

| Chapter | Topic                                  |
| ------: | -------------------------------------- |
|      53 | Process table                          |
|      54 | `ps` support                           |
|      55 | Parent and child process relationships |
|      56 | `waitpid`                              |
|      57 | Zombies and reaping                    |
|      58 | Background processes                   |
|      59 | Process IDs and parent process IDs     |
|      60 | Nonblocking waits                      |
|      61 | Process info syscall                   |
|      62 | Cooperative kill                       |
|      63 | Timer-interrupt kill checks            |
|      64 | CPU-bound process termination testing  |

---

### Phase 8 — VFS and RAMFS

| Chapter | Topic                            |
| ------: | -------------------------------- |
|      65 | VFS design                       |
|      66 | First read-only RAMFS            |
|      67 | `open`, `read`, and `close`      |
|      68 | `cat` command                    |
|      69 | `seek` support                   |
|      70 | `stat` support                   |
|      71 | Directories and `readdir`        |
|      72 | `ls` command                     |
|      73 | `/programs` directory            |
|      74 | Executable metadata              |
|      75 | Path-based program launching     |
|      76 | Current working directory        |
|      77 | Relative path resolution         |
|      78 | Shell `PATH` search              |
|      79 | Writable RAMFS files             |
|      80 | `create` and file-backed `write` |

---

### Phase 9 — First User Shell

| Chapter | Topic                                         |
| ------: | --------------------------------------------- |
|      81 | First user-mode shell                         |
|      82 | Shell command dispatch                        |
|      83 | Shell-launched programs                       |
|      84 | Foreground and background jobs                |
|      85 | Job references like `%1`                      |
|      86 | Shell `PATH` management                       |
|      87 | Shell variables                               |
|      88 | Variable expansion                            |
|      89 | Configurable prompt                           |
|      90 | Last command status with `$?`                 |
|      91 | Command sequencing with `;`                   |
|      92 | Conditionals with `&&` and `||`               |
|      93 | Shell history                                 |
|      94 | History recall with `!!`, `!N`, and `!prefix` |

---

### Phase 10 — Shell I/O Redirection

| Chapter | Topic                                   |
| ------: | --------------------------------------- |
|      95 | Output redirection with `>`             |
|      96 | Append redirection with `>>`            |
|      97 | Process standard descriptor inheritance |
|      98 | Child process stdout redirection        |
|      99 | Stderr redirection with `2>` and `2>>`  |
|     100 | Descriptor merging with `2>&1`          |
|     101 | Input redirection with `<`              |
|     102 | Shell checkpoint and pivot to storage   |

At this point, the shell is useful enough for testing deeper OS features. We will deliberately defer more advanced shell features such as pipes, quoting, scripting, job control, and signal-aware terminal control until the kernel has stronger I/O and process primitives.

---

### Phase 11 — Block Devices and Storage Foundation

| Chapter | Topic                                       |
| ------: | ------------------------------------------- |
|     103 | Block device abstraction                    |
|     104 | RAM disk block device                       |
|     105 | Block device registry and discovery         |
|     106 | Block read/write tests                      |
|     107 | Buffer cache design                         |
|     108 | Buffer cache implementation                 |
|     109 | Dirty buffers and flushing                  |
|     110 | Block cache debugging tools                 |
|     111 | Preparing the storage layer for filesystems |

---

### Phase 12 — ToyFS: A Real Filesystem

| Chapter | Topic                                 |
| ------: | ------------------------------------- |
|     112 | ToyFS design goals                    |
|     113 | ToyFS on-disk layout                  |
|     114 | Superblock                            |
|     115 | Block bitmap                          |
|     116 | Inode bitmap                          |
|     117 | Inode table                           |
|     118 | Directory entry format                |
|     119 | Formatting a ToyFS image              |
|     120 | Mounting ToyFS                        |
|     121 | Reading the ToyFS root directory      |
|     122 | Opening files from ToyFS              |
|     123 | Reading file data blocks              |
|     124 | Creating files                        |
|     125 | Writing file data blocks              |
|     126 | File growth with direct blocks        |
|     127 | Truncating files                      |
|     128 | `unlink`                              |
|     129 | `mkdir`                               |
|     130 | `rmdir`                               |
|     131 | `rename`                              |
|     132 | Filesystem consistency checks         |
|     133 | Mounting ToyFS as the root filesystem |

This is the point where Toyix gains a real filesystem. The earlier RAMFS was useful, but ToyFS will be an actual block-backed filesystem with persistent structure.

---

### Phase 13 — Loading Applications from the Filesystem

| Chapter | Topic                                   |
| ------: | --------------------------------------- |
|     134 | Storing ELF files in ToyFS              |
|     135 | Loading ELF from VFS paths              |
|     136 | Replacing embedded program execution    |
|     137 | `/bin` and `/sbin` directories          |
|     138 | Installing basic user programs          |
|     139 | Shell execution from `/bin`             |
|     140 | Program permissions and executable bits |
|     141 | Init program loaded from filesystem     |
|     142 | Booting into userland from `/sbin/init` |

This phase moves Toyix closer to a normal OS boot flow:

```text
kernel boots
mount root filesystem
launch /sbin/init
init starts shell or services
```

---

### Phase 14 — User Memory and Runtime Support

| Chapter | Topic                           |
| ------: | ------------------------------- |
|     143 | User heap region                |
|     144 | `brk` and `sbrk` syscalls       |
|     145 | Userland `malloc`               |
|     146 | Userland `free`                 |
|     147 | `calloc` and `realloc`          |
|     148 | Guard pages                     |
|     149 | User stack growth checks        |
|     150 | Read-only text pages            |
|     151 | Writable data pages             |
|     152 | Better user page fault handling |
|     153 | Demand allocation               |
|     154 | Copy-on-write introduction      |

This phase lets user applications become more realistic. Without a heap, programs remain tiny and artificial.

---

### Phase 15 — I/O Subsystem and Devices

| Chapter | Topic                                    |
| ------: | ---------------------------------------- |
|     155 | Device model overview                    |
|     156 | Character device abstraction             |
|     157 | Block device abstraction refinements     |
|     158 | `/dev` filesystem                        |
|     159 | `/dev/console`                           |
|     160 | `/dev/null`                              |
|     161 | `/dev/zero`                              |
|     162 | Keyboard device node                     |
|     163 | Terminal device node                     |
|     164 | Device major/minor numbers or equivalent |
|     165 | Driver registration                      |
|     166 | Polling and blocking device reads        |
|     167 | Device permissions                       |

This gives Toyix a more Unix-like I/O model where devices appear as files.

---

### Phase 16 — TTY and Full Terminal Support

| Chapter | Topic                             |
| ------: | --------------------------------- |
|     168 | TTY abstraction                   |
|     169 | Canonical input mode              |
|     170 | Raw input mode                    |
|     171 | Echo control                      |
|     172 | Backspace and line editing        |
|     173 | Arrow-key escape sequence parsing |
|     174 | Shell command-line editing        |
|     175 | Scrollback                        |
|     176 | Terminal resize model             |
|     177 | Ctrl+C and interrupt characters   |
|     178 | Ctrl+D and EOF                    |
|     179 | Ctrl+Z groundwork                 |
|     180 | Foreground process group          |
|     181 | Job-control terminal rules        |

This phase turns the current simple console into a much more complete terminal subsystem.

---

### Phase 17 — Signals and Job Control

| Chapter | Topic                                    |
| ------: | ---------------------------------------- |
|     182 | Signal model overview                    |
|     183 | `SIGKILL`                                |
|     184 | `SIGTERM`                                |
|     185 | `SIGCHLD`                                |
|     186 | `SIGSEGV`                                |
|     187 | `SIGINT` from Ctrl+C                     |
|     188 | Signal delivery to user processes        |
|     189 | Default signal actions                   |
|     190 | Signal masks                             |
|     191 | Waiting for signal-driven child exits    |
|     192 | Shell job control                        |
|     193 | Foreground and background process groups |
|     194 | Suspending and resuming jobs             |
|     195 | `fg` and `bg` commands                   |

This phase makes the shell and process system feel much more like a real Unix-like environment.

---

### Phase 18 — Pipes and IPC

| Chapter | Topic                            |
| ------: | -------------------------------- |
|     196 | Pipe object design               |
|     197 | `pipe()` syscall                 |
|     198 | Pipe file descriptors            |
|     199 | Blocking pipe reads              |
|     200 | Blocking pipe writes             |
|     201 | Pipe EOF behavior                |
|     202 | Shell pipeline parsing           |
|     203 | Running two-command pipelines    |
|     204 | Multi-stage pipelines            |
|     205 | Combining pipes with redirection |
|     206 | Simple IPC tests                 |

Pipes are a major milestone because they connect process management, file descriptors, blocking I/O, and shell syntax.

---

### Phase 19 — Networking

| Chapter | Topic                            |
| ------: | -------------------------------- |
|     207 | Networking architecture overview |
|     208 | Network device abstraction       |
|     209 | QEMU network setup               |
|     210 | Ethernet frame format            |
|     211 | Sending raw Ethernet frames      |
|     212 | Receiving raw Ethernet frames    |
|     213 | ARP                              |
|     214 | IPv4 packet parsing              |
|     215 | IPv4 packet output               |
|     216 | ICMP echo request and reply      |
|     217 | `ping` utility                   |
|     218 | UDP sockets                      |
|     219 | Minimal socket syscall layer     |
|     220 | DNS client                       |
|     221 | TCP design overview              |
|     222 | TCP connection state             |
|     223 | TCP send and receive path        |
|     224 | Simple TCP client                |
|     225 | Simple TCP server                |
|     226 | Basic network tools              |

Networking is a large subject. The first goal is not to build a production TCP/IP stack, but to teach the layers clearly enough that Toyix can send and receive useful packets.

---

### Phase 20 — Multi-User Support

| Chapter | Topic                  |
| ------: | ---------------------- |
|     227 | User and group IDs     |
|     228 | Process credentials    |
|     229 | File ownership         |
|     230 | Permission checks      |
|     231 | `chmod`                |
|     232 | `chown`                |
|     233 | Login process          |
|     234 | Password file format   |
|     235 | Password hashing       |
|     236 | Sessions               |
|     237 | Multiple terminals     |
|     238 | User home directories  |
|     239 | Per-user shell startup |
|     240 | Superuser model        |

This phase moves Toyix from a single-user teaching kernel toward a basic multi-user OS model.

---

### Phase 21 — Security Boundaries

| Chapter | Topic                                         |
| ------: | --------------------------------------------- |
|     241 | Kernel/user isolation review                  |
|     242 | System call permission checks                 |
|     243 | Secure user pointer handling                  |
|     244 | Executable permission enforcement             |
|     245 | Directory permissions                         |
|     246 | Setuid discussion and cautious implementation |
|     247 | Process isolation hardening                   |
|     248 | File descriptor permission checks             |
|     249 | Device access permissions                     |
|     250 | Network permission policy                     |
|     251 | Secure defaults                               |
|     252 | Auditing dangerous syscalls                   |
|     253 | Basic security testing                        |

Security is not a single feature. It is a property of the whole system. This phase revisits earlier subsystems and tightens the rules.

---

### Phase 22 — System Services and Init

| Chapter | Topic                           |
| ------: | ------------------------------- |
|     254 | `/sbin/init`                    |
|     255 | Init configuration              |
|     256 | Starting login terminals        |
|     257 | Starting background services    |
|     258 | Service supervision             |
|     259 | Shutdown and reboot flow        |
|     260 | Syncing filesystems on shutdown |
|     261 | Basic system logging            |
|     262 | Boot messages in `/var/log`     |
|     263 | Recovery shell                  |

At this point Toyix begins to feel like a small complete system rather than a kernel with demos.

---

### Phase 23 — User Applications

| Chapter | Topic                                 |
| ------: | ------------------------------------- |
|     264 | Building standalone user applications |
|     265 | Installing applications into `/bin`   |
|     266 | `cp`                                  |
|     267 | `mv`                                  |
|     268 | `rm`                                  |
|     269 | `mkdir` and `rmdir` user tools        |
|     270 | `touch`                               |
|     271 | `hexdump`                             |
|     272 | `grep`                                |
|     273 | `more` or `less`                      |
|     274 | Text editor prototype                 |
|     275 | Shell scripts                         |
|     276 | Startup scripts                       |
|     277 | Package/install convention            |

This phase turns kernel mechanisms into user-visible tools.

---

### Phase 24 — Toward a Usable Toyix System

| Chapter | Topic                                   |
| ------: | --------------------------------------- |
|     278 | Booting to login                        |
|     279 | Logging in as a user                    |
|     280 | Running programs from disk              |
|     281 | Editing files                           |
|     282 | Creating directories and managing files |
|     283 | Using pipes and redirection together    |
|     284 | Networking smoke test                   |
|     285 | Multi-user permission test              |
|     286 | Filesystem persistence test             |
|     287 | System shutdown test                    |
|     288 | Final checkpoint: a basic usable OS     |

By the end of this roadmap, Toyix should have the core features of a basic usable operating system:

```text
a protected kernel
user processes
a persistent filesystem
a real I/O subsystem
a terminal/TTY layer
a shell
user applications
networking
multi-user accounts
permissions
basic security boundaries
```

It will still not be Linux. It will not have the hardware support, performance, polish, or decades of refinement that Linux has.

But it will be a real operating system in the educational sense: understandable, modifiable, and complete enough to run useful programs.
