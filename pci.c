#include "src/include/syscall.h"

// Simple hex formatter
static void hex16(uint16_t val, char* buf) {
    const char hex[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(val >> 12) & 0xF];
    buf[3] = hex[(val >> 8)  & 0xF];
    buf[4] = hex[(val >> 4)  & 0xF];
    buf[5] = hex[val & 0xF];
    buf[6] = '\0';
}

const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00:
            if (subclass == 0x01) return "VGA Compatible";
            return "Unclassified";
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x02: return "Floppy Controller";
                case 0x05: return "ATA Controller";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default:   return "Storage Controller";
            }
        case 0x02:
            switch (subclass) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default:   return "Network Device";
            }
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA Controller";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller";
                default:   return "Display Controller";
            }
        case 0x04:
            switch (subclass) {
                case 0x00: return "Video Device";
                case 0x01: return "Audio Device";
                case 0x03: return "Audio Device";
                default:   return "Multimedia Device";
            }
        case 0x05: return "Memory Controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                case 0x80: return "Other Bridge";
                default:   return "Bridge Device";
            }
        case 0x07: return "Communication Controller";
        case 0x08: return "System Peripheral";
        case 0x09: return "Input Device";
        case 0x0A: return "Docking Station";
        case 0x0B: return "Processor";
        case 0x0C:
            switch (subclass) {
                case 0x00: return "FireWire Controller";
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        case 0x0D: return "Wireless Controller";
        default:   return "Unknown Device";
    }
}

const char* pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x1234: return "QEMU/Bochs";
        case 0x1AF4: return "Red Hat VirtIO";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        case 0x1B36: return "Red Hat QEMU";
        case 0x1002: return "AMD/ATI";
        case 0x14E4: return "Broadcom";
        case 0x10EC: return "Realtek";
        case 0x8087: return "Intel WiFi";
        default:     return "Unknown";
    }
}

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

static pci_device_t pci_devs[32];
static int pci_count = 0;
static int scroll_offset = 0;

static void draw_pci(int wid) {
    int cw = 440;
    int ch = 360;
    sys_draw_rect(wid, 0, 0, cw, ch, 0x001E1E2E);

    // Header bar
    sys_draw_rect(wid, 0, 0, cw, 20, 0x00181825);
    sys_draw_text(wid, 8,   2, "BUS", 0x00A6ADC8);
    sys_draw_text(wid, 40,  2, "Vendor", 0x00A6ADC8);
    sys_draw_text(wid, 140, 2, "Type", 0x00A6ADC8);
    sys_draw_text(wid, 320, 2, "ID", 0x00A6ADC8);
    sys_draw_rect(wid, 0, 20, cw, 1, 0x00313244);

    // Device count badge
    char cnt[16];
    int ci = 0;
    int n = pci_count;
    if (n == 0) cnt[ci++] = '0';
    else {
        char r[8]; int rl = 0;
        while (n > 0) { r[rl++] = '0' + n % 10; n /= 10; }
        while (rl > 0) cnt[ci++] = r[--rl];
    }
    cnt[ci++] = ' '; cnt[ci++] = 'd'; cnt[ci++] = 'e'; cnt[ci++] = 'v';
    cnt[ci++] = 's'; cnt[ci] = '\0';
    sys_draw_text(wid, cw - 60, 2, cnt, 0x0094E2D5);

    int max_rows = (ch - 24) / 18;
    int y = 24;

    for (int i = scroll_offset; i < pci_count && (i - scroll_offset) < max_rows; i++) {
        pci_device_t *d = &pci_devs[i];
        uint32_t row_bg = (i % 2 == 0) ? 0x001E1E2E : 0x00181825;
        sys_draw_rect(wid, 0, y, cw, 18, row_bg);

        // BUS:SLOT.FUNC
        char bsf[10];
        bsf[0] = '0' + d->bus / 10; bsf[1] = '0' + d->bus % 10;
        bsf[2] = ':';
        bsf[3] = '0' + d->slot / 10; bsf[4] = '0' + d->slot % 10;
        bsf[5] = '.';
        bsf[6] = '0' + d->func; bsf[7] = '\0';
        sys_draw_text(wid, 8, y + 1, bsf, 0x006C7086);

        // Vendor name
        const char* vname = pci_vendor_name(d->vendor_id);
        sys_draw_text(wid, 40, y + 1, vname, 0x0094E2D5);

        // Class name
        const char* cname = pci_class_name(d->class_code, d->subclass);
        sys_draw_text(wid, 140, y + 1, cname, 0x00CDD6F4);

        // Vendor:Device hex IDs
        char vid[7], did[7];
        hex16(d->vendor_id, vid);
        hex16(d->device_id, did);
        sys_draw_text(wid, 320, y + 1, vid, 0x007F849C);
        sys_draw_text(wid, 376, y + 1, did, 0x007F849C);

        y += 18;
    }

    // Scrollbar
    if (pci_count > max_rows) {
        int sb_h = ch - 24;
        int thumb_h = (max_rows * sb_h) / pci_count;
        if (thumb_h < 10) thumb_h = 10;
        int thumb_y = 24 + (scroll_offset * sb_h) / pci_count;
        sys_draw_rect(wid, cw - 6, 24, 6, sb_h, 0x0011111B);
        sys_draw_rect(wid, cw - 6, thumb_y, 6, thumb_h, 0x0045475A);
    }
    
    sys_update_window(wid);
}

void _start() {
    int wid = sys_create_window(60, 60, 440, 360, "PCI Manager (Ring 3)");
    if (wid < 0) sys_exit();
    
    pci_count = sys_get_pci_info(pci_devs, 32);
    if (pci_count < 0) pci_count = 0;
    
    draw_pci(wid);
    
    gui_event_t ev;
    int max_rows = (360 - 24) / 18;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                draw_pci(wid);
            } else if (ev.type == 2) {
                if (ev.key == 27) sys_exit(); // ESC
                // Scroll with up/down arrows (need to check if arrow keys generate scancode or mapped char)
                // We'll use 'w' and 's' as fallbacks too
                if ((ev.key == 'w' || ev.key == 'W') && scroll_offset > 0) {
                    scroll_offset--; draw_pci(wid);
                }
                if ((ev.key == 's' || ev.key == 'S') && scroll_offset < pci_count - max_rows) {
                    scroll_offset++; draw_pci(wid);
                }
            } else if (ev.type == 3) { // Mouse
                // Simple click to scroll down
                if (ev.key == 1) { // Left click
                    if (scroll_offset < pci_count - max_rows) {
                        scroll_offset++; draw_pci(wid);
                    } else {
                        scroll_offset = 0; draw_pci(wid); // wrap around
                    }
                }
                // Right click scroll up
                if (ev.key == 2) {
                    if (scroll_offset > 0) {
                        scroll_offset--; draw_pci(wid);
                    }
                }
            }
        }
        sys_yield();
    }
}
