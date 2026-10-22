#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"
#include "device.h"

#define BLOCK_BATCH_MAX 128

typedef struct block_ops {
    int (*read)(int drive, uint32_t lba, int count, uint8_t* buf);
    int (*write)(int drive, uint32_t lba, int count, const uint8_t* buf);
} block_ops_t;

void block_register(device_t* dev);
int  block_read(int drive, uint32_t lba, int count, uint8_t* buf);
int  block_write(int drive, uint32_t lba, int count, const uint8_t* buf);
int  block_present(int drive);

static inline int block_batch_limit(uint32_t lba, int count) {
    if (count > BLOCK_BATCH_MAX) count = BLOCK_BATCH_MAX;
    int boundary = BLOCK_BATCH_MAX - (int)(lba & (BLOCK_BATCH_MAX - 1));
    if (count > boundary) count = boundary;
    return (count < 1) ? 1 : count;
}

#endif
