#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/pci.h"

static int pci_win_id = -1;
static int pci_open = 0;
static int scroll_offset = 0;

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

static void pci_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    draw_rect(cx, cy, cw, ch, GUI_BG);

    // Header bar
    draw_rect(cx, cy, cw, 20, GUI_BORDER2);
    draw_string_px(cx + 8,   cy + 2, "BUS", 0x00AAAAAA, GUI_BORDER2);
    draw_string_px(cx + 40,  cy + 2, "Vendor", 0x00AAAAAA, GUI_BORDER2);
    draw_string_px(cx + 120, cy + 2, "Type", 0x00AAAAAA, GUI_BORDER2);
    draw_string_px(cx + 320, cy + 2, "ID", 0x00AAAAAA, GUI_BORDER2);
    draw_rect(cx, cy + 20, cw, 1, GUI_BORDER);

    // Device count badge
    char cnt[16];
    int ci = 0;
    int n = pci_device_count;
    if (n == 0) cnt[ci++] = '0';
    else {
        char r[8]; int rl = 0;
        while (n > 0) { r[rl++] = '0' + n % 10; n /= 10; }
        while (rl > 0) cnt[ci++] = r[--rl];
    }
    cnt[ci++] = ' '; cnt[ci++] = 'd'; cnt[ci++] = 'e'; cnt[ci++] = 'v';
    cnt[ci++] = 'i'; cnt[ci++] = 'c'; cnt[ci++] = 'e'; cnt[ci++] = 's';
    cnt[ci] = '\0';
    draw_string_px(cx + cw - 90, cy + 2, cnt, GUI_CYAN, GUI_BORDER2);

    int max_rows = (ch - 24) / 18;
    int y = cy + 24;

    for (int i = scroll_offset; i < pci_device_count && (i - scroll_offset) < max_rows; i++) {
        pci_device_t *d = &pci_devices[i];
        uint32_t row_bg = (i % 2 == 0) ? GUI_BG : 0x00181818;
        draw_rect(cx, y, cw, 18, row_bg);

        // BUS:SLOT.FUNC
        char bsf[10];
        bsf[0] = '0' + d->bus / 10;
        bsf[1] = '0' + d->bus % 10;
        bsf[2] = ':';
        bsf[3] = '0' + d->slot / 10;
        bsf[4] = '0' + d->slot % 10;
        bsf[5] = '.';
        bsf[6] = '0' + d->func;
        bsf[7] = '\0';
        draw_string_px(cx + 8, y + 1, bsf, 0x00888888, row_bg);

        // Vendor name
        const char* vname = pci_vendor_name(d->vendor_id);
        draw_string_px(cx + 40, y + 1, vname, GUI_CYAN, row_bg);

        // Class name
        const char* cname = pci_class_name(d->class_code, d->subclass);
        draw_string_px(cx + 120, y + 1, cname, GUI_TEXT, row_bg);

        // Vendor:Device hex IDs
        char vid[7], did[7];
        hex16(d->vendor_id, vid);
        hex16(d->device_id, did);
        draw_string_px(cx + 320, y + 1, vid, 0x00666666, row_bg);
        draw_string_px(cx + 376, y + 1, did, 0x00666666, row_bg);

        y += 18;
    }

    // Scrollbar
    if (pci_device_count > max_rows) {
        int sb_h = ch - 24;
        int thumb_h = (max_rows * sb_h) / pci_device_count;
        if (thumb_h < 10) thumb_h = 10;
        int thumb_y = cy + 24 + (scroll_offset * sb_h) / pci_device_count;
        draw_rect(cx + cw - 6, cy + 24, 6, sb_h, 0x00111111);
        draw_rect(cx + cw - 6, thumb_y, 6, thumb_h, 0x00444444);
    }
}

static void pci_key(int id, char c, uint8_t sc) {
    (void)id; (void)c;
    int max_rows = 18; // approximate
    if (sc == 0x48 && scroll_offset > 0) scroll_offset--; // Up
    if (sc == 0x50 && scroll_offset < pci_device_count - max_rows) scroll_offset++; // Down
}

static void pci_tick(int id) {
    if (!wm_is_open(id)) {
        pci_open = 0;
        pci_win_id = -1;
    }
}

void open_pci_app() {
    if (pci_open && wm_is_open(pci_win_id)) { wm_raise(pci_win_id); return; }
    scroll_offset = 0;
    pci_win_id = wm_open(60, 60, 440, 360, "PCI Device Manager", pci_draw, pci_key, pci_tick, 0);
    pci_open = (pci_win_id >= 0);
}
