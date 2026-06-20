// arch/x86/gdt.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/console.h"

typedef struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;   
} __attribute__((packed)) gdt_entry_t;

typedef struct gdt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_pointer_t;

static gdt_entry_t gdt[3];
static gdt_pointer_t gdt_ptr;

extern void gdt_flush(uint32_t gdt_pointer_addr);

static void gdt_set_entry(
    int index,
    uint32_t base,
    uint32_t limit,
    uint8_t  access,
    uint8_t  granularity
) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFFu);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFFu);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFFu);
    
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFFu);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0Fu);
    
    gdt[index].granularity |= (int8_t) (granularity & 0xF0u);
    gdt[index].access = access; 
}


void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base = (uint32_t) & gdt[0];
    
    /*
     * Entry 0: mandatory null descriptor.
     *
     * A null selector is invalid. Keeping entry 0 empty helps catch bugs
     * where a segment register accidentally receives selector 0.
     */
     gdt_set_entry(0, 0, 0, 0, 0);
     
     /*
      * Entry 1: kernel code segment.
      *
      * base = 0
      * limit = 0xFFFFF with 4 KiB granularity, giving a flat 4 GiB range
      *
      * access 0x9A:
      *   present
      *   ring 0
      *   code segment
      *   executable
      *   readable
      *
      * granularity 0xCF:
      *   high limit bits = 0x0F
      *   4 KiB granularity
      *   32-bit protected-mode segment
      */
      gdt_set_entry(1, 0, 0xFFFFFu, 0x9Au, 0xCFu);
      
      /*
       * Entry 2: kernel data segment.
       *
       * access 0x92:
       *   present
       *   ring 0
       *   data segment
       *   wrtable
       */
       gdt_set_entry(2, 0, 0xFFFFFu, 0x92u, 0xCFu);
       
       gdt_flush((uint32_t) & gdt_ptr);
       
       console_writeln("GDT: installed flat kernel code/data segments");
}

