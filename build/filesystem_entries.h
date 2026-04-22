/* Auto-generated filesystem hierarchy - do not edit */

struct DirEntry { const char *name; const char *path; };
struct FileEntry { const char *name; const char *path; const char *dir_path; const char *content; };

static const struct DirEntry fs_directories[] = {
    {"/", "/"},
    {"addons", "/addons"},
    {"cmd", "/cmd"},
    {"commands", "/commands"},
    {"config", "/config"},
    {"devices", "/devices"},
    {"docs", "/docs"},
    {"extras", "/extras"},
    {"bin", "/extras/bin"},
    {"share", "/extras/share"},
    {"home", "/home"},
    {"mounts", "/mounts"},
    {"programs", "/programs"},
    {"runtime", "/runtime"},
    {"scratch", "/scratch"},
    {"startup", "/startup"},
    {"state", "/state"},
    {"log", "/state/log"},
    {"status", "/status"},
    {"syscmd", "/syscmd"},
    {0, 0}
};

