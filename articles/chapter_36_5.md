# Chapter 36.5 - A Testing Detour and a Real Smoke Harness

Chapter 36 added `SYS_STAT`, which was the right filesystem step.

But it also exposed a testing problem that has been growing for a while.

Right now, Toyix testing is split across three awkward places:

```text
Makefile test targets
tests/smoke.sh
shell-driven feature probing
```

That worked while the system was small.

It does not scale well now that Toyix has:

```text
multiple embedded user programs
process control
filesystem syscalls
shell built-ins
deliberate exception tests
deliberate page-fault tests
```

The biggest smell is that we started adding test-oriented shell behavior just to verify lower-level APIs.

For example, `seektest` was not a real user-facing shell feature.

It existed because we needed a place to exercise `SYS_SEEK`.

That is backwards.

The shell should be tested as a shell.
The filesystem layer should be tested as a filesystem layer.
The host-side smoke harness should decide what outputs count as pass or fail.

So this chapter takes a small detour before Chapter 37.

We are going to add a proper host-side smoke runner, move assertions out of shell `grep` chains, and replace test-only shell coverage with a dedicated user-mode test program.

After this chapter:

```text
make test             -> build and capture build/test.log
make test-exception   -> build and capture build/exception.log
make test-page-fault  -> build and capture build/pagefault.log
python3 tests/smoke.py
                      -> run the full suite and verify all expectations
```

And instead of using shell commands as our primary filesystem test surface, we will add:

```text
fstest
```

as a dedicated embedded user-mode smoke program.

That keeps the shell focused on shell behavior and gives us a better base for future filesystem chapters.

---

## 1. What this chapter adds

Modify:

```text
Makefile
kernel/program.c
user/shell.c
tests/smoke.sh
.github/workflows/ci.yml
README.md
CHANGELOG.md
index.md
docs/roadmap.md
```

Add:

```text
tests/smoke.py
user/fstest.c
articles/chapter_36_5.md
```

High-level result:

```text
make owns build + QEMU log capture
Python owns assertions + suite orchestration
shell testing shrinks to shell behavior
filesystem API testing moves into fstest
CI runs the Python smoke harness directly
```

---

## 2. Why take this detour now?

If we keep adding filesystem features without fixing the test shape, every chapter gets harder to validate.

That pain shows up in a few concrete ways.

### Shell testing is mixing concerns

Today the shell is checking too many unrelated things:

```text
command parsing
foreground and background jobs
cat output
stat output
seek behavior
```

That means when a test fails, it is not obvious whether the bug is in:

```text
the shell
the syscall layer
the VFS layer
the user program launch path
the test harness itself
```

### Shell `grep` chains are hard to evolve

The current `Makefile` test target contains a long sequence of:

```text
grep -q ...
grep -q ...
grep -q ...
```

That is usable, but brittle.

It has a few real drawbacks:

```text
no shared assertion helpers
no regex helpers beyond inline grep flags
no clean grouping by test suite
no readable failure messages
no obvious place to check build artifacts
```

### We should stop adding test-only shell commands

`seektest` did its job.

But it was really a temporary testing shortcut.

If we keep going down that path, the shell becomes a dumping ground for internal probes instead of a real user interface.

That is exactly the wrong direction for the next filesystem chapters.

---

## 3. What we are going to build

We will make three structural changes.

### 1. `make` becomes a log producer

The `Makefile` test targets will still:

```text
build the right kernel
boot QEMU
capture serial logs
```

But they will stop deciding pass/fail through dozens of inline `grep` calls.

That keeps `make test` useful on its own:

```text
build the image
run it
leave the log behind
```

### 2. Python becomes the smoke harness

We will add `tests/smoke.py`.

This runner will:

```text
run the make targets
check required build artifacts
load captured logs
assert exact strings
assert regex patterns
print one clear pass/fail result
```

We are choosing Python for practical reasons:

```text
available on developer machines
available in GitHub Actions
easy standard-library subprocess and regex support
no third-party dependency needed
```

This is not a unit-test framework.

It is a structured smoke harness built from the Python standard library.

That keeps the detour small and the repo simple.

### 3. `fstest` becomes the filesystem smoke surface

We will add a dedicated embedded user program named `fstest`.

It will verify:

```text
open
read
seek
stat
missing-path failure
```

That gives us a better split:

```text
fstest  -> filesystem syscall smoke coverage
shell   -> shell and process-control smoke coverage
Makefile/Python -> host-side orchestration and assertions
```

That separation is the main point of the chapter.

---

## 4. Simplify the `Makefile` test targets

Open `Makefile`.

First, extend the embedded user program list:

```make
USER_PROGRAMS := demo counter shell fstest
```

Now change the test targets so they only capture logs.

### Replace the normal test target ending

Keep the QEMU launch exactly as it is, but remove the long `grep` block after it.

The `test` target should end like this:

```make
test: iso $(USER_LIB_OBJS)
	@mkdir -p build
	@rm -f build/test.log
	@timeout 10s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/test.log \
		-no-reboot \
		2>/dev/null || true
	@echo "Captured normal boot log at build/test.log"
```

### Replace the exception target ending

The `test-exception` target should end like this:

```make
test-exception:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_TEST_EXCEPTION
	@mkdir -p build
	@rm -f build/exception.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/exception.log \
		-no-reboot \
		2>/dev/null || true
	@echo "Captured exception log at build/exception.log"
```

### Replace the page-fault target ending

The `test-page-fault` target should end like this:

```make
test-page-fault:
	$(MAKE) clean
	$(MAKE) iso CFLAGS_EXTRA=-DTOYIX_TRIGGER_PAGE_FAULT
	@mkdir -p build
	@rm -f build/pagefault.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/pagefault.log \
		-no-reboot \
		2>/dev/null || true
	@echo "Captured page-fault log at build/pagefault.log"
```

Why do this?

Because build orchestration and output verification are different jobs.

`make` is good at:

```text
build dependencies
rebuild variants
launching tools
capturing artifacts
```

Python is better at:

```text
structured assertions
regex checks
error messages
reusable helpers
```

So we let each tool do the work it is actually good at.

---

## 5. Add the Python smoke harness

Create `tests/smoke.py`:

```python
#!/usr/bin/env python3
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def run(*args: str) -> None:
    print("+", " ".join(args), flush=True)
    subprocess.run(args, cwd=ROOT, check=True)


def require_file(path: pathlib.Path) -> None:
    if not path.is_file():
        raise AssertionError(f"missing expected file: {path.relative_to(ROOT)}")


def read_text(path: pathlib.Path) -> str:
    require_file(path)
    return path.read_text(encoding="utf-8", errors="replace")


def require_contains(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label}: missing {needle!r}")


def require_regex(text: str, pattern: str, label: str) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        raise AssertionError(f"{label}: missing pattern {pattern!r}")
```

These helpers are intentionally small.

We are not building a general-purpose framework.

We are building a test runner that is easy to read in one sitting.

### Add the normal boot verifier

Now add the main boot verification function:

```python
def verify_normal_boot() -> None:
    log = read_text(BUILD / "test.log")

    required_files = [
        BUILD / "toyix.iso",
        BUILD / "kernel.elf",
        BUILD / "user" / "demo.elf",
        BUILD / "user" / "counter.elf",
        BUILD / "user" / "shell.elf",
        BUILD / "user" / "fstest.elf",
        BUILD / "user" / "demo_elf_blob.o",
        BUILD / "user" / "counter_elf_blob.o",
        BUILD / "user" / "shell_elf_blob.o",
        BUILD / "user" / "fstest_elf_blob.o",
        BUILD / "user" / "lib" / "toyix.o",
    ]

    for path in required_files:
        require_file(path)
```

This is one of the reasons Python is better here.

Checking files inside shell is possible.

Checking files plus log strings plus regexes cleanly is much easier in Python.

Then add the expected log checks.

The real file in the repository includes the full list.

The important point is the shape:

```python
    for needle in [
        "Toyix kernel alive",
        "Boot protocol: Multiboot OK",
        "Program registry: registered 4 embedded program(s)",
        "fstest: starting filesystem smoke test",
        "fstest: README first read: Toyix RA",
        "Program test: filesystem user smoke test passed",
        "commands: help, echo, args, cat, stat, run, runbg, jobs, wait, kill, exit",
        "shell: runbg counter pid=5",
        "Program test: user shell cleanup sanity check passed",
    ]:
        require_contains(log, needle, "build/test.log")

    for pattern in [
        r"Syscall: process fstest pid=[0-9]+ exited code 0",
        r"Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=fstest",
        r"Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=shell",
    ]:
        require_regex(log, pattern, "build/test.log")
```

That is much easier to maintain than spreading the same logic across dozens of `grep` lines.

### Add the exception and page-fault verifiers

Now add the two smaller log checks:

```python
def verify_exception() -> None:
    log = read_text(BUILD / "exception.log")

    for needle in [
        "Triggering test exception with UD2",
        "CPU EXCEPTION",
        "Invalid Opcode",
        "KERNEL PANIC",
    ]:
        require_contains(log, needle, "build/exception.log")


def verify_page_fault() -> None:
    log = read_text(BUILD / "pagefault.log")

    for needle in [
        "Paging: enabled with identity map of first 16 MiB",
        "Triggering test page fault at 0xC0000000",
        "PAGE FAULT",
        "Fault address CR2=0xC0000000",
        "KERNEL PANIC",
    ]:
        require_contains(log, needle, "build/pagefault.log")
```

### Add the main entry point

Finish the runner with:

```python
def main() -> int:
    run("make", "clean")
    run("make", "test")
    verify_normal_boot()
    run("make", "test-exception")
    verify_exception()
    run("make", "test-page-fault")
    verify_page_fault()
    print("All Chapter 36.5 checks passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"smoke test failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
```

That gives us:

```text
one place to orchestrate the suite
one place to check logs
one place to report failures
```

That is a major improvement even though the code is still simple.

---

## 6. Keep `tests/smoke.sh` as a compatibility wrapper

We do not need to delete the shell wrapper.

Replace its contents with:

```bash
#!/usr/bin/env bash
set -euo pipefail

python3 tests/smoke.py "$@"
```

Why keep it?

Because people already expect:

```text
tests/smoke.sh
```

to exist.

So we preserve the old entry point while moving the real logic into Python.

That is a pragmatic migration, not a ceremonial cleanup.

---

## 7. Add a dedicated filesystem smoke program

Now we fix the more important problem: testing filesystem behavior through shell-only probes.

Create `user/fstest.c`:

```c
#include "toyix.h"

static const char *file_type_name(toyix_u32 type) {
    switch (type) {
        case TOYIX_FILE_REGULAR:
            return "file";
        case TOYIX_FILE_DIRECTORY:
            return "directory";
        default:
            return "unknown";
    }
}

static void print_chunk(const char *label, const char *buffer, toyix_i32 got) {
    toyix_printf("%s", label);

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_putchar('\n');
}

int main(void) {
    char buffer[16];
    toyix_i32 fd;
    toyix_i32 got;
    toyix_stat_t stat;

    toyix_puts("fstest: starting filesystem smoke test");

    fd = toyix_open("/README", 0);
    if (fd < 0) {
        toyix_puts("fstest: could not open /README");
        return 1;
    }

    got = toyix_read((toyix_u32)fd, buffer, 8u);
    if (got < 0) {
        toyix_puts("fstest: first read failed");
        toyix_close((toyix_u32)fd);
        return 2;
    }

    print_chunk("fstest: README first read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, 0, TOYIX_SEEK_SET) < 0) {
        toyix_puts("fstest: rewind failed");
        toyix_close((toyix_u32)fd);
        return 3;
    }

    got = toyix_read((toyix_u32)fd, buffer, 8u);
    if (got < 0) {
        toyix_puts("fstest: rewind read failed");
        toyix_close((toyix_u32)fd);
        return 4;
    }

    print_chunk("fstest: README rewind read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, 6, TOYIX_SEEK_SET) < 0) {
        toyix_puts("fstest: skip seek failed");
        toyix_close((toyix_u32)fd);
        return 5;
    }

    got = toyix_read((toyix_u32)fd, buffer, 5u);
    if (got < 0) {
        toyix_puts("fstest: skip read failed");
        toyix_close((toyix_u32)fd);
        return 6;
    }

    print_chunk("fstest: README skip read: ", buffer, got);

    if (toyix_close((toyix_u32)fd) != 0) {
        toyix_puts("fstest: close failed");
        return 7;
    }

    if (toyix_stat("/README", &stat) != 0) {
        toyix_puts("fstest: stat failed for /README");
        return 8;
    }

    toyix_printf(
        "fstest: README type=%s size=%u\n",
        file_type_name(stat.type),
        stat.size
    );

    if (toyix_stat("/programs", &stat) != 0) {
        toyix_puts("fstest: stat failed for /programs");
        return 9;
    }

    toyix_printf(
        "fstest: /programs type=%s size=%u\n",
        file_type_name(stat.type),
        stat.size
    );

    if (toyix_stat("/missing", &stat) == 0) {
        toyix_puts("fstest: missing path unexpectedly succeeded");
        return 10;
    }

    toyix_puts("fstest: missing path check passed");
    toyix_puts("fstest: completed filesystem smoke test");
    return 0;
}
```

This is better than testing filesystem APIs through the shell for three reasons:

### 1. The test surface matches the API surface

`fstest` calls:

```text
toyix_open
toyix_read
toyix_seek
toyix_close
toyix_stat
```

directly.

That is the interface we actually want to validate.

### 2. Output is deterministic

The program emits fixed, grep-friendly lines like:

```text
fstest: README first read: Toyix RA
fstest: /programs type=file size=...
fstest: missing path check passed
```

That is exactly what a smoke harness wants.

### 3. The shell no longer needs test-only commands

This is the real cleanup.

We are removing a fake testing burden from the shell instead of making the shell absorb more of it.

---

## 8. Register and run `fstest`

Open `kernel/program.c`.

### Add the embedded program declaration

Add:

```c
DECLARE_EMBEDDED_PROGRAM(fstest);
```

### Add it to the registry

Insert:

```c
EMBEDDED_PROGRAM(
    fstest,
    "fstest",
    "user-mode filesystem smoke test"
)
```

The registry now reports four embedded programs instead of three.

### Launch it as part of `program_test_once()`

Add:

```c
static const char *fstest_argv[] = {
    "fstest"
};
```

Then, after the background counter cleanup test, add:

```c
console_writeln("Program test: starting filesystem user test");

rc = program_run_foreground("fstest", 1, fstest_argv, &exit_code);
if (rc != 0 || exit_code != 0u) {
    kernel_panic("program test received wrong fstest result");
}

console_writeln("Program test: filesystem user smoke test passed");
```

This is an important design choice.

We launch `fstest` directly from the kernel-side test flow instead of making the shell launch it.

That keeps the filesystem smoke test independent from shell parsing and shell process-control logic.

Again, separation of concerns is the point.

---

## 9. Stop using the shell as the filesystem smoke test

Open `kernel/program.c` again and trim the scripted shell input.

Remove these old test lines:

```c
inject_text("cat /README\n");
inject_text("cat /programs\n");
inject_text("stat /README\n");
inject_text("stat /programs\n");
inject_text("stat /missing\n");
inject_text("seektest /README\n");
```

The shell test should stay focused on shell behavior:

```text
help
echo
run
runbg
jobs
kill
wait
args
exit
```

Also update the hard-coded background PID commands:

```c
inject_text("kill 5\n");
inject_text("wait 5\n");
```

Why did this change?

Because `fstest` now runs before the shell launch, which consumes another PID.

The shell is created later, its foreground `run counter ...` child gets one PID, and the background `runbg counter victim` child gets the next one.

For this scripted test path, that background child is now PID 5.

---

## 10. Remove the test-only shell command

Open `user/shell.c`.

We are not removing real features like `cat` or `stat`.

We are removing the testing shortcut:

```text
seektest
```

### Update help text

Change:

```c
toyix_puts("commands: help, echo, args, cat, seektest, stat, run, runbg, jobs, wait, kill, exit");
```

to:

```c
toyix_puts("commands: help, echo, args, cat, stat, run, runbg, jobs, wait, kill, exit");
```

### Remove the helper and command

Delete:

```text
print_chunk()
cmd_seektest()
```

and remove the dispatch branch:

```c
if (toyix_streq(cmd_argv[0], "seektest")) {
    cmd_seektest(cmd_argc, cmd_argv);
    continue;
}
```

This is the most visible sign that the testing strategy changed.

The shell is no longer carrying a synthetic filesystem test command just because the harness needed one.

---

## 11. Point CI at the Python harness

Open `.github/workflows/ci.yml`.

Replace:

```yaml
- name: Run smoke test
  run: |
    export PATH="${RUNNER_TOOL_CACHE}/i686-elf/bin:${PATH}"
    tests/smoke.sh
```

with:

```yaml
- name: Run smoke test
  run: |
    export PATH="${RUNNER_TOOL_CACHE}/i686-elf/bin:${PATH}"
    python3 tests/smoke.py
```

Why call Python directly in CI if `tests/smoke.sh` still exists?

Because the shell script is now compatibility glue.

The Python runner is the real harness.

CI should call the real harness.

---

## 12. Update the docs

Now update the repository-facing docs so they match the new testing shape.

### `README.md`

Update:

```text
tests badge link
feature list
test commands
documentation index
```

Important behavior changes to describe:

```text
make test captures logs
python3 tests/smoke.py runs the full suite
tests/smoke.sh is still available as a wrapper
fstest exists as a dedicated filesystem smoke program
```

### `CHANGELOG.md`

Add unreleased notes for:

```text
host-side Python smoke harness
fstest
log-capture-only make test targets
Chapter 36.5 documentation
```

### `index.md` and `docs/roadmap.md`

Add Chapter 36.5 to the published chapter lists.

The roadmap should also note that the current system now includes a structured host-side smoke harness.

This is a minor detour, not a renumbering event.

Chapter 37 still comes next.

---

## 13. Run the suite

The full suite is now:

```sh
python3 tests/smoke.py
```

You can still run:

```sh
tests/smoke.sh
```

and the lower-level capture targets still work individually:

```sh
make test
make test-exception
make test-page-fault
```

That is another deliberate choice.

We did not remove useful entry points.

We changed their responsibilities:

```text
make targets  -> produce logs
Python runner -> verify logs
```

That division will scale much better as we add directories, `ls`, executable metadata, path lookup, and later writable filesystems.

---

## 14. Why these choices were the right ones

It is worth being explicit about the design tradeoffs.

### Why not keep everything in shell?

Because shell `grep` chains become noisy and fragile once the suite has:

```text
artifact checks
exact string checks
regex checks
multiple logs
multiple build variants
```

At that point we are pretending shell is a test framework when it is really just a command runner.

### Why not adopt `pytest` or another framework?

Because that would solve a problem we do not have yet.

Right now we need:

```text
subprocess
path handling
string checks
regex checks
clear failures
```

The Python standard library already gives us all of that.

Keeping the harness dependency-free matters for:

```text
local setup
CI simplicity
tutorial clarity
```

### Why add `fstest` instead of more shell built-ins?

Because `fstest` tests filesystem syscalls directly.

That is the right abstraction boundary.

The shell should not turn into a museum of one-off diagnostics.

### Why keep `cat` and `stat` in the shell at all?

Because they are real user features.

We removed only the test-only feature.

That distinction matters.

We are cleaning up the boundary, not gutting userland.

---

## 15. What this prepares us for

The next chapters are going to make the filesystem story more complex, not less:

```text
directories
directory listings
/programs as a real directory
path-based program launch
later executable metadata
```

A better smoke harness now means those chapters can add focused expectations without turning the shell or the `Makefile` into a mess.

That is why this detour is worth taking before Chapter 37.

---

## Resources

- [Chapter source: Toyix repository](https://github.com/Monotoba/toyix)

## Closure

Chapter 36.5 is a deliberate pause in the filesystem narrative, but it is the right pause.

Toyix now has a cleaner testing split:

```text
host-side Python harness
log-producing make targets
dedicated filesystem smoke program
shell tests that focus on shell behavior
```

That is a better foundation for the chapters ahead.

Next time we will resume our regularly scheduled programming and turn `/programs` into a real directory.

Happy Coding!
