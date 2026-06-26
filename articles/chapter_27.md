# Chapter 27 — A Second User Program for Safe Background Execution

In Chapter 26, we added:

```text
process table
PID lookup
ps
runbg
wait PID
```

That gave us a real background-process path.

But our only user program, `demo`, waits for terminal input:

```text
user> 
```

That makes it awkward for `runbg`, because a background process can compete with the monitor for keyboard input.

This chapter adds a second embedded user program:

```text
counter
```

`counter` does **not** read from stdin. It prints its arguments, counts a few times, sleeps between messages, and exits.

That gives us a safe program for testing:

```text
toyix> runbg counter alpha beta
toyix> ps
toyix> wait 2
```

The milestone output will look like:

```text
Program registry: registered 2 embedded program(s)
Program test: starting background counter test
Program: launching counter argc=3
Process: created pid=1 name=counter
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=1 exited code 4
Process: destroyed pid=1 name=counter
Program test: background counter cleanup sanity check passed
```

---

# 1. What this chapter adds

Add:

```text
user/
└── counter.c
```

Modify:

```text
kernel/program.c
Makefile
tests/smoke.sh
```

No kernel syscall changes are needed.

This chapter mostly expands the userland build pipeline from:

```text
one embedded program
```

to:

```text
multiple embedded programs
```

---

# 2. Add `user/counter.c`

```c
// user/counter.c
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
    write_str("counter: argc=");
    write_uint((toyix_u32)argc);
    write_str("\n");

    for (int i = 0; i < argc; ++i) {
        write_str("counter: argv[");
        write_uint((toyix_u32)i);
        write_str("]=");
        write_str(argv[i]);
        write_str("\n");
    }

    for (toyix_u32 i = 1; i <= 3; ++i) {
        write_str("counter: tick ");
        write_uint(i);
        write_str("\n");

        toyix_sleep(2);
    }

    return 4;
}
```

This program exits with code `4`.

That gives the kernel test an easy value to verify.

---

# 3. Why this program is background-safe

`demo` does this:

```c
toyix_read(FD_STDIN, buffer, sizeof(buffer));
```

`counter` does not.

It only uses:

```text
SYS_WRITE
SYS_SLEEP
SYS_EXIT
```

That means `counter` can run in the background without stealing terminal input from the monitor.

This is exactly the kind of program we need for `runbg`.

---

# 4. Update the Makefile for multiple user ELFs

Chapter 23 added a single user build path for:

```text
user/demo.c
```

Now we need to build and embed two programs:

```text
user/demo.c
user/counter.c
```

There are two ways to do this.

The clean future path is a pattern-based user-program build system.

For this chapter, we will keep it explicit so it is easy to read and debug.

---

# 5. Add counter objects to `OBJS`

Find the kernel `OBJS` list and add:

```make
build/user/counter_elf_blob.o
```

The end of the object list should now look like this:

```make
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o \
    build/user/demo_elf_blob.o \
    build/user/counter_elf_blob.o
```

Order does not matter much here, but keeping all embedded user blobs together is clearer.

---

# 6. Add Makefile rules for `counter`

Keep the existing `demo` rules.

Add these after the `demo` user build rules:

```make
build/user/counter.o: user/counter.c user/include/toyix_syscall.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/counter.elf: build/user/crt0.o build/user/counter.o user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) build/user/crt0.o build/user/counter.o -o $@

build/user/counter_elf_blob.o: build/user/counter.elf | build/user
	$(OBJCOPY) \
		-I binary \
		-O elf32-i386 \
		-B i386 \
		--rename-section .data=.rodata.usercounter,alloc,load,readonly,data,contents \
		--redefine-sym _binary_build_user_counter_elf_start=user_counter_elf_start \
		--redefine-sym _binary_build_user_counter_elf_end=user_counter_elf_end \
		$< $@
```

Now the build generates:

```text
build/user/demo.elf
build/user/counter.elf
```

and embeds both into the kernel.

---

# 7. Avoid map-file collision

In Chapter 23, `USER_LDFLAGS` probably included this:

```make
-Wl,-Map,build/user/demo.map
```

That is fine for one user program, but now both `demo.elf` and `counter.elf` would write the same map file.

Change `USER_LDFLAGS` to remove the fixed map file:

```make
USER_LDFLAGS := \
	-nostdlib \
	-ffreestanding \
	-m32 \
	-Wl,-T,user/linker.ld \
	-Wl,--build-id=none
```

Then add program-specific map files in each link rule.

Update `demo.elf`:

```make
build/user/demo.elf: build/user/crt0.o build/user/demo.o user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) -Wl,-Map,build/user/demo.map build/user/crt0.o build/user/demo.o -o $@
```

Update `counter.elf`:

```make
build/user/counter.elf: build/user/crt0.o build/user/counter.o user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) -Wl,-Map,build/user/counter.map build/user/crt0.o build/user/counter.o -o $@
```

Now each user program gets its own map file.

---

# 8. Inspect both user ELFs

After building, run:

```bash
make build/user/demo.elf
make build/user/counter.elf

i686-elf-readelf -h build/user/demo.elf
i686-elf-readelf -l build/user/demo.elf

i686-elf-readelf -h build/user/counter.elf
i686-elf-readelf -l build/user/counter.elf
```

Both should show:

```text
Class:                             ELF32
Data:                              2's complement, little endian
Type:                              EXEC
Machine:                           Intel 80386
Entry point address:               0x40100000
```

Both should have at least one `LOAD` segment beginning at or near:

```text
0x40100000
```

---

# 9. Update `kernel/program.c`

Add the new embedded symbols:

```c
extern const uint8_t user_demo_elf_start[];
extern const uint8_t user_demo_elf_end[];

extern const uint8_t user_counter_elf_start[];
extern const uint8_t user_counter_elf_end[];
```

Then update the program registry.

Replace:

```c
static const embedded_program_t programs[] = {
    {
        .name = "demo",
        .description = "compiled user-mode demo program",
        .image_start = user_demo_elf_start,
        .image_end = user_demo_elf_end
    }
};
```

with:

```c
static const embedded_program_t programs[] = {
    {
        .name = "demo",
        .description = "interactive stdin/stdout demo",
        .image_start = user_demo_elf_start,
        .image_end = user_demo_elf_end
    },
    {
        .name = "counter",
        .description = "background-safe counter demo",
        .image_start = user_counter_elf_start,
        .image_end = user_counter_elf_end
    }
};
```

Now `programs` will list two entries:

```text
demo - interactive stdin/stdout demo
counter - background-safe counter demo
```

---

# 10. Replace `program_test_once()`

In Chapter 26, `program_test_once()` used `demo` and injected keyboard input.

Now we want the dedicated background test to use `counter`.

Replace the function with this version:

```c
void program_test_once(void) {
    console_writeln("Program test: starting background counter test");

    static const char *argv[] = {
        "counter",
        "alpha",
        "beta"
    };

    process_t *process = 0;

    int rc = program_run_background(
        "counter",
        3,
        argv,
        &process
    );

    if (rc != 0 || process == 0) {
        kernel_panic("program test could not launch counter");
    }

    uint32_t pid = process->pid;

    console_write("Program test: background pid=");
    console_write_u32_dec(pid);
    console_putc('\n');

    process_list();

    process_t *found = process_find(pid);

    if (found != process) {
        kernel_panic("program test could not find background process by PID");
    }

    uint32_t exit_code = process_wait(process);

    if (exit_code != 4) {
        kernel_panic("program test received wrong counter exit code");
    }

    process_list();

    process_destroy(process);

    console_writeln("Program test: background counter cleanup sanity check passed");
}
```

Notice what is gone:

```c
keyboard_debug_inject_char(...)
```

The test no longer needs synthetic keyboard input.

That is the whole point of adding `counter`.

---

# 11. Remove unused include from `kernel/program.c`

If `program.c` no longer uses keyboard injection, remove:

```c
#include "drivers/input/keyboard.h"
```

The top of `kernel/program.c` should now look like:

```c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/elf_loader.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/string.h"
#include "kernel/thread.h"
```

You may still need `kernel/thread.h` because `program_test_once()` waits on the process through `process_wait()`, which itself sleeps, but if `program.c` does not directly call thread functions anymore, you can remove it too.

In the version above, `program.c` does not directly call `thread_sleep_ticks()`, so this include can also be removed unless another function needs it:

```c
#include "kernel/thread.h"
```

A minimal include set is:

```c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/elf_loader.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/string.h"
```

---

# 12. Update monitor behavior

No required monitor code changes are needed.

The existing commands automatically benefit:

```text
toyix> programs
```

now shows:

```text
Embedded programs:
  demo - interactive stdin/stdout demo
  counter - background-safe counter demo
```

And `runbg` can now be used safely:

```text
toyix> runbg counter A B
runbg: started counter pid=2

toyix> ps
PID  STATE     EXIT  NAME
2    running   -     counter

toyix> wait 2
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=A
counter: argv[2]=B
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=2 exited code 4
wait: pid 2 exited code 4
```

Depending on timing, some `counter` output may appear before or after the `ps` prompt. That is expected because it runs concurrently.

---

# 13. Update Makefile greps

Replace:

```make
grep -q "Program registry: registered 1 embedded program(s)" build/test.log
```

with:

```make
grep -q "Program registry: registered 2 embedded program(s)" build/test.log
```

Replace the program descriptions:

```make
grep -q "demo - compiled user-mode demo program" build/test.log
```

with:

```make
grep -q "demo - interactive stdin/stdout demo" build/test.log
grep -q "counter - background-safe counter demo" build/test.log
```

Replace the Chapter 26 test lines:

```make
grep -q "Program test: starting background process table test" build/test.log
grep -q "Program test: background process table cleanup sanity check passed" build/test.log
grep -q "Program: launching demo argc=3" build/test.log
grep -q "Process: created pid=1 name=demo" build/test.log
grep -q "argv\\[0\\]=demo" build/test.log
grep -q "echo: toyix" build/test.log
grep -q "Syscall: process demo pid=1 exited code 9" build/test.log
grep -q "Process: destroyed pid=1 name=demo" build/test.log
```

with:

```make
grep -q "Program test: starting background counter test" build/test.log
grep -q "Program: launching counter argc=3" build/test.log
grep -q "Process: created pid=1 name=counter" build/test.log
grep -q "counter: argc=3" build/test.log
grep -q "counter: argv\\[0\\]=counter" build/test.log
grep -q "counter: argv\\[1\\]=alpha" build/test.log
grep -q "counter: argv\\[2\\]=beta" build/test.log
grep -q "counter: tick 1" build/test.log
grep -q "counter: tick 2" build/test.log
grep -q "counter: tick 3" build/test.log
grep -q "Syscall: process counter pid=1 exited code 4" build/test.log
grep -q "Process: destroyed pid=1 name=counter" build/test.log
grep -q "Program test: background counter cleanup sanity check passed" build/test.log
```

The updated process/program test block should look like this:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Program registry: registered 2 embedded program(s)" build/test.log
	grep -q "Embedded programs:" build/test.log
	grep -q "demo - interactive stdin/stdout demo" build/test.log
	grep -q "counter - background-safe counter demo" build/test.log
	grep -q "usage: runbg PROGRAM" build/test.log
	grep -q "usage: wait PID" build/test.log
	grep -q "Program test: starting background counter test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000" build/test.log
	grep -q "ELF32: entry=0x40100000" build/test.log
	grep -q "Process: initial stack argc=3" build/test.log
	grep -q "Program: launching counter argc=3" build/test.log
	grep -q "Process: created pid=1 name=counter" build/test.log
	grep -q "Program test: background pid=1" build/test.log
	grep -q "PID  STATE" build/test.log
	grep -q "counter: argc=3" build/test.log
	grep -q "counter: argv\\[0\\]=counter" build/test.log
	grep -q "counter: argv\\[1\\]=alpha" build/test.log
	grep -q "counter: argv\\[2\\]=beta" build/test.log
	grep -q "counter: tick 1" build/test.log
	grep -q "counter: tick 2" build/test.log
	grep -q "counter: tick 3" build/test.log
	grep -q "Syscall: process counter pid=1 exited code 4" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=counter" build/test.log
	grep -q "Program test: background counter cleanup sanity check passed" build/test.log
```

Update the final success message:

```make
	@echo "Boot, memory, heap, sync, monitor, process table, and background counter smoke test passed."
```

---

# 14. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 27 checks passed."
```

---

# 15. Expected boot output

A successful boot should include:

```text
Program registry: registered 2 embedded program(s)
...
Embedded programs:
  demo - interactive stdin/stdout demo
  counter - background-safe counter demo
...
Program test: starting background counter test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 ...
ELF32: entry=0x40100000
Process: initial stack argc=3 esp=0x6FFFF...
Program: launching counter argc=3
Thread: created counter id=...
Process: created pid=1 name=counter
Program test: background pid=1
PID  STATE     EXIT  NAME
1    running   -     counter
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=1 exited code 4
Threads: reaping zombie counter id=...
PID  STATE     EXIT  NAME
1    exited    4     counter
Address space: destroyed process page directory, user pages=... tables=...
Process: destroyed pid=1 name=counter
Program test: background counter cleanup sanity check passed
```

The exact interleaving may vary slightly because `counter` is scheduled as a real process.

---

# 16. Interactive test

After boot, try:

```text
toyix> programs
```

Expected:

```text
Embedded programs:
  demo - interactive stdin/stdout demo
  counter - background-safe counter demo
```

Then:

```text
toyix> runbg counter one two
```

Expected:

```text
Program: launching counter argc=3
Process: created pid=...
runbg: started counter pid=...
```

Then:

```text
toyix> ps
```

Depending on timing, the process may be running or already exited:

```text
PID  STATE     EXIT  NAME
2    running   -     counter
```

or:

```text
PID  STATE     EXIT  NAME
2    exited    4     counter
```

Then:

```text
toyix> wait 2
```

Expected:

```text
wait: pid 2 exited code 4
Process: destroyed pid=2 name=counter
```

---

# 17. Common failures

## Failure: undefined `user_counter_elf_start`

Check the `objcopy` rule for `counter`.

The rule must include:

```make
--redefine-sym _binary_build_user_counter_elf_start=user_counter_elf_start
--redefine-sym _binary_build_user_counter_elf_end=user_counter_elf_end
```

Verify with:

```bash
i686-elf-nm build/user/counter_elf_blob.o
```

You should see:

```text
user_counter_elf_start
user_counter_elf_end
```

## Failure: registry still says one program

Check that the registry array has two entries and that `program_count` is computed from the array:

```c
static const uint32_t program_count =
    sizeof(programs) / sizeof(programs[0]);
```

Do not hardcode:

```c
static const uint32_t program_count = 1;
```

## Failure: `runbg counter` says unknown program

Check the registry name:

```c
.name = "counter"
```

and make sure `program_find()` uses `kstrcmp()` correctly:

```c
if (kstrcmp(programs[i].name, name) == 0)
```

## Failure: `counter` exits with wrong code

The user program should return:

```c
return 4;
```

`crt0.S` should turn `main()`’s return value into `SYS_EXIT`:

```asm
call main
mov %eax, %ebx
mov $2, %eax
int $0x80
```

## Failure: `counter` runs but `wait PID` hangs

Likely causes:

```text
process_exit_current() did not mark process->exited
process_wait() is waiting on the wrong process pointer
process table has stale PID/pointer
```

Check:

```c
process->exited = 1;
process->state = PROCESS_EXITED;
```

inside `process_exit_current()`.

---

# 18. What this chapter achieved

Before this chapter:

```text
only one embedded user program
runbg existed but the only program read stdin
```

After this chapter:

```text
two embedded user programs
demo     = interactive stdin/stdout test
counter  = background-safe process test
runbg counter works without keyboard contention
ps/wait can manage real background work
```

This makes the monitor much more useful.

---

# 19. Design limitations

The user-program build system is still explicit and repetitive.

We now have two sets of similar rules:

```text
demo.o
demo.elf
demo_elf_blob.o

counter.o
counter.elf
counter_elf_blob.o
```

That is okay for two programs, but it will get ugly with five or ten.

A later chapter should introduce pattern rules or a `USER_PROGRAMS` variable.

Other limitations remain:

```text
no filesystem exec
no PATH
no job control
no process ownership tree
no kill
no nonblocking wait
no stdout serialization beyond console lock
```

But now the process table has a background-safe workload.

---

# 20. Commit this chapter

After tests pass:

```bash
git status
git add .
git commit -m "Add background-safe counter user program"
```

---

# 21. Next chapter

The next cleanup should be the user-program build system.

Instead of hand-writing rules for every program, we can introduce:

```make
USER_PROGRAMS := demo counter
```

and generate:

```text
build/user/<name>.o
build/user/<name>.elf
build/user/<name>_elf_blob.o
```

That will make adding new user programs much easier before we build a small shell-like user program or start filesystem work.

---

# 22. Resources

- [Chapter 27 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_27)
- [Chapter 27 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_27)
- [GNU objcopy documentation](https://sourceware.org/binutils/docs/binutils/objcopy.html)

---

# 23. Closure

Chapter 27 gives Toyix a practical background-safe workload. The kernel now embeds two compiled user programs, can launch `counter` without fighting the monitor for stdin, and can verify background launch, PID lookup, wait, exit status, and cleanup through a deterministic process test path.

Happy Coding!
