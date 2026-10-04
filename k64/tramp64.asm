; --- Mectov OS M6 AP trampoline (16 -> 32 -> 64 bit), loaded at 0x8000 ---
; Entered via SIPI (vector 0x08) in 16-bit real mode. Uses the mailbox at
; 0x7000 (zeroed + filled by the BSP before SIPI; same area the 32-bit
; kernel used at 0x7FF4/8/C):
;   0x7000  MB_STACK qword  AP stack top (ap_stacks[i])
;   0x7008  MB_CR3   qword  boot PML4 phys (low dword set by 32-bit code)
;   0x7010  MB_IDX   dword  AP index (0 = BSP, never used here)
;   0x7018  MB_ENTRY qword  64-bit entry (ap_entry_c in the kernel image)
; Readiness is reported through C arrays (ap_ready[], written by ap_main),
; not the mailbox. Temp GDTs live inside this blob (<0x8000+2KB,
; real-mode addressable).
; APs enable PAE + SSE + LME here; NXE is intentionally skipped (BSP-gated
; feature — parked APs touch only identity pages, M7 unifies this).

[BITS 16]
[ORG 0x8000]

MB_STACK equ 0x7000
MB_CR3   equ 0x7008
MB_IDX   equ 0x7010
MB_ENTRY equ 0x7018

tramp_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [gdt32_ptr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:prot32

[BITS 32]
prot32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, [MB_CR3]
    mov cr3, eax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)  ; PAE + OSFXSR + OSXMMEXCPT
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)                          ; LME (no NXE on AP, see above)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)                         ; PG
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp 0x08:long64

[BITS 64]
long64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rsp, [MB_STACK]
    mov edi, [MB_IDX]          ; zero-extends: ap index -> RDI (1st arg)
    mov rax, [MB_ENTRY]
    jmp rax                    ; -> ap_entry_c (never returns)

; --- temp GDTs (32-bit accessible, in-blob) ---
align 8
gdt32:
    dq 0
    dq 0x00cf9a000000ffff      ; 0x08 code32
    dq 0x00cf92000000ffff      ; 0x10 data32
gdt32_end:
gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32

align 8
gdt64t:
    dq 0
    dq 0x00209a0000000000      ; 0x08 code64 (L=1)
    dq 0x0000920000000000      ; 0x10 data64
gdt64t_end:
gdt64_ptr:
    dw gdt64t_end - gdt64t - 1
    dd gdt64t

tramp_end:
