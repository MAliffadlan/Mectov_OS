#ifndef GDT_H
#define GDT_H

#include "types.h"

struct gdt_entry_struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

typedef struct gdt_entry_struct gdt_entry_t;

struct gdt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct gdt_ptr_struct gdt_ptr_t;

// TLS segment slots (v38.24): every task slot owns a private FS and GS
// descriptor whose base is its thread control block (TCB), DPL=3 so Ring 3
// can read/write %fs:%gs. All CPUs build the same TLS entries at the same
// selector numbers, so a task keeps its TLS across migration. Slots 0-6 are
// the fixed descriptors; TLS slots start at 8 (selector 0x40).
#define TLS_GDT_BASE_SLOT 8
#define TLS_MAX_TASKS     64     // must stay in sync with MAX_TASKS in task.c
#define TLS_GDT_SLOTS     (2 * TLS_MAX_TASKS)      // FS + GS per task
#define TLS_GDT_ENTRIES   (TLS_GDT_BASE_SLOT + TLS_GDT_SLOTS)

// FS/GS selector for a task's TLS descriptor (0 = no TLS).
static inline uint16_t tls_fs_sel(int tid) {
    return (uint16_t)((TLS_GDT_BASE_SLOT + 2 * tid) * 8);
}
static inline uint16_t tls_gs_sel(int tid) {
    return (uint16_t)((TLS_GDT_BASE_SLOT + 2 * tid + 1) * 8);
}

void init_gdt();
void tss_set_kernel_stack(uint32_t stack);
void gdt_set_df_cr3(uint32_t cr3);   // #DF task-gate TSS page tables (post-paging)
// Point a task's FS/GS descriptors (in EVERY CPU's GDT) at `base` (0 =
// no TLS: the descriptors become not-present, and the task must use
// plain user data segments). Called when a task is created with TLS or
// changes it at runtime.
void gdt_tls_update(int tid, uint32_t base);
void gdt_tls_clear(int tid);
extern uint32_t user_stack[1024];

#endif
