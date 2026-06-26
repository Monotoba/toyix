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

#define ELFCLASS32  1u
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
