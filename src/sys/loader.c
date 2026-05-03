#include "../include/loader.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/task.h"
#include "../include/vmm.h"
#include "../include/utils.h"
#include "../include/serial.h"

int load_mct_app_with_arg(const char* filename, const char* arg) {
    write_serial_string("[LOADER] start\n");
    
    // Use new VFS API
    int node = vfs_get_node(filename);
    if (node < 0 || vfs_is_dir(node)) {
        write_serial_string("[LOADER] NOT FOUND\n");
        return -1;
    }
    
    // Read the whole file into a temp buffer
    static uint8_t buf[65536]; // 64KB max for MCT, static to avoid 4KB kernel stack overflow!
    int sz = vfs_read_file(filename, (char*)buf, sizeof(buf));
    if (sz < (int)sizeof(mct_header_t)) {
        write_serial_string("[LOADER] too small\n");
        return -2;
    }
    
    write_serial_string("[LOADER] sz=");
    write_serial_hex(sz);
    write_serial_string("\n");

    mct_header_t* header = (mct_header_t*)buf;
    if (header->magic != MCT_MAGIC) {
        write_serial_string("[LOADER] Invalid magic\n");
        return -3;
    }
    
    uint32_t total_size = header->code_size + header->data_size;
    write_serial_string("[LOADER] total_size=");
    write_serial_hex(total_size);
    write_serial_string("\n");

    if (total_size == 0 || total_size > 1024 * 1024) return -4;
    // The file only contains the header and the code. data_size (BSS) is uninitialized memory.
    if ((int)(sizeof(mct_header_t) + header->code_size) > sz) return -5;
    
    // Create a new address space
    write_serial_string("[LOADER] vmm_create_address_space...\n");
    uint32_t page_dir = vmm_create_address_space();
    if (page_dir == 0) return -6;
    
    // Explicitly map the memory in the new page directory
    uint32_t num_pages = (total_size + 4095) / 4096;
    if (num_pages == 0) num_pages = 1;
    write_serial_string("[LOADER] mapping pages...\n");
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t addr = 0x08000000 + (i * 4096);
        vmm_alloc_page_at(page_dir, addr, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    
    // CRITICAL SECTION: Temporarily switch to the new address space to copy data.
    __asm__ volatile("cli");
    
    write_serial_string("[LOADER] switching cr3...\n");
    uint32_t old_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
    vmm_switch_page_dir(page_dir);
    
    void* app_mem = (void*)0x08000000;
    
    // Copy code + zero BSS
    write_serial_string("[LOADER] memcpy code...\n");
    memcpy(app_mem, buf + sizeof(mct_header_t), header->code_size);
    write_serial_string("[LOADER] memset BSS...\n");
    memset((uint8_t*)app_mem + header->code_size, 0, header->data_size);
    
    // Switch back to kernel page directory
    write_serial_string("[LOADER] restoring cr3...\n");
    vmm_switch_page_dir(old_cr3);
    
    __asm__ volatile("sti");
    
    void (*entry_point)() = (void (*)())((uint32_t)app_mem + header->entry);
    
    // CRITICAL: Set launch arg BEFORE creating the task to prevent race condition.
    // thread_create() enables interrupts at the end, and the scheduler could run
    // the new task before we get to set the arg.
    extern void task_set_launch_arg(int tid, const char* a);
    
    __asm__ volatile("cli");
    int task_id = thread_create(entry_point, PRIORITY_INTERACTIVE, page_dir);
    if (task_id < 0) {
        __asm__ volatile("sti");
        vmm_free_address_space(page_dir);
        write_serial_string("FAIL\n");
        return -7;
    }
    
    // Set launch arg while interrupts are still disabled (thread_create re-enables
    // them, but we disabled again before calling it — actually thread_create has its
    // own cli/sti, so we set arg right after it returns, before any yield).
    if (arg) {
        task_set_launch_arg(task_id, arg);
    }
    __asm__ volatile("sti");
    
    write_serial_string("OK\n");
    return task_id;
}

int load_mct_app(const char* filename) {
    return load_mct_app_with_arg(filename, "");
}

