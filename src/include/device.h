#ifndef DEVICE_H
#define DEVICE_H

#include "types.h"

#define MAX_DEVICES_PER_CLASS 16

typedef enum {
    DEV_CLASS_BLOCK = 0,
    DEV_CLASS_NET,
    DEV_CLASS_COUNT
} device_class_t;

typedef struct device {
    const char*    name;
    device_class_t cls;
    int            instance;
    int            present;
    int            irq;         /* -1 when poll-only */
    int            drive_base;  /* block only */
    int            drive_count; /* block only */
    const void*    ops;         /* class-specific ops */
} device_t;

void      device_register(device_t* dev);
device_t* device_find(device_class_t cls, const char* name);
device_t* device_at(device_class_t cls, int index);
int       device_count(device_class_t cls);
void      device_log_summary(void);

#endif
