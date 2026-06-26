# Chapter 23 — Building a Real User C Program and Embedding Its ELF

In Chapter 22, the kernel gained a first ELF32 loader.

That was a big step, but our test ELF was still built by hand inside `process.c`:

```text
C byte emitter
  ↓
fake ELF image in memory
  ↓
ELF loader
  ↓
user process
```

This chapter removes that artificial piece.

We will add a tiny userland build pipeline:

```text
user/demo.c
  ↓
i686-elf-gcc
  ↓
user/demo.elf
  ↓
objcopy embeds demo.elf as kernel object
  ↓
kernel ELF loader loads embedded user ELF
```

GNU `objcopy` is specifically designed to copy and transform object files between formats; we will use its binary-input mode to embed the compiled user ELF into the kernel image as ordinary linker symbols. ([Sourceware][1]) GCC’s compilation pipeline separates preprocessing, compilation, assembly, and linking, which is exactly what we need for adding a user-program build path beside the kernel build. ([gcc.gnu.org][2]) The user program linker script will use `ENTRY()` and `SECTIONS`; GNU `ld` documents `ENTRY` as the way to define the executable entry symbol, and `SECTIONS` as the command that maps input sections into output sections. ([gcc.gnu.org][3])

The milestone output becomes:

```text
Process test: starting compiled ELF32 user program test
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
Process: created pid=1 name=compiled-demo
user> toyix
echo: toyix
Syscall: process compiled-demo pid=1 exited code 9
Process test: compiled ELF32 user program cleanup sanity check passed
```

---

# 1. What this chapter adds

Add:

```text
user/
├── include/
│   └── toyix_syscall.h
├── crt0.S
├── demo.c
└── linker.ld
```

Modify:

```text
kernel/process.c
Makefile
tests/smoke.sh
```

Remove the last in-kernel user-program emitter from `process.c`.

After this chapter, user code is real compiled C.

---

# 2. New project tree

The project now looks like this:

```text
toyix/
├── arch/
├── drivers/
├── include/
├── kernel/
├── tests/
└── user/
    ├── include/
    │   └── toyix_syscall.h
    ├── crt0.S
    ├── demo.c
    └── linker.ld
```

`user/` is not kernel code.

It is the beginning of userland.

---

# 3. Add `user/include/toyix_syscall.h`

```c
// user/include/toyix_syscall.h
#ifndef TOYIX_USER_SYSCALL_H
#define TOYIX_USER_SYSCALL_H

typedef unsigned int toyix_u32;
typedef int toyix_i32;

#define FD_STDIN  0u
#define FD_STDOUT 1u
#define FD_STDERR 2u

#define SYS_PUTC  1u
#define SYS_EXIT  2u
#define SYS_WRITE 3u
#define SYS_SLEEP 4u
#define SYS_READ  5u

static inline toyix_i32 toyix_read(
    toyix_u32 fd,
    void *buffer,
    toyix_u32 length
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_READ),
          "b"(fd),
          "c"(buffer),
          "d"(length)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_write(
    toyix_u32 fd,
    const void *buffer,
    toyix_u32 length
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_WRITE),
          "b"(fd),
          "c"(buffer),
          "d"(length)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_sleep(toyix_u32 ticks) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_SLEEP),
          "b"(ticks)
        : "memory"
    );

    return result;
}

static inline void toyix_exit(toyix_u32 code) {
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(SYS_EXIT),
          "b"(code)
        : "memory"
    );

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

#endif
```

## Why syscall constants are duplicated here

The kernel has:

```c
include/kernel/syscall.h
```

Userland now has:

```c
user/include/toyix_syscall.h
```

That duplication is not ideal long-term.

Eventually we should create a shared ABI header:

```text
include/abi/syscall.h
```

and have both kernel and user code include it.

For now, duplication keeps the patch small and avoids changing the kernel include layout.

---

# 4. Add `user/crt0.S`

```asm
// user/crt0.S
//
// Tiny user-mode C runtime entry.
//
// The kernel enters this ELF at _start.
// _start calls main().
// main's return value becomes the process exit code.

.global _start
.extern main

.section .text.start, "ax"

_start:
    call main

    mov %eax, %ebx      // exit code
    mov $2, %eax        // SYS_EXIT
    int $0x80

.hang:
    jmp .hang
```

## Why we need `crt0.S`

C programs do not normally start directly in `main()`.

Hosted systems have a runtime startup object that:

```text
sets up argc/argv/envp
initializes runtime state
calls main()
calls exit(main_return)
```

Our kernel has none of that yet.

So our `crt0.S` does the smallest possible startup:

```text
_start → main → SYS_EXIT
```

---

# 5. Add `user/demo.c`

```c
// user/demo.c
#include "toyix_syscall.h"

static toyix_u32 str_len(const char *text) {
    toyix_u32 len = 0;

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

static void write_str(const char *text) {
    toyix_write(FD_STDOUT, text, str_len(text));
}

int main(void) {
    char buffer[32];

    write_str("user> ");

    toyix_i32 got = toyix_read(FD_STDIN, buffer, sizeof(buffer));

    if (got < 0) {
        write_str("read failed\n");
        return 1;
    }

    write_str("echo: ");

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    write_str("\n");

    toyix_sleep(3);

    return 9;
}
```

This is our first real user C program.

It uses only syscalls.

No libc.

No stack protector.

No global constructors.

No dynamic linker.

---

# 6. Add `user/linker.ld`

```ld
/* user/linker.ld */

ENTRY(_start)

SECTIONS
{
    . = 0x40100000;

    .text ALIGN(4K) :
    {
        *(.text.start)
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }
}
```

## Why link at `0x40100000`

Our ELF loader loads `ET_EXEC` images at their linked virtual addresses.

So the user program must be linked for the address where it will run.

That is currently:

```text
0x40100000
```

Later, when we support position-independent executables or relocation, this can become more flexible.

---

# 7. Modify `kernel/process.c`

Remove the big in-kernel ELF demo builder from Chapter 22.

Delete these now-obsolete pieces:

```text
DEMO_SEGMENT_VA
DEMO_IMAGE_SIZE
DEMO_BSS_SIZE
DEMO_PROMPT_VA
DEMO_PREFIX_VA
DEMO_NEWLINE_VA
DEMO_INPUT_VA
DEMO_INPUT_MAX

emit_u8()
emit_u32()
emit_mov_eax_imm32()
emit_mov_ebx_imm32()
emit_mov_ecx_imm32()
emit_mov_edx_imm32()
emit_int80()

build_stdio_demo_payload()
build_stdio_demo_elf()
```

Keep:

```c
#include "kernel/elf_loader.h"
```

Add these external symbols near the bottom of the file, before `process_test_once()`:

```c
extern const uint8_t user_demo_elf_start[];
extern const uint8_t user_demo_elf_end[];
```

Then replace `process_test_once()` with this version:

```c
void process_test_once(void) {
    console_writeln("Process test: starting compiled ELF32 user program test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    const uint8_t *elf_image = user_demo_elf_start;

    uint32_t elf_size =
        (uint32_t)(user_demo_elf_end - user_demo_elf_start);

    if (elf_size == 0) {
        kernel_panic("compiled user ELF image is empty");
    }

    process_t *process = elf_create_process(
        "compiled-demo",
        elf_image,
        elf_size
    );

    /*
     * Let the user process start, print its prompt, and block in SYS_READ.
     */
    thread_sleep_ticks(2);

    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('y');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('\n');

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("compiled ELF process test received wrong exit code");
    }

    process_destroy(process);

    console_writeln("Process test: compiled ELF32 user program cleanup sanity check passed");
}
```

That is the key handoff.

The kernel no longer constructs the user program.

It only receives an ELF image and passes it to the ELF loader.

---

# 8. Update `Makefile`

This is the biggest part of the chapter.

We need to build:

```text
user/crt0.S        → build/user/crt0.o
user/demo.c        → build/user/demo.o
user/linker.ld     → build/user/demo.elf
build/user/demo.elf → build/user/demo_elf_blob.o
```

Then link `demo_elf_blob.o` into the kernel.

## Add tool variables

Near the top of the Makefile, add:

```make
USER_CFLAGS := \
	-std=gnu11 \
	-ffreestanding \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-fno-pie \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-m32 \
	-march=i686 \
	-O2 \
	-Wall \
	-Wextra \
	-Iuser/include

USER_LDFLAGS := \
	-nostdlib \
	-ffreestanding \
	-m32 \
	-Wl,-T,user/linker.ld \
	-Wl,--build-id=none \
	-Wl,-Map,build/user/demo.map

OBJCOPY ?= i686-elf-objcopy
```

If your Makefile already has `OBJCOPY`, do not duplicate it.

## Add user build objects

Add this to the object list:

```make
build/user/demo_elf_blob.o
```

The relevant object list should include:

```make
OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/address_space.o \
    build/kernel/elf_loader.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/process.o \
    build/kernel/sync.o \
    build/kernel/syscall.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o \
    build/user/demo_elf_blob.o
```

## Add user build rules

Add these rules:

```make
build/user:
	mkdir -p build/user

build/user/crt0.o: user/crt0.S | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/demo.o: user/demo.c user/include/toyix_syscall.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/demo.elf: build/user/crt0.o build/user/demo.o user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) build/user/crt0.o build/user/demo.o -o $@

build/user/demo_elf_blob.o: build/user/demo.elf | build/user
	$(OBJCOPY) \
		-I binary \
		-O elf32-i386 \
		-B i386 \
		--rename-section .data=.rodata.userdemo,alloc,load,readonly,data,contents \
		--redefine-sym _binary_build_user_demo_elf_start=user_demo_elf_start \
		--redefine-sym _binary_build_user_demo_elf_end=user_demo_elf_end \
		$< $@
```

## Why redefine the symbols?

`objcopy -I binary` creates symbols based on the input filename.

For this input:

```text
build/user/demo.elf
```

the default symbols would be something like:

```text
_binary_build_user_demo_elf_start
_binary_build_user_demo_elf_end
_binary_build_user_demo_elf_size
```

Those names are ugly and path-dependent.

So we rename the useful ones to:

```c
user_demo_elf_start
user_demo_elf_end
```

That lets `process.c` use clean extern declarations.

---

# 9. Inspect the user ELF manually

After building, run:

```bash
make build/user/demo.elf
i686-elf-readelf -h build/user/demo.elf
i686-elf-readelf -l build/user/demo.elf
```

You want to see:

```text
Class:                             ELF32
Data:                              2's complement, little endian
Type:                              EXEC
Machine:                           Intel 80386
Entry point address:               0x40100000
```

And at least one `LOAD` segment around:

```text
0x40100000
```

If your `readelf -l` output shows multiple `LOAD` segments, that is okay as long as they are valid and page-aligned. Our loader can iterate multiple `PT_LOAD` headers.

---

# 10. ELF loader note: multiple segments may now appear

The hand-built ELF from Chapter 22 had exactly one `PT_LOAD`.

A real linker may produce more than one.

Our Chapter 22 ELF loader already loops over all `PT_LOAD` program headers, so that part is fine.

However, if the linker emits a `PT_LOAD` with a non-page-aligned virtual address, our first ELF loader will reject it.

This linker script starts the image at a page boundary and aligns major sections to 4 KiB to keep the loader happy.

If needed, verify with:

```bash
i686-elf-readelf -l build/user/demo.elf
```

---

# 11. Update `Makefile` test greps

Replace the Chapter 22 lines:

```make
grep -q "Process test: starting ELF32 user program test" build/test.log
grep -q "Process: created pid=1 name=elf-demo" build/test.log
grep -q "Syscall: process elf-demo pid=1 exited code 9" build/test.log
grep -q "Process: destroyed pid=1 name=elf-demo" build/test.log
grep -q "Process test: ELF32 load/read/write/sleep/exit cleanup sanity check passed" build/test.log
```

with:

```make
grep -q "Process test: starting compiled ELF32 user program test" build/test.log
grep -q "Process: created pid=1 name=compiled-demo" build/test.log
grep -q "echo: toyix" build/test.log
grep -q "Syscall: process compiled-demo pid=1 exited code 9" build/test.log
grep -q "Process: destroyed pid=1 name=compiled-demo" build/test.log
grep -q "Process test: compiled ELF32 user program cleanup sanity check passed" build/test.log
```

Because a real linked ELF may have slightly different `filesz` and `memsz` values, loosen the ELF loader grep.

Instead of:

```make
grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000 filesz=256 memsz=320" build/test.log
```

use:

```make
grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000" build/test.log
grep -q "ELF32: entry=0x40100000" build/test.log
```

The process-related block should look like this:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting compiled ELF32 user program test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000" build/test.log
	grep -q "ELF32: entry=0x40100000" build/test.log
	grep -q "Process: created pid=1 name=compiled-demo" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process compiled-demo pid=1 exited code 9" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=compiled-demo" build/test.log
	grep -q "Process test: compiled ELF32 user program cleanup sanity check passed" build/test.log
```

Update the final message:

```make
	@echo "Boot, memory, heap, sync, monitor, address-space, and compiled user ELF smoke test passed."
```

---

# 12. Update `tests/smoke.sh`

No structural change is required.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 23 checks passed."
```

---

# 13. Expected output

A successful boot should include:

```text
Process test: starting compiled ELF32 user program test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Thread: created compiled-demo id=...
Process: created pid=1 name=compiled-demo
user> toyix
echo: toyix
Syscall: process compiled-demo pid=1 exited code 9
Threads: reaping zombie compiled-demo id=...
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=1 name=compiled-demo
Process test: compiled ELF32 user program cleanup sanity check passed
```

The exact ELF segment sizes may differ from Chapter 22. That is expected because GCC and the linker now control the real ELF layout.

---

# 14. Common failures

## Failure: undefined symbols `user_demo_elf_start`

Check the `objcopy` rule.

The rule must redefine the generated binary symbols:

```make
--redefine-sym _binary_build_user_demo_elf_start=user_demo_elf_start
--redefine-sym _binary_build_user_demo_elf_end=user_demo_elf_end
```

Also verify the input path is exactly:

```text
build/user/demo.elf
```

If you change the path, the auto-generated `_binary_...` symbol names change.

You can inspect the blob object with:

```bash
i686-elf-nm build/user/demo_elf_blob.o
```

You should see:

```text
user_demo_elf_start
user_demo_elf_end
```

## Failure: ELF loader rejects the compiled ELF

Run:

```bash
i686-elf-readelf -h build/user/demo.elf
i686-elf-readelf -l build/user/demo.elf
```

Check:

```text
ELF32
little endian
EXEC
Intel 80386
entry 0x40100000
PT_LOAD vaddr page-aligned
```

If the entry is not `0x40100000`, check `user/linker.ld`.

If the ELF has unexpected dynamic sections or interpreter headers, make sure you are linking with:

```text
-nostdlib
-Wl,-T,user/linker.ld
```

## Failure: linker complains about missing libc functions

The user program must not call libc.

Avoid:

```c
strlen
memcpy
printf
exit
```

Use local helpers and syscalls instead.

For now, userland has no libc.

## Failure: stack protector references `__stack_chk_fail`

Make sure user CFLAGS contain:

```make
-fno-stack-protector
```

Some toolchains enable stack protector defaults depending on configuration.

## Failure: weird unwind or exception sections appear

Use:

```make
-fno-asynchronous-unwind-tables
-fno-unwind-tables
```

These are not strictly always required, but they keep the first user ELF simpler.

## Failure: process exits with wrong code

Check `crt0.S`.

It must pass `main()`’s return value to `SYS_EXIT`:

```asm
call main
mov %eax, %ebx
mov $2, %eax
int $0x80
```

If `main()` returns `9`, the process should exit with code `9`.

---

# 15. What this chapter achieved

Before this chapter:

```text
kernel builds fake ELF bytes manually
```

After this chapter:

```text
user/demo.c
  ↓
compiled by i686-elf-gcc
  ↓
linked as ELF32 ET_EXEC
  ↓
embedded into kernel by objcopy
  ↓
loaded by kernel ELF loader
  ↓
runs in ring 3
```

This is a major milestone.

The kernel now runs a real compiled C user program.

---

# 16. Design limitations

The userland build is still primitive.

Current limitations:

```text
only one embedded user program
no filesystem loading
no argv/envp
no libc
no user malloc
no startup stack ABI
no multiple user binaries
no shell exec command
no read-only text enforcement
```

But the boundary is now correct.

We have:

```text
compiled user C
real ELF
kernel ELF loader
syscall ABI
process lifecycle cleanup
```

That is the foundation for real userland.

---

# 17. Commit this chapter

After tests pass:

```bash
git status
git add .
git commit -m "Build and embed compiled user ELF program"
```

---

# 18. Next chapter

The next best step is to make the user stack look more like a real process startup environment.

Right now `main()` takes no arguments:

```c
int main(void)
```

We should add:

```text
argc
argv
initial user stack layout
crt0 passes argc/argv to main
kernel supplies program name and arguments
```

Then user programs can be written as:

```c
int main(int argc, char **argv)
```

That is the next bridge toward:

```text
exec()
shell command arguments
environment variables
filesystem-launched programs
```

---

# 19. Resources

- [Chapter 23 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_23)
- [Chapter 23 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_23)
- [GNU objcopy documentation](https://sourceware.org/binutils/docs/binutils/objcopy.html)
- [GCC invocation documentation](https://gcc.gnu.org/onlinedocs/gcc/Invoking-GCC.html)
- [GNU ld linker script documentation](https://sourceware.org/binutils/docs/ld/Scripts.html)

---

# 20. Closure

Chapter 23 removes the last fake user-program step from Toyix. The kernel now loads a real compiled user C program through the ELF loader, with the user binary built by the toolchain, embedded into the kernel image, started in ring 3, and cleaned up through the existing process lifecycle.

Happy Coding!

[1]: https://sourceware.org/binutils/docs/binutils/objcopy.html?utm_source=chatgpt.com "objcopy (GNU Binary Utilities)"
[2]: https://gcc.gnu.org/onlinedocs/gcc/Invoking-GCC.html?utm_source=chatgpt.com "Invoking GCC (Using the GNU Compiler Collection (GCC))"
[3]: https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html?utm_source=chatgpt.com "Code Gen Options (Using the GNU Compiler Collection (GCC))"
