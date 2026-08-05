#include "src/include/syscall.h"

static void putn(int n) {
    if (n < 0) { sys_print("-", 0x0A); n = -n; }
    char b[12]; int i = 0;
    if (n == 0) b[i++] = '0';
    while (n > 0 && i < 11) { b[i++] = '0' + (n % 10); n /= 10; }
    b[i] = '\0';
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    sys_print(b, 0x0A);
}

void _start() {
    sys_print("[UDP] udptest starting (UDP API check)\n", 0x0E);

    // Bind a local port
    int b = sys_udp_bind(7777);
    sys_print("[UDP] bind(7777) = ", 0x0F);
    putn(b);
    sys_print("\n", 0x0F);

    // Send a datagram to the QEMU host gateway (10.0.2.2:9999). Even with no
    // listener the send path must return 0 and not crash.
    uint8_t ip[4] = {10, 0, 2, 2};
    const char* msg = "hello-udp-from-mectov";
    int s = sys_udp_send(ip, 9999, msg, 20);
    sys_print("[UDP] send to 10.0.2.2:9999 = ", 0x0F);
    putn(s);
    sys_print("\n", 0x0F);

    // Non-blocking recv on an unbound-peer socket: 0 (nothing yet) is fine.
    char rbuf[64];
    int r = sys_udp_recv(rbuf, 64);
    sys_print("[UDP] recv (no data yet) = ", 0x0F);
    putn(r);
    sys_print("\n", 0x0F);

    sys_print("[UDP] udptest done (API OK)\n", 0x0E);
    sys_exit();
}
