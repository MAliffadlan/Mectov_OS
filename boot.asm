MBALIGN  equ  1 << 0
MEMINFO  equ  1 << 1
FLAGS    equ  MBALIGN | MEMINFO
MAGIC    equ  0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
stack_bottom:
resb 16384 ; 16 KB stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    ; Atur posisi stack pointer
    mov esp, stack_top
    
    ; Panggil fungsi utama C kita
    call kernel_main
    
    ; Kalau OS selesai/keluar, matikan CPU agar tidak crash
    cli
.hang:
    hlt
    jmp .hang