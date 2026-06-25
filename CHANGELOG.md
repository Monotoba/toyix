# Changelog

All notable changes to Toyix will be documented in this file.

## [Unreleased]

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
