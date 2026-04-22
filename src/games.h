#ifndef LCOS_GAMES_H
#define LCOS_GAMES_H

#include "types.h"

int games_start_path(const char *path);
int games_launch_snake(void);
int games_launch_tetris(void);
int games_launch_pingpong(void);
int games_launch_desktop(void);
int games_start_editor(const char *path);
int games_is_active(void);
int games_editor_is_active(void);
void games_handle_event(const struct key_event *event);
void games_tick(uint32_t ticks);

#endif