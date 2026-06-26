# Chapter 18 — File-Descriptor Syscalls and a Tiny User-Mode Console Program

In Chapter 17, we added the first minimal process object:

```text
process_t
  ↓
main user thread
  ↓
user code page
  ↓
user stack page
  ↓
SYS_WRITE / SYS_SLEEP / SYS_EXIT
```

That was a big step, but the syscall interface was still a little too artificial.

This chapter makes user programs feel more like real programs by adding a tiny file-descriptor-style interface:

```text
fd 0 = stdin
fd 1 = stdout
fd 2 = stderr
```

We will update `SYS_WRITE` to use a file descriptor, add `SYS_READ`, and run a user-mode program that does this:

```text
write(1, "user> ", 6)
read(0, buffer, 32)
write(1, "echo: ", 6)
write(1, buffer, bytes_read)
write(1, "\n", 1)
sleep(3)
exit(9)
```

The milestone output will look like:

```text
Process test: starting fd read/write user process test
Process: created pid=1 name=stdio-demo
user>
toyix
echo: toyix
Syscall: process stdio-demo pid=1 exited code 9
Process test: fd read/write/sleep/exit sanity check passed
```

---

# 1. Why file descriptors now?

A file descriptor is just a small integer handle.

For this chapter, we do not have a real VFS yet. So these are special built-in descriptors:

```text
0 → keyboard/terminal input
1 → console output
2 → console error output
```

That gives user programs a stable ABI:

```c
read(0, buf, len);
write(1, buf, len);
write(2, err, len);
```

Later, these same numbers can point into a per-process file table:

```text
process_t
  ↓
fd table
  ├── 0: terminal input
  ├── 1: terminal output
  ├── 2: terminal output
  ├── 3: file
  ├── 4: pipe
  └── ...
```

For now, the syscall layer directly routes `fd 0`, `fd 1`, and `fd 2`.

---

# 2. Patch overview

Modify:

```text
include/kernel/console.h
kernel/console.c
include/kernel/syscall.h
kernel/syscall.c
kernel/process.c
Makefile
tests/smoke.sh
```

We do **not** need new assembly.

The syscall ABI after this chapter:

```text
EAX = syscall number

SYS_READ:
  EBX = fd
  ECX = user buffer
  EDX = length
  return EAX = bytes read or 0xFFFFFFFF on error

SYS_WRITE:
  EBX = fd
  ECX = user buffer
  EDX = length
  return EAX = bytes written or 0xFFFFFFFF on error

SYS_SLEEP:
  EBX = ticks
  return EAX = 0

SYS_EXIT:
  EBX = exit code
  does not return
```

---

# 3. Update `include/kernel/console.h`

We need a length-aware console write function.

Until now, `console_write()` expected a NUL-terminated string. But `write(fd, buf, len)` should write exactly `len` bytes, even if the buffer contains a NUL byte.

Replace `console.h` with this version.

```c
// include/kernel/console.h
#ifndef TOYIX_KERNEL_CONSOLE_H
#define TOYIX_KERNEL_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct console_driver {
    const char *name;
    void (*init)(void);
    void (*putc)(char c);
} console_driver_t;

void console_register(const console_driver_t *driver);
void console_init_all(void);

void console_locking_init(void);
void console_lock(void);
void console_unlock(void);

void console_putc(char c);
void console_write(const char *text);
void console_write_n(const char *text, size_t length);
void console_writeln(const char *text);
void console_write_hex32(uint32_t value);
void console_write_u32_dec(uint32_t value);

/*
 * Raw console output bypasses the console mutex.
 *
 * Use only when the caller already holds the console lock, or in very early
 * boot before the scheduler/sync layer exists.
 */
void console_raw_putc(char c);
void console_raw_write(const char *text);
void console_raw_write_n(const char *text, size_t length);
void console_raw_writeln(const char *text);
void console_raw_write_hex32(uint32_t value);
void console_raw_write_u32_dec(uint32_t value);

void console_lock_test_once(void);

#endif
```

---

# 4. Update `kernel/console.c`

Add these two functions:

```c
void console_raw_write_n(const char *text, size_t length) {
    if (text == NULL) {
        return;
    }

    for (size_t i = 0; i < length; ++i) {
        console_raw_putc(text[i]);
    }
}

void console_write_n(const char *text, size_t length) {
    console_lock();
    console_raw_write_n(text, length);
    console_unlock();
}
```

Place `console_raw_write_n()` near `console_raw_write()`.

Place `console_write_n()` near `console_write()`.

The relevant section should look like this:

```c
void console_raw_write(const char *text) {
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        console_raw_putc(*text++);
    }
}

void console_raw_write_n(const char *text, size_t length) {
    if (text == NULL) {
        return;
    }

    for (size_t i = 0; i < length; ++i) {
        console_raw_putc(text[i]);
    }
}

void console_raw_writeln(const char *text) {
    console_raw_write(text);
    console_raw_putc('\n');
}
```

And the locked section:

```c
void console_write(const char *text) {
    console_lock();
    console_raw_write(text);
    console_unlock();
}

void console_write_n(const char *text, size_t length) {
    console_lock();
    console_raw_write_n(text, length);
    console_unlock();
}

void console_writeln(const char *text) {
    console_lock();
    console_raw_writeln(text);
    console_unlock();
}
```

## Why this matters

This is correct:

```c
console_write_n(buffer, length);
```

This is not always correct:

```c
buffer[length] = '\0';
console_write(buffer);
```

A real `write()` syscall is byte-counted, not NUL-terminated.

---

# 5. Update `include/kernel/syscall.h`

Replace it with this version.

```c
// include/kernel/syscall.h
#ifndef TOYIX_KERNEL_SYSCALL_H
#define TOYIX_KERNEL_SYSCALL_H

#include "arch/x86/interrupts.h"

#define FD_STDIN  0u
#define FD_STDOUT 1u
#define FD_STDERR 2u

#define SYS_PUTC  1u
#define SYS_EXIT  2u
#define SYS_WRITE 3u
#define SYS_SLEEP 4u
#define SYS_READ  5u

void syscall_handler(interrupt_frame_t *frame);

#endif
```

`SYS_PUTC` stays for compatibility with the older Chapter 16 test style, but new user programs should prefer `SYS_WRITE`.

---

# 6. Replace `kernel/syscall.c`

```c
// kernel/syscall.c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/process.h"
#include "kernel/syscall.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"
#include "kernel/usercopy.h"

#define SYSCALL_RW_MAX 256u

static void syscall_write(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    uintptr_t user_buf = (uintptr_t)frame->ecx;
    uint32_t length = frame->edx;

    if (fd != FD_STDOUT && fd != FD_STDERR) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    if (length == 0) {
        frame->eax = 0;
        return;
    }

    char buffer[SYSCALL_RW_MAX];

    if (copy_from_user(buffer, user_buf, length) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    console_write_n(buffer, length);

    frame->eax = length;
}

static void syscall_read(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    uintptr_t user_buf = (uintptr_t)frame->ecx;
    uint32_t length = frame->edx;

    if (fd != FD_STDIN) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    if (length == 0) {
        frame->eax = 0;
        return;
    }

    /*
     * For the first fd-based read syscall, fd 0 is line-oriented through the
     * kernel terminal layer. The newline is consumed by terminal_readline()
     * and is not copied into the user buffer.
     */
    char buffer[SYSCALL_RW_MAX + 1u];

    interrupts_enable();
    size_t got = terminal_readline(buffer, (size_t)length + 1u);

    if (copy_to_user(user_buf, buffer, got) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = (uint32_t)got;
}

void syscall_handler(interrupt_frame_t *frame) {
    if (frame == 0) {
        return;
    }

    uint32_t number = frame->eax;

    switch (number) {
        case SYS_PUTC: {
            char ch = (char)(frame->ebx & 0xFFu);
            console_putc(ch);
            frame->eax = 0;
            return;
        }

        case SYS_READ:
            syscall_read(frame);
            return;

        case SYS_WRITE:
            syscall_write(frame);
            return;

        case SYS_SLEEP: {
            uint32_t ticks = frame->ebx;
            interrupts_enable();
            thread_sleep_ticks(ticks);
            frame->eax = 0;
            return;
        }

        case SYS_EXIT: {
            uint32_t exit_code = frame->ebx;

            process_exit_current(exit_code);
            thread_exit();
            return;
        }

        default:
            console_write("Syscall: unknown syscall ");
            console_write_u32_dec(number);
            console_putc('\n');

            frame->eax = 0xFFFFFFFFu;
            return;
    }
}
```

---

# 7. What changed in `SYS_WRITE`

Previously, Chapter 17 used:

```text
EAX = SYS_WRITE
EBX = user buffer
ECX = length
```

Now it uses:

```text
EAX = SYS_WRITE
EBX = fd
ECX = user buffer
EDX = length
```

That is much closer to a real syscall ABI.

For this chapter:

```text
fd 1 and fd 2 both write to the kernel console
```

Later, `fd 2` may get different formatting or route to a serial debug stream.

---

# 8. What `SYS_READ` does for now

`SYS_READ` on `fd 0` currently calls:

```c
terminal_readline()
```

That means it is line-oriented:

```text
user types characters
terminal echoes them
backspace works
Enter completes the read
kernel copies line into user buffer
syscall returns byte count
```

The newline is consumed and not copied to user memory.

That is not exactly POSIX `read()`, but it is a good early terminal behavior.

Later, we can split this into:

```text
raw keyboard device
terminal canonical mode
terminal raw mode
real file descriptor table
```

---

# 9. Replace the test program in `kernel/process.c`

We will keep the same process object and fixed user mapping, but replace the Chapter 17 user process demo with a new fd-based console program.

Add these constants near the existing process address constants:

```c
#define USER_PROCESS_PROMPT_VA 0x401000A0u
#define USER_PROCESS_PREFIX_VA 0x401000A8u
#define USER_PROCESS_NEWLINE_VA 0x401000B0u
#define USER_PROCESS_INPUT_VA  0x401000C0u

#define USER_PROCESS_INPUT_MAX 32u
```

Then replace the old hardcoded `user_process_demo[]` with a small machine-code builder.

```c
static void emit_u8(uint8_t *program, uint32_t *offset, uint8_t value) {
    program[*offset] = value;
    (*offset)++;
}

static void emit_u32(uint8_t *program, uint32_t *offset, uint32_t value) {
    program[*offset + 0u] = (uint8_t)(value & 0xFFu);
    program[*offset + 1u] = (uint8_t)((value >> 8) & 0xFFu);
    program[*offset + 2u] = (uint8_t)((value >> 16) & 0xFFu);
    program[*offset + 3u] = (uint8_t)((value >> 24) & 0xFFu);
    *offset += 4u;
}

static void emit_mov_eax_imm32(uint8_t *program, uint32_t *offset, uint32_t value) {
    emit_u8(program, offset, 0xB8u);
    emit_u32(program, offset, value);
}

static void emit_mov_ebx_imm32(uint8_t *program, uint32_t *offset, uint32_t value) {
    emit_u8(program, offset, 0xBBu);
    emit_u32(program, offset, value);
}

static void emit_mov_ecx_imm32(uint8_t *program, uint32_t *offset, uint32_t value) {
    emit_u8(program, offset, 0xB9u);
    emit_u32(program, offset, value);
}

static void emit_mov_edx_imm32(uint8_t *program, uint32_t *offset, uint32_t value) {
    emit_u8(program, offset, 0xBAu);
    emit_u32(program, offset, value);
}

static void emit_int80(uint8_t *program, uint32_t *offset) {
    emit_u8(program, offset, 0xCDu);
    emit_u8(program, offset, 0x80u);
}

static void build_stdio_demo_program(uint8_t *program, uint32_t program_size) {
    memset(program, 0x90, program_size);

    uint32_t offset = 0;

    /*
     * write(1, "user> ", 6)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_PROMPT_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    /*
     * bytes = read(0, input_buffer, 32)
     */
    emit_mov_eax_imm32(program, &offset, SYS_READ);
    emit_mov_ebx_imm32(program, &offset, FD_STDIN);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_INPUT_VA);
    emit_mov_edx_imm32(program, &offset, USER_PROCESS_INPUT_MAX);
    emit_int80(program, &offset);

    /*
     * mov esi, eax
     *
     * Save byte count returned by read().
     */
    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xC6u);

    /*
     * write(1, "echo: ", 6)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_PREFIX_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    /*
     * write(1, input_buffer, esi)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_INPUT_VA);

    /*
     * mov edx, esi
     */
    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xF2u);

    emit_int80(program, &offset);

    /*
     * write(1, "\n", 1)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_NEWLINE_VA);
    emit_mov_edx_imm32(program, &offset, 1);
    emit_int80(program, &offset);

    /*
     * sleep(3)
     */
    emit_mov_eax_imm32(program, &offset, SYS_SLEEP);
    emit_mov_ebx_imm32(program, &offset, 3);
    emit_int80(program, &offset);

    /*
     * exit(9)
     */
    emit_mov_eax_imm32(program, &offset, SYS_EXIT);
    emit_mov_ebx_imm32(program, &offset, 9);
    emit_int80(program, &offset);

    /*
     * jmp $
     */
    emit_u8(program, &offset, 0xEBu);
    emit_u8(program, &offset, 0xFEu);

    if (offset >= 0xA0u) {
        kernel_panic("stdio demo program overlapped data area");
    }

    const char prompt[] = "user> ";
    const char prefix[] = "echo: ";
    const char newline[] = "\n";

    memcpy(
        &program[USER_PROCESS_PROMPT_VA - USER_PROCESS_CODE_VA],
        prompt,
        sizeof(prompt) - 1u
    );

    memcpy(
        &program[USER_PROCESS_PREFIX_VA - USER_PROCESS_CODE_VA],
        prefix,
        sizeof(prefix) - 1u
    );

    memcpy(
        &program[USER_PROCESS_NEWLINE_VA - USER_PROCESS_CODE_VA],
        newline,
        1u
    );
}
```

## Why use a tiny emitter?

In Chapter 17, we hand-coded the full byte array.

That works, but it is fragile. If we insert one instruction, all later data offsets may shift.

This chapter’s tiny emitter lets us keep the machine code readable:

```c
emit_mov_eax_imm32(program, &offset, SYS_WRITE);
emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
emit_mov_ecx_imm32(program, &offset, USER_PROCESS_PROMPT_VA);
emit_mov_edx_imm32(program, &offset, 6);
emit_int80(program, &offset);
```

It is still raw machine code, but it is much easier to verify.

---

# 10. Update `process_test_once()` in `kernel/process.c`

Replace the old Chapter 17 version with this one.

```c
void process_test_once(void) {
    console_writeln("Process test: starting fd read/write user process test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    static uint8_t program[256];

    build_stdio_demo_program(program, sizeof(program));

    process_create_user(
        "stdio-demo",
        program,
        sizeof(program)
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

    while (!last_exit_seen) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (last_exit_code != 9) {
        kernel_panic("process fd syscall test received wrong exit code");
    }

    console_writeln("Process test: fd read/write/sleep/exit sanity check passed");
}
```

Also add the keyboard header at the top of `process.c`:

```c
#include "drivers/input/keyboard.h"
```

The top of `process.c` should now include:

```c
#include <stddef.h>
#include <stdint.h>
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/process.h"
#include "kernel/string.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"
#include "kernel/vmem.h"
```

---

# 11. Why the test injects keyboard input

The user process really calls:

```text
read(0, buffer, 32)
```

That blocks on terminal input.

During automated boot tests, nobody is typing into QEMU. So the kernel test injects synthetic input:

```c
keyboard_debug_inject_char('t');
keyboard_debug_inject_char('o');
keyboard_debug_inject_char('y');
keyboard_debug_inject_char('i');
keyboard_debug_inject_char('x');
keyboard_debug_inject_char('\n');
```

That exercises the same path as real keyboard input after the character enters the keyboard buffer:

```text
keyboard buffer
  ↓
terminal_readline()
  ↓
SYS_READ
  ↓
copy_to_user()
  ↓
user buffer
```

This is a good test because it proves real blocking I/O behavior without requiring manual input during CI-style smoke tests.

---

# 12. Update `Makefile`

Update the process test greps.

Remove the old Chapter 17 lines:

```make
grep -q "Process test: starting user process syscall test" build/test.log
grep -q "Process: created pid=1 name=user-demo" build/test.log
grep -q "User process says hello through SYS_WRITE" build/test.log
grep -q "Syscall: process user-demo pid=1 exited code 7" build/test.log
grep -q "Process test: user process syscall/write/sleep/exit sanity check passed" build/test.log
```

Replace them with:

```make
grep -q "Process test: starting fd read/write user process test" build/test.log
grep -q "Process: created pid=1 name=stdio-demo" build/test.log
grep -q "echo: toyix" build/test.log
grep -q "Syscall: process stdio-demo pid=1 exited code 9" build/test.log
grep -q "Process test: fd read/write/sleep/exit sanity check passed" build/test.log
```

The relevant portion of the test target should now include:

```make
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting fd read/write user process test" build/test.log
	grep -q "Process: created pid=1 name=stdio-demo" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process stdio-demo pid=1 exited code 9" build/test.log
	grep -q "Process test: fd read/write/sleep/exit sanity check passed" build/test.log
```

The full target message can become:

```make
	@echo "Boot, memory, heap, sync, monitor, and fd syscall smoke test passed."
```

---

# 13. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 18 checks passed."
```

---

# 14. Expected output

A successful boot should include:

```text
Process: process table initialized
...
Process test: starting fd read/write user process test
Process: created pid=1 name=stdio-demo
user> toyix
echo: toyix
Syscall: process stdio-demo pid=1 exited code 9
Threads: reaping zombie stdio-demo id=...
Process test: fd read/write/sleep/exit sanity check passed
Monitor: monitor thread started
```

The exact display may include scheduler wake messages between the prompt and the injected input because this is a preemptive kernel:

```text
user>
toyix
```

That proves:

```text
user process wrote prompt through SYS_WRITE
user process blocked in SYS_READ
kernel terminal supplied line input
kernel copied data into user memory
user process wrote the echoed line through SYS_WRITE
user process slept through SYS_SLEEP
user process exited through SYS_EXIT
```

---

# 15. Common failures

## Failure: `SYS_WRITE` prints nothing

Check the ABI change.

Old Chapter 17 ABI:

```text
EBX = user buffer
ECX = length
```

New Chapter 18 ABI:

```text
EBX = fd
ECX = user buffer
EDX = length
```

If the user program still uses the old ABI, the syscall handler will interpret the buffer address as a file descriptor and reject it.

## Failure: `SYS_READ` never returns

Likely causes:

```text
process_test_once() did not inject newline
keyboard_debug_inject_char() did not wake wait queue
terminal_readline() is blocked waiting for Enter
SYS_READ called before keyboard_init()
```

The read completes only after:

```c
keyboard_debug_inject_char('\n');
```

## Failure: `echo: toyix` is missing but prompt appears

That means the user process reached `SYS_READ` but did not get data back.

Check:

```c
copy_to_user(user_buf, buffer, got)
```

and verify that the input buffer address is user-accessible:

```text
USER_PROCESS_INPUT_VA = 0x401000C0
```

That address must lie inside the mapped user code page.

## Failure: process exits with wrong code

Check the emitted exit syscall:

```c
emit_mov_eax_imm32(program, &offset, SYS_EXIT);
emit_mov_ebx_imm32(program, &offset, 9);
emit_int80(program, &offset);
```

and the test expectation:

```c
if (last_exit_code != 9) {
    kernel_panic("process fd syscall test received wrong exit code");
}
```

## Failure: test hangs after `echo: toyix`

The user program probably reached `SYS_SLEEP(3)` but did not wake.

Check that PIT interrupts are enabled before `process_test_once()` runs.

The expected boot order remains:

```text
pit_init()
keyboard_init()
thread_preemption_init()
interrupts_enable()
...
process_test_once()
```

---

# 16. What this chapter achieved

We now have a more realistic user-kernel interface:

```text
SYS_READ(fd, user_buf, len)
SYS_WRITE(fd, user_buf, len)
SYS_SLEEP(ticks)
SYS_EXIT(code)
```

And a tiny user-mode console program:

```text
write prompt
read line
write echo prefix
write line
sleep
exit
```

This is the first point where user-mode code feels like an actual program instead of just a privilege-transition test.

---

# 17. Design limitations

This is still not a real VFS or POSIX layer.

Current limitations:

```text
fd 0/1/2 are hardcoded
no per-process fd table yet
SYS_READ is line-oriented, not raw
newline is consumed, not returned
no EOF
no blocking file objects
no pipes
no device abstraction
no terminal modes
```

That is acceptable. We now have the syscall shape that those features can grow into.

---

# 18. Next chapter

Now that user programs can read and write, the next deeper architectural chapter should be **per-process address spaces**.

That means:

```text
new page directory per process
kernel mappings copied into every address space
user mappings private to each process
CR3 switch when scheduler switches process
copy_from_user() checks current address space
process teardown unmaps user pages
```

That is the step that turns:

```text
all user programs share the same address space
```

into:

```text
each process has its own virtual memory
```

That is a major OS boundary, and it is the right next foundation before loading ELF programs.

---

# 19. Resources

- [Chapter 18 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_18)
- [Chapter 18 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_18)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki: System Calls](https://wiki.osdev.org/System_Calls)
- [OSDev Wiki: File Descriptors](https://wiki.osdev.org/File_Systems#File_Descriptors)

---

# 20. Closure

User programs can now write to stdout and stderr, read a line from stdin, sleep, and exit through a small fd-style syscall ABI. That gives the next process and address-space chapters a more realistic user/kernel interface to build on.

Happy Coding!
