// kernel/program.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/elf_loader.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/string.h"

#define DECLARE_EMBEDDED_PROGRAM(symbol) \
    extern const uint8_t user_##symbol##_elf_start[]; \
    extern const uint8_t user_##symbol##_elf_end[]

#define EMBEDDED_PROGRAM(symbol, public_name, text) \
    { \
        .name = public_name, \
        .description = text, \
        .image_start = user_##symbol##_elf_start, \
        .image_end = user_##symbol##_elf_end \
    }

DECLARE_EMBEDDED_PROGRAM(demo);
DECLARE_EMBEDDED_PROGRAM(counter);

static const embedded_program_t programs[] = {
    EMBEDDED_PROGRAM(
        demo,
        "demo",
        "interactive stdin/stdout demo"
    ),
    EMBEDDED_PROGRAM(
        counter,
        "counter",
        "background-safe counter demo"
    )
};

static const uint32_t program_count =
    sizeof(programs) / sizeof(programs[0]);

void program_registry_init(void) {
    console_write("Program registry: registered ");
    console_write_u32_dec(program_count);
    console_writeln(" embedded program(s)");
}

const embedded_program_t *program_find(const char *name) {
    if (name == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < program_count; ++i) {
        if (kstrcmp(programs[i].name, name) == 0) {
            return &programs[i];
        }
    }

    return 0;
}

void program_list(void) {
    console_writeln("Embedded programs:");

    for (uint32_t i = 0; i < program_count; ++i) {
        console_write("  ");
        console_write(programs[i].name);
        console_write(" - ");
        console_writeln(programs[i].description);
    }
}

static uint32_t program_image_size(const embedded_program_t *program) {
    if (program == 0 ||
        program->image_start == 0 ||
        program->image_end == 0 ||
        program->image_end <= program->image_start) {
        return 0;
    }

    return (uint32_t)(program->image_end - program->image_start);
}

process_t *program_create_process(
    const char *name,
    int argc,
    const char **argv
) {
    const embedded_program_t *program = program_find(name);

    if (program == 0) {
        return 0;
    }

    uint32_t image_size = program_image_size(program);

    if (image_size == 0) {
        kernel_panic("embedded program image is empty");
    }

    process_t *process = elf_create_process_suspended(
        program->name,
        program->image_start,
        image_size
    );

    const char *default_argv[] = {
        program->name
    };

    if (argc <= 0 || argv == 0) {
        argc = 1;
        argv = default_argv;
    }

    if (process_setup_arguments(process, argc, argv) != 0) {
        kernel_panic("program argument setup failed");
    }

    console_write("Program: launching ");
    console_write(program->name);
    console_write(" argc=");
    console_write_u32_dec((uint32_t)argc);
    console_putc('\n');

    process_start_user(process);

    return process;
}

int program_run_foreground(
    const char *name,
    int argc,
    const char **argv,
    uint32_t *exit_code_out
) {
    process_t *process = 0;

    int rc = program_run_background(name, argc, argv, &process);

    if (rc != 0 || process == 0) {
        return -1;
    }

    uint32_t exit_code = process_wait(process);

    process_destroy(process);

    if (exit_code_out != 0) {
        *exit_code_out = exit_code;
    }

    return 0;
}

int program_run_background(
    const char *name,
    int argc,
    const char **argv,
    process_t **process_out
) {
    process_t *process = program_create_process(name, argc, argv);

    if (process == 0) {
        return -1;
    }

    if (process_out != 0) {
        *process_out = process;
    }

    return 0;
}

void program_test_once(void) {
    console_writeln("Program test: starting background counter test");

    static const char *argv[] = {
        "counter",
        "alpha",
        "beta"
    };

    process_t *process = 0;

    int rc = program_run_background("counter", 3, argv, &process);

    if (rc != 0 || process == 0) {
        kernel_panic("program test could not launch counter");
    }

    uint32_t pid = process->pid;

    console_write("Program test: background pid=");
    console_write_u32_dec(pid);
    console_putc('\n');

    process_list();

    process_t *found = process_find(pid);

    if (found != process) {
        kernel_panic("program test could not find background process by PID");
    }

    uint32_t exit_code = process_wait(process);

    if (exit_code != 4) {
        kernel_panic("program test received wrong counter exit code");
    }

    process_list();

    process_destroy(process);

    console_writeln("Program test: background counter cleanup sanity check passed");
}
