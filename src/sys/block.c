#include "../include/block.h"
#include "../include/device.h"

#define BLOCK_MAX_PROVIDERS 8

static device_t* providers[BLOCK_MAX_PROVIDERS];
static int provider_count;

void block_register(device_t* dev) {
    if (dev == NULL || dev->cls != DEV_CLASS_BLOCK || dev->ops == NULL) return;
    if (provider_count >= BLOCK_MAX_PROVIDERS) return;
    providers[provider_count++] = dev;
    device_register(dev);
}

static device_t* block_lookup(int drive) {
    for (int i = 0; i < provider_count; i++) {
        device_t* d = providers[i];
        if (drive >= d->drive_base && drive < d->drive_base + d->drive_count)
            return d;
    }
    return NULL;
}

int block_read(int drive, uint32_t lba, int count, uint8_t* buf) {
    device_t* d = block_lookup(drive);
    if (d == NULL || d->ops == NULL) return -1;
    const block_ops_t* ops = (const block_ops_t*)d->ops;
    return ops->read ? ops->read(drive, lba, count, buf) : -1;
}

int block_write(int drive, uint32_t lba, int count, const uint8_t* buf) {
    device_t* d = block_lookup(drive);
    if (d == NULL || d->ops == NULL) return -1;
    const block_ops_t* ops = (const block_ops_t*)d->ops;
    return ops->write ? ops->write(drive, lba, count, buf) : -1;
}

int block_present(int drive) {
    device_t* d = block_lookup(drive);
    return d ? d->present : 0;
}
