# Chapter 24 — User `argc` / `argv` and a Real Initial Stack

In Chapter 23, we crossed another major boundary:

```text
user/demo.c
  ↓
compiled as ELF32
  ↓
embedded in the kernel
  ↓
loaded by the ELF loader
  ↓
run in ring 3
```

But the user program still started like this:

```c
int main(void)
```

That is not how we want user programs to look long-term.

This chapter adds a real initial user stack with:

```text
argc
argv[0]
argv[1]
...
argv[argc] = NULL
envp[0] = NULL
```

Then `crt0.S` will pass those arguments to:

```c
int main(int argc, char **argv)
```

This moves us closer to a normal process startup model. ELF process initialization commonly places `argc`, `argv`, and environment pointers on the initial process stack; the ABI language describes `argc` as the argument count and `argv` as an array of argument strings terminated by a null pointer. ([Linux Foundation Specs][1])

The milestone output will become:

```text
Process test: starting compiled ELF32 argv user program test
ELF32: entry=0x40100000
Process: initial stack argc=3
Process: created pid=1 name=compiled-demo
argv[0]=demo
argv[1]=alpha
argv[2]=beta
user> toyix
echo: toyix
Syscall: process compiled-demo pid=1 exited code 9
Process test: compiled ELF32 argv cleanup sanity check passed
```

---

# 1. What this chapter adds

Modify:

```text
include/kernel/process.h
include/kernel/elf_loader.h
kernel/elf_loader.c
kernel/process.c
kernel/usermode.c
user/crt0.S
user/demo.c
Makefile
tests/smoke.sh
```

The new startup flow is:

```text
ELF loader maps program
  ↓
kernel maps user stack
  ↓
kernel copies argv strings onto user stack
  ↓
kernel writes argc / argv pointer table
  ↓
kernel sets process->user_initial_esp
  ↓
x86_enter_user_mode(entry, user_initial_esp)
  ↓
crt0 reads argc/argv from stack
  ↓
crt0 calls main(argc, argv)
```

---

# 2. User stack layout

We will build this stack:

```text
lower addresses

ESP ->  argc
        argv[0] pointer
        argv[1] pointer
        argv[2] pointer
        NULL
        NULL envp terminator

        padding/alignment

        "demo\0"
        "alpha\0"
        "beta\0"

higher addresses
```

For this chapter, we keep the model simple:

```text
argv exists
envp is empty
no auxiliary vector yet
```

A mature ELF environment would also include environment strings and an auxiliary vector. We are not there yet.

---

# 3. Update `include/kernel/process.h`

Add an initial stack pointer field and argument setup API.

```c
// include/kernel/process.h
#ifndef TOYIX_KERNEL_PROCESS_H
#define TOYIX_KERNEL_PROCESS_H

#include <stdint.h>
#include "kernel/address_space.h"

struct thread;

typedef enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_EXITED,
    PROCESS_DESTROYED
} process_state_t;

typedef struct process {
    uint32_t magic;
    uint32_t pid;

    const char *name;
    process_state_t state;

    address_space_t *address_space;

    struct thread *main_thread;

    uint32_t exit_code;
    int exited;

    uintptr_t user_code_base;
    uintptr_t user_entry;

    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
    uintptr_t user_initial_esp;
} process_t;

void process_init_system(void);

process_t *process_create_empty(const char *name);

int process_map_user_region(
    process_t *process,
    uintptr_t virtual_addr,
    uint32_t size_bytes
);

int process_copy_to_user_init(
    process_t *process,
    uintptr_t user_dest,
    const void *kernel_src,
    uint32_t size
);

int process_zero_user_init(
    process_t *process,
    uintptr_t user_dest,
    uint32_t size
);

void process_set_user_entry(process_t *process, uintptr_t entry);

void process_set_user_stack(
    process_t *process,
    uintptr_t stack_base,
    uintptr_t stack_top
);

int process_setup_arguments(
    process_t *process,
    int argc,
    const char **argv
);

void process_start_user(process_t *process);

process_t *process_current(void);

void process_exit_current(uint32_t exit_code);

uint32_t process_wait(process_t *process);
void process_destroy(process_t *process);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

void process_test_once(void);

#endif
```

The new field is:

```c
uintptr_t user_initial_esp;
```

The new API is:

```c
int process_setup_arguments(process_t *process, int argc, const char **argv);
```

---

# 4. Update `kernel/process.c`

First, initialize the new field in `process_create_empty()`:

```c
process->user_initial_esp = 0;
```

The stack setter should also default the initial ESP to the top of the stack:

```c
void process_set_user_stack(
    process_t *process,
    uintptr_t stack_base,
    uintptr_t stack_top
) {
    validate_process(process);

    process->user_stack_base = stack_base;
    process->user_stack_top = stack_top;
    process->user_initial_esp = stack_top;
}
```

Now add this helper near the other page/stack helpers:

```c
static uintptr_t align_down_4(uintptr_t value) {
    return value & ~(uintptr_t)0x3u;
}
```

Now add argument stack setup.

```c
#define PROCESS_MAX_ARGC 16
#define PROCESS_MAX_ARG_STRING 128

typedef struct argv_copy_context {
    uintptr_t dest;
    const void *src;
    uint32_t size;
} argv_copy_context_t;

static void argv_copy_operation(void *context) {
    argv_copy_context_t *copy = (argv_copy_context_t *)context;

    memcpy((void *)copy->dest, copy->src, copy->size);
}

static int process_copy_stack_bytes(
    process_t *process,
    uintptr_t dest,
    const void *src,
    uint32_t size
) {
    if (size == 0) {
        return 0;
    }

    argv_copy_context_t context;

    context.dest = dest;
    context.src = src;
    context.size = size;

    process_with_address_space(process, argv_copy_operation, &context);

    return 0;
}

int process_setup_arguments(
    process_t *process,
    int argc,
    const char **argv
) {
    validate_live_process(process);

    if (argc < 0 || argc > PROCESS_MAX_ARGC) {
        return -1;
    }

    if (argc > 0 && argv == 0) {
        return -1;
    }

    if (process->user_stack_base == 0 ||
        process->user_stack_top == 0) {
        return -1;
    }

    uintptr_t sp = process->user_stack_top;

    uintptr_t arg_ptrs[PROCESS_MAX_ARGC];

    for (int i = argc - 1; i >= 0; --i) {
        const char *arg = argv[i];

        if (arg == 0) {
            return -1;
        }

        uint32_t len = 0;

        while (arg[len] != '\0') {
            len++;

            if (len >= PROCESS_MAX_ARG_STRING) {
                return -1;
            }
        }

        uint32_t bytes = len + 1u;

        if (sp < process->user_stack_base + bytes) {
            return -1;
        }

        sp -= bytes;

        if (process_copy_stack_bytes(process, sp, arg, bytes) != 0) {
            return -1;
        }

        arg_ptrs[i] = sp;
    }

    sp = align_down_4(sp);

    /*
     * Layout:
     *
     *   argc
     *   argv[0]
     *   ...
     *   argv[argc - 1]
     *   NULL
     *   NULL envp terminator
     */
    uint32_t pointer_words = 1u + (uint32_t)argc + 1u + 1u;
    uint32_t table_bytes = pointer_words * sizeof(uint32_t);

    if (sp < process->user_stack_base + table_bytes) {
        return -1;
    }

    sp -= table_bytes;

    uint32_t stack_table[1u + PROCESS_MAX_ARGC + 1u + 1u];
    uint32_t index = 0;

    stack_table[index++] = (uint32_t)argc;

    for (int i = 0; i < argc; ++i) {
        stack_table[index++] = (uint32_t)arg_ptrs[i];
    }

    stack_table[index++] = 0; /* argv[argc] */
    stack_table[index++] = 0; /* envp[0] */

    if (process_copy_stack_bytes(
            process,
            sp,
            stack_table,
            table_bytes
        ) != 0) {
        return -1;
    }

    process->user_initial_esp = sp;

    console_write("Process: initial stack argc=");
    console_write_u32_dec((uint32_t)argc);
    console_write(" esp=");
    console_write_hex32((uint32_t)sp);
    console_putc('\n');

    return 0;
}
```

## Why `process_with_address_space()` is reused

The user stack exists only in the process address space.

So when the kernel writes argument strings to user addresses, it must temporarily switch into that process address space, just like it did when copying ELF segments.

That is why `process_setup_arguments()` writes through:

```c
process_copy_stack_bytes()
```

instead of directly calling `memcpy()` on a user virtual address from the kernel address space.

---

# 5. Update `process_start_user()`

Change its validation so it requires `user_initial_esp`.

Replace:

```c
if (process->user_entry == 0 ||
    process->user_stack_base == 0 ||
    process->user_stack_top == 0) {
    kernel_panic("process_start_user missing entry or stack");
}
```

with:

```c
if (process->user_entry == 0 ||
    process->user_stack_base == 0 ||
    process->user_stack_top == 0 ||
    process->user_initial_esp == 0) {
    kernel_panic("process_start_user missing entry or initial stack");
}
```

---

# 6. Update `kernel/usermode.c`

Use the new initial user ESP.

Replace:

```c
x86_enter_user_mode(
    (uint32_t)process->user_entry,
    (uint32_t)process->user_stack_top
);
```

with:

```c
x86_enter_user_mode(
    (uint32_t)process->user_entry,
    (uint32_t)process->user_initial_esp
);
```

The full function should now be:

```c
void usermode_enter_current_process(void) {
    process_t *process = process_current();

    if (process == 0) {
        kernel_panic("usermode entry without process");
    }

    if (process->user_entry == 0 || process->user_initial_esp == 0) {
        kernel_panic("usermode entry missing entry point or initial stack");
    }

    tss_set_kernel_stack(current_thread_kernel_stack_top());

    x86_enter_user_mode(
        (uint32_t)process->user_entry,
        (uint32_t)process->user_initial_esp
    );

    kernel_panic("x86_enter_user_mode returned unexpectedly");
}
```

---

# 7. Update `kernel/elf_loader.c`

After setting the stack, install default arguments before starting the process.

The cleanest approach is to leave argument policy outside the loader. So **do not** hardcode argv in `elf_loader.c`.

`elf_create_process()` should still only do:

```text
create process
load ELF
start process
```

But we need a new function that creates without starting, so the caller can set arguments.

Update `include/kernel/elf_loader.h`:

```c
process_t *elf_create_process_suspended(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);
```

Full header:

```c
// include/kernel/elf_loader.h
#ifndef TOYIX_KERNEL_ELF_LOADER_H
#define TOYIX_KERNEL_ELF_LOADER_H

#include <stdint.h>
#include "kernel/process.h"

#define ELF_LOADER_OK 0
#define ELF_LOADER_ERR_INVALID -1
#define ELF_LOADER_ERR_UNSUPPORTED -2
#define ELF_LOADER_ERR_TOO_LARGE -3
#define ELF_LOADER_ERR_LOAD_FAILED -4

#define ELF_USER_STACK_TOP  0x70000000u
#define ELF_USER_STACK_SIZE 4096u

int elf_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
);

process_t *elf_create_process_suspended(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);

process_t *elf_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);

#endif
```

Now update the bottom of `kernel/elf_loader.c`:

```c
process_t *elf_create_process_suspended(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
) {
    process_t *process = process_create_empty(name);

    int rc = elf_load_process(process, image, image_size);

    if (rc != ELF_LOADER_OK) {
        console_write("ELF32: load failed rc=");
        console_write_u32_dec((uint32_t)(-rc));
        console_putc('\n');

        kernel_panic("ELF32 process load failed");
    }

    return process;
}

process_t *elf_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
) {
    process_t *process = elf_create_process_suspended(
        name,
        image,
        image_size
    );

    static const char *default_argv[] = {
        "program"
    };

    if (process_setup_arguments(process, 1, default_argv) != 0) {
        kernel_panic("ELF32 default argument setup failed");
    }

    process_start_user(process);

    return process;
}
```

This preserves the old convenience API while giving tests and future `exec()` code a chance to install custom arguments.

---

# 8. Update `user/crt0.S`

Replace the previous file with this version.

```asm
// user/crt0.S
//
// Tiny user-mode C runtime entry.
//
// Initial stack layout expected from the kernel:
//
//   [esp + 0] = argc
//   [esp + 4] = argv[0]
//   ...
//
// _start calls:
//
//   main(argc, argv)
//
// main's return value becomes the process exit code.

.global _start
.extern main

.section .text.start, "ax"

_start:
    mov (%esp), %eax        // argc
    lea 4(%esp), %edx       // argv

    push %edx               // argv
    push %eax               // argc
    call main

    add $8, %esp

    mov %eax, %ebx          // exit code
    mov $2, %eax            // SYS_EXIT
    int $0x80

.hang:
    jmp .hang
```

Now user C programs can use:

```c
int main(int argc, char **argv)
```

---

# 9. Update `user/demo.c`

Replace it with this version.

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

static void write_uint(toyix_u32 value) {
    char buffer[11];
    toyix_u32 index = 0;

    if (value == 0) {
        write_str("0");
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        char ch = buffer[--index];
        toyix_write(FD_STDOUT, &ch, 1);
    }
}

int main(int argc, char **argv) {
    char buffer[32];

    write_str("argc=");
    write_uint((toyix_u32)argc);
    write_str("\n");

    for (int i = 0; i < argc; ++i) {
        write_str("argv[");
        write_uint((toyix_u32)i);
        write_str("]=");
        write_str(argv[i]);
        write_str("\n");
    }

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

The user program now proves:

```text
argc value was correct
argv pointer table was valid
argv strings were copied into user memory
main(argc, argv) received the expected values
```

---

# 10. Update `process_test_once()` in `kernel/process.c`

Replace the process creation part with the suspended ELF path.

```c
void process_test_once(void) {
    console_writeln("Process test: starting compiled ELF32 argv user program test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    const uint8_t *elf_image = user_demo_elf_start;

    uint32_t elf_size =
        (uint32_t)(user_demo_elf_end - user_demo_elf_start);

    if (elf_size == 0) {
        kernel_panic("compiled user ELF image is empty");
    }

    process_t *process = elf_create_process_suspended(
        "compiled-demo",
        elf_image,
        elf_size
    );

    static const char *argv[] = {
        "demo",
        "alpha",
        "beta"
    };

    if (process_setup_arguments(process, 3, argv) != 0) {
        kernel_panic("process argv setup failed");
    }

    process_start_user(process);

    /*
     * Let the user process start, print argv, print its prompt,
     * and block in SYS_READ.
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
        kernel_panic("compiled ELF argv process test received wrong exit code");
    }

    process_destroy(process);

    console_writeln("Process test: compiled ELF32 argv cleanup sanity check passed");
}
```

Now the process test explicitly says:

```text
argv[0] = demo
argv[1] = alpha
argv[2] = beta
```

---

# 11. Update the Makefile greps

Replace:

```make
grep -q "Process test: starting compiled ELF32 user program test" build/test.log
grep -q "Process test: compiled ELF32 user program cleanup sanity check passed" build/test.log
```

with:

```make
grep -q "Process test: starting compiled ELF32 argv user program test" build/test.log
grep -q "Process: initial stack argc=3" build/test.log
grep -q "argc=3" build/test.log
grep -q "argv\\[0\\]=demo" build/test.log
grep -q "argv\\[1\\]=alpha" build/test.log
grep -q "argv\\[2\\]=beta" build/test.log
grep -q "Process test: compiled ELF32 argv cleanup sanity check passed" build/test.log
```

The process-related test block should now look like this:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting compiled ELF32 argv user program test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000" build/test.log
	grep -q "ELF32: entry=0x40100000" build/test.log
	grep -q "Process: initial stack argc=3" build/test.log
	grep -q "Process: created pid=1 name=compiled-demo" build/test.log
	grep -q "argc=3" build/test.log
	grep -q "argv\\[0\\]=demo" build/test.log
	grep -q "argv\\[1\\]=alpha" build/test.log
	grep -q "argv\\[2\\]=beta" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process compiled-demo pid=1 exited code 9" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=compiled-demo" build/test.log
	grep -q "Process test: compiled ELF32 argv cleanup sanity check passed" build/test.log
```

Update the final test message:

```make
	@echo "Boot, memory, heap, sync, monitor, address-space, ELF, and argv stack smoke test passed."
```

---

# 12. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 24 checks passed."
```

---

# 13. Expected output

A successful boot should include:

```text
Process test: starting compiled ELF32 argv user program test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Thread: created compiled-demo id=...
Process: created pid=1 name=compiled-demo
argc=3
argv[0]=demo
argv[1]=alpha
argv[2]=beta
user> toyix
echo: toyix
Syscall: process compiled-demo pid=1 exited code 9
Threads: reaping zombie compiled-demo id=...
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=1 name=compiled-demo
Process test: compiled ELF32 argv cleanup sanity check passed
```

The exact initial stack pointer may vary, but it should be inside the user stack region.

---

# 14. Common failures

## Failure: `argc` is garbage

Check `crt0.S`.

It must read `argc` from:

```asm
mov (%esp), %eax
```

The kernel enters user mode with `ESP = process->user_initial_esp`.

If `usermode.c` still passes `user_stack_top`, `argc` will be whatever happens to live at the top of the empty stack.

## Failure: `argv[0]` faults

Likely causes:

```text
argv pointer table points to kernel memory
argument strings copied without switching to process address space
argument strings copied outside the mapped stack
```

Check:

```c
process_with_address_space(process, argv_copy_operation, &context);
```

and make sure every `arg_ptrs[i]` is a user-stack address.

## Failure: `argv[1]` prints as `argv[0]`

The stack table is probably written in the wrong order.

It must be:

```text
argc
argv[0]
argv[1]
argv[2]
NULL
NULL
```

and `crt0.S` must pass:

```asm
lea 4(%esp), %edx
```

as `argv`.

## Failure: process start panics with missing initial stack

Make sure `process_setup_arguments()` is called before:

```c
process_start_user(process);
```

The correct sequence is:

```c
process = elf_create_process_suspended(...);
process_setup_arguments(process, argc, argv);
process_start_user(process);
```

## Failure: stack overflow during argument setup

The current stack is only 4096 bytes.

The chapter limits are:

```text
max argc       = 16
max arg string = 128 bytes
```

Those are intentionally conservative.

If you pass long strings, increase `ELF_USER_STACK_SIZE`.

---

# 15. What this chapter achieved

Before this chapter:

```text
_start → main(void)
```

After this chapter:

```text
kernel builds initial user stack
  ↓
_start reads argc and argv
  ↓
_start calls main(argc, argv)
```

The user program now has a real process-startup interface.

This is the bridge toward:

```text
exec(path, argv)
shell command arguments
environment variables
auxiliary vector
standard C runtime startup
```

---

# 16. Design limitations

We still do not provide:

```text
envp
auxiliary vector
stack randomization
guard page
16-byte stack alignment
argc/argv from shell commands
argument copying from another user process
exec()
```

But the core mechanism is now present.

The kernel can construct a user stack, and user C code can receive arguments normally.

---

# 17. Next chapter

The next good step is to add a simple in-kernel program registry and a monitor command that launches user programs.

That would let the kernel monitor do:

```text
toyix> run demo alpha beta
```

Internally:

```text
monitor parses command
  ↓
looks up embedded program by name
  ↓
builds argv from command tokens
  ↓
creates process
  ↓
sets up user stack
  ↓
starts process
  ↓
waits for exit
  ↓
prints exit code
```

That is the first version of an `exec`-like path, even before we have a filesystem.

---

# 18. Resources

- [Chapter 24 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_24)
- [Chapter 24 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_24)
- [Linux Foundation ABI process initialization reference](https://refspecs.linuxfoundation.org/ELF/zSeries/lzsabi0_zSeries/x895.html)

---

# 19. Closure

Chapter 24 gives Toyix its first real user-process startup ABI. The kernel now builds an initial user stack, copies argument strings into user memory, installs the `argc` and `argv` table, enters ring 3 with the prepared stack pointer, and proves the full handoff through a compiled user ELF that prints its arguments before reading input and exiting.

Happy Coding!
