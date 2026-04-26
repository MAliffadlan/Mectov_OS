#include "../include/rtl8139.h"
#include "../include/pci.h"
#include "../include/io.h"
#include "../include/mem.h"
#include "../include/utils.h"

uint8_t rtl_mac[6];
int rtl_present = 0;
static uint32_t rtl_io_base = 0;

// RX buffer (8K + 16 + 1500 wrap padding)
static uint8_t  rtl_rx_buffer[RTL_RX_BUF_SIZE] __attribute__((aligned(4)));
static uint16_t rtl_rx_offset = 0;

// TX: 4 descriptors, rotating
static uint8_t  rtl_tx_buffers[4][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
static int      rtl_tx_cur = 0;

void init_rtl8139(void) {
    uint8_t bus = 0, slot = 0, func = 0;
    int found = 0;

    // 1. Find the RTL8139 device on PCI bus
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == 0x10EC && pci_devices[i].device_id == 0x8139) {
            bus = pci_devices[i].bus;
            slot = pci_devices[i].slot;
            func = pci_devices[i].func;
            found = 1;
            break;
        }
    }

    if (!found) return;
    rtl_present = 1;

    // 2. Read BAR0 to get IO port base
    uint32_t bar0 = pci_read(bus, slot, func, 0x10);
    rtl_io_base = bar0 & ~3;

    // 3. Enable Bus Mastering and IO space
    uint32_t cmd = pci_read(bus, slot, func, 0x04);
    cmd |= (1 << 0) | (1 << 2); // IO Space + Bus Master
    pci_write(bus, slot, func, 0x04, cmd);

    // 4. Power on
    outb(rtl_io_base + RTL_CONFIG1, 0x00);

    // 5. Software Reset
    outb(rtl_io_base + RTL_CMD, 0x10);
    while (inb(rtl_io_base + RTL_CMD) & 0x10) {}

    // 6. Read MAC Address
    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = inb(rtl_io_base + RTL_MAC0 + i);
    }

    // 7. Set RX buffer physical address
    outl(rtl_io_base + RTL_RX_BUF, (uint32_t)(uintptr_t)rtl_rx_buffer);

    // 8. Set TX buffer addresses for all 4 descriptors
    for (int i = 0; i < 4; i++) {
        outl(rtl_io_base + RTL_TX_ADDR0 + (i * 4), (uint32_t)(uintptr_t)rtl_tx_buffers[i]);
    }

    // 9. Configure RX: Accept Broadcast + Multicast + Physical Match + wrap
    //    AB=1, AM=1, APM=1, AAP=0, WRAP=1
    outl(rtl_io_base + RTL_RX_CFG, 0x0000000F | (1 << 7)); // WRAP bit

    // 10. Enable TX and RX
    outb(rtl_io_base + RTL_CMD, 0x0C); // TE=1, RE=1

    // 11. Clear all interrupt status
    outw(rtl_io_base + RTL_ISR, 0xFFFF);

    // 12. Disable interrupts (we use polling)
    outw(rtl_io_base + RTL_IMR, 0x0000);

    rtl_rx_offset = 0;
}

void rtl8139_send_packet(void* data, uint32_t len) {
    if (!rtl_present || len > 1500) return;

    // Copy data into current TX buffer
    uint8_t* buf = rtl_tx_buffers[rtl_tx_cur];
    memcpy(buf, data, len);

    // Tell RTL8139: buffer address already set, now write status+size
    // Bit 13 (OWN) must be CLEAR to let hardware send
    // Size is in bits 0-12
    outl(rtl_io_base + RTL_TX_STATUS0 + (rtl_tx_cur * 4), len & 0x1FFF);

    // Wait for transmission to complete (OWN bit set by hardware)
    int timeout = 10000;
    while (timeout-- > 0) {
        uint32_t status = inl(rtl_io_base + RTL_TX_STATUS0 + (rtl_tx_cur * 4));
        if (status & (1 << 15)) break; // TOK - Transmit OK
        if (status & (1 << 14)) break; // TUN - Transmit underrun (still sent)
    }

    // Advance to next descriptor
    rtl_tx_cur = (rtl_tx_cur + 1) % 4;
}

int rtl8139_poll_rx(uint8_t* out_buf, uint32_t max_len) {
    if (!rtl_present) return 0;

    // Check if buffer is empty
    uint8_t cmd = inb(rtl_io_base + RTL_CMD);
    if (cmd & 0x01) return 0; // BUFE bit = buffer empty

    // Packet header is at current offset in ring buffer
    // Header format: [status:16] [length:16] [data...]
    uint16_t status = *(uint16_t*)(rtl_rx_buffer + rtl_rx_offset);
    uint16_t length = *(uint16_t*)(rtl_rx_buffer + rtl_rx_offset + 2);

    if (!(status & 0x01)) return 0; // ROK bit not set = bad packet

    // length includes the 4-byte CRC at the end
    uint32_t pkt_len = length - 4; // strip CRC
    if (pkt_len > max_len) pkt_len = max_len;

    // Copy packet data (skip 4-byte header)
    memcpy(out_buf, (void*)(rtl_rx_buffer + rtl_rx_offset + 4), pkt_len);

    // Advance RX offset: aligned to 4 bytes, +4 for header
    rtl_rx_offset = (rtl_rx_offset + length + 4 + 3) & ~3;
    rtl_rx_offset %= RTL_RX_BUF_SIZE;

    // Update CAPR (Current Address of Packet Read) = offset - 0x10
    outw(rtl_io_base + RTL_CAPR, rtl_rx_offset - 0x10);

    return (int)pkt_len;
}
