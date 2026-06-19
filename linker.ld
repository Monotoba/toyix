/* linker.ld
 *
 * The linker script controls the physical layout of the kernel image.
 *
 * We place the kernel at 1 MiB. This is traditional for simple x86 kernels:
 * it avoids the low memory area used by BIOS data structures and bootloader
 * scratch space.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(4) :
    {
        KEEP(*(.multiboot))
    }

    .text ALIGN(4K) :
    {
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }
}
