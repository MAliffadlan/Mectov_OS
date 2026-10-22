#ifndef INITCALL_H
#define INITCALL_H

typedef void (*initcall_fn)(void);

#define INITCALL(fn, level) \
    static initcall_fn __attribute__((used, section(".initcall." #level))) \
        _initcall_##fn = fn

void device_init_all(void);

#endif
