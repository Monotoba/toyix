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

This roadmap is not a rigid promise that every chapter title will remain exactly the same. As the operating system grows, some topics may split into multiple chapters and others may merge. But this gives us a realistic map from a tiny bootable kernel to a basic usable OS.

---

### Phase 1 — Bootstrapping the Kernel

| Chapter | Topic                                                       |
| ------: | ----------------------------------------------------------- |
|       1 | Introduction, goals, project layout, and toolchain overview |
|       2 | Building the cross-compiler and development environment     |
|       3 | Creating the first bootable kernel image                    |
|       4 | Multiboot, GRUB, linker script, and kernel entry            |
|       5 | Serial output and early debugging                           |
|       6 | VGA text console                                            |
|       7 | Kernel panic handling and early diagnostics                 |

---

### Phase 2 — CPU Setup, Interrupts, and Timers

| Chapter | Topic                              |
| ------: | ---------------------------------- |
|       8 | Global Descriptor Table            |
|       9 | Interrupt Descriptor Table         |
|      10 | CPU exceptions and fault reporting |
|      11 | Programmable Interrupt Controller  |
|      12 | Timer interrupts with the PIT      |
|      13 | Keyboard interrupts                |
|      14 | Basic input buffering              |
|      15 | Early kernel monitor               |

---

### Phase 3 — Physical and Virtual Memory

| Chapter | Topic                              |
| ------: | ---------------------------------- |
|      16 | Reading the Multiboot memory map   |
|      17 | Physical page allocator            |
|      18 | First identity-mapped paging setup |
|      19 | Page tables and page directories   |
|      20 | Page fault handling                |
|      21 | Kernel virtual memory mapping      |
|      22 | Early kernel heap                  |
|      23 | VMM-backed kernel heap             |
|      24 | Mapping and unmapping pages        |
|      25 | Kernel memory debugging helpers    |

---

### Phase 4 — Threads and Scheduling

| Chapter | Topic                           |
| ------: | ------------------------------- |
|      26 | Cooperative kernel threads      |
|      27 | Context switching               |
|      28 | Preemptive scheduling           |
|      29 | Timer-driven task switching     |
|      30 | Idle thread                     |
|      31 | Sleep queues                    |
|      32 | Zombie thread cleanup           |
|      33 | Wait queues                     |
|      34 | Mutexes                         |
|      35 | Semaphores                      |
|      36 | Scheduler hygiene and debugging |

---

### Phase 5 — Terminal Input and Kernel Services

| Chapter | Topic                                  |
| ------: | -------------------------------------- |
|      37 | Keyboard scancode decoding             |
|      38 | Shift-aware keyboard input             |
|      39 | Terminal line discipline               |
|      40 | Blocking terminal reads                |
|      41 | Console locking                        |
|      42 | Kernel command table                   |
|      43 | Argument parsing in the kernel monitor |

---

### Phase 6 — User Mode and System Calls

| Chapter | Topic                                                         |
| ------: | ------------------------------------------------------------- |
|      44 | Entering user mode                                            |
|      45 | First `int 0x80` syscall                                      |
|      46 | User pointer validation                                       |
|      47 | `SYS_WRITE`, `SYS_EXIT`, and `SYS_SLEEP`                      |
|      48 | Process structure                                             |
|      49 | User stacks                                                   |
|      50 | Per-process address spaces                                    |
|      51 | Returning from syscalls safely                                |
|      52 | Killing faulty user processes instead of panicking the kernel |

---

### Phase 7 — Programs and ELF Loading

| Chapter | Topic                        |
| ------: | ---------------------------- |
|      53 | First tiny executable format |
|      54 | Loading a toy executable     |
|      55 | ELF32 loader introduction    |
|      56 | Loading ELF program headers  |
|      57 | Building user C programs     |
|      58 | User startup code and `crt0` |
|      59 | Passing `argc` and `argv`    |
|      60 | Embedded program registry    |
|      61 | Running named user programs  |
|      62 | Userland library foundation  |
|      63 | Minimal `printf`             |
|      64 | Process exit status          |

---

### Phase 8 — Process Management

| Chapter | Topic                                  |
| ------: | -------------------------------------- |
|      65 | Process table                          |
|      66 | `ps` support                           |
|      67 | Parent and child process relationships |
|      68 | `waitpid`                              |
|      69 | Zombies and reaping                    |
|      70 | Background processes                   |
|      71 | Process IDs and parent process IDs     |
|      72 | Nonblocking waits                      |
|      73 | Process info syscall                   |
|      74 | Cooperative kill                       |
|      75 | Timer-interrupt kill checks            |
|      76 | CPU-bound process termination testing  |

---

### Phase 9 — VFS and RAMFS

| Chapter | Topic                            |
| ------: | -------------------------------- |
|      77 | VFS design                       |
|      78 | First read-only RAMFS            |
|      79 | `open`, `read`, and `close`      |
|      80 | `cat` command                    |
|      81 | `seek` support                   |
|      82 | `stat` support                   |
|      83 | Directories and `readdir`        |
|      84 | `ls` command                     |
|      85 | `/programs` directory            |
|      86 | Executable metadata              |
|      87 | Path-based program launching     |
|      88 | Current working directory        |
|      89 | Relative path resolution         |
|      90 | Shell `PATH` search              |
|      91 | Writable RAMFS files             |
|      92 | `create` and file-backed `write` |

---

### Phase 10 — First User Shell

| Chapter | Topic                                         |   |   |
| ------: | --------------------------------------------- | - | - |
|      93 | First user-mode shell                         |   |   |
|      94 | Shell command dispatch                        |   |   |
|      95 | Shell-launched programs                       |   |   |
|      96 | Foreground and background jobs                |   |   |
|      97 | Job references like `%1`                      |   |   |
|      98 | Shell `PATH` management                       |   |   |
|      99 | Shell variables                               |   |   |
|     100 | Variable expansion                            |   |   |
|     101 | Configurable prompt                           |   |   |
|     102 | Last command status with `$?`                 |   |   |
|     103 | Command sequencing with `;`                   |   |   |
|     104 | Conditionals with `&&` and `                  |   | ` |
|     105 | Shell history                                 |   |   |
|     106 | History recall with `!!`, `!N`, and `!prefix` |   |   |

---

### Phase 11 — Shell I/O Redirection

| Chapter | Topic                                   |
| ------: | --------------------------------------- |
|     107 | Output redirection with `>`             |
|     108 | Append redirection with `>>`            |
|     109 | Process standard descriptor inheritance |
|     110 | Child process stdout redirection        |
|     111 | Stderr redirection with `2>` and `2>>`  |
|     112 | Descriptor merging with `2>&1`          |
|     113 | Input redirection with `<`              |
|     114 | Shell checkpoint and pivot to storage   |

At this point, the shell is useful enough for testing deeper OS features. We will deliberately defer more advanced shell features such as pipes, quoting, scripting, job control, and signal-aware terminal control until the kernel has stronger I/O and process primitives.

---

### Phase 12 — Block Devices and Storage Foundation

| Chapter | Topic                                       |
| ------: | ------------------------------------------- |
|     115 | Block device abstraction                    |
|     116 | RAM disk block device                       |
|     117 | Block device registry and discovery         |
|     118 | Block read/write tests                      |
|     119 | Buffer cache design                         |
|     120 | Buffer cache implementation                 |
|     121 | Dirty buffers and flushing                  |
|     122 | Block cache debugging tools                 |
|     123 | Preparing the storage layer for filesystems |

---

### Phase 13 — ToyFS: A Real Filesystem

| Chapter | Topic                                 |
| ------: | ------------------------------------- |
|     124 | ToyFS design goals                    |
|     125 | ToyFS on-disk layout                  |
|     126 | Superblock                            |
|     127 | Block bitmap                          |
|     128 | Inode bitmap                          |
|     129 | Inode table                           |
|     130 | Directory entry format                |
|     131 | Formatting a ToyFS image              |
|     132 | Mounting ToyFS                        |
|     133 | Reading the ToyFS root directory      |
|     134 | Opening files from ToyFS              |
|     135 | Reading file data blocks              |
|     136 | Creating files                        |
|     137 | Writing file data blocks              |
|     138 | File growth with direct blocks        |
|     139 | Truncating files                      |
|     140 | `unlink`                              |
|     141 | `mkdir`                               |
|     142 | `rmdir`                               |
|     143 | `rename`                              |
|     144 | Filesystem consistency checks         |
|     145 | Mounting ToyFS as the root filesystem |

This is the point where Toyix gains a real filesystem. The earlier RAMFS was useful, but ToyFS will be an actual block-backed filesystem with persistent structure.

---

### Phase 14 — Loading Applications from the Filesystem

| Chapter | Topic                                   |
| ------: | --------------------------------------- |
|     146 | Storing ELF files in ToyFS              |
|     147 | Loading ELF from VFS paths              |
|     148 | Replacing embedded program execution    |
|     149 | `/bin` and `/sbin` directories          |
|     150 | Installing basic user programs          |
|     151 | Shell execution from `/bin`             |
|     152 | Program permissions and executable bits |
|     153 | Init program loaded from filesystem     |
|     154 | Booting into userland from `/sbin/init` |

This phase moves Toyix closer to a normal OS boot flow:

```text
kernel boots
mount root filesystem
launch /sbin/init
init starts shell or services
```

---

### Phase 15 — User Memory and Runtime Support

| Chapter | Topic                           |
| ------: | ------------------------------- |
|     155 | User heap region                |
|     156 | `brk` and `sbrk` syscalls       |
|     157 | Userland `malloc`               |
|     158 | Userland `free`                 |
|     159 | `calloc` and `realloc`          |
|     160 | Guard pages                     |
|     161 | User stack growth checks        |
|     162 | Read-only text pages            |
|     163 | Writable data pages             |
|     164 | Better user page fault handling |
|     165 | Demand allocation               |
|     166 | Copy-on-write introduction      |

This phase lets user applications become more realistic. Without a heap, programs remain tiny and artificial.

---

### Phase 16 — I/O Subsystem and Devices

| Chapter | Topic                                    |
| ------: | ---------------------------------------- |
|     167 | Device model overview                    |
|     168 | Character device abstraction             |
|     169 | Block device abstraction refinements     |
|     170 | `/dev` filesystem                        |
|     171 | `/dev/console`                           |
|     172 | `/dev/null`                              |
|     173 | `/dev/zero`                              |
|     174 | Keyboard device node                     |
|     175 | Terminal device node                     |
|     176 | Device major/minor numbers or equivalent |
|     177 | Driver registration                      |
|     178 | Polling and blocking device reads        |
|     179 | Device permissions                       |

This gives Toyix a more Unix-like I/O model where devices appear as files.

---

### Phase 17 — TTY and Full Terminal Support

| Chapter | Topic                             |
| ------: | --------------------------------- |
|     180 | TTY abstraction                   |
|     181 | Canonical input mode              |
|     182 | Raw input mode                    |
|     183 | Echo control                      |
|     184 | Backspace and line editing        |
|     185 | Arrow-key escape sequence parsing |
|     186 | Shell command-line editing        |
|     187 | Scrollback                        |
|     188 | Terminal resize model             |
|     189 | Ctrl+C and interrupt characters   |
|     190 | Ctrl+D and EOF                    |
|     191 | Ctrl+Z groundwork                 |
|     192 | Foreground process group          |
|     193 | Job-control terminal rules        |

This phase turns the current simple console into a much more complete terminal subsystem.

---

### Phase 18 — Signals and Job Control

| Chapter | Topic                                    |
| ------: | ---------------------------------------- |
|     194 | Signal model overview                    |
|     195 | `SIGKILL`                                |
|     196 | `SIGTERM`                                |
|     197 | `SIGCHLD`                                |
|     198 | `SIGSEGV`                                |
|     199 | `SIGINT` from Ctrl+C                     |
|     200 | Signal delivery to user processes        |
|     201 | Default signal actions                   |
|     202 | Signal masks                             |
|     203 | Waiting for signal-driven child exits    |
|     204 | Shell job control                        |
|     205 | Foreground and background process groups |
|     206 | Suspending and resuming jobs             |
|     207 | `fg` and `bg` commands                   |

This phase makes the shell and process system feel much more like a real Unix-like environment.

---

### Phase 19 — Pipes and IPC

| Chapter | Topic                            |
| ------: | -------------------------------- |
|     208 | Pipe object design               |
|     209 | `pipe()` syscall                 |
|     210 | Pipe file descriptors            |
|     211 | Blocking pipe reads              |
|     212 | Blocking pipe writes             |
|     213 | Pipe EOF behavior                |
|     214 | Shell pipeline parsing           |
|     215 | Running two-command pipelines    |
|     216 | Multi-stage pipelines            |
|     217 | Combining pipes with redirection |
|     218 | Simple IPC tests                 |

Pipes are a major milestone because they connect process management, file descriptors, blocking I/O, and shell syntax.

---

### Phase 20 — Networking

| Chapter | Topic                            |
| ------: | -------------------------------- |
|     219 | Networking architecture overview |
|     220 | Network device abstraction       |
|     221 | QEMU network setup               |
|     222 | Ethernet frame format            |
|     223 | Sending raw Ethernet frames      |
|     224 | Receiving raw Ethernet frames    |
|     225 | ARP                              |
|     226 | IPv4 packet parsing              |
|     227 | IPv4 packet output               |
|     228 | ICMP echo request and reply      |
|     229 | `ping` utility                   |
|     230 | UDP sockets                      |
|     231 | Minimal socket syscall layer     |
|     232 | DNS client                       |
|     233 | TCP design overview              |
|     234 | TCP connection state             |
|     235 | TCP send and receive path        |
|     236 | Simple TCP client                |
|     237 | Simple TCP server                |
|     238 | Basic network tools              |

Networking is a large subject. The first goal is not to build a production TCP/IP stack, but to teach the layers clearly enough that Toyix can send and receive useful packets.

---

### Phase 21 — Multi-User Support

| Chapter | Topic                  |
| ------: | ---------------------- |
|     239 | User and group IDs     |
|     240 | Process credentials    |
|     241 | File ownership         |
|     242 | Permission checks      |
|     243 | `chmod`                |
|     244 | `chown`                |
|     245 | Login process          |
|     246 | Password file format   |
|     247 | Password hashing       |
|     248 | Sessions               |
|     249 | Multiple terminals     |
|     250 | User home directories  |
|     251 | Per-user shell startup |
|     252 | Superuser model        |

This phase moves Toyix from a single-user teaching kernel toward a basic multi-user OS model.

---

### Phase 22 — Security Boundaries

| Chapter | Topic                                         |
| ------: | --------------------------------------------- |
|     253 | Kernel/user isolation review                  |
|     254 | System call permission checks                 |
|     255 | Secure user pointer handling                  |
|     256 | Executable permission enforcement             |
|     257 | Directory permissions                         |
|     258 | Setuid discussion and cautious implementation |
|     259 | Process isolation hardening                   |
|     260 | File descriptor permission checks             |
|     261 | Device access permissions                     |
|     262 | Network permission policy                     |
|     263 | Secure defaults                               |
|     264 | Auditing dangerous syscalls                   |
|     265 | Basic security testing                        |

Security is not a single feature. It is a property of the whole system. This phase revisits earlier subsystems and tightens the rules.

---

### Phase 23 — System Services and Init

| Chapter | Topic                           |
| ------: | ------------------------------- |
|     266 | `/sbin/init`                    |
|     267 | Init configuration              |
|     268 | Starting login terminals        |
|     269 | Starting background services    |
|     270 | Service supervision             |
|     271 | Shutdown and reboot flow        |
|     272 | Syncing filesystems on shutdown |
|     273 | Basic system logging            |
|     274 | Boot messages in `/var/log`     |
|     275 | Recovery shell                  |

At this point Toyix begins to feel like a small complete system rather than a kernel with demos.

---

### Phase 24 — User Applications

| Chapter | Topic                                 |
| ------: | ------------------------------------- |
|     276 | Building standalone user applications |
|     277 | Installing applications into `/bin`   |
|     278 | `cp`                                  |
|     279 | `mv`                                  |
|     280 | `rm`                                  |
|     281 | `mkdir` and `rmdir` user tools        |
|     282 | `touch`                               |
|     283 | `hexdump`                             |
|     284 | `grep`                                |
|     285 | `more` or `less`                      |
|     286 | Text editor prototype                 |
|     287 | Shell scripts                         |
|     288 | Startup scripts                       |
|     289 | Package/install convention            |

This phase turns kernel mechanisms into user-visible tools.

---

### Phase 25 — Toward a Usable Toyix System

| Chapter | Topic                                   |
| ------: | --------------------------------------- |
|     290 | Booting to login                        |
|     291 | Logging in as a user                    |
|     292 | Running programs from disk              |
|     293 | Editing files                           |
|     294 | Creating directories and managing files |
|     295 | Using pipes and redirection together    |
|     296 | Networking smoke test                   |
|     297 | Multi-user permission test              |
|     298 | Filesystem persistence test             |
|     299 | System shutdown test                    |
|     300 | Final checkpoint: a basic usable OS     |

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

