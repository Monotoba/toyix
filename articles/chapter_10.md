# Chapter 10 — Timer-Driven Preemptive Multitasking

In Chapter 9, we built cooperative multitasking:

```text
thread A runs
  ↓
thread A voluntarily calls thread_yield()
  ↓
scheduler switches to thread B
```

That works, but it has a major weakness:

```c
for (;;) {
    // never yields
}
```

A thread like that can monopolize the CPU forever.

This chapter adds the next step: **timer-driven preemption**.

With preemptive scheduling:

```text
PIT timer interrupt fires
  ↓
kernel timer handler runs
  ↓
scheduler decides time slice expired
  ↓
interrupt return path resumes a different thread
```

OSDev describes preemptive multitasking as scheduling where an interrupt, usually timer-driven, allows the kernel to take control away from the currently running task rather than waiting for it to yield voluntarily. OSDev’s context-switching material also emphasizes that switching means saving one execution context and restoring another. ([OSDev][1])

This chapter is a meaningful refactor. We will stop using the Chapter 9 `x86_context_switch()` function and instead use an **interrupt-frame-based scheduler**.

That gives us one unified switch path for:

```text
software yield interrupt
timer IRQ preemption
thread exit
```

---

# 1. Why we are changing the switch model

Chapter 9 used this model:

```text
thread_yield()
  ↓
x86_context_switch()
  ↓
save callee-saved registers
  ↓
switch ESP
  ↓
return into another thread
```

That was good for cooperative switching.

But timer preemption enters the kernel differently:

```text
hardware IRQ
  ↓
CPU pushes interrupt return frame
  ↓
IRQ assembly stub saves registers
  ↓
C IRQ handler runs
  ↓
IRETD returns to interrupted code
```

If we want a timer interrupt to switch threads, we should switch by changing which saved interrupt frame the IRQ stub restores.

So this chapter changes the scheduler model to:

```text
all runnable threads have a saved interrupt-style frame
scheduler returns the ESP of the next frame
IRQ/software-interrupt stub restores that frame
IRETD resumes the selected thread
```

On IA-32, interrupt and exception handling, control transfer, and `IRET/IRETD` behavior are part of the architecture’s operating-system support environment described in Intel Volume 3. Intel’s manual page identifies Volume 3 as covering memory management, protection, task management, and interrupt/exception handling. ([Intel][2])

---

# 2. What this chapter adds

Add:

```text
arch/x86/
└── sched_interrupt.asm
```

Modify:

```text
arch/x86/idt.c
arch/x86/irq.asm
arch/x86/interrupts.h
arch/x86/interrupts.c
arch/x86/pit.c
include/kernel/thread.h
kernel/thread.c
kernel/kmain.c
Makefile
```

Remove from the build:

```text
arch/x86/context_switch.asm
```

You can keep the file in the tree for reference, but it will no longer be linked.

---

# 3. The new scheduling interrupt

We will reserve interrupt vector:

```text
0x30 / decimal 48
```

for software scheduling.

Then:

```c
thread_yield();
```

will simply execute:

```asm
int 0x30
```

That software interrupt builds the same kind of saved frame as a hardware interrupt. The scheduler can then switch using exactly the same mechanism as timer preemption.

This is cleaner than having two unrelated switch paths.

---

# 4. Update `arch/x86/interrupts.h`

Replace the current file with this version.

```c
// arch/x86/interrupts.h
#ifndef TOYIX_ARCH_X86_INTERRUPTS_H
#define TOYIX_ARCH_X86_INTERRUPTS_H

#include <stdint.h>

#define X86_SCHED_INTERRUPT_VECTOR 48u

typedef struct interrupt_frame {
    uint32_t ds;

    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t original_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;

    uint32_t interrupt_number;
    uint32_t error_code;

    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} interrupt_frame_t;

typedef void (*interrupt_handler_t)(interrupt_frame_t *frame);

void isr_handler(interrupt_frame_t *frame);

/*
 * IRQ handlers may return a replacement ESP.
 *
 * Return 0 to continue restoring the current interrupt frame.
 * Return a nonzero ESP to restore a different thread's saved frame.
 */
uintptr_t irq_handler(interrupt_frame_t *frame);

int interrupt_register_handler(uint8_t vector, interrupt_handler_t handler);

void interrupts_enable(void);
void interrupts_disable(void);
void interrupts_wait(void);

#endif
```

## What changed?

The important change is:

```c
uintptr_t irq_handler(interrupt_frame_t *frame);
```

Previously, IRQ dispatch returned nothing.

Now the low-level IRQ dispatcher can say:

```text
restore the same stack frame
```

or:

```text
restore this other thread's stack frame
```

That is how preemption happens.

The registered device handlers still return `void`. The replacement stack pointer is chosen by the IRQ dispatcher after the device handler and EOI path run.

---

# 5. Update `arch/x86/irq.asm`

Replace the current file with this version.

```asm
; arch/x86/irq.asm
;
; Hardware IRQ stubs for remapped PIC vectors 32..47.
;
; IRQ0  -> vector 32
; IRQ1  -> vector 33
; ...
; IRQ15 -> vector 47
;
; irq_handler(frame) may return:
;
;   EAX = 0          restore the same interrupted context
;   EAX = new ESP    restore another thread's saved context

BITS 32

extern irq_handler

%macro IRQ_STUB 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

IRQ_STUB 0,  32
IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

global irq_common_stub
irq_common_stub:
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
    call irq_handler
    add esp, 4

    ; If irq_handler returned a nonzero ESP, switch to that saved frame.
    test eax, eax
    jz .restore_current
    mov esp, eax

.restore_current:
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa

    add esp, 8

    iretd
```

## The key mechanism

This is the heart of preemption:

```asm
test eax, eax
jz .restore_current
mov esp, eax
```

If the C IRQ handler returns zero, the interrupted thread continues.

If it returns another saved stack pointer, the IRQ stub restores that other thread instead.

So the timer interrupt can interrupt thread A and return to thread B.

---

# 6. Add `arch/x86/sched_interrupt.asm`

```asm
; arch/x86/sched_interrupt.asm
;
; Software scheduling interrupt.
;
; thread_yield() invokes:
;
;   int 0x30
;
; This stub creates the same saved-frame layout as the IRQ path, then calls
; schedule_interrupt_handler(frame). The handler may return a replacement ESP.

BITS 32

extern schedule_interrupt_handler

global sched_interrupt_stub

sched_interrupt_stub:
    push dword 0
    push dword 48

sched_common_stub:
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
    call schedule_interrupt_handler
    add esp, 4

    test eax, eax
    jz .restore_current
    mov esp, eax

.restore_current:
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa

    add esp, 8

    iretd
```

## Why software yield uses an interrupt

This makes cooperative yield and preemptive timer scheduling use the same saved-frame format.

That means a thread can be switched out by:

```text
thread_yield()
```

or by:

```text
timer IRQ0
```

and the scheduler does not need two incompatible context formats.

---

# 7. Update `arch/x86/idt.c`

Add the external declaration:

```c
extern void sched_interrupt_stub(void);
```

Then install vector `48` inside `idt_init()` after the IRQ gates.

Add this block:

```c
idt_set_gate(
    X86_SCHED_INTERRUPT_VECTOR,
    (uint32_t)sched_interrupt_stub,
    X86_KERNEL_CODE_SELECTOR,
    0x8Eu
);
```

You also need to include `interrupts.h`:

```c
#include "arch/x86/interrupts.h"
```

Here is the relevant updated part of `idt.c`:

```c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "kernel/console.h"

/* existing ISR externs... */
/* existing IRQ externs... */

extern void sched_interrupt_stub(void);

/* existing code... */

void idt_init(void) {
    void (*exception_stubs[32])(void) = {
        isr0,  isr1,  isr2,  isr3,
        isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11,
        isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19,
        isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27,
        isr28, isr29, isr30, isr31
    };

    void (*irq_stubs[16])(void) = {
        irq0,  irq1,  irq2,  irq3,
        irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11,
        irq12, irq13, irq14, irq15
    };

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base = (uint32_t)&idt[0];

    for (uint16_t i = 0; i < 256; ++i) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    for (uint8_t i = 0; i < 32; ++i) {
        idt_set_gate(
            i,
            (uint32_t)exception_stubs[i],
            X86_KERNEL_CODE_SELECTOR,
            0x8Eu
        );
    }

    for (uint8_t i = 0; i < 16; ++i) {
        idt_set_gate(
            (uint8_t)(32 + i),
            (uint32_t)irq_stubs[i],
            X86_KERNEL_CODE_SELECTOR,
            0x8Eu
        );
    }

    idt_set_gate(
        X86_SCHED_INTERRUPT_VECTOR,
        (uint32_t)sched_interrupt_stub,
        X86_KERNEL_CODE_SELECTOR,
        0x8Eu
    );

    idt_load((uint32_t)&idt_ptr);

    console_writeln("IDT: installed exceptions, IRQs, and scheduler interrupt");
}
```

## Why DPL 0 is fine here

The flags value:

```text
0x8E
```

creates a present ring-0 32-bit interrupt gate.

Later, when user mode exists, user programs should not be allowed to invoke arbitrary kernel-only interrupts. For syscalls, we may deliberately expose a DPL 3 gate, but this scheduler interrupt is kernel-internal for now.

---

# 8. Update `arch/x86/interrupts.c`

We need IRQ dispatch to return a possible replacement stack pointer.

Replace the current `irq_handler()` with this version and include `thread.h`.

```c
// add near the top
#include "kernel/thread.h"
```

Then replace `irq_handler()`:

```c
uintptr_t irq_handler(interrupt_frame_t *frame) {
    if (frame == NULL) {
        kernel_panic("null IRQ frame");
    }

    uint32_t vector = frame->interrupt_number;
    uintptr_t next_esp = 0;

    if (vector >= 32 && vector <= 47) {
        if (interrupt_handlers[vector] != NULL) {
            interrupt_handlers[vector](frame);
        }

        /*
         * Send EOI before switching away from this interrupt frame.
         *
         * If we switched first and delayed EOI until this thread resumed, the
         * PIC could stop delivering further IRQs. This is a common source of
         * confusing timer-preemption failures.
         */
        pic_send_eoi((uint8_t)(vector - 32));

        if (vector == 32 && thread_should_reschedule()) {
            next_esp = thread_schedule_from_interrupt(frame);
        }

        return next_esp;
    }

    console_write("Unexpected IRQ vector ");
    console_write_hex32(vector);
    console_putc('\n');

    return 0;
}
```

## Why EOI must happen before switching

The PIC needs an End Of Interrupt notification.

If the timer IRQ switches away from the interrupted thread before sending EOI, the old interrupt may remain “in service” until that old thread happens to run again. That can prevent later IRQ delivery and make the scheduler appear to freeze. This exact ordering issue appears frequently in OS development discussions about preempting from a timer interrupt. ([OSDev Forums][3])

So we do:

```text
run timer handler
send PIC EOI
then possibly switch to another thread
```

---

# 9. Update `arch/x86/pit.c`

The PIT handler should notify the scheduler that a timer tick happened.

Add:

```c
#include "kernel/thread.h"
```

Then update the handler:

```c
static void pit_irq_handler(interrupt_frame_t *frame) {
    pit_ticks++;
    thread_on_timer_tick(frame);
}
```

The full relevant top of the file becomes:

```c
// arch/x86/pit.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "kernel/console.h"
#include "kernel/thread.h"

#define PIT_CHANNEL0_PORT 0x40u
#define PIT_COMMAND_PORT  0x43u
#define PIT_INPUT_HZ      1193182u

static volatile uint32_t pit_ticks;

static void pit_irq_handler(interrupt_frame_t *frame) {
    pit_ticks++;
    thread_on_timer_tick(frame);
}
```

## Why the timer handler does not directly switch

The timer handler only records the tick and requests rescheduling.

The actual switch decision happens in the IRQ dispatch path, after the registered hardware handler returns and after EOI is sent.

That keeps the IRQ path structured:

```text
IRQ assembly
  ↓
generic IRQ dispatcher
  ↓
device handler
  ↓
EOI
  ↓
scheduler decision
  ↓
restore chosen thread
```

---

# 10. Replace `include/kernel/thread.h`

Replace the current header with this version.

```c
// include/kernel/thread.h
#ifndef TOYIX_KERNEL_THREAD_H
#define TOYIX_KERNEL_THREAD_H

#include <stdint.h>
#include "arch/x86/interrupts.h"

#define THREAD_STACK_SIZE 16384u

typedef void (*thread_entry_t)(void *arg);

typedef enum thread_state {
    THREAD_NEW = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_FINISHED
} thread_state_t;

typedef struct thread_context {
    uint32_t esp;
} thread_context_t;

typedef struct thread {
    uint32_t magic;
    uint32_t id;

    const char *name;
    thread_state_t state;

    thread_context_t context;

    void *stack_base;
    uint32_t stack_size;

    thread_entry_t entry;
    void *arg;

    struct thread *next;
    struct thread *prev;
} thread_t;

void threading_init(void);

thread_t *thread_create(
    const char *name,
    thread_entry_t entry,
    void *arg
);

thread_t *thread_current(void);

void thread_yield(void);
void thread_exit(void) __attribute__((noreturn));

void thread_on_timer_tick(interrupt_frame_t *frame);
int thread_should_reschedule(void);
uintptr_t thread_schedule_from_interrupt(interrupt_frame_t *frame);
uintptr_t schedule_interrupt_handler(interrupt_frame_t *frame);

void thread_preemption_init(uint32_t ticks_per_slice);
void thread_preemption_test_prepare(void);
void thread_preemption_test_wait(void);

uint32_t thread_ready_count(void);
void thread_dump_state(void);
void thread_test_once(void);

#endif
```

## Why `thread.h` now includes `interrupts.h`

A more polished architecture would define a generic `trap_frame_t` in a kernel-level header.

For this tutorial kernel, the scheduler is still x86-specific because its saved-frame layout exactly matches our x86 interrupt stubs.

That is acceptable at this stage. Later, we can introduce:

```text
include/kernel/trap.h
arch/x86/trap.c
```

and hide more architecture details.

---

# 11. Replace `kernel/thread.c`

This is the main scheduler refactor.

```c
// kernel/thread.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/interrupts.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/thread.h"

#define THREAD_MAGIC 0x54485244u
#define INITIAL_EFLAGS 0x00000202u

static thread_t bootstrap_thread;

static thread_t *current_thread;
static thread_t *ready_head;
static thread_t *ready_tail;

static uint32_t next_thread_id;
static uint32_t finished_thread_count;

static int preemption_enabled;
static uint32_t ticks_per_slice;
static uint32_t current_slice_ticks;
static int reschedule_requested;

static volatile uint32_t preempt_test_done_count;

static void thread_bootstrap(void);

static uintptr_t align_down(uintptr_t value, uintptr_t alignment) {
    return value & ~(alignment - 1u);
}

static void validate_thread(const thread_t *thread) {
    if (thread == 0) {
        kernel_panic("thread: null thread pointer");
    }

    if (thread->magic != THREAD_MAGIC) {
        kernel_panic("thread: magic mismatch");
    }
}

static const char *thread_state_name(thread_state_t state) {
    switch (state) {
        case THREAD_NEW:
            return "NEW";

        case THREAD_READY:
            return "READY";

        case THREAD_RUNNING:
            return "RUNNING";

        case THREAD_FINISHED:
            return "FINISHED";

        default:
            return "UNKNOWN";
    }
}

static void ready_push(thread_t *thread) {
    validate_thread(thread);

    thread->next = 0;
    thread->prev = ready_tail;

    if (ready_tail != 0) {
        ready_tail->next = thread;
    } else {
        ready_head = thread;
    }

    ready_tail = thread;
}

static thread_t *ready_pop(void) {
    thread_t *thread = ready_head;

    if (thread == 0) {
        return 0;
    }

    ready_head = thread->next;

    if (ready_head != 0) {
        ready_head->prev = 0;
    } else {
        ready_tail = 0;
    }

    thread->next = 0;
    thread->prev = 0;

    return thread;
}

uint32_t thread_ready_count(void) {
    uint32_t count = 0;

    for (thread_t *t = ready_head; t != 0; t = t->next) {
        validate_thread(t);
        count++;
    }

    return count;
}

static void thread_make_initial_stack(thread_t *thread) {
    validate_thread(thread);

    uintptr_t stack_top =
        (uintptr_t)thread->stack_base + thread->stack_size;

    stack_top = align_down(stack_top, 16u);

    uint32_t *sp = (uint32_t *)stack_top;

    /*
     * Build the stack frame expected by irq.asm and sched_interrupt.asm:
     *
     *   pop ds
     *   popa
     *   add esp, 8
     *   iretd
     *
     * interrupt_frame_t layout at context.esp:
     *
     *   ds
     *   edi esi ebp original_esp ebx edx ecx eax
     *   interrupt_number error_code
     *   eip cs eflags
     */

    *(--sp) = INITIAL_EFLAGS;
    *(--sp) = X86_KERNEL_CODE_SELECTOR;
    *(--sp) = (uint32_t)thread_bootstrap;

    *(--sp) = 0;  // error_code
    *(--sp) = X86_SCHED_INTERRUPT_VECTOR;

    *(--sp) = 0;  // eax
    *(--sp) = 0;  // ecx
    *(--sp) = 0;  // edx
    *(--sp) = 0;  // ebx
    *(--sp) = 0;  // original_esp, ignored by popa
    *(--sp) = 0;  // ebp
    *(--sp) = 0;  // esi
    *(--sp) = 0;  // edi

    *(--sp) = X86_KERNEL_DATA_SELECTOR;

    thread->context.esp = (uint32_t)sp;
}

void threading_init(void) {
    ready_head = 0;
    ready_tail = 0;
    next_thread_id = 1;
    finished_thread_count = 0;

    preemption_enabled = 0;
    ticks_per_slice = 1;
    current_slice_ticks = 0;
    reschedule_requested = 0;
    preempt_test_done_count = 0;

    bootstrap_thread.magic = THREAD_MAGIC;
    bootstrap_thread.id = 0;
    bootstrap_thread.name = "bootstrap";
    bootstrap_thread.state = THREAD_RUNNING;
    bootstrap_thread.context.esp = 0;
    bootstrap_thread.stack_base = 0;
    bootstrap_thread.stack_size = 0;
    bootstrap_thread.entry = 0;
    bootstrap_thread.arg = 0;
    bootstrap_thread.next = 0;
    bootstrap_thread.prev = 0;

    current_thread = &bootstrap_thread;

    console_writeln("Threads: interrupt-frame scheduler initialized");
}

thread_t *thread_create(
    const char *name,
    thread_entry_t entry,
    void *arg
) {
    if (entry == 0) {
        kernel_panic("thread_create received null entry");
    }

    thread_t *thread = (thread_t *)kcalloc(1, sizeof(thread_t));

    if (thread == 0) {
        kernel_panic("thread_create could not allocate thread object");
    }

    void *stack = kmalloc(THREAD_STACK_SIZE);

    if (stack == 0) {
        kernel_panic("thread_create could not allocate kernel stack");
    }

    thread->magic = THREAD_MAGIC;
    thread->id = next_thread_id++;
    thread->name = name != 0 ? name : "unnamed";
    thread->state = THREAD_NEW;

    thread->stack_base = stack;
    thread->stack_size = THREAD_STACK_SIZE;

    thread->entry = entry;
    thread->arg = arg;

    thread->next = 0;
    thread->prev = 0;

    thread_make_initial_stack(thread);

    thread->state = THREAD_READY;
    ready_push(thread);

    console_write("Thread: created ");
    console_write(thread->name);
    console_write(" id=");
    console_write_u32_dec(thread->id);
    console_write(" stack=");
    console_write_hex32((uint32_t)(uintptr_t)thread->stack_base);
    console_putc('\n');

    return thread;
}

thread_t *thread_current(void) {
    return current_thread;
}

void thread_yield(void) {
    __asm__ volatile ("int $0x30");
}

void thread_exit(void) {
    thread_t *old_thread = current_thread;
    validate_thread(old_thread);

    console_write("Thread: exiting ");
    console_write(old_thread->name);
    console_write(" id=");
    console_write_u32_dec(old_thread->id);
    console_putc('\n');

    old_thread->state = THREAD_FINISHED;
    finished_thread_count++;

    thread_yield();

    kernel_panic("thread_exit returned unexpectedly");
}

static void thread_bootstrap(void) {
    thread_t *self = current_thread;
    validate_thread(self);

    if (self->entry == 0) {
        kernel_panic("thread_bootstrap found null entry");
    }

    self->entry(self->arg);

    thread_exit();
}

uintptr_t thread_schedule_from_interrupt(interrupt_frame_t *frame) {
    if (frame == 0) {
        kernel_panic("scheduler received null interrupt frame");
    }

    thread_t *old_thread = current_thread;
    validate_thread(old_thread);

    /*
     * If nobody else is ready, continue current thread.
     */
    if (ready_head == 0) {
        current_slice_ticks = 0;
        reschedule_requested = 0;
        return 0;
    }

    old_thread->context.esp = (uint32_t)(uintptr_t)frame;

    if (old_thread->state == THREAD_RUNNING) {
        old_thread->state = THREAD_READY;
        ready_push(old_thread);
    }

    thread_t *next_thread = ready_pop();

    if (next_thread == 0) {
        current_slice_ticks = 0;
        reschedule_requested = 0;
        return 0;
    }

    validate_thread(next_thread);

    next_thread->state = THREAD_RUNNING;
    current_thread = next_thread;

    current_slice_ticks = 0;
    reschedule_requested = 0;

    return (uintptr_t)next_thread->context.esp;
}

uintptr_t schedule_interrupt_handler(interrupt_frame_t *frame) {
    return thread_schedule_from_interrupt(frame);
}

void thread_on_timer_tick(interrupt_frame_t *frame) {
    (void)frame;

    if (!preemption_enabled) {
        return;
    }

    if (ready_head == 0) {
        current_slice_ticks = 0;
        return;
    }

    current_slice_ticks++;

    if (current_slice_ticks >= ticks_per_slice) {
        reschedule_requested = 1;
    }
}

int thread_should_reschedule(void) {
    return reschedule_requested;
}

void thread_preemption_init(uint32_t requested_ticks_per_slice) {
    if (requested_ticks_per_slice == 0) {
        requested_ticks_per_slice = 1;
    }

    ticks_per_slice = requested_ticks_per_slice;
    current_slice_ticks = 0;
    reschedule_requested = 0;
    preemption_enabled = 1;

    console_write("Threads: preemption enabled, slice ticks=");
    console_write_u32_dec(ticks_per_slice);
    console_putc('\n');
}

void thread_dump_state(void) {
    console_write("Threads: current=");
    console_write(current_thread != 0 ? current_thread->name : "(null)");
    console_write(" ready=");
    console_write_u32_dec(thread_ready_count());
    console_write(" finished=");
    console_write_u32_dec(finished_thread_count);
    console_putc('\n');

    for (thread_t *t = ready_head; t != 0; t = t->next) {
        validate_thread(t);

        console_write("  ready thread id=");
        console_write_u32_dec(t->id);
        console_write(" name=");
        console_write(t->name);
        console_write(" state=");
        console_write(thread_state_name(t->state));
        console_putc('\n');
    }
}

typedef struct thread_test_arg {
    const char *label;
    uint32_t iterations;
} thread_test_arg_t;

static void worker_thread(void *arg) {
    thread_test_arg_t *test = (thread_test_arg_t *)arg;

    for (uint32_t i = 0; i < test->iterations; ++i) {
        console_write("Thread test: worker ");
        console_write(test->label);
        console_write(" step ");
        console_write_u32_dec(i);
        console_putc('\n');

        thread_yield();
    }

    console_write("Thread test: worker ");
    console_write(test->label);
    console_writeln(" done");
}

void thread_test_once(void) {
    console_writeln("Thread test: starting software-yield multitasking test");

    static thread_test_arg_t arg_a = {
        .label = "A",
        .iterations = 3
    };

    static thread_test_arg_t arg_b = {
        .label = "B",
        .iterations = 3
    };

    thread_create("worker-a", worker_thread, &arg_a);
    thread_create("worker-b", worker_thread, &arg_b);

    while (thread_ready_count() > 0) {
        thread_yield();
    }

    console_writeln("Thread test: completed software-yield multitasking test");
    thread_dump_state();
}

typedef struct preempt_test_arg {
    const char *label;
    volatile uint32_t counter;
} preempt_test_arg_t;

static preempt_test_arg_t preempt_arg_a = {
    .label = "P",
    .counter = 0
};

static preempt_test_arg_t preempt_arg_b = {
    .label = "Q",
    .counter = 0
};

static void preempt_worker(void *arg) {
    preempt_test_arg_t *test = (preempt_test_arg_t *)arg;

    /*
     * Deliberately no thread_yield() inside this loop.
     *
     * If both preempt workers finish, timer preemption is working.
     */
    for (uint32_t round = 0; round < 5; ++round) {
        for (volatile uint32_t i = 0; i < 600000u; ++i) {
            test->counter++;
        }

        console_write("Preempt test: worker ");
        console_write(test->label);
        console_write(" round ");
        console_write_u32_dec(round);
        console_putc('\n');
    }

    preempt_test_done_count++;
}

void thread_preemption_test_prepare(void) {
    preempt_test_done_count = 0;
    preempt_arg_a.counter = 0;
    preempt_arg_b.counter = 0;

    console_writeln("Preempt test: creating non-yielding workers");

    thread_create("preempt-p", preempt_worker, &preempt_arg_a);
    thread_create("preempt-q", preempt_worker, &preempt_arg_b);
}

void thread_preemption_test_wait(void) {
    console_writeln("Preempt test: waiting for timer-driven scheduling");

    while (preempt_test_done_count < 2) {
        interrupts_wait();
    }

    console_write("Preempt test: worker P counter ");
    console_write_u32_dec(preempt_arg_a.counter);
    console_putc('\n');

    console_write("Preempt test: worker Q counter ");
    console_write_u32_dec(preempt_arg_b.counter);
    console_putc('\n');

    if (preempt_arg_a.counter == 0 || preempt_arg_b.counter == 0) {
        kernel_panic("preemption test counter did not advance");
    }

    console_writeln("Preempt test: timer-driven preemption sanity check passed");
    thread_dump_state();
}
```

---

# 12. What changed in the thread model?

The old initial stack looked like this:

```text
saved EDI
saved ESI
saved EBX
saved EBP
return address
```

The new initial stack looks like an interrupt frame:

```text
DS
EDI
ESI
EBP
original ESP
EBX
EDX
ECX
EAX
interrupt number
error code
EIP
CS
EFLAGS
```

When a new thread is selected, the interrupt restore path does:

```asm
pop ds
popa
add esp, 8
iretd
```

That returns into:

```c
thread_bootstrap()
```

with kernel code selector `0x08`, kernel data selector `0x10`, and interrupt-enabled flags `0x202`.

---

# 13. Update `kernel/kmain.c`

We need to prepare preemptive workers before enabling interrupts, then let timer IRQs run them after interrupts are enabled.

Replace your current `kernel_main()` with this version.

```c
// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/multiboot.h"
#include "arch/x86/paging.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/thread.h"
#include "kernel/vmem.h"

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    console_register(&serial_console_driver);
    console_register(&vga_text_console_driver);
    console_init_all();

    console_writeln("Toyix kernel alive");

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        console_writeln("Boot protocol: Multiboot OK");
    } else {
        console_write("Boot protocol: unexpected magic ");
        console_write_hex32(multiboot_magic);
        console_putc('\n');
        kernel_panic("unsupported boot protocol");
    }

    const multiboot_info_t *mbi =
        (const multiboot_info_t *)(uintptr_t)multiboot_info_addr;

    console_write("Multiboot info at ");
    console_write_hex32(multiboot_info_addr);
    console_putc('\n');

    gdt_init();
    idt_init();
    pic_init();

    pmm_init(mbi);
    pmm_test_once();

    paging_init();
    paging_test_identity_mapping();

    vmem_init();
    vmem_test_once();

    heap_init(4);
    heap_test_once();

    threading_init();
    thread_test_once();

#ifdef TOYIX_TRIGGER_PAGE_FAULT
    console_writeln("Triggering test page fault at 0xC0000000...");
    volatile uint32_t *bad = (volatile uint32_t *)0xC0000000u;
    uint32_t value = *bad;
    (void)value;
#endif

    pit_init(100);
    keyboard_init();

    thread_preemption_init(2);
    thread_preemption_test_prepare();

    console_writeln("Interrupt hardware: configured");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    interrupts_enable();

    console_writeln("Interrupts: enabled");

    thread_preemption_test_wait();

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: scheduler cleanup, blocking, sleep queues, and thread reaping.");

    kernel_idle();
}
```

## Why prepare threads before enabling interrupts?

Once timer preemption is enabled, the timer can interrupt almost anywhere.

Our scheduler is not protected by locks yet.

So we avoid creating test threads while preemption is active. We create them first:

```c
thread_preemption_test_prepare();
```

Then enable interrupts:

```c
interrupts_enable();
```

Then wait while the timer scheduler runs them:

```c
thread_preemption_test_wait();
```

Later, we need proper critical sections around ready-queue operations.

---

# 14. Update `Makefile`

Remove:

```text
build/arch/x86/context_switch.o
```

Add:

```text
build/arch/x86/sched_interrupt.o
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
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/thread.o \
    build/kernel/vmem.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update test greps:

```make
	grep -q "Threads: interrupt-frame scheduler initialized" build/test.log
	grep -q "Thread test: completed software-yield multitasking test" build/test.log
	grep -q "Threads: preemption enabled, slice ticks=2" build/test.log
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
```

The important part of `test` should now include:

```make
test: iso
	@mkdir -p build
	@timeout 8s $(QEMU) \
		-cdrom build/toyix.iso \
		-serial stdio \
		-display none \
		-monitor none \
		-no-reboot \
		> build/test.log || true
	grep -q "Toyix kernel alive" build/test.log
	grep -q "Boot protocol: Multiboot OK" build/test.log
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	grep -q "Heap test: VMM-backed allocation/free sanity check passed" build/test.log
	grep -q "Threads: interrupt-frame scheduler initialized" build/test.log
	grep -q "Thread test: completed software-yield multitasking test" build/test.log
	grep -q "Threads: preemption enabled, slice ticks=2" build/test.log
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, memory, heap, software yield, and preemption smoke test passed."
```

I changed the QEMU timeout from 5 seconds to 8 seconds because the preemption test performs deliberate busy loops.

---

# 15. Smoke Wrapper

No change is needed in `tests/smoke.sh` for this chapter. The existing wrapper still works because `make test` now covers the preemption path as part of the normal smoke run.

---

# 16. Expected output

A successful boot should include:

```text
Threads: interrupt-frame scheduler initialized
Thread test: starting software-yield multitasking test
Thread test: worker A step 0
Thread test: worker B step 0
Thread test: completed software-yield multitasking test
PIT: timer running at 100 Hz
Keyboard: IRQ1 handler installed
Threads: preemption enabled, slice ticks=2
Preempt test: creating non-yielding workers
Interrupt hardware: configured
Interrupts: enabled
Preempt test: waiting for timer-driven scheduling
Preempt test: worker P round 0
Preempt test: worker Q round 0
Preempt test: worker P round 1
Preempt test: worker Q round 1
...
Preempt test: timer-driven preemption sanity check passed
Timer: observed 3 ticks
```

The exact interleaving may vary.

That variation is a feature, not a bug. Preemptive scheduling introduces timing-dependent interleaving.

The key evidence is:

```text
workers P and Q do not call thread_yield()
both still complete
```

That proves timer-driven scheduling is working.

---

# 17. Common failures

## Failure: timer preemption freezes after one switch

Likely cause:

```text
EOI sent after switching instead of before switching
```

The PIC must receive EOI before the scheduler returns a different ESP.

Check `irq_handler()`:

```c
pic_send_eoi((uint8_t)(vector - 32));

if (vector == 32 && thread_should_reschedule()) {
    next_esp = thread_schedule_from_interrupt(frame);
}
```

Do not reverse those two operations.

## Failure: crash on first timer switch

Likely cause:

```text
thread initial stack does not match interrupt restore path
```

The restore path is:

```asm
pop eax      ; saved DS
mov ds, ax
...
popa
add esp, 8
iretd
```

The thread stack must contain:

```text
DS
PUSHA register image
interrupt_number
error_code
EIP
CS
EFLAGS
```

If any field is missing or in the wrong order, `IRETD` jumps into nonsense.

## Failure: `thread_exit returned unexpectedly`

This usually means:

```text
thread_exit called thread_yield()
but scheduler found no other ready thread
so the exiting thread continued
```

In the preemption test, bootstrap should be ready while workers run. If it is not, check that the scheduler requeues the old running thread:

```c
if (old_thread->state == THREAD_RUNNING) {
    old_thread->state = THREAD_READY;
    ready_push(old_thread);
}
```

## Failure: output is interleaved or messy

Expected.

The console is not synchronized. A timer can preempt one thread while it is printing, and another thread can print before the first finishes.

Later we need:

```text
spinlocks
interrupt-disable critical sections
console lock
scheduler lock
```

For now, the smoke test greps for robust milestone lines rather than relying on perfect output order.

---

# 18. Important limitation: no scheduler locking yet

This scheduler is still single-CPU and fragile.

It assumes:

```text
one CPU
no nested scheduler calls
ready queue not modified from competing CPUs
thread creation mostly happens before preemption is enabled
```

That is acceptable for this chapter, but it is not production-safe.

The next cleanup should introduce critical sections:

```c
irq_flags_t flags = irq_save();
ready_push(thread);
irq_restore(flags);
```

That prevents timer preemption from interrupting ready-queue manipulation.

---

# 19. What this chapter achieved

We now have:

```text
software-yield scheduling through int 0x30
timer-driven preemption through IRQ0
interrupt-frame-based context switching
non-yielding worker threads that still share CPU time
```

The execution model is now:

```text
PIT IRQ0 fires
  ↓
pit_irq_handler increments ticks
  ↓
thread_on_timer_tick marks reschedule needed
  ↓
IRQ dispatcher sends EOI
  ↓
scheduler picks next thread
  ↓
IRQ stub restores selected thread's saved frame
  ↓
IRETD resumes that selected thread
```

That is the first real preemptive kernel scheduler.

---

# 20. Commit this chapter

After the tests pass:

```bash
git status
git add .
git commit -m "Add timer-driven preemptive kernel scheduling"
```

This is a major milestone. Preemption is where many hobby kernels become unstable, so commit the working state before cleanup.

---

# 21. Next chapter

The next chapter should not rush into user mode yet.

We need scheduler hygiene first:

```text
interrupt save/restore helpers
critical sections
scheduler lock
safe thread_create() while preemption is active
sleep queues
thread states: BLOCKED, SLEEPING, ZOMBIE
thread reaper to free finished stacks
console lock
```

The immediate next milestone should be:

```text
thread_sleep_ticks(n)
```

That gives the kernel a blocking primitive:

```text
thread sleeps
  ↓
scheduler removes it from ready queue
  ↓
timer ticks count down
  ↓
thread wakes and returns to ready queue
```

Once blocking and wakeup work, we can build better keyboard input, kernel monitors, device waits, and eventually user processes.

---

# Resources

- [Chapter 10 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_10)
- [OSDev preemptive multitasking](https://wiki.osdev.org/Preemptive_Multitasking)
- [OSDev context switching](https://wiki.osdev.org/Context_Switching)
- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev forum discussion on timer interrupt context switching](https://f.osdev.org/viewtopic.php?t=32207)

# Closure

Chapter 10 turns the scheduler into a preemptive one by adding a dedicated scheduling interrupt, a shared interrupt-frame layout, and a timer path that can take the CPU away from a thread that does not yield. The implementation is still intentionally small, but it now proves the kernel can preempt work instead of relying on cooperative calls alone.

Happy Coding!

[1]: https://f.osdev.org/viewtopic.php?t=32207&utm_source=chatgpt.com "Context switch on timer interrupt - OSDev.org"
[2]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html?utm_source=chatgpt.com "Manuals for Intel® 64 and IA-32 Architectures"
[3]: https://forum.osdev.org/viewtopic.php?start=15&t=56972&utm_source=chatgpt.com "Confused about context switch - Page 2 - OSDev.org"
