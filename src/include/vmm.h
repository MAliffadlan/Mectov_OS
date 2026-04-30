#ifndef VMM_H
#define VMM_H

#include "types.h"

// Virtual Memory Manager
// Added on top of existing paging — does NOT break identity mapping.
// Tasks can optionally use per-process page directories.
// Default page_dir = 0 means use the global identity map.

// Allocate a new page directory for a process
uint32_t vmm_create_address_space(void);

// Free a page directory (for task cleanup)
void vmm_free_address_space(uint32_t page_dir);

// Map a virtual address to a physical frame in a specific page directory
int vmm_map_page(uint32_t page_dir, uint32_t vaddr, uint32_t paddr, uint32_t flags);

// Unmap a virtual address in a specific page directory
int vmm_unmap_page(uint32_t page_dir, uint32_t vaddr);

// Allocate a physical frame and map it at vaddr in the given page directory
uint32_t vmm_alloc_page_at(uint32_t page_dir, uint32_t vaddr, uint32_t flags);

// Find a free virtual address range in a page directory
uint32_t vmm_find_free_region(uint32_t page_dir, uint32_t size);

// Clone an address space (for fork/spawn — COW)
uint32_t vmm_clone_address_space(uint32_t src_page_dir);

// Switch active page directory (load CR3)
void vmm_switch_page_dir(uint32_t page_dir);

#endif