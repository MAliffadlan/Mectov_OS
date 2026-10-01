/* Mectov OS 64-bit link script (M1: low 1M, no higher-half yet).
 * Mirrors linker.ld layout but emits ELF64. The .multiboot section must stay
 * within the first 32KB of the image for GRUB Multiboot2 detection. */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
ENTRY(_start)

SECTIONS {
    /* Keep the 1MB Multiboot load address like the 32-bit kernel for M1. */
    . = 1M;

    /* Image boundaries. mem64 reserves [0, _kernel_end) before handing out any
     * frame; the kernel is low-linked, so these are physical addresses too. */
    _kernel_start = .;

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
    }

    /* End of the kernel image. Everything below this (rounded up to a 2MB
     * boundary by mem64) must stay reserved: the image carries the embedded
     * demo images, the 8x16 font and the GUI/console buffers, so it is several
     * MB — a hardcoded 2MB reservation leaves kernel .data/.bss (and the
     * console's shadow grid) in the free-frame pool. */
    _kernel_end = .;
}
