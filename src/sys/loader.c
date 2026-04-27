#include "../include/loader.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/task.h"
#include "../include/utils.h"

int load_mct_app(const char* filename) {
    int idx = vfs_find(filename);
    if (idx < 0) {
        return -1; // File not found
    }
    
    // Check file size against header size
    if (fs[idx].size < (int)sizeof(mct_header_t)) {
        return -2; // File too small
    }
    
    mct_header_t* header = (mct_header_t*)fs[idx].data;
    
    // Verify magic number
    if (header->magic != MCT_MAGIC) {
        return -3; // Invalid magic
    }
    
    // Calculate total size needed
    uint32_t total_size = header->code_size + header->data_size;
    if (total_size == 0 || total_size > 1024 * 1024) { // Max 1MB app size
        return -4; // Invalid size
    }
    
    // Allocate memory for the app
    // We use a FIXED memory address (32MB mark) so we can compile the app
    // with a fixed base address (-Ttext 0x02000000) and avoid PIC/GOT issues!
    void* app_mem = (void*)0x02000000;
    
    // Copy code
    memcpy(app_mem, fs[idx].data + sizeof(mct_header_t), header->code_size);
    
    // Zero out data/bss section
    memset((uint8_t*)app_mem + header->code_size, 0, header->data_size);
    
    // The entry point in the header is an offset relative to the start of the app memory
    // Calculate the absolute virtual address
    void (*entry_point)() = (void (*)())((uint32_t)app_mem + header->entry);
    
    // Create the user task
    int task_id = create_user_task(entry_point);
    if (task_id < 0) {
        kfree(app_mem);
        return -6; // Failed to create task
    }
    
    return task_id; // Success
}
