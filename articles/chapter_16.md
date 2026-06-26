# Chapter 16 — Entering User Mode and Returning Through Syscalls

We now have an interactive, preemptive kernel with memory management, blocking, synchronization, keyboard input, terminal editing, and a monitor.

So far, however, everything runs in **ring 0**.

That means every thread has full kernel privilege. There is no separation between kernel code and user code yet.

This chapter adds the first user-mode milestone:

```text
ring 0 kernel thread
  ↓
prepare user code page and user stack page
  ↓
IRET into ring 3
  ↓
user code executes
  ↓
user code calls int 0x80
  ↓
kernel syscall handler runs
  ↓
user code exits through syscall
```

Intel’s IA-32 system programming manuals are the architectural source for protected-mode privilege levels, task management, interrupts/exceptions, paging, and the `IRET`-based privilege transition machinery we are using here. Intel describes Volume 3 as covering operating-system support including protection, task management, memory management, and interrupt/exception handling. ([Intel][1])

---

# 1. What this chapter adds

Add:

```text
arch/x86/
├── syscall.asm
└── user_enter.asm

include/kernel/
├── syscall.h
└── usermode.h

kernel/
├── syscall.c
└── usermode.c
```

Modify:

```text
arch/x86/gdt.h
arch/x86/gdt.c
arch/x86/gdt_flush.asm
arch/x86/idt.c
arch/x86/interrupts.h
kernel/thread.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

The new milestone:

```text
User mode test: entering ring 3
U3
Syscall: user thread requested exit
User mode test: ring 3 syscall/exit sanity check passed
```

The `U3` comes from ring-3 code making `int 0x80` syscalls.

---

# 2. Why user mode needs more than paging

Paging alone is not enough.

We already have virtual memory, but we have not yet used the page-table `USER` bit meaningfully. We also need segmentation privilege levels and a safe kernel stack for privilege transitions.

To enter ring 3, we need:

```text
user code segment selector, RPL 3
user data segment selector, RPL 3
user-accessible code page
user-accessible stack page
TSS with SS0/ESP0 for kernel entry
syscall interrupt gate callable from ring 3
```

The TSS is especially important. When ring-3 code traps into the kernel through an interrupt gate, the CPU switches to the ring-0 stack described by the TSS. Without a valid ring-0 stack, the first syscall or interrupt from user mode will crash.

---

# 3. Update `arch/x86/gdt.h`

Replace the current file with this version.

```c
// arch/x86/gdt.h
#ifndef TOYIX_ARCH_X86_GDT_H
#define TOYIX_ARCH_X86_GDT_H

#include <stdint.h>

#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_KERNEL_DATA_SELECTOR 0x10u

#define X86_USER_CODE_SELECTOR   0x1Bu
#define X86_USER_DATA_SELECTOR   0x23u

#define X86_TSS_SELECTOR         0x28u

void gdt_init(void);

void tss_set_kernel_stack(uint32_t esp0);

#endif
```

## Selector values

The selector layout is:

```text
bits 15..3 = GDT index
bit 2      = table indicator
bits 1..0  = requested privilege level
```

So:

```text
GDT index 3 user code = 0x18 | 3 = 0x1B
GDT index 4 user data = 0x20 | 3 = 0x23
GDT index 5 TSS       = 0x28
```

Ring-3 selectors need the low bits set to `3`.

---

# 4. Update `arch/x86/gdt_flush.asm`

Append `tss_flush`.

```asm
; arch/x86/gdt_flush.asm

BITS 32

global gdt_flush
global tss_flush

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.reload_cs

.reload_cs:
    ret

tss_flush:
    mov ax, [esp + 4]
    ltr ax
    ret
```

`LTR` loads the task register with the TSS selector. We are not using hardware task switching. We are using the TSS only for ring transitions, especially `SS0` and `ESP0`.

---

# 5. Replace `arch/x86/gdt.c`

```c
// arch/x86/gdt.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/console.h"
#include "kernel/string.h"

#define GDT_ENTRY_COUNT 6

typedef struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct gdt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_pointer_t;

typedef struct tss_entry {
    uint32_t prev_tss;

    uint32_t esp0;
    uint32_t ss0;

    uint32_t esp1;
    uint32_t ss1;

    uint32_t esp2;
    uint32_t ss2;

    uint32_t cr3;

    uint32_t eip;
    uint32_t eflags;

    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;

    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;

    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;

    uint32_t ldt_selector;

    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

static gdt_entry_t gdt[GDT_ENTRY_COUNT];
static gdt_pointer_t gdt_ptr;
static tss_entry_t tss;

extern void gdt_flush(uint32_t gdt_pointer_addr);
extern void tss_flush(uint32_t selector);

static void gdt_set_entry(
    uint32_t index,
    uint32_t base,
    uint32_t limit,
    uint8_t access,
    uint8_t granularity
) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFFu);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFFu);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFFu);

    gdt[index].limit_low = (uint16_t)(limit & 0xFFFFu);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0Fu);

    gdt[index].granularity =
        (uint8_t)(gdt[index].granularity | (granularity & 0xF0u));

    gdt[index].access = access;
}

static void tss_init(void) {
    memset(&tss, 0, sizeof(tss));

    tss.ss0 = X86_KERNEL_DATA_SELECTOR;
    tss.esp0 = 0;

    /*
     * Set I/O map base beyond the TSS limit. This means there is no I/O
     * permission bitmap present, so user mode cannot perform raw port I/O.
     */
    tss.iomap_base = sizeof(tss);

    uint32_t base = (uint32_t)&tss;
    uint32_t limit = sizeof(tss) - 1u;

    /*
     * Access byte 0x89:
     *   present
     *   DPL 0
     *   available 32-bit TSS
     */
    gdt_set_entry(5, base, limit, 0x89u, 0x00u);
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base = (uint32_t)&gdt[0];

    /*
     * 0: null descriptor
     */
    gdt_set_entry(0, 0, 0, 0, 0);

    /*
     * 1: kernel code, ring 0
     * 2: kernel data, ring 0
     */
    gdt_set_entry(1, 0, 0xFFFFFu, 0x9Au, 0xCFu);
    gdt_set_entry(2, 0, 0xFFFFFu, 0x92u, 0xCFu);

    /*
     * 3: user code, ring 3
     * 4: user data, ring 3
     */
    gdt_set_entry(3, 0, 0xFFFFFu, 0xFAu, 0xCFu);
    gdt_set_entry(4, 0, 0xFFFFFu, 0xF2u, 0xCFu);

    /*
     * 5: TSS
     */
    tss_init();

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush(X86_TSS_SELECTOR);

    console_writeln("GDT: installed kernel/user segments and TSS");
}
```

## Why the TSS matters

For this tutorial kernel, the TSS is not used for hardware task switching.

We use software scheduling.

But the CPU still consults the TSS when transitioning from ring 3 to ring 0 through an interrupt or exception. The fields we care about are:

```text
SS0  = kernel data selector
ESP0 = kernel stack pointer to use after entering ring 0
```

Every user-capable thread needs a valid kernel stack top in the TSS before it can safely run in ring 3.

---

# 6. Update `arch/x86/interrupts.h`

Add a syscall vector definition.

```c
#define X86_SYSCALL_INTERRUPT_VECTOR 0x80u
```

The top of the file should now include:

```c
#define X86_SCHED_INTERRUPT_VECTOR 48u
#define X86_SYSCALL_INTERRUPT_VECTOR 0x80u
```

We use `0x80` because it is the classic software interrupt number used by older Unix-like x86 systems for syscalls. We are not trying to be Linux-compatible. We are only borrowing a familiar convention.

---

# 7. Add `arch/x86/syscall.asm`

```asm
; arch/x86/syscall.asm
;
; int 0x80 syscall entry.
;
; The saved frame layout matches interrupt_frame_t for the portion used by C.
; If the syscall came from ring 3, the CPU also pushed user ESP and user SS
; below EFLAGS. IRETD will consume those automatically when returning to ring 3.

BITS 32

extern syscall_handler

global syscall_stub

syscall_stub:
    push dword 0
    push dword 128

    pusha

    xor eax, eax
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call syscall_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa

    add esp, 8

    iretd
```

The syscall handler can modify saved registers through the frame. For example, if it changes `frame->eax`, `popa` restores that value into user `EAX`.

---

# 8. Add `arch/x86/user_enter.asm`

```asm
; arch/x86/user_enter.asm
;
; C prototype:
;
;   void x86_enter_user_mode(uint32_t user_eip, uint32_t user_esp);
;
; Does not return.

BITS 32

global x86_enter_user_mode

x86_enter_user_mode:
    mov eax, [esp + 4]      ; user EIP
    mov edx, [esp + 8]      ; user ESP

    mov cx, 0x23            ; user data selector, RPL 3
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push dword 0x23         ; user SS
    push edx                ; user ESP

    pushfd
    pop ecx
    or ecx, 0x00000200      ; IF = 1
    push ecx                ; user EFLAGS

    push dword 0x1B         ; user CS
    push eax                ; user EIP

    iretd
```

This performs the actual transition to ring 3.

The `IRETD` frame contains:

```text
SS
ESP
EFLAGS
CS
EIP
```

Because the target `CS` has RPL 3, the CPU performs a privilege transition.

---

# 9. Update `arch/x86/idt.c`

Add:

```c
extern void syscall_stub(void);
```

Then install vector `0x80` with DPL 3:

```c
idt_set_gate(
    X86_SYSCALL_INTERRUPT_VECTOR,
    (uint32_t)syscall_stub,
    X86_KERNEL_CODE_SELECTOR,
    0xEEu
);
```

The `0xEE` flags are important:

```text
0x80 = present
0x60 = DPL 3
0x0E = 32-bit interrupt gate
```

The scheduler interrupt remains DPL 0. User code should not be able to call `int 0x30`. But user code is allowed to call `int 0x80`.

Update the final message:

```c
console_writeln("IDT: installed exceptions, IRQs, scheduler interrupt, and syscall gate");
```

---

# 10. Add `include/kernel/syscall.h`

```c
// include/kernel/syscall.h
#ifndef TOYIX_KERNEL_SYSCALL_H
#define TOYIX_KERNEL_SYSCALL_H

#include "arch/x86/interrupts.h"

#define SYS_PUTC 1u
#define SYS_EXIT 2u

void syscall_handler(interrupt_frame_t *frame);

#endif
```

---

# 11. Add `kernel/syscall.c`

```c
// kernel/syscall.c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"

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

        case SYS_EXIT: {
            uint32_t exit_code = frame->ebx;

            console_writeln("");
            console_write("Syscall: user thread requested exit code ");
            console_write_u32_dec(exit_code);
            console_putc('\n');

            usermode_notify_exit(exit_code);

            thread_exit();

            /*
             * thread_exit() is noreturn.
             */
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

## Why `SYS_EXIT` calls `thread_exit()`

Our first user program runs inside a kernel-managed thread.

When user code calls:

```asm
mov eax, SYS_EXIT
int 0x80
```

the kernel handles that as:

```text
mark user test done
turn current thread into zombie
switch away
```

That is not yet a full process model, but it is enough for the first ring-3 milestone.

---

# 12. Add `include/kernel/usermode.h`

```c
// include/kernel/usermode.h
#ifndef TOYIX_KERNEL_USERMODE_H
#define TOYIX_KERNEL_USERMODE_H

#include <stdint.h>

void usermode_test_once(void);
void usermode_notify_exit(uint32_t exit_code);

#endif
```

---

# 13. Add `kernel/usermode.c`

```c
// kernel/usermode.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"
#include "kernel/vmem.h"

#define USER_TEST_CODE_VA   0x40000000u
#define USER_TEST_STACK_VA  0x40001000u
#define USER_TEST_STACK_TOP 0x40002000u

extern void x86_enter_user_mode(uint32_t user_eip, uint32_t user_esp);

static volatile uint32_t usermode_test_done;
static volatile uint32_t usermode_exit_code;
static int user_pages_mapped;

/*
 * Tiny ring-3 program:
 *
 *   syscall putc 'U'
 *   syscall putc '3'
 *   syscall putc '\n'
 *   syscall exit 0
 *   loop forever if exit returns
 */
static const uint8_t user_test_program[] = {
    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,  // mov eax, SYS_PUTC
    0xBB, 'U',      0x00, 0x00, 0x00,  // mov ebx, 'U'
    0xCD, 0x80,                         // int 0x80

    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,  // mov eax, SYS_PUTC
    0xBB, '3',      0x00, 0x00, 0x00,  // mov ebx, '3'
    0xCD, 0x80,                         // int 0x80

    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,  // mov eax, SYS_PUTC
    0xBB, '\n',     0x00, 0x00, 0x00,  // mov ebx, '\n'
    0xCD, 0x80,                         // int 0x80

    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,  // mov eax, SYS_EXIT
    0x31, 0xDB,                         // xor ebx, ebx
    0xCD, 0x80,                         // int 0x80

    0xEB, 0xFE                          // jmp $
};

static void map_user_test_pages(void) {
    if (user_pages_mapped) {
        return;
    }

    uintptr_t code_phys = pmm_alloc_page();
    uintptr_t stack_phys = pmm_alloc_page();

    if (code_phys == PMM_INVALID_PAGE ||
        stack_phys == PMM_INVALID_PAGE) {
        kernel_panic("user mode test could not allocate physical pages");
    }

    int rc = vmem_map_page(
        USER_TEST_CODE_VA,
        code_phys,
        VMEM_FLAG_WRITABLE | VMEM_FLAG_USER
    );

    if (rc != VMEM_OK) {
        kernel_panic("user mode test could not map code page");
    }

    rc = vmem_map_page(
        USER_TEST_STACK_VA,
        stack_phys,
        VMEM_FLAG_WRITABLE | VMEM_FLAG_USER
    );

    if (rc != VMEM_OK) {
        kernel_panic("user mode test could not map stack page");
    }

    memset((void *)USER_TEST_CODE_VA, 0x90, PMM_PAGE_SIZE);
    memcpy((void *)USER_TEST_CODE_VA, user_test_program, sizeof(user_test_program));

    memset((void *)USER_TEST_STACK_VA, 0, PMM_PAGE_SIZE);

    user_pages_mapped = 1;
}

static uint32_t current_thread_kernel_stack_top(void) {
    thread_t *self = thread_current();

    if (self == 0 || self->stack_base == 0) {
        kernel_panic("user mode entry requires current thread stack");
    }

    return (uint32_t)((uintptr_t)self->stack_base + self->stack_size);
}

static void user_test_thread(void *arg) {
    (void)arg;

    console_writeln("User mode test: entering ring 3");

    /*
     * The CPU uses ESP0 from the TSS when ring-3 code traps into ring 0.
     * Point it at the current thread's kernel stack top.
     */
    tss_set_kernel_stack(current_thread_kernel_stack_top());

    x86_enter_user_mode(USER_TEST_CODE_VA, USER_TEST_STACK_TOP);

    kernel_panic("x86_enter_user_mode returned unexpectedly");
}

void usermode_notify_exit(uint32_t exit_code) {
    usermode_exit_code = exit_code;
    usermode_test_done = 1;
}

void usermode_test_once(void) {
    console_writeln("User mode test: preparing ring 3 program");

    usermode_test_done = 0;
    usermode_exit_code = 0xFFFFFFFFu;

    map_user_test_pages();

    thread_create("user-test", user_test_thread, 0);

    while (!usermode_test_done) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (usermode_exit_code != 0) {
        kernel_panic("user mode test returned nonzero exit code");
    }

    console_writeln("User mode test: ring 3 syscall/exit sanity check passed");
}
```

## Why code and stack are mapped at `0x40000000`

We deliberately avoid:

```text
low identity map
kernel heap region
temporary VMM test region
higher-half kernel candidate region
```

This page is user-accessible because it is mapped with:

```c
VMEM_FLAG_USER
```

Without the user flag, the CPU would page fault when ring-3 code tried to execute or use the page.

---

# 14. Update `kernel/thread.c` to refresh TSS ESP0

When the scheduler switches to a thread that has its own kernel stack, update the TSS kernel stack pointer.

Add this helper near the other static helpers:

```c
static uint32_t thread_kernel_stack_top(thread_t *thread) {
    if (thread == 0 || thread->stack_base == 0) {
        return 0;
    }

    return (uint32_t)((uintptr_t)thread->stack_base + thread->stack_size);
}
```

Then in `thread_schedule_from_interrupt()`, after selecting `next_thread` and before returning its saved ESP, add:

```c
uint32_t next_stack_top = thread_kernel_stack_top(next_thread);

if (next_stack_top != 0) {
    tss_set_kernel_stack(next_stack_top);
}
```

The end of `thread_schedule_from_interrupt()` should look like this:

```c
next_thread->state = THREAD_RUNNING;
current_thread = next_thread;

uint32_t next_stack_top = thread_kernel_stack_top(next_thread);

if (next_stack_top != 0) {
    tss_set_kernel_stack(next_stack_top);
}

current_slice_ticks = 0;
reschedule_requested = 0;

return (uintptr_t)next_thread->context.esp;
```

## Why this is necessary

Timer interrupts can occur while ring-3 code is running.

When that happens, the CPU needs a kernel stack. It gets that stack from the TSS.

As the scheduler changes the current thread, we must keep the TSS stack pointer consistent with the thread being resumed.

This is still a simple model because every thread has one kernel stack.

---

# 15. Update `kernel/kmain.c`

Add:

```c
#include "kernel/usermode.h"
```

Then call:

```c
usermode_test_once();
```

after the monitor command test and before starting the interactive monitor.

Here is the relevant section:

```c
terminal_init();
terminal_test_once();

monitor_init();
monitor_test_once();

usermode_test_once();

monitor_start();

pit_wait_ticks(3);
console_writeln("Timer: observed 3 ticks");

console_writeln("Interactive kernel monitor is running.");
console_writeln("Try: help, help sleep, ticks, threads, mem, heap, echo Hello!");
```

The full file is otherwise the same as Chapter 15.

---

# 16. Update `Makefile`

Add these objects:

```text
build/arch/x86/syscall.o
build/arch/x86/user_enter.o
build/kernel/syscall.o
build/kernel/usermode.o
```

The relevant object list becomes:

```make
OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging_asm.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/idle.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/sync.o \
    build/kernel/syscall.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the test greps:

```make
	grep -q "GDT: installed kernel/user segments and TSS" build/test.log
	grep -q "User mode test: preparing ring 3 program" build/test.log
	grep -q "User mode test: entering ring 3" build/test.log
	grep -q "U3" build/test.log
	grep -q "Syscall: user thread requested exit code 0" build/test.log
	grep -q "User mode test: ring 3 syscall/exit sanity check passed" build/test.log
```

The full `test` target should include:

```make
test: iso
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
	grep -q "Toyix kernel alive" build/test.log
	grep -q "Boot protocol: Multiboot OK" build/test.log
	grep -q "GDT: installed kernel/user segments and TSS" build/test.log
	grep -q "PMM: parsing Multiboot memory map" build/test.log
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
	grep -q "Paging test: identity-mapped kernel data is readable/writable" build/test.log
	grep -q "Heap: initialized virtual heap with 4 page(s)" build/test.log
	grep -q "Heap test: VMM-backed allocation/free sanity check passed" build/test.log
	grep -q "Threads: blocking scheduler initialized" build/test.log
	grep -q "Thread test: worker A step 0" build/test.log
	grep -q "Thread test: worker B step 0" build/test.log
	grep -q "Thread test: completed software-yield multitasking test" build/test.log
	grep -q "Threads: preemption enabled, slice ticks=2" build/test.log
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
	grep -q "Sleep test: blocking sleep sanity check passed" build/test.log
	grep -q "Keyboard: IRQ1 handler, modifiers, and input buffer installed" build/test.log
	grep -q "Console: output mutex enabled" build/test.log
	grep -q "Sync test: mutex/semaphore sanity check passed" build/test.log
	grep -q "Console lock test: non-interleaved line output sanity check passed" build/test.log
	grep -q "Keyboard test: blocking input-buffer sanity check passed" build/test.log
	grep -q "Terminal test: readline/backspace sanity check passed" build/test.log
	grep -q "Monitor test: command table sanity check passed" build/test.log
	grep -q "User mode test: preparing ring 3 program" build/test.log
	grep -q "User mode test: entering ring 3" build/test.log
	grep -q "U3" build/test.log
	grep -q "Syscall: user thread requested exit code 0" build/test.log
	grep -q "User mode test: ring 3 syscall/exit sanity check passed" build/test.log
	grep -q "Monitor: monitor thread started" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	grep -q "VMM: initialized kernel address-space mapper" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	@echo "Boot, memory, heap, sync, monitor, and user-mode syscall smoke test passed."
```

---

# 17. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 16 checks passed."
```

---

# 18. Expected output

A successful boot should include:

```text
GDT: installed kernel/user segments and TSS
...
User mode test: preparing ring 3 program
Thread: created user-test id=...
User mode test: entering ring 3
U3

Syscall: user thread requested exit code 0
Thread: exiting user-test id=...
Threads: reaping zombie user-test id=...
User mode test: ring 3 syscall/exit sanity check passed
Monitor: monitor thread started
Timer: observed 3 ticks
Interactive kernel monitor is running.
```

The important milestone is:

```text
U3
Syscall: user thread requested exit code 0
```

That means ring-3 code executed and trapped back into ring 0 through `int 0x80`.

---

# 19. Common failures

## Failure: triple fault immediately after entering user mode

Likely causes:

```text
TSS not loaded with LTR
TSS ESP0 is zero
user stack page not mapped user-accessible
user code page not mapped user-accessible
bad user CS/SS selectors
bad IRET frame order
```

Check:

```c
tss_flush(X86_TSS_SELECTOR);
tss_set_kernel_stack(current_thread_kernel_stack_top());
```

and make sure the user selectors are:

```text
CS = 0x1B
SS = 0x23
```

## Failure: page fault at `0x40000000`

The code page was not mapped with the user flag.

Check:

```c
vmem_map_page(
    USER_TEST_CODE_VA,
    code_phys,
    VMEM_FLAG_WRITABLE | VMEM_FLAG_USER
);
```

Also verify that the generic VMEM layer translates `VMEM_FLAG_USER` to `X86_PAGE_USER`.

## Failure: `int 0x80` causes general protection fault

Likely cause:

```text
syscall gate DPL is not 3
```

The syscall gate must use:

```c
0xEE
```

not:

```c
0x8E
```

A DPL-0 gate cannot be called directly from ring 3.

## Failure: user code prints `U3` but never exits

Check the machine-code bytes for:

```asm
mov eax, SYS_EXIT
xor ebx, ebx
int 0x80
```

and confirm `SYS_EXIT` is `2`.

Also confirm the syscall handler calls:

```c
usermode_notify_exit(exit_code);
thread_exit();
```

## Failure: timer interrupt crashes while user code is running

Likely cause:

```text
TSS ESP0 does not match the current thread's kernel stack
```

Make sure the scheduler updates the TSS when switching threads:

```c
tss_set_kernel_stack(next_stack_top);
```

---

# 20. What this chapter achieved

We now have:

```text
ring 0 kernel
ring 3 user code
TSS ring transition stack
user-accessible pages
int 0x80 syscall gate
syscall handler
user exit path
```

The operating system has crossed a major boundary.

Before this chapter:

```text
all code was kernel code
```

After this chapter:

```text
the kernel can run less-privileged code and regain control through syscalls
```

That is one of the defining features of a real OS.

---

# 21. What is still missing

This is only the first user-mode milestone.

We do not yet have:

```text
process objects
per-process page directories
ELF loading
user heap
user stack growth
file descriptors
copy_from_user()
copy_to_user()
sys_read()
sys_write()
sys_exit() for real processes
user-mode fault cleanup
process wait()
```

Also, the user program is currently raw machine code copied into a page.

That is acceptable for the first test. The next major step is to load a real executable format.

---

# 22. Next chapter

The next chapter should introduce a minimal process abstraction:

```text
process_t
address space ownership
user thread association
process exit status
copy_from_user()
copy_to_user()
```

Then we can add:

```text
SYS_WRITE
SYS_EXIT
SYS_SLEEP
```

and eventually:

```text
ELF loader
init process
user shell
```

[1]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel® 64 and IA-32 Architectures"

---

# 23. Resources

- [Chapter 16 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_16)
- [Chapter 16 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_16)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki: Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- [OSDev Wiki: System Calls](https://wiki.osdev.org/System_Calls)

---

# 24. Closure

The kernel can now prepare user-accessible pages, enter ring 3 with `IRETD`, return through an `int 0x80` syscall gate, and let the user thread exit cleanly. That is the first working boundary between kernel code and less-privileged code.

Happy Coding!
