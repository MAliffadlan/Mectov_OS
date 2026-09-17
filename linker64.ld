/* Mectov OS 64-bit link script (M1: low 1M, no higher-half yet).
 * Mirrors linker.ld layout but emits ELF64. The .multiboot section must stay
 * within the first 32KB of the image for GRUB Multiboot2 detection. */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
ENTRY(_start)

SECTIONS {
    /* Keep the 1MB Multiboot load address like the 32-bit kernel for M1. */
    . = 1M;

    .multiboot BLOCK(4K) : ALIGN(8) {
        *(.multiboot)
    }

    .text BLOCK(4K) : ALIGN(4K) {
        *(.text*)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata*)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data*)
    }

    .bss BLOCK(4K) : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
        /* End of kernel image (M3 reserves [0,2MB) wholesale, so this is
         * diagnostic for now; M4/M7 can tighten the reservation to it. */
        PROVIDE(_kernel_end = .);
    }
}
