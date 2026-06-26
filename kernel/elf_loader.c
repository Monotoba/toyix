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

    uint32_t phdr_bytes = (uint32_t)header->e_phnum * sizeof(elf32_phdr_t);

    if (range_overflows(header->e_phoff, phdr_bytes)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (header->e_phoff + phdr_bytes > image_size) {
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

    if (range_overflows(program_header->p_offset, program_header->p_filesz)) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (program_header->p_offset + program_header->p_filesz > image_size) {
        return ELF_LOADER_ERR_INVALID;
    }

    if (range_overflows(program_header->p_vaddr, program_header->p_memsz)) {
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

    if ((program_header->p_vaddr & (PMM_PAGE_SIZE - 1u)) != 0) {
        return ELF_LOADER_ERR_UNSUPPORTED;
    }

    return ELF_LOADER_OK;
}

static uint32_t segment_flags_to_process_flags(const elf32_phdr_t *phdr) {
    uint32_t flags = ADDRESS_SPACE_FLAG_USER;

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

    uintptr_t stack_top = ELF_USER_STACK_TOP;
    uintptr_t stack_base = stack_top - ELF_USER_STACK_SIZE;

    if (process_map_user_region(process, stack_base, ELF_USER_STACK_SIZE) != 0) {
        return ELF_LOADER_ERR_LOAD_FAILED;
    }

    if (process_zero_user_init(process, stack_base, ELF_USER_STACK_SIZE) != 0) {
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
