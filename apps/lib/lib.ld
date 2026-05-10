SECTIONS {
    . = 0x03000000; /* Load libraries at a different address than apps (0x02000000) */
    .export_table : { KEEP(*(.export_table)) }
    .text : { *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .bss : { *(.bss) }
}
