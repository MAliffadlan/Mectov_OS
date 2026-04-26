[extern isr_handler]
[extern irq_handler]

; common_stub: Saves CPU state, calls C handler, restores state
isr_common_stub:
    pushad                    ; Push eax, ecx, edx, ebx, esp, ebp, esi, edi
    mov ax, ds                ; Lower 16-bits of eax = ds
    push eax                  ; Save ds

    mov ax, 0x10              ; Load kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call isr_handler

    pop eax                   ; Restore ds
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad                     ; POPAD
    add esp, 8                ; Clean up error code and int number
    iret                      ; Return from interrupt

irq_common_stub:
    pushad
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call irq_handler

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8
    iret

global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; ISR/IRQ Macro
%macro IRQ 2
  global irq%1
  irq%1:
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro

IRQ 0, 32    ; Timer
IRQ 1, 33    ; Keyboard
