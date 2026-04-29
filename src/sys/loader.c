#include "../include/loader.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/task.h"
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
    if ((int)(sizeof(mct_header_t) + total_size) > sz) return -5;
    
    void* app_mem = (void*)0x02000000;
    
    // Explicitly map the memory (Identity map for now, with User/RW/Present flags)
    // Map enough pages to hold total_size
    uint32_t num_pages = (total_size + 4095) / 4096;
    if (num_pages == 0) num_pages = 1;
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t addr = 0x02000000 + (i * 4096);
        page_map(addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    
    // CRITICAL for KVM: Flush TLB so instruction fetch sees the new mapping.
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
    
    // Copy code + zero BSS
    memcpy(app_mem, buf + sizeof(mct_header_t), header->code_size);
    memset((uint8_t*)app_mem + header->code_size, 0, header->data_size);
    
    // CRITICAL for KVM: Flush TLB so instruction fetch sees the new code.
    // Without this, KVM may execute stale/garbage data from the old page content.
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
    
    void (*entry_point)() = (void (*)())((uint32_t)app_mem + header->entry);
    
    // Serial marker 'L' = about to create task
    write_serial_string("LOAD_MCT: Entry=");
    write_serial_hex(header->entry);
    write_serial_string(" First bytes=");
    write_serial_hex(*(uint32_t*)entry_point);
    write_serial_string("\n");
    
    int task_id = create_user_task(entry_point);
    if (task_id < 0) {
        write_serial_string("FAIL\n");
        return -6;
    }
    
    write_serial_string("OK\n");
    return task_id;
}
