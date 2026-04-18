#ifndef LCOS_BUILTIN_COMMANDS_H
#define LCOS_BUILTIN_COMMANDS_H

#include "types.h"

struct builtin_command {
    const char *name;
    const char *help;
    int (*handler)(const char *args);
};

const struct builtin_command *builtin_commands_get(uint32_t *count);
int builtin_commands_execute(const char *name, const char *args);

#endif