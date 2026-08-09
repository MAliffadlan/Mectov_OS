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
; Runs on a dedicated per-CPU exception stack so a corrupt or overflowed
; kernel stack — the very thing a #PF/#DF reports — cannot prevent the
; handler from running. Without this, a deep stack overflow that hits a
; guard page faults while the #PF handler itself is running on the same bad
; stack and the CPU triple-faults (a silent reset) before anything can
; print. The original ESP is saved on the fault stack and restored before
; iret, so handled faults (e.g. COW page promotion) resume exactly as
; before. The stack is per-CPU (indexed by LAPIC ID): ordinary faults like
; COW page promotion fire constantly on every core, so a single shared
; buffer would have two CPUs clobbering each other's saved ESP under load.
section .bss
align 16
global fault_stacks
global fault_stack_tops
fault_stacks:   resb 4096 * 16      ; 4KB exception stack per CPU (max 16)
fault_stack_tops:
                resd 16             ; top pointer of each CPU's stack (filled by C)
section .text

isr_common_stub:
    pushad
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Switch to THIS CPU's fault stack, passing the ORIGINAL ESP (the
    ; register frame) to the handler. LAPIC ID register is identity-mapped
    ; (apic.c) and its ID sits in bits 31:24; cid == apic_id & 15 matches
    ; task.c's get_cid().
    ;
    ; NESTED FAULT: if the handler itself faults (e.g. a COW promotion inside
    ; the #PF handler touching another COW page), this stub re-enters while
    ; ESP is already somewhere on THIS CPU's fault stack. Unconditionally
    ; switching to the top again would push the nested frame over the outer
    ; handler's saved-ESP slot and corrupt the stack (eventually a #DF). So
    ; switch only when ESP is outside [top-4096, top): a nested fault keeps
    ; its current ESP and pushes deeper, exactly like a normal stack.
    mov eax, [0xFEE00020]           ; LAPIC ID register
    shr eax, 24
    and eax, 15
    mov edx, [fault_stack_tops + eax*4] ; top of this CPU's fault stack
    mov ecx, esp                    ; original ESP (register frame)
    lea ebx, [edx - 4096]           ; bottom of this CPU's fault stack
    cmp ecx, ebx
    jb  isr_use_fault_stack         ; ESP below the stack => normal fault
    cmp ecx, edx
    jae isr_use_fault_stack         ; ESP above the top => normal fault
    jmp isr_keep_stack              ; ESP inside => nested fault, keep it
isr_use_fault_stack:
    mov esp, edx
isr_keep_stack:
    push ecx

    push ecx
    call isr_handler
    add esp, 4

    pop ecx
    mov esp, ecx

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
; ISR 1: Debug / single-step trap (no error code)
; Routed to the GDB stub when active.
; ============================================================
global isr1
isr1:
    push byte 0
    push byte 1
    jmp isr_common_stub

; ============================================================
; ISR 3: Breakpoint (int3, no error code)
; Routed to the GDB stub when active. DPL=3 gate lets Ring 3
; apps trigger it too.
; ============================================================
global isr3
isr3:
    push byte 0
    push byte 3
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
; Remaining CPU exceptions (correct error-code handling)
; ============================================================
; The CPU pushes an error code for #DF(8), #TS(10), #NP(11), #SS(12),
; #GP(13), #PF(14), #AC(17), #CP(21). Their stubs must push ONLY the
; int_no — pushing a dummy error code like isr_default does would leave
; BOTH the real error code and the dummy on the stack, shifting
; registers_t by 4 bytes. The handler would read int_no/eip/cs from the
; wrong offsets and the trailing `add esp, 8; iret` would pop the real
; error code as EIP — jumping to a garbage address (triple fault /
; silent reboot) instead of a logged panic.
%macro ISR_WITH_ERR 1      ; CPU already pushed an error code
  global isr%1
  isr%1:
    push byte %1           ; int_no
    jmp isr_common_stub
%endmacro

%macro ISR_NO_ERR 1        ; no error code from CPU
  global isr%1
  isr%1:
    push byte 0            ; dummy err_code
    push byte %1           ; int_no
    jmp isr_common_stub
%endmacro

ISR_NO_ERR 2               ; NMI (non-maskable interrupt)
ISR_NO_ERR 5               ; #BR Bound Range Exceeded
ISR_NO_ERR 6               ; #UD Invalid Opcode
ISR_NO_ERR 7               ; #NM Device Not Available (x87 not present)
ISR_WITH_ERR 8             ; #DF Double Fault
ISR_NO_ERR 9               ; Coprocessor Segment Overrun (legacy)
ISR_WITH_ERR 10            ; #TS Invalid TSS
ISR_WITH_ERR 11            ; #NP Segment Not Present
ISR_WITH_ERR 12            ; #SS Stack-Segment Fault
ISR_NO_ERR 16              ; #MF x87 Floating-Point Error
ISR_WITH_ERR 17            ; #AC Alignment Check
ISR_NO_ERR 18              ; #MC Machine Check
ISR_NO_ERR 19              ; #XF SIMD Floating-Point Exception
ISR_NO_ERR 20              ; #VE Virtualization Exception
ISR_WITH_ERR 21            ; #CP Control Protection

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
IRQ 5, 37    ; Sound (SB16)
IRQ 11, 43   ; Network (RTL8139)
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
    ; TRAP gate (IDT flags 0xEF): the CPU leaves IF=1 on entry, so interrupts
    ; are live the moment we arrive. Push the register frame with IF=0 so the
    ; registers_t layout is deterministic (fork/exec read the frame at the top
    ; of the kernel stack), then re-enable interrupts for the C handler so the
    ; scheduler can preempt long syscalls.
    cli
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

    sti                   ; preemptible from here on
    push esp              ; Pass registers_t* to handler
    call isr_handler
    add esp, 4

    cli                   ; deterministic epilogue (no nested IRQ frames)
    pop eax               ; Restore user's DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popad
    add esp, 8            ; Skip int_no + err_code
    iret                  ; Return to Ring 3 (CPU pops SS/ESP automatically)
