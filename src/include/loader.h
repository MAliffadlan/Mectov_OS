#ifndef LOADER_H
#define LOADER_H

#include "types.h"

#define MCT_MAGIC 0x4D435431 // "MCT1"

typedef struct {
    uint32_t magic;      // "MCT1"
    uint32_t entry;      // entry point offset
    uint32_t code_size;  // ukuran kode
    uint32_t data_size;  // ukuran data + bss
    // diikuti oleh code[] dan data[]
} mct_header_t;

// A loadable program image: a fully built address space plus the metadata
// needed to run it. Shared by load_mct_app_with_arg() (creates a new task)
// and task_exec() (replaces the current task's image in place).
typedef struct {
    uint32_t page_dir;    // built address space (0 on failure)
    uint32_t entry;       // entry point VA
    uint32_t heap_start;  // initial heap break for SYS_MALLOC
} loader_image_t;

int load_mct_app(const char* filename);
int load_mct_app_with_arg(const char* filename, const char* arg);
// Build an image WITHOUT creating a task (used by exec). The caller owns
// img->page_dir on success. `filename` must be a kernel-side copy.
int loader_build_image(const char* filename, const char* arg, loader_image_t* img);

#endif
