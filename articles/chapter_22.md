# Chapter 22 — First ELF32 Loader Milestone

In Chapter 21, we completed the process lifecycle:

```text
create process
load executable
run user code
exit process
wait for status
destroy process
free user pages
free page tables
free page directory
```

Now we can replace the temporary TOYEXE format with the first real executable-loader milestone: **ELF32**.

ELF is the Executable and Linkable Format used by many Unix-like systems. The ELF specification defines the file header, program header table, and how loadable segments describe a process image; the generic ABI describes program headers as the structures a system uses to prepare a program for execution. ([Linux Foundation Specs][1])

For this first milestone, we will support only a small subset:

```text
32-bit ELF
little-endian
ET_EXEC
EM_386
PT_LOAD segments
static executable shape
in-kernel byte-array image
no relocations
no dynamic linking
no filesystem yet
```

That is enough to replace this:

```text
TOYEXE header
  ↓
TOYEXE loader
```

with this:

```text
ELF32 header
  ↓
program headers
  ↓
PT_LOAD segments
  ↓
entry point
  ↓
user process
```

The milestone output will look like:

```text
Process test: starting ELF32 user program test
ELF32: loaded PT_LOAD vaddr=0x40100000 filesz=256 memsz=320
Process: created pid=1 name=elf-demo
user> toyix
echo: toyix
Syscall: process elf-demo pid=1 exited code 9
Process test: ELF32 load/read/write/sleep/exit cleanup sanity check passed
```

---

# 1. Why ELF now?

TOYEXE gave us the loader shape:

```text
validate
map
copy
zero BSS
set entry
set stack
start process
```

ELF uses the same basic shape, but with a standard format:

```text
ELF header
  tells us architecture, type, entry point, program header location

program header table
  tells us what segments to load

PT_LOAD segment
  tells us file offset, virtual address, file size, memory size, and flags
```

The System V Intel386 ABI supplement is the relevant processor-specific ABI family for 32-bit i386 ELF conventions. ([SCO][2])

We are not yet trying to support every valid ELF file. We are building the smallest useful loader.

---

# 2. Patch overview

Add:

```text
include/kernel/
├── elf32.h
└── elf_loader.h

kernel/
└── elf_loader.c
```

Modify:

```text
kernel/process.c
Makefile
tests/smoke.sh
```

Optionally keep the TOYEXE files in the tree as a historical reference, but remove `kernel/toyexe.o` from the build if it is no longer used.

---

# 3. Add `include/kernel/elf32.h`

This gives us our own freestanding ELF definitions instead of depending on a hosted system’s `<elf.h>`.

```c
// include/kernel/elf32.h
#ifndef TOYIX_KERNEL_ELF32_H
#define TOYIX_KERNEL_ELF32_H

#include <stdint.h>

#define ELF_NIDENT 16u

#define EI_MAG0       0u
#define EI_MAG1       1u
#define EI_MAG2       2u
#define EI_MAG3       3u
#define EI_CLASS      4u
#define EI_DATA       5u
#define EI_VERSION    6u
#define EI_OSABI      7u
#define EI_ABIVERSION 8u

#define ELFMAG0 0x7Fu
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32 1u
#define ELFDATA2LSB 1u

#define EV_NONE    0u
#define EV_CURRENT 1u

#define ET_NONE 0u
#define ET_REL  1u
#define ET_EXEC 2u
#define ET_DYN  3u

#define EM_NONE 0u
#define EM_386  3u

#define PT_NULL 0u
#define PT_LOAD 1u

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;

typedef struct elf32_ehdr {
    unsigned char e_ident[ELF_NIDENT];

    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;

    Elf32_Addr e_entry;
    Elf32_Off  e_phoff;
    Elf32_Off  e_shoff;

    Elf32_Word e_flags;

    Elf32_Half e_ehsize;

    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;

    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct elf32_phdr {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} __attribute__((packed)) elf32_phdr_t;

#endif
```

## Why define these ourselves?

A kernel is freestanding.

That means we should not depend on the host system’s libc headers. We want the kernel source tree to define exactly the ABI structures it uses.

---

# 4. Add `include/kernel/elf_loader.h`

```c
// include/kernel/elf_loader.h
#ifndef TOYIX_KERNEL_ELF_LOADER_H
#define TOYIX_KERNEL_ELF_LOADER_H

#include <stdint.h>
#include "kernel/process.h"

#define ELF_LOADER_OK 0
#define ELF_LOADER_ERR_INVALID -1
#define ELF_LOADER_ERR_UNSUPPORTED -2
#define ELF_LOADER_ERR_TOO_LARGE -3
#define ELF_LOADER_ERR_LOAD_FAILED -4

#define ELF_USER_STACK_TOP  0x70000000u
#define ELF_USER_STACK_SIZE 4096u

int elf_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
);

process_t *elf_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);

#endif
```

---

# 5. Add `kernel/elf_loader.c`

```c
// kernel/elf_loader.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/elf32.h"
#include "kernel/elf_loader.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/process.h"

#define ELF_MAX_IMAGE_MEMORY (1024u * 1024u)

static uintptr_t align_down_page(uintptr_t value) {
    return value & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static uintptr_t align_up_page(uintptr_t value) {
    return (value + PMM_PAGE_SIZE - 1u) &
           ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static int range_overflows(uint32_t start, uint32_t size) {
    return start + size < start;
}

static int validate_elf_header(
    const elf32_ehdr_t *header,
    uint32_t image_size
) {
    if (header == 0) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (image_size < sizeof(elf32_ehdr_t)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (header->e_ident[EI_CLASS] != ELFCLASS32) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    if (header->e_ident[EI_VERSION] != EV_CURRENT) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    if (header->e_type != ET_EXEC) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    if (header->e_machine != EM_386) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    if (header->e_version != EV_CURRENT) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    if (header->e_ehsize != sizeof(elf32_ehdr_t)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (header->e_phoff == 0 || header->e_phnum == 0) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (header->e_phentsize != sizeof(elf32_phdr_t)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (range_overflows(header->e_phoff,
                        header->e_phnum * sizeof(elf32_phdr_t))) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (header->e_phoff +
            header->e_phnum * sizeof(elf32_phdr_t) > image_size) {
        return ELF_LOADER_ERR_INVALID;
    }

    return ELF_LOADER_OK;
}

static int validate_load_segment(
    const elf32_phdr_t *program_header,
    uint32_t image_size
) {
    if (program_header->p_type != PT_LOAD) {
        return ELF_LOADER_OK;
    }

    if (program_header->p_memsz == 0) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (program_header->p_filesz > program_header->p_memsz) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (range_overflows(program_header->p_offset,
                        program_header->p_filesz)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (program_header->p_offset + program_header->p_filesz > image_size) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (range_overflows(program_header->p_vaddr,
                        program_header->p_memsz)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (program_header->p_memsz > ELF_MAX_IMAGE_MEMORY) {
        return ELF_LOADER_ERR_TOO_LARGE;
    }

    if (program_header->p_vaddr < ADDRESS_SPACE_USER_BASE ||
        program_header->p_vaddr >= ADDRESS_SPACE_USER_TOP) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (program_header->p_vaddr + program_header->p_memsz >
        ADDRESS_SPACE_USER_TOP) {
        return ELF_LOADER_ERR_INVALID;
    }

    /*
     * First milestone restriction:
     *
     * We require page alignment for loadable segment virtual addresses.
     * Real ELF loaders handle more combinations of p_vaddr, p_offset, and
     * p_align. We will loosen this later.
     */
    if ((program_header->p_vaddr & (PMM_PAGE_SIZE - 1u)) != 0) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    return ELF_LOADER_OK;
}

static uint32_t segment_flags_to_process_flags(const elf32_phdr_t *phdr) {
    uint32_t flags = ADDRESS_SPACE_FLAG_USER;

    /*
     * Our current process_map_user_region() maps pages writable. We still
     * compute flags here because later chapters will add read-only text pages.
     */
    if ((phdr->p_flags & PF_W) != 0) {
        flags |= ADDRESS_SPACE_FLAG_WRITABLE;
    } else {
        flags |= ADDRESS_SPACE_FLAG_WRITABLE;
    }

    return flags;
}

static int load_segment(
    process_t *process,
    const uint8_t *image,
    const elf32_phdr_t *phdr
) {
    uintptr_t segment_start = align_down_page(phdr->p_vaddr);
    uintptr_t segment_end = align_up_page(phdr->p_vaddr + phdr->p_memsz);

    uint32_t runtime_size = (uint32_t)(segment_end - segment_start);

    /*
     * The process helper currently maps writable user pages. The computed
     * flags will become important once process_map_user_region accepts page
     * permissions.
     */
    (void)segment_flags_to_process_flags(phdr);

    if (process_map_user_region(process, segment_start, runtime_size) != 0) {
        return ELF_LOADER_ERR_LOAD_FAILED;
    }

    if (process_copy_to_user_init(
            process,
            phdr->p_vaddr,
            image + phdr->p_offset,
            phdr->p_filesz
        ) != 0) {
        return ELF_LOADER_ERR_LOAD_FAILED;
    }

    if (phdr->p_memsz > phdr->p_filesz) {
        uintptr_t bss_start = phdr->p_vaddr + phdr->p_filesz;
        uint32_t bss_size = phdr->p_memsz - phdr->p_filesz;

        if (process_zero_user_init(process, bss_start, bss_size) != 0) {
            return ELF_LOADER_ERR_LOAD_FAILED;
        }
    }

    console_write("ELF32: loaded PT_LOAD vaddr=");
    console_write_hex32(phdr->p_vaddr);
    console_write(" filesz=");
    console_write_u32_dec(phdr->p_filesz);
    console_write(" memsz=");
    console_write_u32_dec(phdr->p_memsz);
    console_putc('\n');

    return ELF_LOADER_OK;
}

int elf_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
) {
    if (process == 0 || image == 0) {
        return ELF_LOADER_ERR_INVALID;
    }

    const elf32_ehdr_t *header = (const elf32_ehdr_t *)image;

    int rc = validate_elf_header(header, image_size);

    if (rc != ELF_LOADER_OK) {
        return rc;
    }

    const elf32_phdr_t *program_headers =
        (const elf32_phdr_t *)(image + header->e_phoff);

    int saw_load = 0;

    for (uint32_t i = 0; i < header->e_phnum; ++i) {
        const elf32_phdr_t *phdr = &program_headers[i];

        rc = validate_load_segment(phdr, image_size);

        if (rc != ELF_LOADER_OK) {
            return rc;
        }

        if (phdr->p_type == PT_LOAD) {
            saw_load = 1;
        }
    }

    if (!saw_load) {
        return ELF_LOADER_ERR_INVALID;
    }

    /*
     * Map stack before loading the image. If this fails, process creation
     * fails before the thread can run.
     */
    uintptr_t stack_top = ELF_USER_STACK_TOP;
    uintptr_t stack_base = stack_top - ELF_USER_STACK_SIZE;

    if (process_map_user_region(
            process,
            stack_base,
            ELF_USER_STACK_SIZE
        ) != 0) {
        return ELF_LOADER_ERR_LOAD_FAILED;
    }

    if (process_zero_user_init(
            process,
            stack_base,
            ELF_USER_STACK_SIZE
        ) != 0) {
        return ELF_LOADER_ERR_LOAD_FAILED;
    }

    process_set_user_stack(process, stack_base, stack_top);

    for (uint32_t i = 0; i < header->e_phnum; ++i) {
        const elf32_phdr_t *phdr = &program_headers[i];

        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        rc = load_segment(process, image, phdr);

        if (rc != ELF_LOADER_OK) {
            return rc;
        }
    }

    if (header->e_entry < ADDRESS_SPACE_USER_BASE ||
        header->e_entry >= ADDRESS_SPACE_USER_TOP) {
        return ELF_LOADER_ERR_INVALID;
    }

    process->user_code_base = header->e_entry;
    process_set_user_entry(process, header->e_entry);

    console_write("ELF32: entry=");
    console_write_hex32(header->e_entry);
    console_putc('\n');

    return ELF_LOADER_OK;
}

process_t *elf_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
) {
    process_t *process = process_create_empty(name);

    int rc = elf_load_process(process, image, image_size);

    if (rc != ELF_LOADER_OK) {
        console_write("ELF32: load failed rc=");
        console_write_u32_dec((uint32_t)(-rc));
        console_putc('\n');

        kernel_panic("ELF32 process load failed");
    }

    process_start_user(process);

    return process;
}
```

---

# 6. Why this loader ignores section headers

ELF files have two major views:

```text
program headers
  runtime loading view

section headers
  linker/debugger/static-analysis view
```

The generic ABI describes the program header table as the array of structures used by the system to prepare a program for execution. For loading an executable process image, program headers are the part we care about first. ([Linux Foundation Specs][3])

So this first loader ignores:

```text
section headers
symbol tables
string tables
relocation sections
debug sections
```

That is normal for a minimal executable loader.

---

# 7. Replace the test image builder in `kernel/process.c`

Now we build a minimal ELF image instead of a TOYEXE image.

Update the includes.

Remove:

```c
#include "kernel/toyexe.h"
```

Add:

```c
#include "kernel/elf32.h"
#include "kernel/elf_loader.h"
```

Now replace the TOYEXE-specific constants with ELF constants.

```c
#define DEMO_SEGMENT_VA 0x40100000u
#define DEMO_IMAGE_SIZE 256u
#define DEMO_BSS_SIZE   64u

#define DEMO_PROMPT_VA  (DEMO_SEGMENT_VA + 0xA0u)
#define DEMO_PREFIX_VA  (DEMO_SEGMENT_VA + 0xA8u)
#define DEMO_NEWLINE_VA (DEMO_SEGMENT_VA + 0xB0u)
#define DEMO_INPUT_VA   (DEMO_SEGMENT_VA + DEMO_IMAGE_SIZE)

#define DEMO_INPUT_MAX 32u
```

Keep the emitter helpers from Chapter 20:

```c
emit_u8()
emit_u32()
emit_mov_eax_imm32()
emit_mov_ebx_imm32()
emit_mov_ecx_imm32()
emit_mov_edx_imm32()
emit_int80()
```

Update `build_stdio_demo_payload()` so it references `DEMO_SEGMENT_VA`, not `TOYEXE_USER_BASE`.

```c
static void build_stdio_demo_payload(uint8_t *program, uint32_t program_size) {
    memset(program, 0x90, program_size);

    uint32_t offset = 0;

    /*
     * write(1, "user> ", 6)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_PROMPT_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    /*
     * bytes = read(0, input_buffer, 32)
     */
    emit_mov_eax_imm32(program, &offset, SYS_READ);
    emit_mov_ebx_imm32(program, &offset, FD_STDIN);
    emit_mov_ecx_imm32(program, &offset, DEMO_INPUT_VA);
    emit_mov_edx_imm32(program, &offset, DEMO_INPUT_MAX);
    emit_int80(program, &offset);

    /*
     * mov esi, eax
     */
    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xC6u);

    /*
     * write(1, "echo: ", 6)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_PREFIX_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    /*
     * write(1, input_buffer, esi)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_INPUT_VA);

    /*
     * mov edx, esi
     */
    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xF2u);

    emit_int80(program, &offset);

    /*
     * write(1, "\n", 1)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_NEWLINE_VA);
    emit_mov_edx_imm32(program, &offset, 1);
    emit_int80(program, &offset);

    /*
     * sleep(3)
     */
    emit_mov_eax_imm32(program, &offset, SYS_SLEEP);
    emit_mov_ebx_imm32(program, &offset, 3);
    emit_int80(program, &offset);

    /*
     * exit(9)
     */
    emit_mov_eax_imm32(program, &offset, SYS_EXIT);
    emit_mov_ebx_imm32(program, &offset, 9);
    emit_int80(program, &offset);

    /*
     * jmp $
     */
    emit_u8(program, &offset, 0xEBu);
    emit_u8(program, &offset, 0xFEu);

    if (offset >= 0xA0u) {
        kernel_panic("ELF demo program overlapped data area");
    }

    const char prompt[] = "user> ";
    const char prefix[] = "echo: ";
    const char newline[] = "\n";

    memcpy(
        &program[DEMO_PROMPT_VA - DEMO_SEGMENT_VA],
        prompt,
        sizeof(prompt) - 1u
    );

    memcpy(
        &program[DEMO_PREFIX_VA - DEMO_SEGMENT_VA],
        prefix,
        sizeof(prefix) - 1u
    );

    memcpy(
        &program[DEMO_NEWLINE_VA - DEMO_SEGMENT_VA],
        newline,
        1u
    );
}
```

The lower `0x80`/`0x88`/`0x90` data offsets look tidy on paper, but with the current emitted instruction stream they overlap the generated code. In the working code, the embedded strings start at `0xA0` instead so the payload and its data stay separated cleanly.

Now add the ELF image builder.

```c
static uint32_t build_stdio_demo_elf(
    uint8_t *image,
    uint32_t image_capacity
) {
    uint32_t payload_offset =
        sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t);

    uint32_t total_size = payload_offset + DEMO_IMAGE_SIZE;

    if (image_capacity < total_size) {
        kernel_panic("ELF demo image buffer too small");
    }

    memset(image, 0, image_capacity);

    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)image;
    elf32_phdr_t *phdr =
        (elf32_phdr_t *)(image + sizeof(elf32_ehdr_t));

    ehdr->e_ident[EI_MAG0] = ELFMAG0;
    ehdr->e_ident[EI_MAG1] = ELFMAG1;
    ehdr->e_ident[EI_MAG2] = ELFMAG2;
    ehdr->e_ident[EI_MAG3] = ELFMAG3;

    ehdr->e_ident[EI_CLASS] = ELFCLASS32;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;

    ehdr->e_type = ET_EXEC;
    ehdr->e_machine = EM_386;
    ehdr->e_version = EV_CURRENT;

    ehdr->e_entry = DEMO_SEGMENT_VA;

    ehdr->e_phoff = sizeof(elf32_ehdr_t);
    ehdr->e_shoff = 0;
    ehdr->e_flags = 0;

    ehdr->e_ehsize = sizeof(elf32_ehdr_t);
    ehdr->e_phentsize = sizeof(elf32_phdr_t);
    ehdr->e_phnum = 1;

    ehdr->e_shentsize = 0;
    ehdr->e_shnum = 0;
    ehdr->e_shstrndx = 0;

    phdr->p_type = PT_LOAD;
    phdr->p_offset = payload_offset;
    phdr->p_vaddr = DEMO_SEGMENT_VA;
    phdr->p_paddr = DEMO_SEGMENT_VA;
    phdr->p_filesz = DEMO_IMAGE_SIZE;
    phdr->p_memsz = DEMO_IMAGE_SIZE + DEMO_BSS_SIZE;
    phdr->p_flags = PF_R | PF_W | PF_X;
    phdr->p_align = PMM_PAGE_SIZE;

    build_stdio_demo_payload(
        image + payload_offset,
        DEMO_IMAGE_SIZE
    );

    return total_size;
}
```

Finally, replace `process_test_once()`.

```c
void process_test_once(void) {
    console_writeln("Process test: starting ELF32 user program test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    static uint8_t elf_image[
        sizeof(elf32_ehdr_t) +
        sizeof(elf32_phdr_t) +
        DEMO_IMAGE_SIZE
    ];

    uint32_t elf_size = build_stdio_demo_elf(
        elf_image,
        sizeof(elf_image)
    );

    process_t *process = elf_create_process(
        "elf-demo",
        elf_image,
        elf_size
    );

    /*
     * Let the user process start, print its prompt, and block in SYS_READ.
     */
    thread_sleep_ticks(2);

    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('y');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('\n');

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("ELF process test received wrong exit code");
    }

    process_destroy(process);

    console_writeln("Process test: ELF32 load/read/write/sleep/exit cleanup sanity check passed");
}
```

---

# 8. Why we still build the ELF image in C

This chapter is the loader milestone, not the userland toolchain milestone.

The image is now a real ELF-shaped byte array:

```text
ELF header
program header
PT_LOAD payload
```

But the payload is still generated by our tiny machine-code emitter.

That keeps the chapter focused.

The next step can introduce:

```text
user programs compiled with i686-elf-gcc
linker script for user virtual address
objcopy/incbin embedding
kernel loading compiled ELF bytes
```

That will remove the emitter.

---

# 9. Update `Makefile`

Remove this object if it is still present:

```make
build/kernel/toyexe.o
```

Add:

```make
build/kernel/elf_loader.o
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
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/address_space.o \
    build/kernel/elf_loader.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/process.o \
    build/kernel/sync.o \
    build/kernel/syscall.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the process-related greps.

Replace the Chapter 21 TOYEXE greps:

```make
grep -q "Process test: starting TOYEXE lifecycle cleanup test" build/test.log
grep -q "TOYEXE: loaded image bytes=256 bss=64" build/test.log
grep -q "Process: created pid=1 name=toyexe-demo" build/test.log
grep -q "Syscall: process toyexe-demo pid=1 exited code 9" build/test.log
grep -q "Process: destroyed pid=1 name=toyexe-demo" build/test.log
grep -q "Process test: TOYEXE lifecycle cleanup sanity check passed" build/test.log
```

with:

```make
grep -q "Process test: starting ELF32 user program test" build/test.log
grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000 filesz=256 memsz=320" build/test.log
grep -q "ELF32: entry=0x40100000" build/test.log
grep -q "Process: created pid=1 name=elf-demo" build/test.log
grep -q "echo: toyix" build/test.log
grep -q "Syscall: process elf-demo pid=1 exited code 9" build/test.log
grep -q "Process: destroyed pid=1 name=elf-demo" build/test.log
grep -q "Process test: ELF32 load/read/write/sleep/exit cleanup sanity check passed" build/test.log
```

The process block should now be:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting ELF32 user program test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "ELF32: loaded PT_LOAD vaddr=0x40100000 filesz=256 memsz=320" build/test.log
	grep -q "ELF32: entry=0x40100000" build/test.log
	grep -q "Process: created pid=1 name=elf-demo" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process elf-demo pid=1 exited code 9" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=elf-demo" build/test.log
	grep -q "Process test: ELF32 load/read/write/sleep/exit cleanup sanity check passed" build/test.log
```

Update the final success line:

```make
	@echo "Boot, memory, heap, sync, monitor, address-space, and ELF32 loader smoke test passed."
```

---

# 10. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 22 checks passed."
```

---

# 11. Expected output

A successful boot should include:

```text
Process test: starting ELF32 user program test
Address space: created process page directory
ELF32: loaded PT_LOAD vaddr=0x40100000 filesz=256 memsz=320
ELF32: entry=0x40100000
Thread: created elf-demo id=...
Process: created pid=1 name=elf-demo
user> toyix
echo: toyix
Syscall: process elf-demo pid=1 exited code 9
Threads: reaping zombie elf-demo id=...
Address space: destroyed process page directory, user pages=2 tables=2
Process: destroyed pid=1 name=elf-demo
Process test: ELF32 load/read/write/sleep/exit cleanup sanity check passed
```

This proves:

```text
ELF magic validated
ELF class/data/type/machine validated
program header table parsed
PT_LOAD segment mapped
segment file bytes copied
segment BSS zeroed
entry point used
stack mapped
user process ran
syscalls worked
process cleaned up
```

That is the first real ELF loader milestone.

---

# 12. Common failures

## Failure: `ELF32 process load failed`

Print the return code from `elf_load_process()`.

Most likely causes:

```text
bad ELF magic
wrong e_ehsize
wrong e_phentsize
e_phoff points outside file
e_phnum is zero
PT_LOAD p_offset + p_filesz exceeds file size
entry address outside user range
```

Check these fields in the builder:

```c
ehdr->e_phoff = sizeof(elf32_ehdr_t);
ehdr->e_phentsize = sizeof(elf32_phdr_t);
ehdr->e_phnum = 1;
```

## Failure: unsupported segment

This first loader requires page-aligned `p_vaddr`:

```c
if ((program_header->p_vaddr & (PMM_PAGE_SIZE - 1u)) != 0) {
    return ELF_LOADER_ERR_UNSUPPORTED;
}
```

The demo uses:

```c
#define DEMO_SEGMENT_VA 0x40100000u
```

which is page-aligned.

## Failure: page fault at the input buffer

The input buffer is in BSS:

```c
DEMO_INPUT_VA = DEMO_SEGMENT_VA + DEMO_IMAGE_SIZE
```

That means the loader must map `p_memsz`, not only `p_filesz`.

The demo segment uses:

```text
filesz = 256
memsz  = 320
```

If only 256 bytes are mapped, the BSS input buffer may fault or fail `copy_to_user()`.

## Failure: `echo: toyix` missing

Likely causes:

```text
SYS_READ did not copy into user buffer
BSS was not mapped or zeroed
keyboard injection did not include newline
process address space was not active during syscall
```

Check:

```c
keyboard_debug_inject_char('\n');
```

and:

```c
process_zero_user_init(process, bss_start, bss_size);
```

## Failure: process destroy frees unexpected page/table counts

The current demo should usually free:

```text
user pages=2
tables=2
```

One page is the loaded ELF segment. One page is the user stack.

There are two tables because:

```text
0x40100000 and 0x6FFFF000
```

normally live under different page-directory entries.

If your counts differ, check whether stack top, segment address, or mapping strategy changed.

---

# 13. What this chapter achieved

Before this chapter:

```text
temporary TOYEXE format
custom header
toyexe loader
```

After this chapter:

```text
ELF32 header
program header table
PT_LOAD segment loading
entry point
BSS zeroing
user stack
process cleanup
```

The executable loader boundary is now much closer to a real OS.

---

# 14. Design limitations

This is still only the first ELF milestone.

Current limitations:

```text
only ELFCLASS32
only little-endian
only ET_EXEC
only EM_386
only simple PT_LOAD segments
requires page-aligned p_vaddr
does not enforce read-only text
does not support multiple overlapping segments
does not support relocations
does not support ET_DYN
does not load from filesystem
does not pass argc/argv/envp
does not build a user stack ABI
```

But this is a strong step.

The kernel now understands the structure of a real executable file.

---

# 15. Commit this chapter

After tests pass:

```bash
git status
git add .
git commit -m "Add initial ELF32 user process loader"
```

---

# 16. Next chapter

The next chapter should remove the last major artificial piece: the in-kernel machine-code emitter.

We should add a tiny user-program build pipeline:

```text
user/
├── include/toyix_syscall.h
├── crt0.S
├── demo.c
└── linker.ld
```

Then the build can:

```text
compile user/demo.c to ELF32
convert the ELF into an object or C array
link it into the kernel
load it through elf_loader.c
```

That gives us:

```text
real compiled user C program
real ELF file
kernel ELF loader
syscalls from C wrappers
```

That is the bridge from “toy bytecode inside the kernel” to actual userland.

---

# 17. Resources

- [Chapter 22 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_22)
- [Chapter 22 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_22)
- [Linux Foundation Specs: ELF and ABI Standards](https://refspecs.linuxbase.org/elf/index.html)
- [System V ABI Intel386 Supplement](https://www.sco.com/developers/devspecs/abi386-4.pdf)
- [Linux Foundation Specs: Program Header](https://refspecs.linuxbase.org/elf/gabi4%2B/ch5.pheader.html)

---

# 18. Closure

Chapter 22 moves Toyix from a custom tutorial executable format to the first real ELF32 loader milestone. The kernel now validates ELF headers, walks program headers, maps a `PT_LOAD` segment, zeros BSS, enters user mode through the ELF entry point, and tears the process down cleanly after exit.

Happy Coding!

[1]: https://refspecs.linuxbase.org/elf/index.html?utm_source=chatgpt.com "ELF and ABI Standards"
[2]: https://www.sco.com/developers/devspecs/abi386-4.pdf?utm_source=chatgpt.com "SYSTEM V APPLICATION BINARY INTERFACE Intel386 ..."
[3]: https://refspecs.linuxbase.org/elf/gabi4%2B/ch5.pheader.html?utm_source=chatgpt.com "Program Header"
