// include/kernel/program.h
#ifndef TOYIX_KERNEL_PROGRAM_H
#define TOYIX_KERNEL_PROGRAM_H

#include <stdint.h>
#include "kernel/process.h"

typedef struct embedded_program {
    const char *name;
    const char *description;

    const uint8_t *image_start;
    const uint8_t *image_end;
} embedded_program_t;

void program_registry_init(void);

const embedded_program_t *program_find(const char *name);
void program_list(void);

process_t *program_create_process(
    const char *name,
    int argc,
    const char **argv
);

int program_run_background(
    const char *name,
    int argc,
    const char **argv,
    process_t **process_out
);

int program_run_foreground(
    const char *name,
    int argc,
    const char **argv,
    uint32_t *exit_code_out
);

void program_test_once(void);

#endif
