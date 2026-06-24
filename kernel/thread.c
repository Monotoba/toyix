// kernel/thread.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/thread.h"

#define THREAD_MAGIC 0x54485244u

extern void x86_context_switch(
	thread_context_t *old_context,
	thread_context_t *new_context
);

static thread_t bootstrap_thread;

static thread_t *current_thread;
static thread_t *ready_head;
static thread_t *ready_tail;

static uint32_t next_thread_id;
static uint32_t finished_thread_count;

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

	*(--sp) = (uint32_t)thread_bootstrap;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;
	*(--sp) = 0;

	thread->context.esp = (uint32_t)sp;
}

void threading_init(void) {
	ready_head = 0;
	ready_tail = 0;
	next_thread_id = 1;
	finished_thread_count = 0;

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

	console_writeln("Threads: cooperative scheduler initialized");
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
	thread_t *old_thread = current_thread;
	validate_thread(old_thread);

	thread_t *next_thread = ready_pop();

	if (next_thread == 0) {
		return;
	}

	validate_thread(next_thread);

	if (old_thread->state == THREAD_RUNNING) {
		old_thread->state = THREAD_READY;
		ready_push(old_thread);
	}

	next_thread->state = THREAD_RUNNING;
	current_thread = next_thread;

	x86_context_switch(&old_thread->context, &next_thread->context);
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

	thread_t *next_thread = ready_pop();

	if (next_thread == 0) {
		kernel_panic("thread_exit: no thread to switch to");
	}

	validate_thread(next_thread);

	next_thread->state = THREAD_RUNNING;
	current_thread = next_thread;

	x86_context_switch(&old_thread->context, &next_thread->context);

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
	console_writeln("Thread test: starting cooperative multitasking test");

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

	console_writeln("Thread test: completed cooperative multitasking test");
	thread_dump_state();
}
