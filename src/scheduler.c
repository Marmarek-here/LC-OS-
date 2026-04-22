#include "scheduler.h"

#include "games.h"

static volatile uint32_t scheduler_ticks_value = 0;
static volatile uint32_t scheduler_current_task_value = 0;
static volatile uint32_t scheduler_task_count_value = 1;

void scheduler_init(void)
{
    scheduler_ticks_value = 0;
    scheduler_current_task_value = 0;
    scheduler_task_count_value = 2;
}

void scheduler_tick(void)
{
    scheduler_ticks_value++;
    if (scheduler_task_count_value > 1)
        scheduler_current_task_value =
            (scheduler_current_task_value + 1u) % scheduler_task_count_value;
}

void scheduler_refresh_tasks(void)
{
    scheduler_task_count_value = 2;
    if (games_editor_is_active())
        scheduler_task_count_value++;
    if (games_is_active())
        scheduler_task_count_value++;
    if (scheduler_current_task_value >= scheduler_task_count_value)
        scheduler_current_task_value = 0;
}

uint32_t scheduler_ticks_get(void)
{
    return scheduler_ticks_value;
}

uint32_t scheduler_current_task_get(void)
{
    return scheduler_current_task_value;
}

uint32_t scheduler_task_count_get(void)
{
    return scheduler_task_count_value;
}
