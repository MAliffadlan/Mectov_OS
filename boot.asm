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
    dd 1024          ; Width
    dd 768           ; Height
    dd 32            ; Depth

section .bss
align 16
stack_bottom:
    resb 32768
global stack_top
stack_top:

section .text
global _start
extern kernel_main
extern __stack_chk_guard

_start:
    mov esp, stack_top
    push eax                     ; save multiboot magic
    push ebx                     ; save multiboot info
    rdtsc                        ; seed the stack canary before any C frame
    xor eax, 0x4D454354          ; 'MECT'
    xor edx, 0x5A5A5A5A
    add eax, edx
    jnz .seed_ok
    mov eax, 0x4D454354
.seed_ok:
    mov [__stack_chk_guard], eax
    pop ebx
    pop eax
    push ebx
    push eax
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang
