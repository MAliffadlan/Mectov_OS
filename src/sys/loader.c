#include "../include/loader.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/task.h"
#include "../include/vmm.h"
#include "../include/utils.h"
#include "../include/serial.h"

// ============================================================
// ELF32 structures (executable & linking format, i386)
// ============================================================
#define ELFCLASS32     1
#define ELFDATA2LSB    1
#define ET_EXEC        2
#define EM_386         3
#define PT_LOAD        1

// NOTE: e_ident is a full 16-byte array — e_type sits at offset 16. Shrinking
// e_ident (e.g. only magic+class+data+version) misaligns every later field and
// makes valid binaries fail the e_type/e_machine checks.
typedef struct {
    uint8_t  e_ident[16];  // [0..3]=0x7F'ELF', [4]=class, [5]=data, [6]=ver
    uint16_t e_type;       // ET_EXEC
    uint16_t e_machine;    // EM_386
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;    // PT_LOAD
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

// ============================================================
// MCT loader (existing proprietary format)
// ============================================================

static int finish_loaded_task(const char* filename, const char* arg,
                              loader_image_t* img);

// Build the address space + copy the image. Returns 0 on success; on failure
// returns a negative error and frees any partially built address space.
static int build_mct_image(const char* filename, const char* arg,
                           uint8_t* buf, mct_header_t* header,
                           uint32_t total_size, loader_image_t* out) {
    (void)filename;
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
        // A failed mapping (OOM) would make the memcpy below fault at CPL 0
        // and panic the kernel — abort the load cleanly instead.
        if (!vmm_alloc_page_at(page_dir, addr, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            write_serial_string("[LOADER] MCT out of memory mapping image\n");
            vmm_free_address_space(page_dir);
            return -6;
        }
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

    (void)arg;
    out->page_dir = page_dir;
    out->entry = 0x08000000 + header->entry;
    out->heap_start = 0x08000000 + (num_pages * 4096);
    write_serial_string("OK\n");
    return 0;
}

// ============================================================
// ELF loader — maps PT_LOAD segments at their p_vaddr
// ============================================================

static int build_elf_image(const char* filename, const char* arg,
                           uint8_t* buf, int sz, loader_image_t* out) {
    (void)filename;
    if (sz < (int)sizeof(elf32_ehdr_t)) {
        write_serial_string("[LOADER] ELF too small\n");
        return -20;
    }
    elf32_ehdr_t* eh = (elf32_ehdr_t*)buf;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) {
        write_serial_string("[LOADER] ELF bad ident\n");
        return -21;
    }
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_386) {
        write_serial_string("[LOADER] ELF not i386 ET_EXEC\n");
        return -22;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(elf32_phdr_t)) {
        write_serial_string("[LOADER] ELF no program headers\n");
        return -23;
    }
    // Range check the program-header table with 64-bit math: e_phnum and
    // e_phentsize are uint16 each, so their product can overflow 32 bits and
    // slip a crafted table past a 32-bit comparison.
    uint64_t phdrs_end = (uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (phdrs_end > (uint64_t)sz) {
        write_serial_string("[LOADER] ELF phdrs out of bounds\n");
        return -24;
    }

    // First pass: compute total memory footprint so we can size the heap.
    // Clamp segment sizes: a crafted p_memsz of 4GB would make the mapping
    // loop below churn through a million pages (or wrap) and trivially DoS the
    // kernel. Everything this loader runs is a small freestanding app.
    uint32_t max_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        elf32_phdr_t* ph = (elf32_phdr_t*)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz > 16u * 1024 * 1024) {
            write_serial_string("[LOADER] ELF segment too large\n");
            return -25;
        }
        uint64_t end64 = (uint64_t)ph->p_vaddr + ph->p_memsz;
        if (end64 > 0x0FFFFFFF) { // stay well below the 0x09000000 lib region
            write_serial_string("[LOADER] ELF segment past limit\n");
            return -25;
        }
        uint32_t end = (uint32_t)end64;
        if (end > max_end) max_end = end;
    }
    if (max_end == 0) {
        write_serial_string("[LOADER] ELF no loadable segments\n");
        return -25;
    }

    // Create a new address space
    write_serial_string("[LOADER] ELF vmm_create_address_space...\n");
    uint32_t page_dir = vmm_create_address_space();
    if (page_dir == 0) return -26;
    out->page_dir = page_dir;

    // Map every page covered by any PT_LOAD segment
    write_serial_string("[LOADER] ELF mapping segments...\n");
    for (int i = 0; i < eh->e_phnum; i++) {
        elf32_phdr_t* ph = (elf32_phdr_t*)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint32_t start = ph->p_vaddr & ~0xFFFu;
        uint32_t end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFu;
        for (uint32_t a = start; a < end; a += 4096) {
            if (!vmm_alloc_page_at(page_dir, a, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
                write_serial_string("[LOADER] ELF out of memory mapping segments\n");
                vmm_free_address_space(page_dir);
                return -26;
            }
        }
    }

    // CRITICAL SECTION: copy segment data in the new address space
    __asm__ volatile("cli");
    uint32_t old_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(old_cr3));
    vmm_switch_page_dir(page_dir);

    for (int i = 0; i < eh->e_phnum; i++) {
        elf32_phdr_t* ph = (elf32_phdr_t*)(buf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        // Guard: segment must lie fully inside the file we read.
        // 64-bit math: a crafted p_offset (e.g. 0xFFFFFF00) plus p_filesz can
        // wrap 32 bits and slip past the check, then buf + p_offset in the
        // memcpy below points into unmapped memory and panics the kernel.
        if ((uint64_t)ph->p_offset + ph->p_filesz > (uint64_t)sz) {
            write_serial_string("[LOADER] ELF segment out of file bounds\n");
            vmm_switch_page_dir(old_cr3);
            __asm__ volatile("sti");
            vmm_free_address_space(page_dir);
            return -27;
        }
        if (ph->p_filesz > 0) {
            memcpy((void*)(uintptr_t)ph->p_vaddr, buf + ph->p_offset, ph->p_filesz);
        }
        // Zero-fill the BSS remainder (.bss within the segment)
        if (ph->p_memsz > ph->p_filesz) {
            memset((void*)(uintptr_t)(ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        }
    }

    vmm_switch_page_dir(old_cr3);
    __asm__ volatile("sti");

    void (*entry_point)() = (void (*)())(uintptr_t)eh->e_entry;
    write_serial_string("[LOADER] ELF entry=");
    write_serial_hex(eh->e_entry);
    write_serial_string("\n");

    (void)entry_point;
    (void)arg;
    out->entry = eh->e_entry;
    out->heap_start = (max_end + 0xFFF) & ~0xFFFu;
    write_serial_string("[LOADER] ELF OK\n");
    return 0;
}

// ============================================================
// Public entry — format auto-detected (MCT1 vs ELF)
// ============================================================

int load_mct_app_with_arg(const char* filename, const char* arg) {
    write_serial_string("[LOADER] start\n");

    // Use new VFS API
    int node = vfs_get_node(filename);
    if (node < 0 || vfs_is_dir(node)) {
        write_serial_string("[LOADER] NOT FOUND\n");
        return -1;
    }

    int file_size = fs_nodes[node].size;
    if (file_size <= 0 || file_size > 1024 * 1024) {
        write_serial_string("[LOADER] invalid size\n");
        return -2;
    }

    // Read the whole file into a temp buffer
    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) return -10;

    int sz = vfs_read_file(filename, (char*)buf, file_size);
    if (sz < 4) {
        write_serial_string("[LOADER] too small\n");
        kfree(buf);
        return -2;
    }

    write_serial_string("[LOADER] sz=");
    write_serial_hex(sz);
    write_serial_string("\n");

    loader_image_t img = {0};
    int rc;

    // ---- ELF detection: 0x7F 'E' 'L' 'F' ----
    if (buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        rc = build_elf_image(filename, arg, buf, sz, &img);
        kfree(buf);
        if (rc < 0) return rc;
        return finish_loaded_task(filename, arg, &img);
    }

    // ---- MCT format ----
    if (sz < (int)sizeof(mct_header_t)) {
        write_serial_string("[LOADER] too small (mct)\n");
        kfree(buf);
        return -2;
    }

    mct_header_t* header = (mct_header_t*)buf;
    if (header->magic != MCT_MAGIC) {
        write_serial_string("[LOADER] Invalid magic\n");
        kfree(buf);
        return -3;
    }

    uint32_t total_size = header->code_size + header->data_size;
    write_serial_string("[LOADER] total_size=");
    write_serial_hex(total_size);
    write_serial_string("\n");

    if (total_size == 0 || total_size > 1024 * 1024) { kfree(buf); return -4; }
    // The file only contains the header and the code. data_size (BSS) is uninitialized memory.
    if ((int)(sizeof(mct_header_t) + header->code_size) > sz) { kfree(buf); return -5; }

    rc = build_mct_image(filename, arg, buf, header, total_size, &img);
    kfree(buf);
    if (rc < 0) return rc;
    return finish_loaded_task(filename, arg, &img);
}

// Create a new task for a built image: thread_create + launch arg + heap.
// Shared by the MCT and ELF build paths above.
static int finish_loaded_task(const char* filename, const char* arg,
                              loader_image_t* img) {
    // CRITICAL: Set launch arg BEFORE creating the task to prevent race condition.
    // thread_create() leaves interrupts disabled at the end, and the scheduler
    // could run the new task before we get to set the arg.
    extern void task_set_launch_arg(int tid, const char* a);

    __asm__ volatile("cli");
    int task_id = thread_create((void (*)(void))(uintptr_t)img->entry,
                                PRIORITY_INTERACTIVE, img->page_dir);
    if (task_id < 0) {
        __asm__ volatile("sti");
        vmm_free_address_space(img->page_dir);
        write_serial_string("[LOADER] task create failed\n");
        return -7;
    }

    if (arg && arg[0] != '\0') {
        task_set_launch_arg(task_id, arg);
    } else {
        task_set_launch_arg(task_id, filename);
    }
    extern void task_set_heap_ptr(int tid, uint32_t ptr);
    task_set_heap_ptr(task_id, img->heap_start);
    __asm__ volatile("sti");

    write_serial_string("[LOADER] task ");
    write_serial_hex(task_id);
    write_serial_string("\n");
    return task_id;
}

int load_mct_app(const char* filename) {
    return load_mct_app_with_arg(filename, "");
}

// ============================================================
// Exec support — build an image WITHOUT creating a task.
// ============================================================
// Used by task_exec() to replace the current task's image in place. The
// caller owns img->page_dir: it must either hand it to the task or free it.
// Returns 0 on success, negative error code on failure.
// NOTE: `filename` is a kernel-side copy made by the syscall layer; never a
// raw user pointer (it is dereferenced across a CR3 switch).
int loader_build_image(const char* filename, const char* arg,
                       loader_image_t* img) {
    write_serial_string("[LOADER] exec: ");
    write_serial_string(filename);
    write_serial_string("\n");

    int node = vfs_get_node(filename);
    if (node < 0 || vfs_is_dir(node)) {
        write_serial_string("[LOADER] NOT FOUND\n");
        return -1;
    }

    int file_size = fs_nodes[node].size;
    if (file_size <= 0 || file_size > 1024 * 1024) {
        write_serial_string("[LOADER] invalid size\n");
        return -2;
    }

    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) return -10;

    int sz = vfs_read_file(filename, (char*)buf, file_size);
    if (sz < 4) {
        write_serial_string("[LOADER] too small\n");
        kfree(buf);
        return -2;
    }

    memset(img, 0, sizeof(*img));
    int rc;
    if (buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        rc = build_elf_image(filename, arg, buf, sz, img);
    } else {
        if (sz < (int)sizeof(mct_header_t)) {
            write_serial_string("[LOADER] too small (mct)\n");
            kfree(buf);
            return -2;
        }
        mct_header_t* header = (mct_header_t*)buf;
        if (header->magic != MCT_MAGIC) {
            write_serial_string("[LOADER] Invalid magic\n");
            kfree(buf);
            return -3;
        }
        uint32_t total_size = header->code_size + header->data_size;
        if (total_size == 0 || total_size > 1024 * 1024) { kfree(buf); return -4; }
        if ((int)(sizeof(mct_header_t) + header->code_size) > sz) { kfree(buf); return -5; }
        rc = build_mct_image(filename, arg, buf, header, total_size, img);
    }
    kfree(buf);
    return rc;
}

void* load_mct_library(const char* filename) {
    write_serial_string("[LOADER] load lib: ");
    write_serial_string(filename);
    write_serial_string("\n");

    int node = vfs_get_node(filename);
    if (node < 0 || vfs_is_dir(node)) return 0;

    int file_size = fs_nodes[node].size;
    if (file_size <= 0 || file_size > 1024 * 1024) return 0;

    uint8_t* lib_buf = (uint8_t*)kmalloc(file_size);
    if (!lib_buf) return 0;

    int sz = vfs_read_file(filename, (char*)lib_buf, file_size);
    if (sz < (int)sizeof(mct_header_t)) { kfree(lib_buf); return 0; }

    mct_header_t* header = (mct_header_t*)lib_buf;
    if (header->magic != MCT_MAGIC) { kfree(lib_buf); return 0; }

    uint32_t total_size = header->code_size + header->data_size;
    if (total_size == 0 || total_size > 1024 * 1024) { kfree(lib_buf); return 0; }
    // The file only contains the header and the code — a forged header with a
    // huge code_size would make the memcpy below read way past the small
    // kmalloc'd buffer, leaking kernel heap into the caller's address space.
    if ((int)(sizeof(mct_header_t) + header->code_size) > sz) { kfree(lib_buf); return 0; }

    uint32_t lib_base = 0x09000000;
    uint32_t num_pages = (total_size + 4095) / 4096;
    if (num_pages == 0) num_pages = 1;

    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    // CRITICAL: Prevent task switching while modifying page tables
    __asm__ volatile("cli");

    // Switch to the target page directory to allocate and map pages
    uint32_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    vmm_switch_page_dir(cr3);

    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t addr = lib_base + (i * 4096);
        vmm_alloc_page_at(cr3, addr, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    void* lib_mem = (void*)lib_base;
    memcpy(lib_mem, lib_buf + sizeof(mct_header_t), header->code_size);
    memset((uint8_t*)lib_mem + header->code_size, 0, header->data_size);

    // Restore original page directory
    vmm_switch_page_dir(old_cr3);
    __asm__ volatile("sti");

    kfree(lib_buf);

    // In our library, the export table (MLIB) is placed right at the beginning of .text (offset 0)
    return lib_mem;
}
