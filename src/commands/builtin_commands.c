#include "commands/builtin_commands.h"
#include "os_api.h"
#include "shell.h"

static int char_is_space(char ch)
{
    return ch == ' ' || ch == '\t';
}

static int str_eq(const char *left, const char *right)
{
    uint32_t index = 0;

    while (left[index] && right[index]) {
        if (left[index] != right[index])
            return 0;
        index++;
    }

    return left[index] == right[index];
}

static uint32_t str_len(const char *text)
{
    uint32_t length = 0;

    while (text[length])
        length++;

    return length;
}

static int path_has_slash(const char *text)
{
    while (*text) {
        if (*text == '/')
            return 1;
        text++;
    }

    return 0;
}

static int path_has_suffix(const char *path, const char *suffix)
{
    uint32_t path_len = str_len(path);
    uint32_t suffix_len = str_len(suffix);
    uint32_t index;

    if (path_len < suffix_len)
        return 0;

    for (index = 0; index < suffix_len; index++) {
        if (path[path_len - suffix_len + index] != suffix[index])
            return 0;
    }

    return 1;
}

static void parse_first_arg(const char *args, char *buffer)
{
    uint32_t length = 0;

    while (*args && !char_is_space(*args) && length < INPUT_MAX) {
        buffer[length] = *args;
        length++;
        args++;
    }

    buffer[length] = '\0';
}

static const char *parse_next_token(const char *input, char *buffer)
{
    uint32_t length = 0;

    while (*input && char_is_space(*input))
        input++;

    while (*input && !char_is_space(*input) && length < INPUT_MAX) {
        buffer[length++] = *input;
        input++;
    }

    buffer[length] = '\0';
    return input;
}

static int cmd_ls(const char *args)
{
    char arg1[INPUT_MAX + 1];
    char arg2[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    const char *rest;
    const char *path_arg = 0;
    int show_sizes = 0;

    rest = parse_next_token(args, arg1);
    rest = parse_next_token(rest, arg2);

    if (arg1[0] == '\0') {
        os_fs_list_directory(shell_current_dir());
        return 1;
    }

    if (str_eq(arg1, "-s") || str_eq(arg1, "-sizes")) {
        show_sizes = 1;
        if (arg2[0] != '\0')
            path_arg = arg2;
    } else {
        path_arg = arg1;
    }

    if (!path_arg) {
        if (show_sizes)
            os_fs_list_directory_sizes(shell_current_dir());
        else
            os_fs_list_directory(shell_current_dir());
        return 1;
    }

    if (!os_fs_resolve_path(path_arg, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_dir_exists(fullpath)) {
        if (os_fs_file_exists_any(fullpath)) {
            os_term_puts("Not a directory: ", OS_ATTR_MUTED);
            os_term_puts(fullpath, OS_ATTR_MUTED);
        } else {
            os_term_puts("Path not found: ", OS_ATTR_MUTED);
            os_term_puts(fullpath, OS_ATTR_MUTED);
        }
        return 1;
    }

    if (show_sizes)
        os_fs_list_directory_sizes(fullpath);
    else
        os_fs_list_directory(fullpath);
    return 1;
}

static int cmd_clear(const char *args)
{
    (void)args;
    os_term_clear();
    return 1;
}

static int cmd_shutdown(const char *args)
{
    (void)args;
    os_term_puts("Shutting down...", OS_ATTR_MUTED);
    os_power_shutdown();
    return 1;
}

static int cmd_reboot(const char *args)
{
    (void)args;
    os_term_puts("Rebooting...", OS_ATTR_MUTED);
    os_power_reboot();
    return 1;
}

static int cmd_date(const char *args)
{
    char date_buffer[32];

    (void)args;
    os_time_get_date(date_buffer, sizeof(date_buffer));
    os_term_puts(date_buffer, OS_ATTR_TEXT);
    return 1;
}

static int cmd_arch(const char *args)
{
    (void)args;
    os_term_puts(os_cpu_arch(), OS_ATTR_TEXT);
    return 1;
}

static int cmd_echo(const char *args)
{
    char echo_text[INPUT_MAX + 1];
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    const char *redirect_ptr = args;
    int existed = 0;
    uint32_t text_len = 0;
    uint32_t file_len = 0;

    while (*redirect_ptr && *redirect_ptr != '>' && text_len < INPUT_MAX) {
        echo_text[text_len] = *redirect_ptr;
        text_len++;
        redirect_ptr++;
    }
    echo_text[text_len] = '\0';

    while (text_len > 0 && echo_text[text_len - 1] == ' ')
        text_len--;
    echo_text[text_len] = '\0';

    if (*redirect_ptr == '>') {
        redirect_ptr++;
        while (*redirect_ptr && char_is_space(*redirect_ptr))
            redirect_ptr++;

        while (*redirect_ptr && !char_is_space(*redirect_ptr) && file_len < INPUT_MAX) {
            filename[file_len] = *redirect_ptr;
            file_len++;
            redirect_ptr++;
        }
        filename[file_len] = '\0';

        if (file_len == 0) {
            os_term_puts("Usage: echo <text> > <filename>", OS_ATTR_MUTED);
            return 1;
        }

        if (!os_fs_resolve_path(filename, fullpath)) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return 1;
        }

        if (os_fs_dir_exists(fullpath)) {
            os_term_puts("Cannot write, path is directory: ", OS_ATTR_MUTED);
            os_term_puts(fullpath, OS_ATTR_MUTED);
            return 1;
        }

        existed = os_fs_file_exists_any(fullpath);
        if (!os_fs_write_file(fullpath, echo_text)) {
            os_term_puts("Failed to write file (storage full)", OS_ATTR_MUTED);
            return 1;
        }

        os_term_puts(existed ? "Updated: " : "Created: ", OS_ATTR_TEXT);
        os_term_puts(fullpath, OS_ATTR_TEXT);
        return 1;
    }

    if (*args)
        os_term_puts(args, OS_ATTR_TEXT);

    return 1;
}

static int cmd_mkdir(const char *args)
{
    char dirname[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, dirname);
    if (dirname[0] == '\0') {
        os_term_puts("Usage: mkdir <dirname>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(dirname, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_file_exists_any(fullpath)) {
        os_term_puts("Cannot create directory, file exists: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_add_dir(fullpath)) {
        os_term_puts("Directory already exists: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Created directory: ", OS_ATTR_TEXT);
    os_term_puts(fullpath, OS_ATTR_TEXT);
    return 1;
}

static int cmd_cd(const char *args)
{
    char dirname[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, dirname);
    if (dirname[0] == '\0') {
        shell_set_current_dir("/");
        os_term_puts("Changed to: /", OS_ATTR_TEXT);
        return 1;
    }

    if (!os_fs_resolve_path(dirname, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_dir_exists(fullpath)) {
        os_term_puts("Directory not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    shell_set_current_dir(fullpath);
    os_term_puts("Changed to: ", OS_ATTR_TEXT);
    os_term_puts(fullpath, OS_ATTR_TEXT);
    return 1;
}

static int cmd_run(const char *args)
{
    char target[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, target);
    if (target[0] == '\0') {
        os_term_puts("Usage: run <program>", OS_ATTR_MUTED);
        return 1;
    }

    if (target[0] == '/') {
        if (str_len(target) > INPUT_MAX) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return 1;
        }
        {
            uint32_t i = 0;
            while (target[i]) {
                fullpath[i] = target[i];
                i++;
            }
            fullpath[i] = '\0';
        }
    } else if (path_has_slash(target)) {
        if (!os_fs_resolve_path(target, fullpath)) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return 1;
        }
    } else {
        uint32_t i;
        const char *prefix = "/programs/";
        uint32_t prefix_len = str_len(prefix);
        uint32_t target_len = str_len(target);

        if (prefix_len + target_len > INPUT_MAX) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return 1;
        }

        for (i = 0; i < prefix_len; i++)
            fullpath[i] = prefix[i];
        for (i = 0; i < target_len; i++)
            fullpath[prefix_len + i] = target[i];
        fullpath[prefix_len + target_len] = '\0';
    }

    if (os_fs_dir_exists(fullpath)) {
        os_term_puts("Cannot run a directory: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_get_file_content(fullpath) == 0) {
        os_term_puts("Program not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (os_game_launch(fullpath))
        return 1;

    if (path_has_suffix(fullpath, ".c") || path_has_suffix(fullpath, ".cc") ||
        path_has_suffix(fullpath, ".cpp") || path_has_suffix(fullpath, ".cxx")) {
        os_term_puts("Shell error: source files are not executable. Compile first.", OS_ATTR_MUTED);
        return 1;
    }

    if (path_has_suffix(fullpath, ".asm") || path_has_suffix(fullpath, ".s")) {
        os_term_puts("Shell error: assembly source is not executable. Assemble/link first.", OS_ATTR_MUTED);
        return 1;
    }

    if (path_has_suffix(fullpath, ".bin") || path_has_suffix(fullpath, ".com") ||
        path_has_suffix(fullpath, ".exe")) {
        os_term_puts("Shell error: binary execution is not implemented yet.", OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Shell error: unsupported executable type: ", OS_ATTR_MUTED);
    os_term_puts(fullpath, OS_ATTR_MUTED);
    return 1;
}

static int cmd_read(const char *args)
{
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    const char *content;

    parse_first_arg(args, filename);
    if (filename[0] == '\0') {
        os_term_puts("Usage: read <filename>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(filename, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    content = os_fs_get_file_content(fullpath);
    if (content == 0) {
        os_term_puts("File not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts(content, OS_ATTR_TEXT);
    return 1;
}

static int cmd_touch(const char *args)
{
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, filename);
    if (filename[0] == '\0') {
        os_term_puts("Usage: touch <filename>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(filename, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_exists(fullpath)) {
        os_term_puts("Cannot create file, directory exists: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_file_exists_any(fullpath)) {
        os_term_puts("File already exists: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_write_file(fullpath, "")) {
        os_term_puts("Failed to create file (storage full)", OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Created: ", OS_ATTR_TEXT);
    os_term_puts(fullpath, OS_ATTR_TEXT);
    return 1;
}

static int cmd_rmdir(const char *args)
{
    char dirname[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, dirname);
    if (dirname[0] == '\0') {
        os_term_puts("Usage: rmdir <dirname>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(dirname, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (str_eq(fullpath, "/")) {
        os_term_puts("Cannot remove root directory", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_dir_exists(fullpath)) {
        os_term_puts("Directory not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_is_builtin(fullpath)) {
        os_term_puts("Cannot remove built-in directory: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_has_entries(fullpath)) {
        os_term_puts("Directory not empty: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_remove_dir(fullpath)) {
        os_term_puts("Failed to remove directory: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Removed directory: ", OS_ATTR_TEXT);
    os_term_puts(fullpath, OS_ATTR_TEXT);
    return 1;
}

static int cmd_rmfile(const char *args)
{
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, filename);
    if (filename[0] == '\0') {
        os_term_puts("Usage: rmfile <filename>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(filename, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_exists(fullpath)) {
        os_term_puts("rmfile only removes files: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_file_exists_builtin(fullpath)) {
        os_term_puts("Cannot remove built-in file: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_remove_file(fullpath)) {
        os_term_puts("File not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Removed file: ", OS_ATTR_TEXT);
    os_term_puts(fullpath, OS_ATTR_TEXT);
    return 1;
}

static int cmd_cpfile(const char *args)
{
    char src[INPUT_MAX + 1];
    char dst[INPUT_MAX + 1];
    char src_path[INPUT_MAX + 1];
    char dst_path[INPUT_MAX + 1];
    const char *rest;

    rest = parse_next_token(args, src);
    parse_next_token(rest, dst);

    if (src[0] == '\0' || dst[0] == '\0') {
        os_term_puts("Usage: cpfile <src> <dst>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(src, src_path) || !os_fs_resolve_path(dst, dst_path)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_exists(src_path)) {
        os_term_puts("Source is a directory: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_file_exists_any(src_path)) {
        os_term_puts("Source file not found: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_exists(dst_path)) {
        os_term_puts("Destination is a directory: ", OS_ATTR_MUTED);
        os_term_puts(dst_path, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_copy_file(src_path, dst_path)) {
        os_term_puts("cpfile failed", OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Copied file", OS_ATTR_TEXT);
    return 1;
}

static int cmd_mvfile(const char *args)
{
    char src[INPUT_MAX + 1];
    char dst[INPUT_MAX + 1];
    char src_path[INPUT_MAX + 1];
    char dst_path[INPUT_MAX + 1];
    const char *rest;

    rest = parse_next_token(args, src);
    parse_next_token(rest, dst);

    if (src[0] == '\0' || dst[0] == '\0') {
        os_term_puts("Usage: mvfile <src> <dst>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(src, src_path) || !os_fs_resolve_path(dst, dst_path)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_file_exists_builtin(src_path)) {
        os_term_puts("Cannot move built-in file: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_move_file(src_path, dst_path)) {
        os_term_puts("mvfile failed", OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Moved file", OS_ATTR_TEXT);
    return 1;
}

static int cmd_cpdir(const char *args)
{
    char src[INPUT_MAX + 1];
    char dst[INPUT_MAX + 1];
    char src_path[INPUT_MAX + 1];
    char dst_path[INPUT_MAX + 1];
    const char *rest;

    rest = parse_next_token(args, src);
    parse_next_token(rest, dst);

    if (src[0] == '\0' || dst[0] == '\0') {
        os_term_puts("Usage: cpdir <src> <dst>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(src, src_path) || !os_fs_resolve_path(dst, dst_path)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_dir_exists(src_path)) {
        os_term_puts("Source directory not found: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_file_exists_any(dst_path)) {
        os_term_puts("Destination is a file: ", OS_ATTR_MUTED);
        os_term_puts(dst_path, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_copy_dir(src_path, dst_path)) {
        os_term_puts("cpdir failed", OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Copied directory", OS_ATTR_TEXT);
    return 1;
}

static int cmd_mvdir(const char *args)
{
    char src[INPUT_MAX + 1];
    char dst[INPUT_MAX + 1];
    char src_path[INPUT_MAX + 1];
    char dst_path[INPUT_MAX + 1];
    const char *rest;

    rest = parse_next_token(args, src);
    parse_next_token(rest, dst);

    if (src[0] == '\0' || dst[0] == '\0') {
        os_term_puts("Usage: mvdir <src> <dst>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(src, src_path) || !os_fs_resolve_path(dst, dst_path)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_is_builtin(src_path)) {
        os_term_puts("Cannot move built-in directory: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_move_dir(src_path, dst_path)) {
        os_term_puts("mvdir failed", OS_ATTR_MUTED);
        return 1;
    }

    os_term_puts("Moved directory", OS_ATTR_TEXT);
    return 1;
}

static const struct builtin_command command_table[] = {
    { "ls", "list files/directories", cmd_ls },
    { "clear", "clear screen", cmd_clear },
    { "echo", "print text or write to file", cmd_echo },
    { "mkdir", "create a directory", cmd_mkdir },
    { "cd", "change directory", cmd_cd },
    { "run", "run compiled program from /programs", cmd_run },
    { "arch", "show kernel/cpu architecture", cmd_arch },
    { "read", "read file contents", cmd_read },
    { "touch", "create empty file", cmd_touch },
    { "cpfile", "copy file from src to dst", cmd_cpfile },
    { "mvfile", "move file from src to dst", cmd_mvfile },
    { "cpdir", "copy directory recursively", cmd_cpdir },
    { "mvdir", "move directory recursively", cmd_mvdir },
    { "rmdir", "remove empty directory", cmd_rmdir },
    { "rmfile", "remove non-directory file", cmd_rmfile },
    { "date", "show current RTC date/time", cmd_date },
    { "shutdown", "power off the machine", cmd_shutdown },
    { "reboot", "restart the machine", cmd_reboot }
};

const struct builtin_command *builtin_commands_get(uint32_t *count)
{
    *count = (uint32_t)(sizeof(command_table) / sizeof(command_table[0]));
    return command_table;
}

int builtin_commands_execute(const char *name, const char *args)
{
    uint32_t index;
    uint32_t count;
    const struct builtin_command *commands = builtin_commands_get(&count);

    for (index = 0; index < count; index++) {
        if (str_eq(commands[index].name, name))
            return commands[index].handler(args);
    }

    return 0;
}