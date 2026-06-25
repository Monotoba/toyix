// kernel/thread.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/thread.h"

#define THREAD_MAGIC 0x54485244u
#define INITIAL_EFLAGS 0x00000202u

static thread_t bootstrap_thread;
static thread_t *idle_thread;

static thread_t *current_thread;

static thread_t *ready_head;
static thread_t *ready_tail;

static thread_t *sleep_head;
static thread_t *sleep_tail;

static thread_t *zombie_head;
static thread_t *zombie_tail;

static uint32_t next_thread_id;
static uint32_t zombie_created_count;
static uint32_t zombie_reaped_count;

static int scheduler_initialized;
static int preemption_enabled;

static uint32_t ticks_per_slice;
static uint32_t current_slice_ticks;
static int reschedule_requested;

static volatile uint32_t scheduler_ticks;
static volatile uint32_t preempt_test_done_count;
static volatile uint32_t sleep_test_done_count;

static void thread_bootstrap(void);
static void idle_thread_entry(void *arg);

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
		case THREAD_BLOCKED:
			return "BLOCKED";
		case THREAD_SLEEPING:
			return "SLEEPING";
		case THREAD_ZOMBIE:
			return "ZOMBIE";
		default:
			return "UNKNOWN";
	}
}

static uint32_t count_list(const thread_t *head) {
	uint32_t count = 0;

	for (const thread_t *t = head; t != 0; t = t->next) {
		validate_thread(t);
		count++;
	}

	return count;
}

static void queue_push(
	thread_t **head,
	thread_t **tail,
	thread_t *thread
) {
	validate_thread(thread);

	thread->next = 0;
	thread->prev = *tail;

	if (*tail != 0) {
		(*tail)->next = thread;
	} else {
		*head = thread;
	}

	*tail = thread;
}

static thread_t *queue_pop(
	thread_t **head,
	thread_t **tail
) {
	thread_t *thread = *head;

	if (thread == 0) {
		return 0;
	}

	*head = thread->next;

	if (*head != 0) {
		(*head)->prev = 0;
	} else {
		*tail = 0;
	}

	thread->next = 0;
	thread->prev = 0;

	return thread;
}

static void ready_push(thread_t *thread) {
	if (thread == idle_thread) {
		return;
	}

	thread->state = THREAD_READY;
	queue_push(&ready_head, &ready_tail, thread);
}

static thread_t *ready_pop(void) {
	return queue_pop(&ready_head, &ready_tail);
}

static void zombie_push(thread_t *thread) {
	if (thread == idle_thread || thread == &bootstrap_thread) {
		return;
	}

	queue_push(&zombie_head, &zombie_tail, thread);
}

static thread_t *zombie_pop(void) {
	return queue_pop(&zombie_head, &zombie_tail);
}

static thread_t *sleep_pop_front(void) {
	return queue_pop(&sleep_head, &sleep_tail);
}

static int tick_reached(uint32_t now, uint32_t target) {
	return (int32_t)(now - target) >= 0;
}

static void sleep_insert(thread_t *thread) {
	validate_thread(thread);

	thread->state = THREAD_SLEEPING;
	thread->next = 0;
	thread->prev = 0;

	if (sleep_head == 0) {
		sleep_head = thread;
		sleep_tail = thread;
		return;
	}

	thread_t *current = sleep_head;

	while (current != 0 &&
	       !tick_reached(current->wake_tick, thread->wake_tick)) {
		current = current->next;
	}

	if (current == sleep_head) {
		thread->next = sleep_head;
		sleep_head->prev = thread;
		sleep_head = thread;
		return;
	}

	if (current == 0) {
		thread->prev = sleep_tail;
		sleep_tail->next = thread;
		sleep_tail = thread;
		return;
	}

	thread_t *previous = current->prev;
	previous->next = thread;
	thread->prev = previous;
	thread->next = current;
	current->prev = thread;
}

static void wake_due_sleepers(void) {
	while (sleep_head != 0 &&
	       tick_reached((uint32_t)scheduler_ticks, sleep_head->wake_tick)) {
		thread_t *thread = sleep_pop_front();

		validate_thread(thread);

		console_write("Threads: waking ");
		console_write(thread->name);
		console_write(" at tick ");
		console_write_u32_dec((uint32_t)scheduler_ticks);
		console_putc('\n');

		ready_push(thread);
		reschedule_requested = 1;
	}
}

uint32_t thread_ready_count(void) {
	irq_flags_t flags = irq_save();
	uint32_t count = count_list(ready_head);
	irq_restore(flags);
	return count;
}

uint32_t thread_sleeping_count(void) {
	irq_flags_t flags = irq_save();
	uint32_t count = count_list(sleep_head);
	irq_restore(flags);
	return count;
}

uint32_t thread_zombie_count(void) {
	irq_flags_t flags = irq_save();
	uint32_t count = count_list(zombie_head);
	irq_restore(flags);
	return count;
}

uint32_t thread_ticks(void) {
	return (uint32_t)scheduler_ticks;
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
	 *   pop saved DS
	 *   popa
	 *   add esp, 8
	 *   iretd
	 */

	*(--sp) = INITIAL_EFLAGS;
	*(--sp) = X86_KERNEL_CODE_SELECTOR;
	*(--sp) = (uint32_t)thread_bootstrap;

	*(--sp) = 0;
	*(--sp) = X86_SCHED_INTERRUPT_VECTOR;

	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;

	*(--sp) = X86_KERNEL_DATA_SELECTOR;

	thread->context.esp = (uint32_t)sp;
}

static thread_t *thread_create_internal(
	const char *name,
	thread_entry_t entry,
	void *arg,
	int enqueue
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

	thread->wake_tick = 0;
	thread->next = 0;
	thread->prev = 0;

	thread_make_initial_stack(thread);

	if (enqueue) {
		irq_flags_t flags = irq_save();
		ready_push(thread);
		irq_restore(flags);
	} else {
		thread->state = THREAD_READY;
	}

	console_write("Thread: created ");
	console_write(thread->name);
	console_write(" id=");
	console_write_u32_dec(thread->id);
	console_write(" stack=");
	console_write_hex32((uint32_t)(uintptr_t)thread->stack_base);
	console_putc('\n');

	return thread;
}

void threading_init(void) {
	ready_head = 0;
	ready_tail = 0;

	sleep_head = 0;
	sleep_tail = 0;

	zombie_head = 0;
	zombie_tail = 0;

	next_thread_id = 1;
	zombie_created_count = 0;
	zombie_reaped_count = 0;

	scheduler_initialized = 0;
	preemption_enabled = 0;

	ticks_per_slice = 1;
	current_slice_ticks = 0;
	reschedule_requested = 0;

	scheduler_ticks = 0;
	preempt_test_done_count = 0;
	sleep_test_done_count = 0;

	bootstrap_thread.magic = THREAD_MAGIC;
	bootstrap_thread.id = 0;
	bootstrap_thread.name = "bootstrap";
	bootstrap_thread.state = THREAD_RUNNING;
	bootstrap_thread.context.esp = 0;
	bootstrap_thread.stack_base = 0;
	bootstrap_thread.stack_size = 0;
	bootstrap_thread.entry = 0;
	bootstrap_thread.arg = 0;
	bootstrap_thread.wake_tick = 0;
	bootstrap_thread.next = 0;
	bootstrap_thread.prev = 0;

	current_thread = &bootstrap_thread;

	idle_thread = thread_create_internal(
		"idle",
		idle_thread_entry,
		0,
		0
	);

	scheduler_initialized = 1;

	console_writeln("Threads: blocking scheduler initialized");
}

thread_t *thread_create(
	const char *name,
	thread_entry_t entry,
	void *arg
) {
	return thread_create_internal(name, entry, arg, 1);
}

thread_t *thread_current(void) {
	return current_thread;
}

void thread_yield(void) {
	__asm__ volatile ("int $0x30" ::: "memory");
}

void thread_block_current(void) {
	irq_flags_t flags = irq_save();

	thread_t *self = current_thread;
	validate_thread(self);

	if (self == idle_thread) {
		irq_restore(flags);
		kernel_panic("idle thread attempted to block");
	}

	if (self->state != THREAD_RUNNING) {
		irq_restore(flags);
		kernel_panic("thread_block_current called on non-running thread");
	}

	self->state = THREAD_BLOCKED;
	reschedule_requested = 1;

	irq_restore(flags);
}

void thread_wake(thread_t *thread) {
	if (thread == 0) {
		return;
	}

	irq_flags_t flags = irq_save();

	validate_thread(thread);

	if (thread->state == THREAD_BLOCKED) {
		ready_push(thread);
		reschedule_requested = 1;
	}

	irq_restore(flags);
}

void thread_sleep_ticks(uint32_t ticks) {
	if (ticks == 0) {
		thread_yield();
		return;
	}

	irq_flags_t flags = irq_save();

	if (!irq_flags_interrupts_enabled(flags)) {
		irq_restore(flags);
		kernel_panic("thread_sleep_ticks called with interrupts disabled");
	}

	thread_t *self = current_thread;
	validate_thread(self);

	if (self == idle_thread) {
		irq_restore(flags);
		return;
	}

	self->wake_tick = (uint32_t)scheduler_ticks + ticks;
	sleep_insert(self);
	reschedule_requested = 1;

	irq_restore(flags);

	/*
	 * If we are preempted immediately after restoring interrupts, we may
	 * resume here while already marked SLEEPING. A second yield is still
	 * safe; the scheduler will skip requeueing sleeping threads.
	 */
	thread_yield();
}

void thread_exit(void) {
	irq_flags_t flags = irq_save();

	thread_t *old_thread = current_thread;
	validate_thread(old_thread);

	console_write("Thread: exiting ");
	console_write(old_thread->name);
	console_write(" id=");
	console_write_u32_dec(old_thread->id);
	console_putc('\n');

	old_thread->state = THREAD_ZOMBIE;
	zombie_push(old_thread);
	zombie_created_count++;
	reschedule_requested = 1;

	irq_restore(flags);

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

static void idle_thread_entry(void *arg) {
	(void)arg;

	for (;;) {
		thread_reap_zombies();
		interrupts_wait();
	}
}

uintptr_t thread_schedule_from_interrupt(interrupt_frame_t *frame) {
	if (frame == 0) {
		kernel_panic("scheduler received null interrupt frame");
	}

	if (!scheduler_initialized) {
		return 0;
	}

	thread_t *old_thread = current_thread;
	validate_thread(old_thread);

	old_thread->context.esp = (uint32_t)(uintptr_t)frame;

	thread_t *next_thread = 0;

	if (ready_head == 0) {
		if (old_thread == idle_thread) {
			current_slice_ticks = 0;
			reschedule_requested = 0;
			return 0;
		}

		if (old_thread->state == THREAD_RUNNING) {
			current_slice_ticks = 0;
			reschedule_requested = 0;
			return 0;
		}

		next_thread = idle_thread;
	} else {
		if (old_thread != idle_thread &&
		    old_thread->state == THREAD_RUNNING) {
			ready_push(old_thread);
		}

		next_thread = ready_pop();
	}

	if (next_thread == 0) {
		next_thread = idle_thread;
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

	if (!scheduler_initialized) {
		return;
	}

	scheduler_ticks++;
	wake_due_sleepers();

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

	irq_flags_t flags = irq_save();

	ticks_per_slice = requested_ticks_per_slice;
	current_slice_ticks = 0;
	reschedule_requested = 0;
	preemption_enabled = 1;

	irq_restore(flags);

	console_write("Threads: preemption enabled, slice ticks=");
	console_write_u32_dec(ticks_per_slice);
	console_putc('\n');
}

void thread_reap_zombies(void) {
	for (;;) {
		irq_flags_t flags = irq_save();
		thread_t *zombie = zombie_pop();
		irq_restore(flags);

		if (zombie == 0) {
			return;
		}

		validate_thread(zombie);

		console_write("Threads: reaping zombie ");
		console_write(zombie->name);
		console_write(" id=");
		console_write_u32_dec(zombie->id);
		console_putc('\n');

		void *stack = zombie->stack_base;
		zombie->magic = 0;

		if (stack != 0) {
			kfree(stack);
		}

		kfree(zombie);
		zombie_reaped_count++;
	}
}

void thread_dump_state(void) {
	irq_flags_t flags = irq_save();

	uint32_t ready_count = count_list(ready_head);
	uint32_t sleeping_count = count_list(sleep_head);
	uint32_t zombie_count = count_list(zombie_head);

	console_write("Threads: current=");
	console_write(current_thread != 0 ? current_thread->name : "(null)");
	console_write(" ready=");
	console_write_u32_dec(ready_count);
	console_write(" sleeping=");
	console_write_u32_dec(sleeping_count);
	console_write(" zombies=");
	console_write_u32_dec(zombie_count);
	console_write(" ticks=");
	console_write_u32_dec((uint32_t)scheduler_ticks);
	console_putc('\n');

	console_write("Threads: zombies created=");
	console_write_u32_dec(zombie_created_count);
	console_write(" reaped=");
	console_write_u32_dec(zombie_reaped_count);
	console_putc('\n');

	for (thread_t *t = ready_head; t != 0; t = t->next) {
		validate_thread(t);

		console_write("  ready id=");
		console_write_u32_dec(t->id);
		console_write(" name=");
		console_write(t->name);
		console_write(" state=");
		console_write(thread_state_name(t->state));
		console_putc('\n');
	}

	for (thread_t *t = sleep_head; t != 0; t = t->next) {
		validate_thread(t);

		console_write("  sleeping id=");
		console_write_u32_dec(t->id);
		console_write(" name=");
		console_write(t->name);
		console_write(" wake=");
		console_write_u32_dec(t->wake_tick);
		console_putc('\n');
	}

	irq_restore(flags);
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

	thread_reap_zombies();

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

	thread_reap_zombies();

	console_writeln("Preempt test: timer-driven preemption sanity check passed");
	thread_dump_state();
}

typedef struct sleep_test_arg {
	const char *label;
	uint32_t sleep_ticks;
} sleep_test_arg_t;

static sleep_test_arg_t sleep_arg_a = {
	.label = "S1",
	.sleep_ticks = 4
};

static sleep_test_arg_t sleep_arg_b = {
	.label = "S2",
	.sleep_ticks = 8
};

static void sleep_worker(void *arg) {
	sleep_test_arg_t *test = (sleep_test_arg_t *)arg;

	console_write("Sleep test: worker ");
	console_write(test->label);
	console_write(" sleeping for ");
	console_write_u32_dec(test->sleep_ticks);
	console_writeln(" ticks");

	thread_sleep_ticks(test->sleep_ticks);

	console_write("Sleep test: worker ");
	console_write(test->label);
	console_write(" woke at tick ");
	console_write_u32_dec(thread_ticks());
	console_putc('\n');

	sleep_test_done_count++;
}

void thread_sleep_test_once(void) {
	console_writeln("Sleep test: starting blocking sleep test");

	sleep_test_done_count = 0;

	thread_create("sleep-s1", sleep_worker, &sleep_arg_a);
	thread_create("sleep-s2", sleep_worker, &sleep_arg_b);

	while (sleep_test_done_count < 2) {
		thread_sleep_ticks(1);
		thread_reap_zombies();
	}

	thread_reap_zombies();

	console_writeln("Sleep test: blocking sleep sanity check passed");
	thread_dump_state();
}
