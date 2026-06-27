# Writing a Linux-style Operating System From Scratch

## Preface

Writing an operating system is one of those projects that feels almost mythical from the outside. We use operating systems every day, but most of the time they remain hidden behind terminals, windows, files, drivers, and applications. They feel too large, too complex, and too far removed from ordinary programming to approach directly.

This series is my attempt to make that journey approachable.

In **Writing a Linux-style Operating System From Scratch**, we will build a small but real operating system step by step using C and assembly language. We will use GRUB to get the machine started, but we will not stop at printing a message on the screen. That is where many operating system tutorials end, just as the most interesting work is beginning.

Our goal is to go further.

We will build a kernel, memory management, interrupts, device support, a scheduler, blocking primitives, a file system, a terminal, commands, and the foundations needed for a usable experimental system. The result will not be Linux, and it will not try to compete with Linux. Instead, it will be Linux-style in spirit: modular, understandable, extensible, and designed so that major pieces can be replaced, improved, or redesigned as you learn.

The operating system we build in this series is called **Toyix**. The name is intentional. Toyix is a teaching system, not a production operating system. It is small enough to understand, but serious enough to demonstrate the real ideas behind operating system design. It is meant to be studied, modified, broken, repaired, and extended.

This will not be a weekend project. A complete operating system, even a small one, takes time. The series may span several months, and each article will focus on one meaningful part of the system. Along the way, I will try to explain not only what code to write, but why that code exists, what design choices are being made, and what alternatives are available.

You do not need to be an expert kernel developer to follow along. You should, however, be comfortable with C, willing to read some assembly language, and patient enough to work through low-level details carefully. Operating system development rewards curiosity, persistence, and precision.

By the end of this series, my hope is that you will have more than a bootable demo. You will have a working foundation for your own operating system experiments: a small Linux-style system that you understand from the first instruction executed by the CPU to the commands typed at its terminal.

More importantly, you will have gained the confidence to keep going.


## Index

- [Chapter 1: Booting a Freestanding Kernel](articles/chapter_01.md)
- [Chapter 2: Descriptor Tables and CPU Exceptions](articles/chapter_02.md)
- [Chapter 3: Hardware IRQs, Timer Ticks, and Keyboard Input](articles/chapter_03.md)
- [Chapter 4: Multiboot Memory Maps and Physical Page Allocation](articles/chapter_04.md)
- [Chapter 5: Turning On Paging](articles/chapter_05.md)
- [Chapter 6: Building the First Kernel Heap](articles/chapter_06.md)
- [Chapter 7: A Real Virtual Memory Mapping Layer](articles/chapter_07.md)
- [Chapter 8: Moving the Heap onto Virtual Memory](articles/chapter_08.md)
- [Chapter 9: Cooperative Multitasking and Kernel Threads](articles/chapter_09.md)
- [Chapter 10: Timer-Driven Preemptive Multitasking](articles/chapter_10.md)
- [Chapter 11: Blocking Primitives, Sleep Queues, and Scheduler Hygiene](articles/chapter_11.md)
- [Chapter 12: Wait Queues and Blocking Keyboard Input](articles/chapter_12.md)
- [Chapter 13: Mutexes, Semaphores, and a Console Lock](articles/chapter_13.md)
- [Chapter 14: Terminal Line Discipline and a Kernel Monitor](articles/chapter_14.md)
- [Chapter 15: Command Tables, Argument Parsing, and Shift-Aware Keyboard Input](articles/chapter_15.md)
- [Chapter 16: Entering User Mode and Returning Through Syscalls](articles/chapter_16.md)
- [Chapter 17: Minimal Processes, User Memory Copying, and More Robust Syscalls](articles/chapter_17.md)
- [Chapter 18: File-Descriptor Syscalls and a Tiny User-Mode Console Program](articles/chapter_18.md)
- [Chapter 19: Per-Process Address Spaces and CR3 Switching](articles/chapter_19.md)
- [Chapter 20: A Tiny Executable Format and User Program Loader](articles/chapter_20.md)
- [Chapter 21: Process Teardown and Address-Space Cleanup](articles/chapter_21.md)
- [Chapter 22: First ELF32 Loader Milestone](articles/chapter_22.md)
- [Chapter 23: Building a Real User C Program and Embedding Its ELF](articles/chapter_23.md)
- [Chapter 24: User `argc` / `argv` and a Real Initial Stack](articles/chapter_24.md)
- [Chapter 25: Embedded Program Registry and `run` Monitor Command](articles/chapter_25.md)
- [Chapter 26: Process Table, `ps`, `runbg`, and `wait PID`](articles/chapter_26.md)
- [Chapter 27: A Second User Program for Safe Background Execution](articles/chapter_27.md)
- [Chapter 28: Pattern-Based User Program Build System](articles/chapter_28.md)
- [Chapter 29: First Userland Runtime](articles/chapter_29.md)
- [Chapter 30: First User-Mode Shell](articles/chapter_30.md)
- [Chapter 31: `SYS_EXEC`, `SYS_WAITPID`, and Shell-Launched Programs](articles/chapter_31.md)
- [Chapter 32: Process Ownership, Waiting, and Job State](articles/chapter_32.md)
