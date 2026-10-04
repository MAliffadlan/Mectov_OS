; --- Mectov OS 64-bit bootstrap (Multiboot2 + long-mode entry) ---
; GRUB enters in 32-bit protected mode (no paging). This stub:
;   1. saves Multiboot2 magic/pointer,
;   2. verifies long-mode support (CPUID 0x80000001:EDX bit 29),
;   3. builds a minimal PML4 (identity 0-8MB via 2MB large pages),
;   4. enables PAE + EFER.LME + CR0.PG, loads a 64-bit GDT,
;   5. far-jumps to 64-bit code and calls kernel64_main(magic, mb_info).
; Error path writes directly to COM1 (0x3F8) — no kernel needed.

MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0            ; i386 (bootloader still enters in 32-bit)

section .multiboot
align 8
mb2_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd mb2_end - mb2_start
    dd -(MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_start))

    ; Framebuffer tag (type 5): prefer 1024x768x32, depth 0 = text fallback ok
    align 8
    dw 5, 0
    dd 20
    dd 1024
    dd 768
    dd 32

    ; End tag
    align 8
    dw 0, 0
    dd 8
mb2_end:

section .bss
align 4096
global pml4_boot
pml4_boot:
    resb 4096
global pdpt_low
pdpt_low:
    resb 4096
global pd_low
pd_low:
    resb 4096

align 16
stack_bottom64:
    resb 32768
global stack_top64
stack_top64:

; Saved bootloader handoff (32-bit phys values, zero-extended later)
mb_magic_save:
    resd 1
mb_info_save:
    resd 1

section .rodata
align 8
gdt64:
    dq 0x0000000000000000          ; 0x00 null
    dq 0x00209A0000000000          ; 0x08 code: P=1 DPL=0 S=1 Ex=1 RW=1, L=1
    dq 0x0000920000000000          ; 0x10 data: P=1 DPL=0 S=1 RW=1
gdt64_end:
gdt64_ptr32:
    dw gdt64_end - gdt64 - 1
    dd gdt64

section .text
BITS 32
global _start
extern kernel64_main

_start:
    mov esp, stack_top64
    mov [mb_magic_save], eax
    mov [mb_info_save], ebx

    ; --- long-mode support check ---
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_longmode
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no_longmode

    ; --- build minimal paging: PML4[0]->PDPT, PDPT[0]->PD, PD[0..3]=2MB id ---
    ; pml4_boot[0] = pdpt_low | P|RW
    mov eax, pdpt_low
    or eax, 0x3
    mov [pml4_boot], eax
    ; pdpt_low[0] = pd_low | P|RW
    mov eax, pd_low
    or eax, 0x3
    mov [pdpt_low], eax
    ; pd_low[i] = i*2MB | P|RW|PS (0x83), i = 0..3  (covers 0-8MB)
    mov ecx, 0
.fill_pd:
    mov eax, ecx
    shl eax, 21                    ; i * 2MB
    or eax, 0x83
    mov [pd_low + ecx*8], eax
    mov dword [pd_low + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 4
    jb .fill_pd

    ; --- enable long mode ---
    mov eax, pml4_boot
    mov cr3, eax
    mov eax, cr4
    or eax, (1 << 5)               ; CR4.PAE
    ; CR4.OSFXSR (bit 9) + OSXMMEXCPT (bit 10): GCC -m64 emits SSE even for
    ; plain integer loops (e.g. pxor+movaps to zero the GDT), which faults
    ; with #UD/#GP while these bits are clear. Every 64-bit CPU has them;
    ; APs repeat this in ap_main (M6). fxsave management itself is M4.
    or eax, (1 << 9) | (1 << 10)
    mov cr4, eax
    mov ecx, 0xC0000080            ; EFER
    rdmsr
    or eax, (1 << 8)               ; EFER.LME
    ; EFER.NXE (bit 11): CPUID-gated like the 32-bit kernel (src/sys/mem.c).
    ; qemu64 always has it; without NXE, bit 63 in any entry is reserved and
    ; NX mappings fault. M3 maps the framebuffer NX as its first NX user.
    ; NOTE: cpuid clobbers EAX/ECX/EDX(EBX) — ECX holds the EFER address for
    ; the wrmsr below, so it must be saved/restored too.
    push eax
    push ecx
    push edx
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 20)
    pop edx
    pop ecx
    pop eax
    jz .no_nxe
    or eax, (1 << 11)              ; EFER.NXE
.no_nxe:
    wrmsr
    mov eax, cr0
    or eax, (1 << 31)              ; CR0.PG
    mov cr0, eax

    lgdt [gdt64_ptr32]
    jmp 0x08:long_entry

.no_longmode:
    ; Minimal serial writer: "NO64" + halt. COM1 0x3F8, LSR 0x3FD bit 5.
    mov dx, 0x3FD
.wait1: in al, dx
    test al, 0x20
    jz .wait1
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al
    mov dx, 0x3FD
.wait2: in al, dx
    test al, 0x20
    jz .wait2
    mov dx, 0x3F8
    mov al, 'O'
    out dx, al
    mov dx, 0x3FD
.wait3: in al, dx
    test al, 0x20
    jz .wait3
    mov dx, 0x3F8
    mov al, '6'
    out dx, al
    mov dx, 0x3FD
.wait4: in al, dx
    test al, 0x20
    jz .wait4
    mov dx, 0x3F8
    mov al, '4'
    out dx, al
    cli
.hang32:
    hlt
    jmp .hang32

BITS 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, stack_top64
    ; Multiboot2 values are 32-bit phys: 32-bit mov zero-extends to 64-bit.
    mov edi, [mb_magic_save]
    mov esi, [mb_info_save]
    call kernel64_main
    cli
.hang64:
    hlt
    jmp .hang64
