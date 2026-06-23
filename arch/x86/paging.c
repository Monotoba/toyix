// arch/x86/paging.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/paging.h"
#include "kernel/console.h"
#include "kernel/panic.h"

#define PAGE_DIRECTORY_ENTRIES 1024u
#define PAGE_TABLE_ENTRIES     1024u

#define IDENTITY_MAP_MIB       16u
#define IDENTITY_TABLE_COUNT   (IDENTITY_MAP_MIB / 4u)



typedef uint32_t page_directory_entry_t;
typedef uint32_t page_table_entry_t;

static page_directory_entry_t page_directory[PAGE_DIRECTORY_ENTRIES]
    __attribute__((aligned(X86_PAGE_SIZE)));

static page_table_entry_t identity_tables[IDENTITY_TABLE_COUNT][PAGE_TABLE_ENTRIES]
    __attribute__((aligned(X86_PAGE_SIZE)));

static volatile uint32_t paging_test_word = 0x12345678u;

extern void paging_load_directory(uint32_t physical_addr);
extern void paging_enable_asm(void);
extern uint32_t paging_read_cr0(void);
extern uint32_t paging_read_cr2(void);
extern uint32_t paging_read_cr3(void);



static void zero_page_directory(void) {
    for (uint32_t i = 0; i < PAGE_DIRECTORY_ENTRIES; ++i) {
        page_directory[i] = 0;
    }
}



static void build_identity_tables(void) {
    for (uint32_t table = 0; table < IDENTITY_TABLE_COUNT; ++table) {
        for (uint32_t entry = 0; entry < PAGE_TABLE_ENTRIES; ++entry) {
            uint32_t physical_addr =
                ((table * PAGE_TABLE_ENTRIES) + entry) * X86_PAGE_SIZE;

            identity_tables[table][entry] =
                (physical_addr & X86_PAGE_FRAME_MASK) |
                X86_PAGE_PRESENT |
                X86_PAGE_WRITABLE;
        }

        page_directory[table] =
            ((uint32_t)&identity_tables[table][0] & X86_PAGE_FRAME_MASK) |
            X86_PAGE_PRESENT |
            X86_PAGE_WRITABLE;
    }
}



static void print_page_fault_error(uint32_t error_code) {
    console_write("Page fault error bits: ");

    if ((error_code & 0x01u) != 0) {
        console_write("present-protection ");
    } else {
        console_write("not-present ");
    }

    if ((error_code & 0x02u) != 0) {
        console_write("write ");
    } else {
        console_write("read ");
    }

    if ((error_code & 0x04u) != 0) {
        console_write("user ");
    } else {
        console_write("supervisor ");
    }

    if ((error_code & 0x08u) != 0) {
        console_write("reserved-bit ");
    }

    if ((error_code & 0x10u) != 0) {
        console_write("instruction-fetch ");
    }

    console_putc('\n');
}



static void page_fault_handler(interrupt_frame_t *frame) {
    uint32_t fault_addr = paging_read_cr2();

    console_writeln("");
    console_writeln("*** PAGE FAULT ***");

    console_write("Fault address CR2=");
    console_write_hex32(fault_addr);
    console_putc('\n');

    console_write("EIP=");
    console_write_hex32(frame->eip);
    console_write(" error=");
    console_write_hex32(frame->error_code);
    console_putc('\n');

    print_page_fault_error(frame->error_code);

    kernel_panic("page fault");
}



void paging_init(void) {
    console_writeln("Paging: building identity map");

    zero_page_directory();
    build_identity_tables();

    interrupt_register_handler(14, page_fault_handler);

    paging_load_directory((uint32_t)&page_directory[0]);
    paging_enable_asm();

    if (!paging_is_enabled()) {
        kernel_panic("paging enable bit did not stick");
    }

    console_write("Paging: enabled with identity map of first ");
    console_write_u32_dec(IDENTITY_MAP_MIB);
    console_writeln(" MiB");

    console_write("Paging: CR3=");
    console_write_hex32(paging_read_cr3());
    console_putc('\n');
}

int paging_is_enabled(void) {
    return (paging_read_cr0() & 0x80000000u) != 0;
}



void paging_test_identity_mapping(void) {
    uint32_t before = paging_test_word;

    paging_test_word = 0xCAFEBABEu;

    if (paging_test_word != 0xCAFEBABEu) {
        kernel_panic("paging identity test failed: write/read mismatch");
    }

    paging_test_word = before;

    console_writeln("Paging test: identity-mapped kernel data is readable/writable");
}


uint32_t *paging_get_kernel_directory(void) {
	return page_directory;
}




