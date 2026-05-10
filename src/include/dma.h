#ifndef DMA_H
#define DMA_H

#include "types.h"

// DMA Channel modes
#define DMA_MODE_READ   0x44 // I/O to memory, auto-init, single transfer
#define DMA_MODE_WRITE  0x48 // Memory to I/O, auto-init, single transfer

void dma_init(void);
void dma_set_channel(uint8_t channel, uint8_t mode, uint32_t address, uint32_t count);

#endif
