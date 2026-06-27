# Changelog

All notable changes to Toyix will be documented in this file.

## [Unreleased]

## [Chapter_33] - 2026-06-27

- Add `kill_requested` process state, cooperative child-kill helpers, and `PROCESS_KILLED_EXIT_CODE` for parent-owned process termination.
- Add `SYS_GETPID`, `SYS_GETPPID`, `SYS_PROCINFO`, and `SYS_KILL` to the kernel and user syscall ABI.
- Extend the shell with `runbg`, `jobs`, `wait`, and `kill` so background child processes can be inspected, terminated, and reaped from user mode.
- Update the Chapter 33 boot smoke flow, README, series index, and roadmap for the first cooperative user-process termination milestone.

## [Chapter_32] - 2026-06-27

- Add per-process parent PID tracking and helper APIs for parent ownership checks.
- Change exited child processes to a zombie state until explicit collection and include PPID data in process listing and destroy logs.
- Restrict `SYS_WAITPID` so user processes can wait only on their own children while keeping kernel-side wait/destroy flows available to monitor code and tests.
- Update Chapter 32, the README, the series index, and the roadmap for the first owned zombie child lifecycle milestone.

## [Chapter_31] - 2026-06-27

- Add `SYS_EXEC` and `SYS_WAITPID` to the kernel and user syscall ABI.
- Add kernel-side user-string copying, embedded-program launch, and wait/cleanup handling for shell-started child processes.
- Add the shell `run PROGRAM [ARGS...]` built-in and extend the boot smoke flow to verify shell-launched `counter` execution and exit-status reporting.
- Update Chapter 31, the README, the series index, and the roadmap for the first userland process-control milestone.

## [Chapter_30] - 2026-06-27

- Extend the shared userland runtime with formatted output, hex printing, numeric parsing, string comparison, whitespace checks, and line-oriented input helpers.
- Add `user/shell.c` as the first interactive ring-3 user-mode shell with `help`, `echo`, `args`, and `exit`.
- Expand the embedded program registry and boot-time smoke coverage to build, embed, launch, script, and clean up the shell alongside `demo` and `counter`.
- Update Chapter 30, the README, the series index, and the roadmap for the first user-mode shell milestone.

## [Chapter_29] - 2026-06-26

- Add a first shared userland runtime with `user/include/toyix.h` and `user/lib/toyix.c`.
- Move common string and integer output helpers out of `demo.c` and `counter.c` into the shared runtime layer.
- Update the user-program build rules and test assertions so the shared runtime object is compiled, linked into every user ELF, and verified by the smoke suite.
- Update Chapter 29, the README, the series index, and the roadmap for the first userland runtime milestone.

## [Chapter_28] - 2026-06-26

- Replace explicit per-program user build rules with `USER_PROGRAMS`, `USER_ELFS`, and `USER_BLOBS` pattern-driven Makefile targets.
- Add `user-programs`, `user-blobs`, `list-user-programs`, and `readelf-user` helper targets for inspecting the embedded userland build.
- Clean up the embedded program registry declarations in `kernel/program.c` with reusable declaration and table macros.
- Keep the Chapter 27 runtime path intact while updating smoke expectations and chapter text for the scalable user-program build milestone.

## [Chapter_27] - 2026-06-26

- Add a second embedded user program, `counter`, that prints its arguments, sleeps between ticks, and exits with a predictable status code.
- Expand the userland build pipeline to compile, link, and embed both `demo.elf` and `counter.elf` into the kernel image.
- Replace the Chapter 26 background-launch smoke path with a dedicated `counter` test that verifies PID lookup, process listing, wait, exit status, and cleanup without keyboard input contention.
- Update Chapter 27, the README, the series index, the roadmap, and smoke suite expectations for the multi-program embedded userland milestone.

## [Chapter_26] - 2026-06-26

- Add a global linked-list process table with PID lookup, state names, and monitor-friendly process listing.
- Add `program_run_background()` plus `ps`, `runbg PROGRAM [ARGS...]`, and `wait PID` monitor commands.
- Keep exited processes visible until explicit collection so the monitor can wait on and destroy them by PID.
- Update boot smoke expectations for background launch, process table visibility, and PID-based wait/cleanup.
- Update Chapter 26, the README, the series index, and the roadmap for the new process-management layer.

## [Chapter_25] - 2026-06-26

- Add an embedded program registry with `embedded_program_t`, named lookup, and a foreground launch helper.
- Add `programs` and `run PROGRAM [ARGS...]` monitor commands for listing and launching embedded user programs.
- Move the Chapter 24 compiled-user launch test out of `process.c` and into a dedicated program-layer smoke path.
- Update boot smoke expectations for registry initialization, monitor command coverage, and foreground `demo` launch output.
- Update Chapter 25, the README, the series index, and the roadmap for the new program-launch layer.

## [Chapter_24] - 2026-06-26

- Add `process->user_initial_esp` and a `process_setup_arguments()` helper for building user stacks in the target address space.
- Add `elf_create_process_suspended()` so callers can install argv data before starting a user process.
- Update user-mode entry and `crt0` so compiled user programs start as `main(int argc, char **argv)`.
- Update the embedded demo program and smoke expectations to verify `argc=3`, `argv[0]=demo`, `argv[1]=alpha`, and `argv[2]=beta`.
- Update Chapter 24, the README, the series index, and the roadmap for the current process startup ABI.

## [Chapter_23] - 2026-06-26

- Add a `user/` build pipeline with syscall headers, startup assembly, a C demo program, and a user linker script.
- Build a real user ELF with `i686-elf-gcc` and embed it into the kernel with `objcopy`.
- Replace the last in-kernel ELF byte emitter with linker symbols for the compiled demo image.
- Update process smoke expectations for the compiled `compiled-demo` user program path.
- Update Chapter 23, the README, the series index, the roadmap, and smoke suite expectations.

## [Chapter_22] - 2026-06-26

- Add freestanding ELF32 header definitions and an initial ELF32 process loader.
- Replace the Chapter 21 TOYEXE smoke demo with an ELF32 `PT_LOAD` demo image.
- Remove `kernel/toyexe.o` from the active build while keeping the old TOYEXE files as reference.
- Update process smoke expectations for ELF32 load, entry, exit, and cleanup output.
- Update Chapter 22, the README, the series index, the roadmap, and smoke suite expectations.

## [Chapter_21] - 2026-06-26

- Add owned-user-page tracking to process address spaces.
- Add `address_space_destroy()` to free user pages, user page tables, and process page directories.
- Add `process_wait()` and `process_destroy()` for a complete process lifecycle.
- Replace the Chapter 20 TOYEXE smoke path with a lifecycle cleanup test.
- Update Chapter 21, the README, the series index, the roadmap, and smoke suite expectations.

## [Chapter_20] - 2026-06-26

- Add a tiny TOYEXE executable format and loader for user programs.
- Split process creation into empty-process, mapping, copy, stack, and start helpers.
- Make user-mode entry use the process loader-provided entry point.
- Replace the Chapter 19 byte-array user demo with a TOYEXE-backed user program test.
- Update Chapter 20, the README, the series index, and smoke suite expectations.

## [Chapter_19] - 2026-06-26

- Add per-process address spaces with private user mappings and shared kernel mappings.
- Add scheduler CR3 switching based on the next thread's owning process.
- Add suspended thread creation so process metadata is attached before user threads run.
- Update checked user-memory copying to validate against the current address space.
- Update Chapter 19, the README, the series index, and smoke suite expectations.

## [Chapter_18] - 2026-06-26

- Add byte-counted console output helpers for fd-style writes.
- Add `FD_STDIN`, `FD_STDOUT`, `FD_STDERR`, and `SYS_READ`.
- Update `SYS_WRITE` to use fd, user buffer, and byte count registers.
- Replace the user process demo with a prompt/read/echo/sleep/exit fd syscall test.
- Update Chapter 18, the README, the series index, and smoke suite expectations.

## [Chapter_17] - 2026-06-26

- Add a minimal `process_t` abstraction and attach user threads to processes.
- Add checked user-memory copying helpers and user-page flag queries.
- Add `SYS_WRITE` and `SYS_SLEEP` alongside process-aware `SYS_EXIT`.
- Replace the raw user-mode boot test with a process syscall/write/sleep/exit smoke test.
- Update Chapter 17, the README, the series index, and smoke suite expectations.

## [Chapter_16] - 2026-06-26

- Add user and TSS GDT entries plus TSS kernel-stack refresh during scheduling.
- Add an `int 0x80` syscall gate and syscall entry stub for ring-3 callers.
- Add a user-mode test program that prints `U3` and exits through syscalls.
- Add user-mode page setup, syscall handling, and boot smoke checks.
- Update Chapter 16, the README, the series index, and smoke suite expectations.

## [Chapter_15] - 2026-06-25

- Refactor the kernel monitor around a command table with usage strings and per-command help.
- Add `argc`/`argv` tokenization and bounded command-line copying.
- Add Shift, Caps Lock, and shifted punctuation handling to keyboard input.
- Add kernel string helpers for bounded copying, character classes, and command parsing.
- Update Chapter 15, the README, the series index, and smoke suite expectations.

## [Chapter_14] - 2026-06-25

- Add terminal readline support with echo, newline, fixed-size buffers, and backspace handling.
- Add VGA text backspace support for terminal editing.
- Add an interactive kernel monitor with help, ticks, threads, mem, heap, sleep, echo, and clear commands.
- Add terminal and monitor boot-time sanity tests.
- Update Chapter 14, the README, the series index, and smoke suite expectations.

## [Chapter_13] - 2026-06-25

- Add blocking mutexes and counting semaphores on top of wait queues.
- Add synchronized console output with raw console helpers for already-locked paths.
- Add mutex, semaphore, and console-lock boot-time sanity tests.
- Update Chapter 13, the README, the series index, and the smoke suite expectations.

## [Chapter_12] - 2026-06-24

- Add wait queues with interrupt-safe blocking and wakeup helpers.
- Add a blocking keyboard input buffer with synthetic input testing.
- Add `thread_block_current()` and `thread_wake()` for wait-queue handoff.
- Update Chapter 12, the README, the roadmap, the series index, and the Makefile smoke expectations.

## [Chapter_11] - 2026-06-24

- Add interrupt-safe blocking primitives with `irq_save()` and `irq_restore()`.
- Add an idle thread, sleep queue, zombie queue, and zombie reaping.
- Add `thread_sleep_ticks()` and a blocking sleep smoke test.
- Update Chapter 11, the README, the roadmap, the series index, and the Makefile smoke expectations.

## [Chapter_10] - 2026-06-24

- Add timer-driven preemptive multitasking with a dedicated scheduling interrupt.
- Add interrupt-frame-based thread switching and timer-triggered rescheduling.
- Update Chapter 10, the README, the series index, and the Makefile smoke expectations.

## [Chapter_09] - 2026-06-24

- Add cooperative kernel threads with per-thread stacks and a software context switch.
- Add a round-robin `thread_yield()` path and a two-thread smoke test.
- Update Chapter 9, the README, the series index, the roadmap, and the Makefile test expectations.

## [Chapter_08] - 2026-06-24

- Move the heap onto a VMM-backed virtual address range.
- Add the generic `kernel/vmem` wrapper over the x86 VMM.
- Add heap growth through virtual page mapping and large-allocation smoke coverage.
- Update Chapter 8, the README, the changelog, the series index, and the Makefile test expectations.

## [Chapter_07] - 2026-06-23

- Add a virtual memory manager with page mapping, unmapping, and translation helpers.
- Add on-demand page-table setup and TLB invalidation helpers for the paging layer.
- Move the Chapter 7 kernel boot path to initialize and smoke-test VMM before the heap.
- Update the Chapter 7 article, README, roadmap, and series index for the current code and test flow.

## [Chapter_06] - 2026-06-22

- Move public kernel headers under `include/kernel`.
- Add freestanding string/memory declarations for shared kernel code.
- Add early kernel heap support with `kmalloc`, `kcalloc`, and `kfree`.
- Add heap block splitting, coalescing, statistics, and boot-time sanity checks.
- Add PMM allocation helpers for pages below the identity-mapped limit.
- Add heap coverage to the QEMU smoke test.
- Add build-flag tracking so test-only `CFLAGS_EXTRA` variants do not poison later normal builds.
- Add Chapter 6 documentation and resource links.

## [Chapter_05] - 2026-06-22

- Add initial 32-bit x86 paging support with identity mapping for the first 16 MiB.
- Add CR0, CR2, and CR3 assembly helpers for paging setup and diagnostics.
- Add page-fault handling through the registered interrupt-handler dispatch path.
- Add paging boot checks and a deliberate page-fault test target.
- Update Chapter 5 documentation for the current paging implementation and test commands.

## [Chapter_04] - 2026-06-21

- Add Multiboot memory map parsing.
- Add a bitmap-backed physical page allocator for 4 KiB frames.
- Add decimal console output for memory statistics.
- Add linker symbols for reserving the kernel image in the PMM.
- Add build flag tracking so exception-test builds do not poison later normal runs.
- Add Chapter 4 documentation and resource links.

## [Chapter_03] - 2026-06-21

- Add remapped PIC hardware IRQ handling.
- Add PIT timer ticks and an interruptible kernel idle loop.
- Add an early PS/2 keyboard scancode echo driver.
- Add Chapter 3 documentation and resource links.

## [Chapter_02] - 2026-06-20

- Add Chapter 2 GDT, IDT, CPU exception handling, and panic path.
- Add boot and exception smoke coverage for the Chapter 2 kernel.

## [Chapter_01] - 2026-06-20

- Add project documentation, license, CI workflows, and repository metadata.

## [0.1.0] - 2026-06-19

- Boot a minimal Multiboot-compatible i686 kernel.
- Add serial and VGA text console drivers.
- Add a QEMU smoke test that validates boot output.
