#include "program_registry.h"

int program_launch_snake(const char *name);
int program_launch_tetris(const char *name);
int program_launch_pingpong(const char *name);
int program_launch_desktop(const char *name);

static const char *path_basename(const char *path)
{
    const char *name = path;
    while (*path) {
        if (*path == '/')
            name = path + 1;
        path++;
    }
    return name;
}

int programs_launch_path(const char *path)
{
    const char *name = path_basename(path);

    if (program_launch_snake(name))
        return 1;
    if (program_launch_tetris(name))
        return 1;
    if (program_launch_pingpong(name))
        return 1;
    if (program_launch_desktop(name))
        return 1;

    return 0;
}
