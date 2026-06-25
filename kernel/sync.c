#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/sync.h"
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

static int mutex_available_condition(void *context) {
	mutex_t *mutex = (mutex_t *)context;
	return mutex->locked == 0;
}

static int semaphore_available_condition(void *context) {
	semaphore_t *sem = (semaphore_t *)context;
	return sem->count > 0;
}

void mutex_init(mutex_t *mutex, const char *name) {
	if (mutex == 0) {
		kernel_panic("mutex_init received null mutex");
	}

	mutex->name = name != 0 ? name : "unnamed";
	mutex->locked = 0;
	mutex->owner = 0;
	wait_queue_init(&mutex->waiters, mutex->name);
}

void mutex_lock(mutex_t *mutex) {
	if (mutex == 0) {
		kernel_panic("mutex_lock received null mutex");
	}

	thread_t *self = thread_current();
	irq_flags_t flags = irq_save();

	if (mutex->locked && mutex->owner == self) {
		irq_restore(flags);
		kernel_panic("mutex_lock attempted recursive lock");
	}

	while (mutex->locked) {
		irq_restore(flags);
		wait_queue_wait(&mutex->waiters, mutex_available_condition, mutex);
		flags = irq_save();
	}

	mutex->locked = 1;
	mutex->owner = self;

	irq_restore(flags);
}

int mutex_try_lock(mutex_t *mutex) {
	if (mutex == 0) {
		kernel_panic("mutex_try_lock received null mutex");
	}

	thread_t *self = thread_current();
	irq_flags_t flags = irq_save();

	if (!mutex->locked) {
		mutex->locked = 1;
		mutex->owner = self;
		irq_restore(flags);
		return 1;
	}

	irq_restore(flags);
	return 0;
}

void mutex_unlock(mutex_t *mutex) {
	if (mutex == 0) {
		kernel_panic("mutex_unlock received null mutex");
	}

	thread_t *self = thread_current();
	irq_flags_t flags = irq_save();

	if (!mutex->locked) {
		irq_restore(flags);
		kernel_panic("mutex_unlock attempted unlock of unlocked mutex");
	}

	if (mutex->owner != self) {
		irq_restore(flags);
		kernel_panic("mutex_unlock attempted by non-owner");
	}

	mutex->locked = 0;
	mutex->owner = 0;

	irq_restore(flags);

	wait_queue_wake_one(&mutex->waiters);
}

int mutex_is_locked(mutex_t *mutex) {
	if (mutex == 0) {
		return 0;
	}

	irq_flags_t flags = irq_save();
	int locked = mutex->locked;
	irq_restore(flags);

	return locked;
}

void semaphore_init(semaphore_t *sem, const char *name, int32_t initial_count) {
	if (sem == 0) {
		kernel_panic("semaphore_init received null semaphore");
	}

	if (initial_count < 0) {
		kernel_panic("semaphore_init received negative count");
	}

	sem->name = name != 0 ? name : "unnamed";
	sem->count = initial_count;
	wait_queue_init(&sem->waiters, sem->name);
}

void semaphore_wait(semaphore_t *sem) {
	if (sem == 0) {
		kernel_panic("semaphore_wait received null semaphore");
	}

	irq_flags_t flags = irq_save();

	while (sem->count <= 0) {
		irq_restore(flags);
		wait_queue_wait(&sem->waiters, semaphore_available_condition, sem);
		flags = irq_save();
	}

	sem->count--;

	irq_restore(flags);
}

int semaphore_try_wait(semaphore_t *sem) {
	if (sem == 0) {
		kernel_panic("semaphore_try_wait received null semaphore");
	}

	irq_flags_t flags = irq_save();

	if (sem->count > 0) {
		sem->count--;
		irq_restore(flags);
		return 1;
	}

	irq_restore(flags);
	return 0;
}

void semaphore_signal(semaphore_t *sem) {
	if (sem == 0) {
		kernel_panic("semaphore_signal received null semaphore");
	}

	irq_flags_t flags = irq_save();

	sem->count++;

	irq_restore(flags);

	wait_queue_wake_one(&sem->waiters);
}

typedef struct sync_mutex_test_arg {
	const char *label;
	mutex_t *mutex;
	volatile uint32_t *shared_counter;
	volatile uint32_t *done_counter;
} sync_mutex_test_arg_t;

typedef struct sync_semaphore_test_arg {
	const char *label;
	semaphore_t *start_sem;
	volatile uint32_t *order_counter;
	volatile uint32_t *done_counter;
} sync_semaphore_test_arg_t;

static void sync_mutex_worker(void *arg) {
	sync_mutex_test_arg_t *test = (sync_mutex_test_arg_t *)arg;

	for (uint32_t i = 0; i < 50; ++i) {
		mutex_lock(test->mutex);

		uint32_t old_value = *test->shared_counter;
		thread_sleep_ticks(1);
		*test->shared_counter = old_value + 1;

		mutex_unlock(test->mutex);

		thread_sleep_ticks(1);
	}

	console_write("Sync test: mutex worker ");
	console_write(test->label);
	console_writeln(" done");

	(*test->done_counter)++;
}

static void sync_semaphore_worker(void *arg) {
	sync_semaphore_test_arg_t *test = (sync_semaphore_test_arg_t *)arg;

	semaphore_wait(test->start_sem);

	uint32_t order = *test->order_counter;
	*test->order_counter = order + 1;

	console_write("Sync test: semaphore worker ");
	console_write(test->label);
	console_write(" passed order ");
	console_write_u32_dec(order);
	console_putc('\n');

	(*test->done_counter)++;
}

void sync_test_once(void) {
	console_writeln("Sync test: starting mutex/semaphore test");

	static mutex_t test_mutex;
	static volatile uint32_t shared_counter;
	static volatile uint32_t mutex_done;

	shared_counter = 0;
	mutex_done = 0;

	mutex_init(&test_mutex, "sync-test-mutex");

	static sync_mutex_test_arg_t mutex_arg_a;
	static sync_mutex_test_arg_t mutex_arg_b;

	mutex_arg_a.label = "M1";
	mutex_arg_a.mutex = &test_mutex;
	mutex_arg_a.shared_counter = &shared_counter;
	mutex_arg_a.done_counter = &mutex_done;

	mutex_arg_b.label = "M2";
	mutex_arg_b.mutex = &test_mutex;
	mutex_arg_b.shared_counter = &shared_counter;
	mutex_arg_b.done_counter = &mutex_done;

	thread_create("mutex-m1", sync_mutex_worker, &mutex_arg_a);
	thread_create("mutex-m2", sync_mutex_worker, &mutex_arg_b);

	while (mutex_done < 2) {
		thread_sleep_ticks(1);
		thread_reap_zombies();
	}

	thread_reap_zombies();

	if (shared_counter != 100) {
		console_write("Sync test: shared_counter=");
		console_write_u32_dec(shared_counter);
		console_putc('\n');
		kernel_panic("mutex test lost updates");
	}

	static semaphore_t sem;
	static volatile uint32_t sem_order;
	static volatile uint32_t sem_done;

	sem_order = 0;
	sem_done = 0;

	semaphore_init(&sem, "sync-test-semaphore", 0);

	static sync_semaphore_test_arg_t sem_arg_a;
	static sync_semaphore_test_arg_t sem_arg_b;

	sem_arg_a.label = "S1";
	sem_arg_a.start_sem = &sem;
	sem_arg_a.order_counter = &sem_order;
	sem_arg_a.done_counter = &sem_done;

	sem_arg_b.label = "S2";
	sem_arg_b.start_sem = &sem;
	sem_arg_b.order_counter = &sem_order;
	sem_arg_b.done_counter = &sem_done;

	thread_create("sem-s1", sync_semaphore_worker, &sem_arg_a);
	thread_create("sem-s2", sync_semaphore_worker, &sem_arg_b);

	thread_sleep_ticks(2);

	semaphore_signal(&sem);
	thread_sleep_ticks(1);

	semaphore_signal(&sem);

	while (sem_done < 2) {
		thread_sleep_ticks(1);
		thread_reap_zombies();
	}

	thread_reap_zombies();

	if (sem_order != 2) {
		kernel_panic("semaphore test did not release two workers");
	}

	console_writeln("Sync test: mutex/semaphore sanity check passed");
}
