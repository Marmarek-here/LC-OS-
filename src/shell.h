#ifndef LCOS_SHELL_H
#define LCOS_SHELL_H

#include "types.h"

void shell_init(void);
void shell_handle_event(const struct key_event *event);

const char *shell_current_dir(void);
void shell_set_current_dir(const char *path);

uint32_t shell_input_length(void);
uint32_t shell_input_cursor(void);
char shell_input_char_at(uint32_t index);
uint32_t shell_prompt_length(void);

#endif