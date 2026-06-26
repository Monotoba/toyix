// kernel/program.c
#include <stddef.h>
#include <stdint.h>
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/elf_loader.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/string.h"
#include "kernel/thread.h"

extern const uint8_t user_demo_elf_start[];
extern const uint8_t user_demo_elf_end[];

static const embedded_program_t programs[] = {
    {
        .name = "demo",
        .description = "compiled user-mode demo program",
        .image_start = user_demo_elf_start,
        .image_end = user_demo_elf_end
    }
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
    process_t *process = program_create_process(name, argc, argv);

    if (process == 0) {
        return -1;
    }

    uint32_t exit_code = process_wait(process);

    process_destroy(process);

    if (exit_code_out != 0) {
        *exit_code_out = exit_code;
    }

    return 0;
}

void program_test_once(void) {
    console_writeln("Program test: starting embedded program run test");

    static const char *argv[] = {
        "demo",
        "alpha",
        "beta"
    };

    process_t *process = program_create_process("demo", 3, argv);

    if (process == 0) {
        kernel_panic("program test could not launch demo");
    }

    thread_sleep_ticks(2);

    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('y');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('\n');

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("program test received wrong exit code");
    }

    process_destroy(process);

    console_writeln("Program test: embedded ELF program run cleanup sanity check passed");
}
