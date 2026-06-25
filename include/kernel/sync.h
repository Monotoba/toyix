#ifndef TOYIX_KERNEL_SYNC_H
#define TOYIX_KERNEL_SYNC_H

#include <stdint.h>
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

typedef struct mutex {
	const char *name;
	int locked;
	thread_t *owner;
	wait_queue_t waiters;
} mutex_t;

typedef struct semaphore {
	const char *name;
	int32_t count;
	wait_queue_t waiters;
} semaphore_t;

void mutex_init(mutex_t *mutex, const char *name);
void mutex_lock(mutex_t *mutex);
int mutex_try_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);
int mutex_is_locked(mutex_t *mutex);

void semaphore_init(semaphore_t *sem, const char *name, int32_t initial_count);
void semaphore_wait(semaphore_t *sem);
int semaphore_try_wait(semaphore_t *sem);
void semaphore_signal(semaphore_t *sem);

void sync_test_once(void);

#endif
