; ============================================================
; interrupt_entry.asm — Mectov OS Interrupt Stubs
; ============================================================
; This file handles the low-level assembly for:
;   1. IRQ handlers (timer, keyboard, mouse) via irq_common_stub
;   2. CPU exception handlers (ISR 0, 13, 14) via isr_common_stub
;   3. Syscall entry (int 0x80) via isr128
;   4. GDT/IDT flush routines
;
; KEY DESIGN: irq_common_stub is "ring-aware" by design of x86 iret.
; When iret sees CS on the stack has RPL=3, it automatically pops
; SS and ESP too. When RPL=0, it only pops EIP/CS/EFLAGS.
; So the SAME stub works for both Ring 0 and Ring 3 interrupts.
; ============================================================

[extern isr_handler]
[extern irq_handler]

; ============================================================
; IRQ Common Stub — used by timer, keyboard, mouse
; ============================================================
; Stack layout when we enter (after push err_code + push int_no):
;   For Ring 3 interrupt: [SS, ESP, EFLAGS, CS, EIP, err_code, int_no] (CPU pushed SS/ESP)
;   For Ring 0 interrupt: [EFLAGS, CS, EIP, err_code, int_no] (no SS/ESP from CPU)
;
; After pushad + push ds, the stack matches registers_t struct.
; irq_handler returns the (possibly new) ESP via EAX.
; mov esp, eax switches to the new task's saved frame.
; iret then correctly returns to either Ring 0 or Ring 3.
; ============================================================
irq_common_stub:
    pushad
    mov ax, ds
    push eax              ; Save caller's DS
    mov ax, 0x10          ; Switch to kernel data segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp              ; Pass pointer to registers_t
    call irq_handler
    add esp, 4            ; Clean up argument

    mov esp, eax          ; <<< CONTEXT SWITCH: load new task's ESP

    pop eax               ; Restore DS (could be 0x10 or 0x23)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8            ; Skip int_no + err_code
    iret                  ; Return to Ring 0 or Ring 3 (auto-detected by CPU)

; ============================================================
; ISR Common Stub — used by CPU exceptions (no scheduler)
; ============================================================
isr_common_stub:
    pushad
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call isr_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8
    iret

; ============================================================
; GDT / IDT flush
; ============================================================
global gdt_flush
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
.flush:
    ret

global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; ============================================================
; Default ISR (for unregistered interrupts)
; ============================================================
global isr_default
isr_default:
    push byte 0           ; err_code = 0
    push dword 255        ; int_no = 255
    jmp isr_common_stub   ; Use ISR stub (no EOI, no scheduler)

; ============================================================
; ISR 0: Division by Zero
; ============================================================
global isr0
isr0:
    push byte 0
    push byte 0
    jmp isr_common_stub

; ============================================================
; ISR 4: Overflow (INTO instruction, no error code)
; DPL=3 in IDT so Ring 3 apps can trigger this safely.
; ============================================================
global isr4
isr4:
    push byte 0
    push byte 4
    jmp isr_common_stub

; ============================================================
; ISR 13: General Protection Fault (has error code from CPU)
; ============================================================
global isr13
isr13:
    ; CPU already pushed error code
    push byte 13
    jmp isr_common_stub

; ============================================================
; ISR 14: Page Fault (has error code from CPU)
; ============================================================
global isr14
isr14:
    ; CPU already pushed error code
    push byte 14
    jmp isr_common_stub

; ============================================================
; IRQ handlers (hardware interrupts from PIC)
; ============================================================
%macro IRQ 2
  global irq%1
  irq%1:
    push byte 0           ; dummy err_code
    push byte %2          ; int_no
    jmp irq_common_stub
%endmacro

IRQ 0, 32    ; Timer
IRQ 1, 33    ; Keyboard
IRQ 12, 44   ; Mouse

; ============================================================
; Syscall entry: int 0x80
; ============================================================
; Separate path — no scheduler, no EOI. Just dispatch and return.
; Uses isr_handler which calls the registered handler for int 0x80.
; DPL=3 gate allows Ring 3 code to call this.
; ============================================================
global isr128
isr128:
    push byte 0           ; dummy err_code
    push dword 128        ; int_no = 0x80 (use dword to avoid sign extension!)
    pushad
    mov ax, ds
    push eax
    mov ax, 0x10          ; Switch to kernel data segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp              ; Pass registers_t* to handler
    call isr_handler
    add esp, 4

    pop eax               ; Restore user's DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8            ; Skip int_no + err_code
    iret                  ; Return to Ring 3 (CPU pops SS/ESP automatically)
