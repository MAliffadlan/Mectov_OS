#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"

// RTL8139 Registers
#define RTL_MAC0       0x00
#define RTL_MAR0       0x08
#define RTL_TX_STATUS0 0x10  // TX status for desc 0 (0x10,0x14,0x18,0x1C)
#define RTL_TX_ADDR0   0x20  // TX buffer addr for desc 0 (0x20,0x24,0x28,0x2C)
#define RTL_RX_BUF     0x30
#define RTL_RX_BUF_PTR 0x38
#define RTL_RX_BUF_ADDR 0x3A
#define RTL_CMD        0x37
#define RTL_CAPR       0x38  // Current Address of Packet Read
#define RTL_CBR        0x3A  // Current Buffer Address
#define RTL_IMR        0x3C
#define RTL_ISR        0x3E
#define RTL_TX_CFG     0x40
#define RTL_RX_CFG     0x44
#define RTL_RX_MISSE   0x4C
#define RTL_CONFIG1    0x52

// RX buffer size: 8K + 16 + 1500 (wrap padding)
#define RTL_RX_BUF_SIZE  (8192 + 16 + 1500)
#define RTL_TX_BUF_SIZE  1536

void init_rtl8139(void);
void rtl8139_send_packet(void* data, uint32_t len);
int  rtl8139_poll_rx(uint8_t* out_buf, uint32_t max_len);

extern uint8_t rtl_mac[6];
extern int rtl_present;

#endif
