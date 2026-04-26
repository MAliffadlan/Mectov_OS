#ifndef TASK_H
#define TASK_H

#include "types.h"

void init_tasking();
int create_task(void (*entry)());
uint32_t schedule(uint32_t esp);

#endif
