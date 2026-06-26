ENTRY(_start)

SECTIONS
{
    . = 0x40100000;

    .text ALIGN(4K) :
    {
        *(.text.start)
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
