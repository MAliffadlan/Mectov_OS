[extern isr_handler]
[extern irq_handler]

irq_common_stub:
    pushad
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp          ; KIRIM POINTER KE STACK (ESP menunjuk ke struct registers)
    call irq_handler
    add esp, 4        ; Bersihkan pointer

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

global isr_default
isr_default:
    push byte 0
    push byte 255
    jmp irq_common_stub

%macro IRQ 2
  global irq%1
  irq%1:
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro

IRQ 0, 32
IRQ 1, 33
