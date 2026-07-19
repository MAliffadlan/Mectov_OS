[BITS 16]
[ORG 0x8000]

smp_trampoline:
    cli
    cld
    
    ; Setup segment registers
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Load temporary GDT
    lgdt [gdt_ptr]

    ; Enable Protected Mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to 32-bit mode
    jmp 0x08:(protected_mode)

[BITS 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Enable Paging using CR3 passed at 0x7FF8
    mov eax, [0x7FF8]
    mov cr3, eax
    
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; Set up stack using ESP passed at 0x7FFC
    mov esp, [0x7FFC]

    ; Call ap_main
    ; We need an absolute address since it's an external symbol.
    ; But we don't know ap_main address. We can pass ap_main address at 0x7FF4!
    mov eax, [0x7FF4]
    call eax

    ; Halt if ap_main returns
.halt:
    cli
    hlt
    jmp .halt

align 4
gdt_start:
    dq 0 ; null
    dq 0x00cf9a000000ffff ; code (0x08)
    dq 0x00cf92000000ffff ; data (0x10)
gdt_end:

align 4
gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start
