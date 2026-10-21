#ifndef E1000E_H
#define E1000E_H

#include "types.h"

// Intel 82574/82540-family Gigabit Ethernet (e1000/e1000e) driver (v38.170).
// Registers itself as a net_backend_t with the same contract rtl8139.c uses,
// so the protocol layer (net.c) switches NICs without knowing which one is
// present. QEMU exposes it as `-device e1000` (82540EM, device 0x100E) or
// `-device e1000e` (82574, device 0x10D3); both share this register layout at
// MMIO BAR0.

void e1000e_init(void);
int  e1000e_present(void);          // 1 once a NIC probed up successfully
void e1000e_irq_handler(void);      // called from the shared net IRQ dispatch

// Compat symbols the protocol layer may read directly (mirrors rtl8139.h):
// the backend get_mac() reads these, and user-space apps keep compiling.
extern uint8_t e1000e_mac[6];
extern int     e1000e_present_flag;

#endif
