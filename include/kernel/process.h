// include/kernel/process.h
#ifndef TOYIX_KERNEL_PROCESS_H
#define TOYIX_KERNEL_PROCESS_H

#include <stdint.h>
#include "kernel/address_space.h"

struct thread;

typedef enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_EXITED
} process_state_t;

typedef struct process {
    uint32_t magic;
    uint32_t pid;

    const char *name;
    process_state_t state;

    address_space_t *address_space;

    struct thread *main_thread;

    uint32_t exit_code;
    int exited;

    uintptr_t user_code_base;
    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
} process_t;

void process_init_system(void);

process_t *process_create_user(
    const char *name,
    const uint8_t *program,
    uint32_t program_size
);

process_t *process_current(void);

void process_exit_current(uint32_t exit_code);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

void process_test_once(void);

#endif
