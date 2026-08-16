#ifndef VMM_H
#define VMM_H

#include "types.h"

// Virtual Memory Manager
// Added on top of existing paging — does NOT break identity mapping.
// Tasks can optionally use per-process page directories.
// Default page_dir = 0 means use the global identity map.

// ---- Ring 3 stack layout (one per address space) ----
// The Ring 3 stack MUST live in the task's own address space, never in kernel
// BSS: the kernel identity map is not user-accessible, and a stack inside
// task_t would put the scheduler's saved register frames one buffer overrun
// away from any user program.
//
// Placed at the top of the demand-paging window handled in idt.c, well clear of
// the app image and heap (0x08000000..0x08F00000, capped by SYS_MALLOC) and the
// shared library base at 0x09000000. The page below the stack is deliberately
// left unmapped as a guard page, so an overflow faults instead of silently
// corrupting whatever sits underneath.
#define USER_STACK_TOP    0x1F000000u
#define USER_STACK_SIZE   0x10000u                       // 64 KB
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_PAGES  (USER_STACK_SIZE / 4096)

// Signal trampoline: a user-readable page mapped into every address space at
// this fixed VA. Signal handlers `ret` into it and it performs SYS_SIGRETURN
// (mov eax,75; int $0x80; ret). It sits above the SYS_MALLOC cap
// (0x08F00000) and below the shared-library base (0x09000000), so nothing
// else ever maps there. Kept in sync with SYS_SIGRETURN in syscall.h.
#define SIG_TRAMPOLINE_VA 0x08F7F000u

// Allocate a new page directory for a process
uint32_t vmm_create_address_space(void);

// Map a fresh, zeroed Ring 3 stack into page_dir, ending at stack_top
// (stack occupies [stack_top - USER_STACK_SIZE, stack_top)).
// Returns the initial ESP (stack_top), or 0 on failure.
uint32_t vmm_setup_user_stack(uint32_t page_dir, uint32_t stack_top);

// Free a page directory (for task cleanup)
void vmm_free_address_space(uint32_t page_dir);

// ---- Physical frame allocator (single source of truth) ----
// One bitmap + refcount pair, owned by vmm.c, sized for PHYS_MAX_PAGES but
// bounded at runtime by phys_init(total_pages) — the RAM size detected from
// the multiboot header in kernel_main/init_mem. The old hardcoded 128MB
// bitmap in vmm.c and the write-only mem_bitmap in mem.c are gone; every
// physical-frame decision reads this one structure.
#define PHYS_MAX_PAGES (512 * 256)  // 512MB / 4KB ceiling for static bitmaps

void phys_init(uint32_t total_pages);                    // called from init_mem
void phys_reserve_region(uint32_t start, uint32_t len);  // mark MMIO (fb, ...) used
uint32_t phys_get_used_pages(void);                      // reserved + allocated
uint32_t phys_get_zero_page(void);                       // shared zero frame (pinned)
uint32_t frame_alloc(void);
void frame_free(uint32_t paddr);
extern uint8_t frame_ref_count[];

// Map a virtual address to a physical frame in a specific page directory
int vmm_map_page(uint32_t page_dir, uint32_t vaddr, uint32_t paddr, uint32_t flags);

// Unmap a virtual address in a specific page directory
int vmm_unmap_page(uint32_t page_dir, uint32_t vaddr);

// Allocate a physical frame and map it at vaddr in the given page directory
uint32_t vmm_alloc_page_at(uint32_t page_dir, uint32_t vaddr, uint32_t flags);

// Clone an address space (for fork/spawn — COW)
uint32_t vmm_clone_address_space(uint32_t src_page_dir);

// Switch active page directory (load CR3)
void vmm_switch_page_dir(uint32_t page_dir);

#endif