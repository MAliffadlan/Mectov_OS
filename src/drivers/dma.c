#include "../include/dma.h"
#include "../include/io.h"
#include "../include/utils.h"

// DMA Controller 1 (8-bit) Ports
#define DMA1_MASK_REG      0x0A
#define DMA1_MODE_REG      0x0B
#define DMA1_CLEAR_FF_REG  0x0C

// DMA Controller 2 (16-bit) Ports
#define DMA2_MASK_REG      0xD4
#define DMA2_MODE_REG      0xD6
#define DMA2_CLEAR_FF_REG  0xD8

// Page Registers
static const uint8_t dma_page_ports[8] = {
    0x87, 0x83, 0x81, 0x82,  // Channels 0-3 (8-bit)
    0x8F, 0x8B, 0x89, 0x8A   // Channels 4-7 (16-bit)
};

// Address and Count Ports
static const uint8_t dma_addr_ports[8] = {
    0x00, 0x02, 0x04, 0x06,  // Channels 0-3
    0xC0, 0xC4, 0xC8, 0xCC   // Channels 4-7
};

static const uint8_t dma_count_ports[8] = {
    0x01, 0x03, 0x05, 0x07,  // Channels 0-3
    0xC2, 0xC6, 0xCA, 0xCE   // Channels 4-7
};

void dma_init(void) {
    // Basic init if needed, ISA DMA is usually ready to go.
}

void dma_set_channel(uint8_t channel, uint8_t mode, uint32_t address, uint32_t count) {
    if (channel > 7) return;
    
    // The DMA count register expects the number of transfers minus 1
    // For 8-bit channels (0-3), it's bytes.
    // For 16-bit channels (4-7), it's words (16-bit words) AND address must be shifted right by 1!
    if (channel >= 4) {
        count = count / 2;
        address = address / 2; // Address is in words for 16-bit DMA
    }
    count = count - 1;

    uint8_t page = (address >> 16) & 0xFF;
    uint8_t offset_low = address & 0xFF;
    uint8_t offset_high = (address >> 8) & 0xFF;
    
    uint8_t count_low = count & 0xFF;
    uint8_t count_high = (count >> 8) & 0xFF;
    
    uint8_t mask_port = (channel < 4) ? DMA1_MASK_REG : DMA2_MASK_REG;
    uint8_t mode_port = (channel < 4) ? DMA1_MODE_REG : DMA2_MODE_REG;
    uint8_t clear_port = (channel < 4) ? DMA1_CLEAR_FF_REG : DMA2_CLEAR_FF_REG;
    uint8_t chan_idx = channel % 4;
    
    // 1. Mask channel
    outb(mask_port, 0x04 | chan_idx);
    
    // 2. Set mode
    outb(mode_port, mode | chan_idx);
    
    // 3. Clear flip-flop
    outb(clear_port, 0xFF);
    
    // 4. Set address
    outb(dma_addr_ports[channel], offset_low);
    outb(dma_addr_ports[channel], offset_high);
    
    // 5. Set page
    outb(dma_page_ports[channel], page);
    
    // 6. Clear flip-flop again just to be safe
    outb(clear_port, 0xFF);
    
    // 7. Set count
    outb(dma_count_ports[channel], count_low);
    outb(dma_count_ports[channel], count_high);
    
    // 8. Unmask channel
    outb(mask_port, chan_idx);
}
