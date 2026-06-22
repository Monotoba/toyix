// arch/x86/multiboot.h
#ifndef TOYIX_ARCH_X86_MULTIBOOT_H
#define TOYIX_ARCH_X86_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

#define MULTIBOOT_INFO_MEMORY		(1u << 0)
#define MULTIBOOT_INFO_BOOT_DEVICE 	(1u << 1)
#define MULTIBOOT_INFO_CMDLINE		(1u << 2)
#define MULTIBOOT_INFO_MODS			(1u << 3)
#define MULTIBOOT_INFO_AOUT_SYMS	(1u << 4)
#define MULTIBOOT_INFO_ELF_SHDR		(1u << 5)
#define MULTIBOOT_INFO_MEM_MAP		(1u << 6)


typedef struct multiboot_info {
	uint32_t flags;
	
	uint32_t	mem_lower;
	uint32_t 	mem_upper;
	
	uint32_t	boot_device;
	
	uint32_t	cmdline;
	
	uint32_t	mods_count;
	uint32_t	mods_addr;
	
	uint32_t 	syms[4];
	
	uint32_t 	mmap_length;
	uint32_t 	mmap_addr;
} __attribute__((packed)) multiboot_info_t;


typedef struct multiboot_mmap_entry {
	uint32_t size;
	uint32_t base_addr_low;
	uint32_t base_addr_high;
	uint32_t length_low;
	uint32_t length_high;
	uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;


#endif
