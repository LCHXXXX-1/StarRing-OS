OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)
SECTIONS
{
    . = 0xffffffff80000000;
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    }
    .bss : { *(.bss .bss.*) }
}
