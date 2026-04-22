#include "games.h"

static int str_eq(const char *a, const char *b)
{
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == b[i];
}

int program_launch_desktop(const char *name)
{
    if (!str_eq(name, "desktop") && !str_eq(name, "gui"))
        return 0;

    return games_launch_desktop();
}
