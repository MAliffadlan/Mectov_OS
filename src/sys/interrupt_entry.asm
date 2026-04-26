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
    mov esp, eax      ; --- MAGIC: Switch stack to the new task's ESP ---

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8
    iret

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
IRQ 12, 44

global isr0
isr0:
    push byte 0
    push byte 0
    jmp irq_common_stub

; --- Syscall entry (int 0x80) ---
global isr128
isr128:
    push byte 0       ; dummy err_code
    push byte 0x80    ; int_no = 128
    pushad
    mov ax, ds
    push eax
    mov ax, 0x10      ; Switch to kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp           ; Pass pointer to registers_t
    extern isr_handler
    call isr_handler
    add esp, 4

    pop eax
    mov ds, ax         ; Restore user data segments
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8         ; Clean int_no, err_code
    iret

