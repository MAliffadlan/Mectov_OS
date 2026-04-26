; --- Mectov OS Bootloader (VBE 800x600 Stable) ---
MAGIC    equ  0x1BADB002
FLAGS    equ  0x00000007 ; ALIGN + MEMINFO + VIDEO_MODE
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0 ; Address fields (unused for ELF)
    dd 0             ; Mode (0 = linear)
    dd 800           ; Width
    dd 600           ; Height
    dd 32            ; Depth

section .bss
align 16
stack_bottom:
    resb 32768
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang
