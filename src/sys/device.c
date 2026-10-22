#include "../include/device.h"
#include "../include/initcall.h"
#include "../include/serial.h"

extern initcall_fn __initcall_start[];
extern initcall_fn __initcall_end[];

static device_t* devices[DEV_CLASS_COUNT][MAX_DEVICES_PER_CLASS];
static int device_counts[DEV_CLASS_COUNT];

static void dev_write_uint(uint32_t v) {
    char buf[10];
    int n = 0;
    if (v == 0) { write_serial('0'); return; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) write_serial(buf[--n]);
}

static int dev_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void device_register(device_t* dev) {
    if (dev == NULL || dev->cls >= DEV_CLASS_COUNT) return;
    int n = device_counts[dev->cls];
    if (n >= MAX_DEVICES_PER_CLASS) {
        write_serial_string("[DEV] registry full\n");
        return;
    }
    dev->instance = n;
    devices[dev->cls][n] = dev;
    device_counts[dev->cls] = n + 1;
}

device_t* device_find(device_class_t cls, const char* name) {
    if (cls >= DEV_CLASS_COUNT) return NULL;
    for (int i = 0; i < device_counts[cls]; i++)
        if (dev_streq(devices[cls][i]->name, name)) return devices[cls][i];
    return NULL;
}

device_t* device_at(device_class_t cls, int index) {
    if (cls >= DEV_CLASS_COUNT || index < 0 || index >= device_counts[cls])
        return NULL;
    return devices[cls][index];
}

int device_count(device_class_t cls) {
    return (cls < DEV_CLASS_COUNT) ? device_counts[cls] : 0;
}

void device_log_summary(void) {
    for (int c = 0; c < DEV_CLASS_COUNT; c++) {
        for (int i = 0; i < device_counts[c]; i++) {
            device_t* d = devices[c][i];
            write_serial_string("[DEV] ");
            write_serial_string(c == DEV_CLASS_BLOCK ? "block " : "net ");
            write_serial_string(d->name);
            if (c == DEV_CLASS_BLOCK) {
                write_serial_string(" drives=");
                dev_write_uint((uint32_t)d->drive_base);
                write_serial('-');
                dev_write_uint((uint32_t)(d->drive_base + d->drive_count - 1));
            }
            write_serial_string(" present=");
            write_serial('0' + (d->present ? 1 : 0));
            write_serial_string("\n");
        }
    }
}

void device_init_all(void) {
    int count = 0;
    for (initcall_fn* it = __initcall_start; it < __initcall_end; it++) {
        (*it)();
        count++;
    }
    write_serial_string("[DEV] initcalls=");
    dev_write_uint((uint32_t)count);
    write_serial_string("\n");
    device_log_summary();
}
