# Changelog

All notable changes to Toyix will be documented in this file.

## [Unreleased]

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
