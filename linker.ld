ENTRY(_start)

SECTIONS {
    /* Mulai taruh kode di alamat memori 1 Megabyte (Standar Multiboot) */
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K) {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data)
        . = ALIGN(4);
        __initcall_start = .;
        KEEP(*(.initcall.10))
        KEEP(*(.initcall.11))
        KEEP(*(.initcall.12))
        KEEP(*(.initcall.13))
        KEEP(*(.initcall.14))
        KEEP(*(.initcall.15))
        KEEP(*(.initcall.16))
        KEEP(*(.initcall.17))
        KEEP(*(.initcall.20))
        KEEP(*(.initcall.*))
        __initcall_end = .;
    }

    .bss BLOCK(4K) : ALIGN(4K) {
        *(COMMON)
        *(.bss)
    }
}