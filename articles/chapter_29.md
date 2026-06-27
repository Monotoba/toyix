# Chapter 29 — First Userland Runtime

In Chapter 28, we cleaned up the user-program build system:

```text
USER_PROGRAMS := demo counter
```

now automatically builds:

```text
build/user/demo.elf
build/user/counter.elf
build/user/demo_elf_blob.o
build/user/counter_elf_blob.o
```

But our user programs still duplicate small helper functions:

```text
str_len()
write_str()
write_uint()
```

Both `demo.c` and `counter.c` contain nearly the same code.

This chapter adds the first tiny Toyix userland support library:

```text
user/include/toyix.h
user/lib/toyix.c
```

This is not a real libc yet, but it is the beginning of one.

After this chapter, user programs can simply call:

```c
toyix_puts("hello");
toyix_write_uint(123);
toyix_putchar('\n');
```

The runtime behavior stays the same:

```text
Program registry: registered 2 embedded program(s)
Program test: starting background counter test
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=alpha
counter: argv[2]=beta
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=1 exited code 4
Program test: background counter cleanup sanity check passed
```

This is mostly a userland cleanup chapter.

---

## 1. What this chapter adds

Add:

```text
user/
├── include/
│   └── toyix.h
└── lib/
    └── toyix.c
```

Modify:

```text
user/demo.c
user/counter.c
Makefile
tests/smoke.sh
```

No kernel changes are required.

No syscall ABI changes are required.

---

## 2. Current problem

`demo.c` and `counter.c` both have local versions of:

```c
static toyix_u32 str_len(const char *text);
static void write_str(const char *text);
static void write_uint(toyix_u32 value);
```

That was fine while we only had one or two programs.

But it will become annoying quickly.

We want user programs to look more like this:

```c
#include "toyix.h"

int main(int argc, char **argv) {
    toyix_puts("hello from userland");
    return 0;
}
```

The syscall wrappers stay in:

```text
user/include/toyix_syscall.h
```

Higher-level user helpers go in:

```text
user/include/toyix.h
user/lib/toyix.c
```

---

## 3. Add `user/include/toyix.h`

```c
#ifndef TOYIX_USER_TOYIX_H
#define TOYIX_USER_TOYIX_H

#include "toyix_syscall.h"

typedef unsigned int toyix_size_t;

toyix_size_t toyix_strlen(const char *text);

void toyix_putchar(char ch);
void toyix_write_str(const char *text);
void toyix_puts(const char *text);

void toyix_write_uint(toyix_u32 value);
void toyix_write_int(toyix_i32 value);

int toyix_streq(const char *a, const char *b);

#endif
```

### Why not call these `strlen`, `puts`, and `printf`?

Because this is not a full libc yet.

For now, using a `toyix_` prefix avoids pretending we have standard C library compatibility.

Later, we can decide whether to provide libc-like names.

---

## 4. Add `user/lib/toyix.c`

```c
#include "toyix.h"

toyix_size_t toyix_strlen(const char *text) {
    toyix_size_t len = 0;

    if (text == 0) {
        return 0;
    }

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

void toyix_putchar(char ch) {
    toyix_write(FD_STDOUT, &ch, 1);
}

void toyix_write_str(const char *text) {
    if (text == 0) {
        return;
    }

    toyix_write(FD_STDOUT, text, (toyix_u32)toyix_strlen(text));
}

void toyix_puts(const char *text) {
    toyix_write_str(text);
    toyix_putchar('\n');
}

void toyix_write_uint(toyix_u32 value) {
    char buffer[11];
    toyix_u32 index = 0;

    if (value == 0) {
        toyix_putchar('0');
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        toyix_putchar(buffer[--index]);
    }
}

void toyix_write_int(toyix_i32 value) {
    if (value < 0) {
        toyix_putchar('-');

        /*
         * Avoid relying on undefined behavior for INT_MIN by converting
         * through unsigned arithmetic.
         */
        toyix_u32 magnitude = (toyix_u32)(-(value + 1)) + 1u;
        toyix_write_uint(magnitude);
        return;
    }

    toyix_write_uint((toyix_u32)value);
}

int toyix_streq(const char *a, const char *b) {
    if (a == 0 || b == 0) {
        return a == b;
    }

    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == *b;
}
```

This gives user programs a small common base.

---

## 5. Update `user/demo.c`

Replace `demo.c` with this smaller version:

```c
#include "toyix.h"

int main(int argc, char **argv) {
    char buffer[32];

    toyix_write_str("argc=");
    toyix_write_int(argc);
    toyix_putchar('\n');

    for (int i = 0; i < argc; ++i) {
        toyix_write_str("argv[");
        toyix_write_int(i);
        toyix_write_str("]=");
        toyix_puts(argv[i]);
    }

    toyix_write_str("user> ");

    toyix_i32 got = toyix_read(FD_STDIN, buffer, sizeof(buffer));

    if (got < 0) {
        toyix_puts("read failed");
        return 1;
    }

    toyix_write_str("echo: ");

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_putchar('\n');

    toyix_sleep(3);

    return 9;
}
```

The behavior is unchanged.

The source is cleaner.

---

## 6. Update `user/counter.c`

Replace `counter.c` with this smaller version:

```c
// user/counter.c
#include "toyix.h"

int main(int argc, char **argv) {
    toyix_write_str("counter: argc=");
    toyix_write_int(argc);
    toyix_putchar('\n');

    for (int i = 0; i < argc; ++i) {
        toyix_write_str("counter: argv[");
        toyix_write_int(i);
        toyix_write_str("]=");
        toyix_puts(argv[i]);
    }

    for (toyix_u32 i = 1; i <= 3; ++i) {
        toyix_write_str("counter: tick ");
        toyix_write_uint(i);
        toyix_putchar('\n');

        toyix_sleep(2);
    }

    return 4;
}
```

Again, behavior is unchanged.

But now `counter.c` is focused on the program’s logic.

---

## 7. Update Makefile user variables

Add a user library source list.

Near the user-program section:

```make
USER_PROGRAMS := demo counter
USER_ELFS := $(USER_PROGRAMS:%=build/user/%.elf)
USER_BLOBS := $(USER_PROGRAMS:%=build/user/%_elf_blob.o)

USER_LIB_SRCS := user/lib/toyix.c
USER_LIB_OBJS := $(USER_LIB_SRCS:user/lib/%.c=build/user/lib/%.o)
```

This gives us:

```text
USER_LIB_OBJS = build/user/lib/toyix.o
```

---

## 8. Add a build directory rule for `build/user/lib`

Add:

```make
build/user/lib:
	mkdir -p build/user/lib
```

Keep the existing:

```make
build/user:
	mkdir -p build/user
```

---

## 9. Add a pattern rule for user library objects

Add:

```make
build/user/lib/%.o: user/lib/%.c user/include/toyix.h user/include/toyix_syscall.h | build/user/lib
	$(CC) $(USER_CFLAGS) -c $< -o $@
```

This compiles:

```text
user/lib/toyix.c
```

into:

```text
build/user/lib/toyix.o
```

---

## 10. Update the user program object rule

The existing rule probably looks like this:

```make
build/user/%.o: user/%.c user/include/toyix_syscall.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@
```

Replace it with:

```make
build/user/%.o: user/%.c user/include/toyix.h user/include/toyix_syscall.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@
```

Now programs rebuild if the support library header changes.

---

## 11. Update the ELF link rule

The old rule links only:

```text
crt0.o
program.o
```

We now need:

```text
crt0.o
program.o
user library objects
```

Replace:

```make
build/user/%.elf: build/user/crt0.o build/user/%.o user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) \
		-Wl,-Map,build/user/$*.map \
		build/user/crt0.o build/user/$*.o \
		-o $@
```

with:

```make
build/user/%.elf: build/user/crt0.o build/user/%.o $(USER_LIB_OBJS) user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) \
		-Wl,-Map,build/user/$*.map \
		build/user/crt0.o build/user/$*.o $(USER_LIB_OBJS) \
		-o $@
```

Now every user program links against the tiny support library.

---

## 12. Updated user build section

The user portion of the Makefile should now look roughly like this:

```make
USER_PROGRAMS := demo counter
USER_ELFS := $(USER_PROGRAMS:%=build/user/%.elf)
USER_BLOBS := $(USER_PROGRAMS:%=build/user/%_elf_blob.o)

USER_LIB_SRCS := user/lib/toyix.c
USER_LIB_OBJS := $(USER_LIB_SRCS:user/lib/%.c=build/user/lib/%.o)

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
	-Wl,--build-id=none

OBJCOPY ?= i686-elf-objcopy

build/user:
	mkdir -p build/user

build/user/lib:
	mkdir -p build/user/lib

build/user/crt0.o: user/crt0.S | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/lib/%.o: user/lib/%.c user/include/toyix.h user/include/toyix_syscall.h | build/user/lib
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.o: user/%.c user/include/toyix.h user/include/toyix_syscall.h | build/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.elf: build/user/crt0.o build/user/%.o $(USER_LIB_OBJS) user/linker.ld | build/user
	$(CC) $(USER_LDFLAGS) \
		-Wl,-Map,build/user/$*.map \
		build/user/crt0.o build/user/$*.o $(USER_LIB_OBJS) \
		-o $@

build/user/%_elf_blob.o: build/user/%.elf | build/user
	$(OBJCOPY) \
		-I binary \
		-O elf32-i386 \
		-B i386 \
		--rename-section .data=.rodata.user_$*,alloc,load,readonly,data,contents \
		--redefine-sym _binary_build_user_$*_elf_start=user_$*_elf_start \
		--redefine-sym _binary_build_user_$*_elf_end=user_$*_elf_end \
		$< $@

user-programs: $(USER_LIB_OBJS) $(USER_ELFS)

user-blobs: $(USER_BLOBS)

list-user-programs:
	@echo "$(USER_PROGRAMS)"

readelf-user: $(USER_ELFS)
	@for elf in $(USER_ELFS); do \
		echo "==== $$elf ===="; \
		i686-elf-readelf -h $$elf | grep -E "Class:|Data:|Type:|Machine:|Entry point"; \
		i686-elf-readelf -l $$elf | grep LOAD; \
	done
```

---

## 13. Check for accidental libc calls

Because we now have a library source file, it is easy to accidentally introduce normal libc calls.

Avoid these in userland:

```text
strlen
memcpy
printf
puts
exit
malloc
```

Use Toyix helpers instead:

```text
toyix_strlen
toyix_write_str
toyix_puts
toyix_exit
```

A quick check:

```bash
i686-elf-nm -u build/user/demo.elf
i686-elf-nm -u build/user/counter.elf
```

For this stage, you generally want no unresolved symbols except any toolchain-specific oddities you intentionally support.

Ideally, the output should be empty.

---

## 14. Inspect the user ELFs

Run:

```bash
make user-programs
make readelf-user
```

You should still see:

```text
ELF32
Intel 80386
EXEC
Entry point 0x40100000
```

The segment sizes may grow slightly because both programs now link with `toyix.o`.

That is expected.

Your greps should not depend on exact `filesz` or `memsz`.

---

## 15. Runtime tests remain the same

The Chapter 27 and Chapter 28 runtime greps should still pass.

Keep:

```make
	grep -q "Program registry: registered 2 embedded program(s)" build/test.log
	grep -q "demo - interactive stdin/stdout demo" build/test.log
	grep -q "counter - background-safe counter demo" build/test.log
	grep -q "Program test: starting background counter test" build/test.log
	grep -q "Program: launching counter argc=3" build/test.log
	grep -q "Process: created pid=1 name=counter" build/test.log
	grep -q "counter: argc=" build/test.log
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

Add build-artifact checks:

```make
	@test -f build/user/lib/toyix.o
	@test -f build/user/demo.elf
	@test -f build/user/counter.elf
```

---

## 16. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 29 checks passed."
```

---

## 17. Expected output

Runtime output should remain equivalent to Chapter 27 and Chapter 28:

```text
Program registry: registered 2 embedded program(s)
Embedded programs:
  demo - interactive stdin/stdout demo
  counter - background-safe counter demo

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

The segment sizes may be larger now. That is fine.

---

## 18. Interactive check

After boot:

```text
toyix> run counter one two
```

Expected:

```text
Program: launching counter argc=3
Process: created pid=...
counter: argc=3
counter: argv[0]=counter
counter: argv[1]=one
counter: argv[2]=two
counter: tick 1
counter: tick 2
counter: tick 3
Syscall: process counter pid=... exited code 4
Process: destroyed pid=... name=counter
run: counter exited code 4
```

Then:

```text
toyix> run demo hello world
```

Expected:

```text
argc=3
argv[0]=demo
argv[1]=hello
argv[2]=world
user>
```

Type a line and press Enter.

---

## 19. Common failures

### Failure: undefined reference to `toyix_write_str`

Check that the ELF link rule includes:

```make
$(USER_LIB_OBJS)
```

The link command must include:

```make
build/user/crt0.o build/user/$*.o $(USER_LIB_OBJS)
```

not only:

```make
build/user/crt0.o build/user/$*.o
```

### Failure: `toyix.h` not found

Check:

```make
-Iuser/include
```

in `USER_CFLAGS`.

Also verify the include path in user programs:

```c
#include "toyix.h"
```

### Failure: library object directory missing

Make sure this rule exists:

```make
build/user/lib:
	mkdir -p build/user/lib
```

and the library object rule has:

```make
| build/user/lib
```

as an order-only prerequisite.

### Failure: user program accidentally links against host libc

Make sure `USER_LDFLAGS` includes:

```make
-nostdlib
```

and user code does not call standard library functions.

### Failure: segment layout changes and loader rejects ELF

The support library can change the section sizes. Usually this is fine.

But check:

```bash
make readelf-user
```

If a `PT_LOAD` segment has a non-page-aligned virtual address, our current ELF loader may reject it.

The current `user/linker.ld` should keep major sections page-aligned.

---

## 20. What this chapter achieved

Before this chapter:

```text
demo.c and counter.c duplicated basic output helpers
```

After this chapter:

```text
user/lib/toyix.c
  provides shared user helpers

user/include/toyix.h
  declares user helpers

demo.c and counter.c
  focus on program behavior
```

This is the first small step toward a Toyix userland C library.

---

## 21. Design limitations

This is still not libc.

Missing:

```text
memcpy
memset
strcmp
atoi
printf
malloc
errno
open/read/write/close wrappers with conventional names
startup environment helpers
file descriptor abstractions
```

But the structure is now in place.

Future user programs can share helper code instead of copying it.

---

## Resources

- Chapter source: [Toyix repository](https://github.com/Monotoba/toyix)
- Chapter release: [Chapter_29](https://github.com/Monotoba/toyix/releases/tag/Chapter_29)

## Closure

Chapter 29 gives Toyix its first shared userland support library. The kernel behavior stays the same, but user programs now have a cleaner base to build on before the next runtime features arrive.

Happy Coding!
