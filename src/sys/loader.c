#include "../include/loader.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/task.h"
#include "../include/vmm.h"
#include "../include/utils.h"
#include "../include/serial.h"

int load_mct_app(const char* filename) {
    write_serial_string("[LOADER] start\n");
    
    // Use new VFS API
    int node = vfs_get_node(filename);
    if (node < 0 || vfs_is_dir(node)) {
        write_serial_string("[LOADER] NOT FOUND\n");
        return -1;
    }
    
    // Read the whole file into a temp buffer
    uint8_t buf[65536]; // 64KB max for MCT
    int sz = vfs_read_file(filename, (char*)buf, sizeof(buf));
    if (sz < (int)sizeof(mct_header_t)) {
        write_serial_string("[LOADER] too small\n");
        return -2;
    }
    
    mct_header_t* header = (mct_header_t*)buf;
    if (header->magic != MCT_MAGIC) return -3;
    
    uint32_t total_size = header->code_size + header->data_size;
    if (total_size == 0 || total_size > 1024 * 1024) return -4;
    // The file only contains the header and the code. data_size (BSS) is uninitialized memory.
    if ((int)(sizeof(mct_header_t) + header->code_size) > sz) return -5;
    
    // Create a new address space
    uint32_t page_dir = vmm_create_address_space();
    if (page_dir == 0) return -6;
    
    // Explicitly map the memory in the new page directory
    // MUST do this BEFORE switching to the new page_dir, because vmm_alloc_page_at
    // uses the kernel's identity map to access the page tables!
    uint32_t num_pages = (total_size + 4095) / 4096;
    if (num_pages == 0) num_pages = 1;
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t addr = 0x02000000 + (i * 4096);
        vmm_alloc_page_at(page_dir, addr, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    
    // Temporarily switch to the new address space to copy data
    uint32_t old_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
    vmm_switch_page_dir(page_dir);
    
    void* app_mem = (void*)0x02000000;
    
    // Copy code + zero BSS
    memcpy(app_mem, buf + sizeof(mct_header_t), header->code_size);
    memset((uint8_t*)app_mem + header->code_size, 0, header->data_size);
    
    // Switch back to kernel page directory
    vmm_switch_page_dir(old_cr3);
    
    void (*entry_point)() = (void (*)())((uint32_t)app_mem + header->entry);
    
    // Create the task, assigning the new page directory
    // We use thread_create because it takes a page_dir argument
    int task_id = thread_create(entry_point, PRIORITY_INTERACTIVE, page_dir);
    if (task_id < 0) {
        vmm_free_address_space(page_dir);
        write_serial_string("FAIL\n");
        return -7;
    }
    
    write_serial_string("OK\n");
    return task_id;
}
