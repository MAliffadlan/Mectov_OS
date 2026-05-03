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

int load_mct_app(const char* filename);
int load_mct_app_with_arg(const char* filename, const char* arg);

#endif
