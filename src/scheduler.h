#ifndef LCOS_SCHEDULER_H
#define LCOS_SCHEDULER_H

#include "types.h"

void scheduler_init(void);
void scheduler_tick(void);
void scheduler_refresh_tasks(void);
uint32_t scheduler_ticks_get(void);
uint32_t scheduler_current_task_get(void);
uint32_t scheduler_task_count_get(void);

#endif
