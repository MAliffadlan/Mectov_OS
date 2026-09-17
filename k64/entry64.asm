; --- Mectov OS M2 64-bit interrupt stubs + GDT flush ---
; 48 entry stubs (vectors 0-47). CPUs push error codes only for
; 8,10,11,12,13,14,17,21,30 — the rest get a dummy 0 so every frame has
; identical layout: [vec][err][rip][cs][rflags][rsp][ss] + saved GPRs.
; isr64_common saves all GPRs, calls isr64_handler(regs64_t*) with rdi=rsp,
; restores, drops vec+err, and returns with iretq (restores saved RFLAGS,
; so no sti needed here).

BITS 64
default rel

section .text

extern isr64_handler

%macro ISR_NOERR 1
global isr64_%1
isr64_%1:
    cli
    push 0
    push %1
    jmp isr64_common
%endmacro

%macro ISR_ERR 1
global isr64_%1
isr64_%1:
    cli
    push %1
    jmp isr64_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; Frame layout must match regs64_t in k64/cpu64.h:
;   r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax,vec,err,...
; The handler returns the rsp to resume in RAX (same frame to continue, or
; another task's saved rsp after a context switch — M4 scheduler).
global isr64_common
isr64_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    call isr64_handler
    mov rsp, rax               ; resume rsp (may be another task)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                ; drop vec + err/code
    ; Segment fixup for the iretq target: Ring 3 needs user data segments,
    ; kernel tasks need kernel ones (they are stale after running Ring 3).
    ; [rsp]=rip, [rsp+8]=cs. NOTE: RAX holds the syscall result here — the
    ; selector shuffle must preserve it (a bare `mov ax, ...` cost us every
    ; return value once: all reads came back 0x23).
    cmp qword [rsp+8], 0x1B
    push rax
    jne .ksegs
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    jmp .popax
.ksegs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
.popax:
    pop rax
.doi:
    iretq

; M4 syscall entry: int $0x80 from Ring 3 (gate installed DPL3 in idt64.c).
ISR_NOERR 128

; M6 IPI + spurious entries (gates installed in idt64_init, referenced
; directly — not part of isr64_table[48]).
ISR_NOERR 96
ISR_NOERR 97
ISR_NOERR 255

; M7 AP LAPIC timer (per-CPU preemption; handler only reschedules).
ISR_NOERR 80

; M6 AP C entry: the trampoline jmps here with RDI = AP index (BSP never
; runs this). Calls ap_main(idx); parks if it ever returns.
global ap_entry_c
extern ap_main
ap_entry_c:
    call ap_main
    cli
.halt:
    hlt
    jmp .halt

; Stub address table for C: void *isr64_table[48].
section .rodata
align 8
global isr64_table
isr64_table:
    dq isr64_0,  isr64_1,  isr64_2,  isr64_3
    dq isr64_4,  isr64_5,  isr64_6,  isr64_7
    dq isr64_8,  isr64_9,  isr64_10, isr64_11
    dq isr64_12, isr64_13, isr64_14, isr64_15
    dq isr64_16, isr64_17, isr64_18, isr64_19
    dq isr64_20, isr64_21, isr64_22, isr64_23
    dq isr64_24, isr64_25, isr64_26, isr64_27
    dq isr64_28, isr64_29, isr64_30, isr64_31
    dq isr64_32, isr64_33, isr64_34, isr64_35
    dq isr64_36, isr64_37, isr64_38, isr64_39
    dq isr64_40, isr64_41, isr64_42, isr64_43
    dq isr64_44, isr64_45, isr64_46, isr64_47

; void gdt64_flush(const void *gdt_ptr10, uint16_t tss_sel)
; rdi = 10-byte GDTR image {limit:16, base:64}, rsi = TSS selector.
; Reloads GDTR + DS/ES/SS, LTR, then far-returns to reload CS=0x08.
section .text
global gdt64_flush
gdt64_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov ax, si
    ltr ax
    pop rax                    ; caller return address
    push 0x08
    push rax
    db 0x48, 0xCB              ; retfq (byte-encoded, NASM-proof)
