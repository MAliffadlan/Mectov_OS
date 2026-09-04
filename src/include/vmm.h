#ifndef VMM_H
#define VMM_H

#include "types.h"
#include "idt.h"   // registers_t (TLB shootdown handlers)

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

// Map a virtual address to a physical frame in a specific address space
// (page_dir = PDPT frame under PAE). flags is uint64 so PAGE_NX fits.
int vmm_map_page(uint32_t page_dir, uint32_t vaddr, uint32_t paddr, uint64_t flags);

// Unmap a virtual address in a specific address space
int vmm_unmap_page(uint32_t page_dir, uint32_t vaddr);

// Allocate a physical frame and map it at vaddr in the given address space
uint32_t vmm_alloc_page_at(uint32_t page_dir, uint32_t vaddr, uint64_t flags);

// Clone an address space (for fork/spawn — COW)
uint32_t vmm_clone_address_space(uint32_t src_page_dir);

// Switch active page directory (load CR3)
void vmm_switch_page_dir(uint32_t page_dir);

// ---- TLB shootdown IPI (v38.66) ----
//
// When a page-table mutation on one core changes what OTHER cores may still
// hold in their TLBs (COW-marking during clone, vmm_unmap_page, munmap), the
// mutating core broadcasts a directed fixed IPI on TLB_SD_VECTOR. Each peer's
// handler reloads its CR3 — a full TLB flush — but only if it is currently
// RUNNING the target page directory: under PAE without PCID, every `mov cr3`
// invalidates all non-global entries, so entries for a non-current CR3 cannot
// exist. schedule() also reloads CR3 every timer tick, so the exposure this
// closes is the sub-tick window between the mutation and the peer's next
// preemption.
#define TLB_SD_VECTOR   0x61
#define TLB_TEST_VECTOR 0x62

// Flush `page_dir`'s TLB entries on every other core that currently runs it
// (page_dir == 0 flushes every core — the shared identity space). Callers
// may be in any IF state (this disables interrupts while it waits for peer
// acks) but must NOT hold a lock a peer could be spinning on: a peer that is
// IF=0 inside such a lock cannot take the IPI until it exits. The ack wait
// is bounded, so a pathological peer degrades this to the pre-shootdown
// behavior (flush at the peer's next CR3 reload) instead of deadlocking.
void vmm_tlb_shootdown(uint32_t page_dir);

// CI self-test (boot arg `tlb_self_test`): parks the first AP and verifies
// the shootdown handler's receive + conditional-reload + ack protocol with
// two shootdowns (matching page_dir -> reload happens; non-matching -> the
// handler runs but skips the reload).
void vmm_parse_cmdline(const char* cmd);
void vmm_self_test_tick(void);

// IPI handlers, registered on every core through the shared IDT/handler
// table (vmm.c implements both).
void vmm_tlb_sd_handler(registers_t* r);
void vmm_tlb_test_handler(registers_t* r);

#endif