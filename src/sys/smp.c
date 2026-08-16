#include "../include/smp.h"
#include "../include/acpi.h"
#include "../include/apic.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/mem.h"

#include "../include/serial.h"
#include "../include/mem.h"

extern void init_gdt(void);
extern void idt_load_ap(void);
extern void apic_init(void);
extern uint32_t tasks_get_boot_cr3(void);

static volatile int ap_startup_count = 0;

void ap_main(void) {
    // We are now in 32-bit protected mode on an Application Processor
    
    // 1. Initialize per-core GDT
    init_gdt();

    // 1b. Per-core FPU enable (v38.41): CR4.OSFXSR is per-CPU and NOT
    //     inherited from the BSP, so every AP must set up its own FPU
    //     before the scheduler can fxsave/fxrstor on this core. No task
    //     runs here yet, so rebuilding the clean template is safe.
    extern void fpu_init_cpu(void);
    fpu_init_cpu();
    
    // 2. Load IDT
    idt_load_ap();
    
    // 3. Initialize LAPIC for this core
    apic_init();
    
    // 3b. Program this core's own LAPIC timer (~1kHz, PIT-calibrated). IRQ0
    //     is routed to the BSP only, so without this the AP would never tick
    //     and the per-CPU runqueue would starve (see Fase 3 scheduler).
    extern void lapic_timer_init(void);
    lapic_timer_init();
    
    int cid = apic_get_id() & 15;

    write_serial_string("[SMP] CPU ");
    write_serial_hex(cid);
    write_serial_string(" is awake!\n");
    
    __asm__ __volatile__("lock incl %0" : "+m"(ap_startup_count));
    
    // Enable interrupts
    __asm__ __volatile__("sti");
    
    // Idle loop
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

// LAPIC ICR (Interrupt Command Register) offsets
#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310

static void apic_send_ipi(uint8_t lapic_id, uint32_t dsh, uint32_t vector) {
    volatile uint32_t* icr_high = (volatile uint32_t*)(smp_lapic_addr + LAPIC_ICR_HIGH);
    volatile uint32_t* icr_low  = (volatile uint32_t*)(smp_lapic_addr + LAPIC_ICR_LOW);
    
    *icr_high = (lapic_id << 24);
    *icr_low  = dsh | vector;
    
    // Wait for delivery status bit to clear
    while (*icr_low & (1 << 12)) {
        __asm__ __volatile__("pause");
    }
}

extern uint8_t _binary_obj_src_sys_smp_trampoline_bin_start[];
extern uint8_t _binary_obj_src_sys_smp_trampoline_bin_end[];

void smp_init(void) {
    if (smp_cpu_count <= 1) {
        write_serial_string("[SMP] Only 1 CPU detected. SMP disabled.\n");
        return;
    }
    
    write_serial_string("[SMP] Starting Application Processors...\n");

    // Calibrate the LAPIC timer rate ONCE here, before any AP wakes. The PIT
    // is shared hardware: letting every AP measure it at boot corrupts each
    // reading (timer storms later). APs just program their LAPIC with this
    // shared value in ap_main() -> lapic_timer_init().
    extern void lapic_timer_calibrate(void);
    lapic_timer_calibrate();
    
    uint32_t trampoline_len = _binary_obj_src_sys_smp_trampoline_bin_end - _binary_obj_src_sys_smp_trampoline_bin_start;
    
    // Identity map 0x7000 and 0x8000
    page_map(0x7000, 0x7000, PAGE_PRESENT | PAGE_RW);
    page_map(0x8000, 0x8000, PAGE_PRESENT | PAGE_RW);
    
    // Copy trampoline to 0x8000
    memcpy((void*)0x8000, _binary_obj_src_sys_smp_trampoline_bin_start, trampoline_len);
    
    volatile uint32_t* boot_ap_main = (volatile uint32_t*)0x7FF4;
    volatile uint32_t* boot_cr3 = (volatile uint32_t*)0x7FF8;
    volatile uint32_t* boot_esp = (volatile uint32_t*)0x7FFC;
    
    *boot_ap_main = (uint32_t)ap_main;
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    *boot_cr3 = cr3;
    
    smp_bsp_lapic_id = apic_get_id();
    
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        uint8_t lapic_id = smp_lapic_ids[i];
        if (lapic_id == smp_bsp_lapic_id) continue;
        
        void* ap_stack_alloc = kmalloc(16384);
        if (!ap_stack_alloc) {
            write_serial_string("[SMP] out of memory for AP stack, aborting AP startup\n");
            break;
        }
        uint32_t ap_stack = (uint32_t)ap_stack_alloc + 16384;
        *boot_esp = ap_stack;
        
        write_serial_string("[SMP] Waking up CPU APIC ID ");
        write_serial_hex(lapic_id);
        write_serial_string("\n");
        
        // INIT IPI (Assert)
        apic_send_ipi(lapic_id, 0x00004500, 0);
        
        // Wait 10ms (roughly)
        for(volatile int d=0; d<1000000; d++); 
        
        // INIT IPI (De-assert)
        apic_send_ipi(lapic_id, 0x00008500, 0);
        
        // Wait 10ms
        for(volatile int d=0; d<1000000; d++); 
        
        // SIPI IPI 1
        // Vector is page number of 0x8000 (0x08)
        apic_send_ipi(lapic_id, 0x00004600, 0x08);
        
        // Wait 200us
        for(volatile int d=0; d<20000; d++); 
        
        // SIPI IPI 2
        apic_send_ipi(lapic_id, 0x00004600, 0x08);
        
        // Wait for AP to finish booting
        for(volatile int d=0; d<1000000; d++); 
    }
    
    write_serial_string("[SMP] AP startup complete. Active APs: ");
    write_serial_hex(ap_startup_count);
    write_serial_string("\n");
}
