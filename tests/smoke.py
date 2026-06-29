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

    for needle in [
        "Toyix kernel alive",
        "Boot protocol: Multiboot OK",
        "GDT: installed kernel/user segments and TSS",
        "PMM: parsing Multiboot memory map",
        "PMM test: allocation/free sanity check passed",
        "Paging: enabled with identity map of first 16 MiB",
        "Paging test: identity-mapped kernel data is readable/writable",
        "Address space: kernel address space registered",
        "Heap: initialized virtual heap with 4 page(s)",
        "Heap test: VMM-backed allocation/free sanity check passed",
        "Threads: blocking scheduler initialized",
        "Process: process table initialized",
        "VFS: initialized RAMFS with 2 file(s)",
        "VFS test: starting RAMFS open/read/seek/stat/close test",
        "VFS test: /README size=",
        "VFS test: first bytes: Toyix RA",
        "VFS test: rewind bytes: Toyix RA",
        "VFS test: seek bytes: RAMFS",
        "VFS test: RAMFS stat/seek sanity check passed",
        "Thread test: worker A step 0",
        "Thread test: worker B step 0",
        "Thread test: completed software-yield multitasking test",
        "Threads: preemption enabled, slice ticks=2",
        "Preempt test: timer-driven preemption sanity check passed",
        "Sleep test: blocking sleep sanity check passed",
        "Keyboard: IRQ1 handler, modifiers, and input buffer installed",
        "Console: output mutex enabled",
        "Sync test: mutex/semaphore sanity check passed",
        "Console lock test: non-interleaved line output sanity check passed",
        "Keyboard test: blocking input-buffer sanity check passed",
        "Terminal test: readline/backspace sanity check passed",
        "Monitor test: command table sanity check passed",
        "Program registry: registered 4 embedded program(s)",
        "Embedded programs:",
        "demo - interactive stdin/stdout demo",
        "counter - background-safe counter demo",
        "shell - interactive user-mode shell",
        "fstest - user-mode filesystem smoke test",
        "Program test: starting background counter test",
        "Address space: created process page directory",
        "ELF32: loaded PT_LOAD vaddr=0x40100000",
        "ELF32: entry=0x40100000",
        "Process: initial stack argc=3",
        "Program: launching counter argc=3 ppid=0",
        "Process: created pid=1 name=counter",
        "Program test: background pid=1",
        "PID  PPID STATE",
        "zombie",
        "counter: argc=",
        "counter: argv[0]=counter",
        "counter: argv[1]=alpha",
        "counter: argv[2]=beta",
        "counter: tick 1",
        "counter: tick 2",
        "counter: tick 3",
        "Program test: background counter cleanup sanity check passed",
        "Program test: starting filesystem user test",
        "Program: launching fstest argc=1 ppid=0",
        "fstest: starting filesystem smoke test",
        "fstest: README first read: Toyix RA",
        "fstest: README rewind read: Toyix RA",
        "fstest: README skip read: RAMFS",
        "fstest: README type=file",
        "fstest: /programs type=file size=",
        "fstest: missing path check passed",
        "fstest: completed filesystem smoke test",
        "Program test: filesystem user smoke test passed",
        "Program test: starting user shell test",
        "Program: launching shell argc=3 ppid=0",
        "shell: Toyix user shell",
        "shell: startup argc=3",
        "shell: argv[0]=shell",
        "shell: argv[1]=alpha",
        "shell: argv[2]=beta",
        "commands: help, echo, args, cat, stat, run, runbg, jobs, wait, kill, exit",
        "hello from shell",
        "Program: launching counter argc=3 ppid=",
        "shell: run counter pid=",
        "shell: counter exited code 4",
        "shell: runbg counter pid=5",
        "shell jobs:",
        "state=running",
        "shell: kill requested pid=5",
        "state=zombie code=128",
        "shell: wait pid=5 name=counter code=128",
        "  none",
        "argc=3",
        "argv[0]=shell",
        "argv[1]=alpha",
        "argv[2]=beta",
        "Syscall: process shell pid=",
        "exited code 7",
        "Program test: user shell cleanup sanity check passed",
        "Monitor: monitor thread started",
        "Interrupts: enabled",
        "Timer: observed 3 ticks",
        "VMM: initialized kernel address-space mapper",
        "VMM test: map/translate/write/unmap sanity check passed",
    ]:
        require_contains(log, needle, "build/test.log")

    for pattern in [
        r"Syscall: process counter pid=1 exited code 4",
        r"Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=counter",
        r"Syscall: process fstest pid=[0-9]+ exited code 0",
        r"Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=fstest",
        r"Syscall: process counter pid=[0-9]+ exited code 4",
        r"Syscall: process counter pid=5 exited code 128",
        r"Process: destroyed pid=[0-9]+ ppid=[0-9]+ name=shell",
    ]:
        require_regex(log, pattern, "build/test.log")


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
