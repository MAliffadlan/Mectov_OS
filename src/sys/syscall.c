#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/gdt.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/timer.h"
#include "../include/mem.h"

// Syscall handler — dispatches based on EAX register
static void syscall_handler(registers_t* regs) {
    switch (regs->eax) {
        case SYS_PRINT: {
            // arg1 = string pointer, arg2 = color
            const char* msg = (const char*)regs->ebx;
            uint8_t color = (uint8_t)regs->ecx;
            print(msg, color);
            regs->eax = 0;
            break;
        }
        case SYS_GET_TICKS: {
            regs->eax = get_ticks();
            break;
        }
        case SYS_YIELD: {
            // Just return — the scheduler runs on timer IRQ
            regs->eax = 0;
            break;
        }
        case SYS_EXIT: {
            // For now, just halt the user process
            print("[syscall] Process exited.\n", 0x0E);
            for(;;) __asm__("hlt");
            break;
        }
        default:
            regs->eax = -1; // Unknown syscall
            break;
    }
}

void init_syscalls(void) {
    register_interrupt_handler(0x80, syscall_handler);
}

// Switch from Ring 0 to Ring 3
void switch_to_user_mode(void) {
    // Set kernel stack in TSS for when we return from user mode
    extern uint32_t stack_top; // from boot.asm
    tss_set_kernel_stack((uint32_t)&stack_top);

    // User data segment = 0x20 | 3 (RPL=3) = 0x23
    // User code segment = 0x18 | 3 (RPL=3) = 0x1B
    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"    // User data selector (Ring 3)
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%esp, %%eax\n"   // Save current stack
        "pushl $0x23\n"        // SS  = user data segment
        "pushl %%eax\n"        // ESP = current stack pointer
        "pushf\n"              // EFLAGS
        "pop %%eax\n"          // Modify EFLAGS to re-enable interrupts
        "or $0x200, %%eax\n"   // Set IF (interrupt flag)
        "push %%eax\n"
        "pushl $0x1B\n"        // CS  = user code segment
        "push $1f\n"           // EIP = label "1" (continue here in user mode)
        "iret\n"               // Switch to Ring 3!
        "1:\n"                 // Now executing in Ring 3
        ::: "eax", "memory"
    );
}
