#include "commands/builtin_commands.h"
#include "os_api.h"
#include "shell.h"

#define COMMAND_EXIT_SUCCESS 0
#define COMMAND_EXIT_FAILURE 1

#define DOS_BATCH_FLOW_CONTINUE 1
#define DOS_BATCH_FLOW_EXIT     2
#define DOS_BATCH_FLOW_GOTO     3

/* Forward declarations for utilities used by the helpers below */
static int str_eq(const char *left, const char *right);
static char ascii_to_lower(char ch);
static void u32_to_dec_local(uint32_t value, char *buffer);
static void batch_copy_string(char *dst, const char *src);

/* --- Script execution stack (prevents any script from running itself) --- */
#define SCRIPT_STACK_MAX 8
static char script_exec_stack[SCRIPT_STACK_MAX][INPUT_MAX + 1];
static uint32_t script_stack_depth = 0;

static int script_stack_contains(const char *path)
{
    uint32_t i;
    for (i = 0; i < script_stack_depth; i++) {
        if (str_eq(script_exec_stack[i], path))
            return 1;
    }
    return 0;
}

static int script_stack_push(const char *path)
{
    uint32_t k = 0;
    if (script_stack_depth >= SCRIPT_STACK_MAX)
        return 0;
    while (path[k] && k < INPUT_MAX) {
        script_exec_stack[script_stack_depth][k] = path[k];
        k++;
    }
    script_exec_stack[script_stack_depth][k] = '\0';
    script_stack_depth++;
    return 1;
}

static void script_stack_pop(void)
{
    if (script_stack_depth > 0)
        script_stack_depth--;
}

/* Label used by GOTO — written by execute_dos_batch_segment */
static char batch_goto_label[INPUT_MAX + 1];

/* Find content position after a label line ":name" (case-insensitive). */
static int batch_find_label(const char *content, const char *label, uint32_t *out_pos)
{
    uint32_t i = 0;

    while (content[i]) {
        uint32_t s = i;

        /* Advance i to the end of this line */
        while (content[i] && content[i] != '\n')
            i++;

        /* Check if this line starts with ':label' */
        while (content[s] == ' ' || content[s] == '\t')
            s++;

        if (content[s] == ':') {
            uint32_t li = 0;
            s++;
            while (label[li] && content[s] &&
                   ascii_to_lower(content[s]) == ascii_to_lower(label[li])) {
                s++;
                li++;
            }
            if (label[li] == '\0' &&
                (content[s] == '\0' || content[s] == '\r' ||
                 content[s] == '\n' || content[s] == ' ' || content[s] == '\t')) {
                *out_pos = (content[i] == '\n') ? i + 1 : i;
                return 1;
            }
        }

        if (content[i] == '\n')
            i++;
    }

    return 0;
}

/* Check if a raw .lcbat line is a GOTO and extract the label. */
static int lcos_batch_extract_goto(const char *line, char *label_out)
{
    uint32_t s = 0;
    uint32_t i;

    while (line[s] == ' ' || line[s] == '\t')
        s++;
    if (line[s] == '@') {
        s++;
        while (line[s] == ' ' || line[s] == '\t')
            s++;
    }

    if (!(ascii_to_lower(line[s])     == 'g' &&
          ascii_to_lower(line[s + 1]) == 'o' &&
          ascii_to_lower(line[s + 2]) == 't' &&
          ascii_to_lower(line[s + 3]) == 'o'))
        return 0;

    if (line[s + 4] != ' ' && line[s + 4] != '\t' && line[s + 4] != '\0')
        return 0;

    s += 4;
    while (line[s] == ' ' || line[s] == '\t')
        s++;
    if (line[s] == ':')
        s++;  /* optional leading ':' */

    i = 0;
    while (line[s] && line[s] != ' ' && line[s] != '\t' && i < INPUT_MAX)
        label_out[i++] = line[s++];
    label_out[i] = '\0';

    return 1;
}

static int last_command_exit_code = COMMAND_EXIT_SUCCESS;

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

static char ascii_to_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (char)(ch + ('a' - 'A'));
    return ch;
}

static int str_eq_ignore_case(const char *left, const char *right)
{
    uint32_t index = 0;

    while (left[index] && right[index]) {
        if (ascii_to_lower(left[index]) != ascii_to_lower(right[index]))
            return 0;
        index++;
    }

    return left[index] == right[index];
}

static void u32_to_dec_local(uint32_t value, char *buffer)
{
    char tmp[11];
    uint32_t count = 0;
    uint32_t i;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0 && count < 10) {
        tmp[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    for (i = 0; i < count; i++)
        buffer[i] = tmp[count - i - 1];
    buffer[count] = '\0';
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

static int path_has_suffix_ignore_case(const char *path, const char *suffix)
{
    uint32_t path_len = str_len(path);
    uint32_t suffix_len = str_len(suffix);
    uint32_t index;

    if (path_len < suffix_len)
        return 0;

    for (index = 0; index < suffix_len; index++) {
        if (ascii_to_lower(path[path_len - suffix_len + index]) != ascii_to_lower(suffix[index]))
            return 0;
    }

    return 1;
}

static const char *path_basename_local(const char *path)
{
    const char *name = path;

    while (*path) {
        if (*path == '/')
            name = path + 1;
        path++;
    }

    return name;
}

static int parse_extension_glob(const char *pattern, char *dir_path, char *ext)
{
    uint32_t i = 0;
    uint32_t last_slash = 0;

    while (pattern[i]) {
        if (pattern[i] == '/')
            last_slash = i + 1;
        i++;
    }

    if (!(pattern[last_slash] == '*' && pattern[last_slash + 1] == '.'))
        return 0;

    if (last_slash == 0) {
        batch_copy_string(dir_path, shell_current_dir());
    } else {
        char dir_arg[INPUT_MAX + 1];
        uint32_t j;

        for (j = 0; j + 1 < last_slash && j < INPUT_MAX; j++)
            dir_arg[j] = pattern[j];
        dir_arg[j] = '\0';

        if (!os_fs_resolve_path(dir_arg, dir_path))
            return -1;
    }

    batch_copy_string(ext, &pattern[last_slash + 1]);
    return 1;
}

static int path_is_lcos_batch_script(const char *path)
{
    return path_has_suffix_ignore_case(path, ".lcbat");
}

static int path_is_dos_batch_script(const char *path)
{
    return path_has_suffix_ignore_case(path, ".bat");
}

static int path_is_markdown(const char *path)
{
    return path_has_suffix_ignore_case(path, ".md") ||
           path_has_suffix_ignore_case(path, ".markdown");
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

static void print_line_with_attr(const char *text, uint8_t attr)
{
    os_term_puts(text, attr);
    os_term_newline();
}

static void print_markdown_heading(const char *text, char underline)
{
    char line[INPUT_MAX + 1];
    uint32_t i = 0;

    while (text[i] && i < INPUT_MAX) {
        line[i] = text[i];
        i++;
    }
    line[i] = '\0';

    print_line_with_attr(line, OS_ATTR_TEXT);

    for (i = 0; line[i] && i < INPUT_MAX; i++)
        line[i] = underline;
    line[i] = '\0';

    if (i > 0)
        print_line_with_attr(line, OS_ATTR_MUTED);
}

static void render_markdown_content(const char *content)
{
    char line[INPUT_MAX + 1];
    uint32_t line_len = 0;
    uint32_t index = 0;
    int in_code_block = 0;

    while (1) {
        char ch = content[index];

        if (ch == '\r') {
            index++;
            continue;
        }

        if (ch == '\n' || ch == '\0') {
            uint32_t start = 0;
            line[line_len] = '\0';

            while (line[start] == ' ' || line[start] == '\t')
                start++;

            if (line[start] == '\0') {
                os_term_newline();
            } else if (line[start] == '`' && line[start + 1] == '`' && line[start + 2] == '`') {
                in_code_block = !in_code_block;
                os_term_puts(in_code_block ? "[code]" : "[/code]", OS_ATTR_MUTED);
                os_term_newline();
            } else if (in_code_block) {
                print_line_with_attr(&line[start], OS_ATTR_TEXT);
            } else if (line[start] == '#' && line[start + 1] == ' ') {
                print_markdown_heading(&line[start + 2], '=');
            } else if (line[start] == '#' && line[start + 1] == '#' && line[start + 2] == ' ') {
                print_markdown_heading(&line[start + 3], '-');
            } else if (line[start] == '#' && line[start + 1] == '#' && line[start + 2] == '#' && line[start + 3] == ' ') {
                os_term_puts("> ", OS_ATTR_MUTED);
                print_line_with_attr(&line[start + 4], OS_ATTR_TEXT);
            } else if ((line[start] == '-' || line[start] == '*') && line[start + 1] == ' ') {
                os_term_puts(" * ", OS_ATTR_MUTED);
                print_line_with_attr(&line[start + 2], OS_ATTR_TEXT);
            } else if (line[start] == '>' && line[start + 1] == ' ') {
                os_term_puts("| ", OS_ATTR_MUTED);
                print_line_with_attr(&line[start + 2], OS_ATTR_MUTED);
            } else {
                print_line_with_attr(&line[start], OS_ATTR_TEXT);
            }

            line_len = 0;
            if (ch == '\0')
                break;
            index++;
            continue;
        }

        if (line_len < INPUT_MAX)
            line[line_len++] = ch;

        index++;
    }
}

static int batch_line_is_comment(const char *line)
{
    if (line[0] == '#' || line[0] == ';')
        return 1;
    if (line[0] == ':' && line[1] == ':')
        return 1;
    if (ascii_to_lower(line[0]) == 'r' &&
        ascii_to_lower(line[1]) == 'e' &&
        ascii_to_lower(line[2]) == 'm' &&
        (line[3] == '\0' || char_is_space(line[3])))
        return 1;
    return 0;
}

static int execute_batch_line(const char *line)
{
    uint32_t start = 0;

    while (line[start] == ' ' || line[start] == '\t')
        start++;

    if (line[start] == '@') {
        start++;
        while (line[start] == ' ' || line[start] == '\t')
            start++;
    }

    if (line[start] == '\0' || batch_line_is_comment(&line[start]))
        return 1;

    shell_run_line(&line[start]);
    return 1;
}

static void batch_copy_string(char *dst, const char *src)
{
    uint32_t index = 0;

    while (src[index] && index < INPUT_MAX) {
        dst[index] = src[index];
        index++;
    }

    dst[index] = '\0';
}

static void batch_trim_leading(const char *src, char *dst)
{
    while (*src == ' ' || *src == '\t')
        src++;
    batch_copy_string(dst, src);
}

static void batch_trim_copy(const char *src, char *dst)
{
    uint32_t start = 0;
    uint32_t end = str_len(src);
    uint32_t index = 0;

    while (src[start] == ' ' || src[start] == '\t')
        start++;

    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t'))
        end--;

    while (start < end && index < INPUT_MAX)
        dst[index++] = src[start++];

    dst[index] = '\0';
}

static void command_exit_code_set(int exit_code)
{
    last_command_exit_code = exit_code;
}

static int command_exit_code_get(void)
{
    return last_command_exit_code;
}

static int command_succeeded(void)
{
    return command_exit_code_get() == COMMAND_EXIT_SUCCESS;
}

#define DOS_BATCH_MAX_VARS 16

struct dos_batch_var {
    char name[INPUT_MAX + 1];
    char value[INPUT_MAX + 1];
};

static struct dos_batch_var dos_batch_vars[DOS_BATCH_MAX_VARS];
static uint32_t dos_batch_var_count = 0;

static int dos_batch_find_var(const char *name)
{
    uint32_t index;

    for (index = 0; index < dos_batch_var_count; index++) {
        if (str_eq_ignore_case(dos_batch_vars[index].name, name))
            return (int)index;
    }

    return -1;
}

static void dos_batch_print_vars(void)
{
    uint32_t index;

    for (index = 0; index < dos_batch_var_count; index++) {
        os_term_puts(dos_batch_vars[index].name, OS_ATTR_TEXT);
        os_term_puts("=", OS_ATTR_TEXT);
        os_term_puts(dos_batch_vars[index].value, OS_ATTR_TEXT);
        os_term_newline();
    }
}

static const char *dos_batch_get_var(const char *name)
{
    static char dynamic_buffer[32];
    int index = dos_batch_find_var(name);

    if (str_eq_ignore_case(name, "cd"))
        return shell_current_dir();

    if (str_eq_ignore_case(name, "errorlevel")) {
        u32_to_dec_local((uint32_t)command_exit_code_get(), dynamic_buffer);
        return dynamic_buffer;
    }

    if (str_eq_ignore_case(name, "date")) {
        os_time_get_date(dynamic_buffer, sizeof(dynamic_buffer));
        return dynamic_buffer;
    }

    if (index < 0)
        return "";

    return dos_batch_vars[index].value;
}

static int dos_batch_set_var(const char *name, const char *value)
{
    int index;

    if (name[0] == '\0')
        return 0;

    index = dos_batch_find_var(name);
    if (value[0] == '\0') {
        uint32_t move_index;

        if (index < 0)
            return 1;

        for (move_index = (uint32_t)index; move_index + 1 < dos_batch_var_count; move_index++)
            dos_batch_vars[move_index] = dos_batch_vars[move_index + 1];
        dos_batch_var_count--;
        return 1;
    }

    if (index < 0) {
        if (dos_batch_var_count >= DOS_BATCH_MAX_VARS)
            return 0;
        index = (int)dos_batch_var_count;
        dos_batch_var_count++;
    }

    batch_copy_string(dos_batch_vars[index].name, name);
    batch_copy_string(dos_batch_vars[index].value, value);
    return 1;
}

static void dos_batch_expand_vars(const char *src, char *dst)
{
    uint32_t src_index = 0;
    uint32_t dst_index = 0;

    while (src[src_index] && dst_index < INPUT_MAX) {
        if (src[src_index] == '%') {
            char var_name[INPUT_MAX + 1];
            uint32_t name_len = 0;
            uint32_t lookahead = src_index + 1;

            while (src[lookahead] && src[lookahead] != '%' && name_len < INPUT_MAX) {
                var_name[name_len++] = src[lookahead];
                lookahead++;
            }

            if (src[lookahead] == '%') {
                const char *value;
                uint32_t value_index = 0;

                var_name[name_len] = '\0';
                value = dos_batch_get_var(var_name);
                while (value[value_index] && dst_index < INPUT_MAX)
                    dst[dst_index++] = value[value_index++];
                src_index = lookahead + 1;
                continue;
            }
        }

        dst[dst_index++] = src[src_index++];
    }

    dst[dst_index] = '\0';
}

static int dos_batch_handle_set(const char *args)
{
    char trimmed[INPUT_MAX + 1];
    char name[INPUT_MAX + 1];
    char value[INPUT_MAX + 1];
    uint32_t split = 0;
    uint32_t index = 0;

    batch_trim_leading(args, trimmed);
    if (trimmed[0] == '\0') {
        dos_batch_print_vars();
        return 1;
    }

    while (trimmed[split] && trimmed[split] != '=' && split < INPUT_MAX)
        split++;

    if (trimmed[split] != '=') {
        os_term_puts("BAT error: SET requires NAME=VALUE", OS_ATTR_MUTED);
        return 1;
    }

    while (index < split && index < INPUT_MAX) {
        name[index] = trimmed[index];
        index++;
    }
    name[index] = '\0';

    batch_copy_string(value, &trimmed[split + 1]);

    while (index > 0 && (name[index - 1] == ' ' || name[index - 1] == '\t')) {
        index--;
        name[index] = '\0';
    }

    if (!dos_batch_set_var(name, value))
        os_term_puts("BAT error: variable storage full", OS_ATTR_MUTED);

    command_exit_code_set(COMMAND_EXIT_SUCCESS);

    return 1;
}

static int execute_dos_batch_segment(const char *line)
{
    char command[INPUT_MAX + 1];
    char args[INPUT_MAX + 1];
    char mapped_args[INPUT_MAX + 1];
    char token[INPUT_MAX + 1];
    char nested_command[INPUT_MAX + 1];
    uint32_t start = 0;
    uint32_t command_len;
    uint32_t args_start;

    while (line[start] == ' ' || line[start] == '\t')
        start++;

    if (line[start] == '@') {
        start++;
        while (line[start] == ' ' || line[start] == '\t')
            start++;
    }

    if (line[start] == '\0' || batch_line_is_comment(&line[start])) {
        command_exit_code_set(COMMAND_EXIT_SUCCESS);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    parse_first_arg(&line[start], command);
    if (command[0] == '\0')
        return DOS_BATCH_FLOW_CONTINUE;

    command_len = str_len(command);
    args_start = start + command_len;
    while (line[args_start] == ' ' || line[args_start] == '\t')
        args_start++;
    batch_copy_string(args, &line[args_start]);

    if (str_eq_ignore_case(command, "rem")) {
        command_exit_code_set(COMMAND_EXIT_SUCCESS);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "if")) {
        const char *rest = parse_next_token(args, token);
        int invert = 0;
        int condition_met = 0;

        if (str_eq_ignore_case(token, "not")) {
            invert = 1;
            rest = parse_next_token(rest, token);
        }

        if (str_eq_ignore_case(token, "exist")) {
            char path_token[INPUT_MAX + 1];
            char fullpath[INPUT_MAX + 1];

            rest = parse_next_token(rest, path_token);
            batch_trim_leading(rest, nested_command);

            if (path_token[0] == '\0' || nested_command[0] == '\0') {
                os_term_puts("BAT error: IF EXIST requires a path and command", OS_ATTR_MUTED);
                command_exit_code_set(COMMAND_EXIT_FAILURE);
                return DOS_BATCH_FLOW_CONTINUE;
            }

            if (!os_fs_resolve_path(path_token, fullpath)) {
                os_term_puts("BAT error: IF EXIST path too long", OS_ATTR_MUTED);
                command_exit_code_set(COMMAND_EXIT_FAILURE);
                return DOS_BATCH_FLOW_CONTINUE;
            }

            condition_met = os_fs_dir_exists(fullpath) || os_fs_file_exists_any(fullpath);
        } else if (str_eq_ignore_case(token, "errorlevel")) {
            uint32_t level = 0;
            uint32_t index = 0;

            rest = parse_next_token(rest, token);
            batch_trim_leading(rest, nested_command);

            if (token[0] == '\0' || nested_command[0] == '\0') {
                os_term_puts("BAT error: IF ERRORLEVEL requires a value and command", OS_ATTR_MUTED);
                command_exit_code_set(COMMAND_EXIT_FAILURE);
                return DOS_BATCH_FLOW_CONTINUE;
            }

            while (token[index] >= '0' && token[index] <= '9') {
                level = level * 10u + (uint32_t)(token[index] - '0');
                index++;
            }

            if (token[index] != '\0') {
                os_term_puts("BAT error: IF ERRORLEVEL expects a number", OS_ATTR_MUTED);
                command_exit_code_set(COMMAND_EXIT_FAILURE);
                return DOS_BATCH_FLOW_CONTINUE;
            }

            condition_met = (uint32_t)command_exit_code_get() >= level;
        } else {
            char right_value[INPUT_MAX + 1];

            rest = parse_next_token(rest, right_value);
            batch_trim_leading(rest, nested_command);

            if (token[0] == '\0' || right_value[0] == '\0' || nested_command[0] == '\0') {
                os_term_puts("BAT error: IF requires a simple condition and command", OS_ATTR_MUTED);
                command_exit_code_set(COMMAND_EXIT_FAILURE);
                return DOS_BATCH_FLOW_CONTINUE;
            }

            condition_met = str_eq_ignore_case(token, right_value);
        }

        if (invert)
            condition_met = !condition_met;

        if (!condition_met) {
            command_exit_code_set(COMMAND_EXIT_SUCCESS);
            return DOS_BATCH_FLOW_CONTINUE;
        }

        return execute_dos_batch_segment(nested_command);
    }

    if (str_eq_ignore_case(command, "set"))
        return dos_batch_handle_set(args), DOS_BATCH_FLOW_CONTINUE;

    if (str_eq_ignore_case(command, "echo")) {
        if (str_eq_ignore_case(args, "off") || str_eq_ignore_case(args, "on")) {
            command_exit_code_set(COMMAND_EXIT_SUCCESS);
            return DOS_BATCH_FLOW_CONTINUE;
        }
        builtin_commands_execute("echo", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "dir")) {
        builtin_commands_execute("ls", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "type")) {
        builtin_commands_execute("read", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "cls")) {
        builtin_commands_execute("clear", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "ver")) {
        builtin_commands_execute("version", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "md") || str_eq_ignore_case(command, "mkdir")) {
        builtin_commands_execute("mkdir", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "rd") || str_eq_ignore_case(command, "rmdir")) {
        builtin_commands_execute("rmdir", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "cd") || str_eq_ignore_case(command, "chdir")) {
        builtin_commands_execute("cd", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "copy")) {
        builtin_commands_execute("cpfile", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "move")) {
        builtin_commands_execute("mvfile", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "del") || str_eq_ignore_case(command, "erase")) {
        builtin_commands_execute("rmfile", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "date")) {
        builtin_commands_execute("date", args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "call")) {
        batch_trim_leading(args, mapped_args);
        if (mapped_args[0] == '\0') {
            os_term_puts("BAT error: CALL requires a target", OS_ATTR_MUTED);
            command_exit_code_set(COMMAND_EXIT_FAILURE);
            return DOS_BATCH_FLOW_CONTINUE;
        }
        builtin_commands_execute("run", mapped_args);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "pause")) {
        os_term_puts("Press any key to continue . . .", OS_ATTR_MUTED);
        os_wait_for_keypress();
        command_exit_code_set(COMMAND_EXIT_SUCCESS);
        return DOS_BATCH_FLOW_CONTINUE;
    }

    if (str_eq_ignore_case(command, "exit")) {
        batch_trim_leading(args, mapped_args);
        if (str_eq_ignore_case(mapped_args, "1") || str_eq_ignore_case(mapped_args, "/b 1"))
            command_exit_code_set(COMMAND_EXIT_FAILURE);
        else
            command_exit_code_set(COMMAND_EXIT_SUCCESS);
        return DOS_BATCH_FLOW_EXIT;
    }

    if (str_eq_ignore_case(command, "goto")) {
        char label[INPUT_MAX + 1];
        uint32_t li = 0;
        batch_trim_leading(args, label);
        /* Strip optional leading ':' */
        if (label[0] == ':') {
            while (label[li + 1]) {
                label[li] = label[li + 1];
                li++;
            }
            label[li] = '\0';
        }
        if (label[0] == '\0') {
            os_term_puts("BAT error: GOTO requires a label", OS_ATTR_MUTED);
            command_exit_code_set(COMMAND_EXIT_FAILURE);
            return DOS_BATCH_FLOW_CONTINUE;
        }
        batch_copy_string(batch_goto_label, label);
        return DOS_BATCH_FLOW_GOTO;
    }

    os_term_puts("BAT error: unsupported DOS command: ", OS_ATTR_MUTED);
    os_term_puts(command, OS_ATTR_MUTED);
    command_exit_code_set(COMMAND_EXIT_FAILURE);
    return DOS_BATCH_FLOW_CONTINUE;
}

static int execute_dos_batch_line(const char *line)
{
    char expanded_line[INPUT_MAX + 1];
    uint32_t segment_start = 0;

    dos_batch_expand_vars(line, expanded_line);

    {
        uint32_t pipe_scan = 0;
        while (expanded_line[pipe_scan]) {
            if (expanded_line[pipe_scan] == '|') {
                shell_run_line(expanded_line);
                return DOS_BATCH_FLOW_CONTINUE;
            }
            pipe_scan++;
        }
    }

    while (1) {
        char segment[INPUT_MAX + 1];
        uint32_t segment_len = 0;
        uint32_t cursor = segment_start;
        int has_and_and = 0;
        int flow;

        while (expanded_line[cursor]) {
            if (expanded_line[cursor] == '&' && expanded_line[cursor + 1] == '&') {
                has_and_and = 1;
                break;
            }

            if (segment_len < INPUT_MAX)
                segment[segment_len++] = expanded_line[cursor];
            cursor++;
        }

        segment[segment_len] = '\0';
        batch_trim_copy(segment, segment);

        if (segment[0] != '\0') {
            flow = execute_dos_batch_segment(segment);
            if (flow == DOS_BATCH_FLOW_EXIT || flow == DOS_BATCH_FLOW_GOTO)
                return flow;
            if (has_and_and && !command_succeeded())
                return DOS_BATCH_FLOW_CONTINUE;
        }

        if (!has_and_and)
            break;

        segment_start = cursor + 2;
    }

    return DOS_BATCH_FLOW_CONTINUE;
}

static int cmd_ls(const char *args)
{
    char token[INPUT_MAX + 1];
    char path_buf[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    const char *rest = args;
    int has_path = 0;
    int show_sizes = 0;
    int show_rights = 0;

    path_buf[0] = '\0';

    /* Parse each whitespace-separated token; flags start with '-' */
    while (*rest) {
        rest = parse_next_token(rest, token);
        if (token[0] == '\0')
            break;

        if (token[0] == '-') {
            /* Long forms: -sizes, -rights */
            if (str_eq(token, "-sizes")) {
                show_sizes = 1;
            } else if (str_eq(token, "-rights")) {
                show_rights = 1;
            } else {
                /* Walk each character after '-': -rs, -sr, -s, -r, -rsa, etc. */
                uint32_t fi = 1;
                while (token[fi]) {
                    if (token[fi] == 's')
                        show_sizes = 1;
                    else if (token[fi] == 'r')
                        show_rights = 1;
                    fi++;
                }
            }
        } else {
            /* First non-flag token is the path */
            if (!has_path) {
                uint32_t ci = 0;
                while (token[ci]) {
                    path_buf[ci] = token[ci];
                    ci++;
                }
                path_buf[ci] = '\0';
                has_path = 1;
            }
        }
    }

    if (has_path) {
        char dir_path[INPUT_MAX + 1];
        char ext[INPUT_MAX + 1];
        int glob = parse_extension_glob(path_buf, dir_path, ext);

        if (glob < 0) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
        }

        if (glob > 0) {
            char matches[4096];
            uint32_t i = 0;
            int printed = 0;

            if (!os_fs_dir_exists(dir_path)) {
                os_term_puts("Directory not found: ", OS_ATTR_MUTED);
                os_term_puts(dir_path, OS_ATTR_MUTED);
                return -1;
            }

            if (!os_fs_collect_extension_matches(dir_path, ext, matches, sizeof(matches))) {
                os_term_puts("No files match extension: ", OS_ATTR_MUTED);
                os_term_puts(ext, OS_ATTR_MUTED);
                return -1;
            }

            while (matches[i]) {
                char match_path[INPUT_MAX + 1];
                uint32_t mlen = 0;
                const char *base;
                const char *mcontent;

                while (matches[i] && matches[i] != '\n' && mlen < INPUT_MAX)
                    match_path[mlen++] = matches[i++];
                match_path[mlen] = '\0';
                if (matches[i] == '\n')
                    i++;

                base = path_basename_local(match_path);
                mcontent = os_fs_get_file_content(match_path);
                os_term_puts(base, OS_ATTR_TEXT);
                if (show_sizes && mcontent) {
                    char size_buf[11];
                    u32_to_dec_local((uint32_t)str_len(mcontent), size_buf);
                    os_term_puts(" (", OS_ATTR_MUTED);
                    os_term_puts(size_buf, OS_ATTR_MUTED);
                    os_term_puts(" B)", OS_ATTR_MUTED);
                }
                if (show_rights) {
                    const char *perm = os_fs_file_exists_builtin(match_path) ? "  [-r--]" : "  [-rw-]";
                    os_term_puts(perm, OS_ATTR_MUTED);
                }
                os_term_newline();
                printed = 1;
            }

            return printed ? 1 : -1;
        }

        if (!os_fs_resolve_path(path_buf, fullpath)) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
        }
        if (!os_fs_dir_exists(fullpath)) {
            if (os_fs_file_exists_any(fullpath)) {
                os_term_puts("Not a directory: ", OS_ATTR_MUTED);
                os_term_puts(fullpath, OS_ATTR_MUTED);
            } else {
                os_term_puts("Path not found: ", OS_ATTR_MUTED);
                os_term_puts(fullpath, OS_ATTR_MUTED);
            }
            return -1;
        }
        os_fs_list_directory_ex(fullpath, show_sizes, show_rights);
    } else {
        os_fs_list_directory_ex(shell_current_dir(), show_sizes, show_rights);
    }

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

static int cmd_version(const char *args)
{
    (void)args;
    os_term_puts("LC(OS) version 0.1", OS_ATTR_TEXT);
    return 1;
}

static int cmd_storagetest(const char *args)
{
    int ata_legacy;
    int ahci;
    int usb_uhci;
    int usb_ohci;
    int usb_ehci;
    int usb_xhci;

    (void)args;
    os_storage_probe(&ata_legacy, &ahci, &usb_uhci, &usb_ohci, &usb_ehci, &usb_xhci);

    os_term_puts("Storage/controller probe (temporary):", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts(ata_legacy ? "ATA/IDE: detected" : "ATA/IDE: not detected", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts(ahci ? "AHCI SATA: detected" : "AHCI SATA: not detected", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts(usb_uhci ? "USB UHCI: detected" : "USB UHCI: not detected", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts(usb_ohci ? "USB OHCI: detected" : "USB OHCI: not detected", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts(usb_ehci ? "USB EHCI: detected" : "USB EHCI: not detected", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts(usb_xhci ? "USB xHCI: detected" : "USB xHCI: not detected", OS_ATTR_TEXT);
    return 1;
}

static int cmd_echo(const char *args)
{
    char echo_text[INPUT_MAX + 1];
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    uint32_t text_len = 0;
    uint32_t file_len = 0;
    const char *p = args;
    const char *pipe_input = os_pipe_input_get();
    int in_quotes = 0;
    int existed = 0;

    /* Skip leading whitespace */
    while (*p && char_is_space(*p))
        p++;

    /* Optional opening quote */
    if (*p == '"') {
        in_quotes = 1;
        p++;
    }

    /* Collect text up to closing quote (if quoted) or up to '>' (if unquoted) */
    while (*p && text_len < INPUT_MAX) {
        if (in_quotes) {
            if (*p == '"') {
                p++;
                break;  /* end of quoted string */
            }
        } else {
            if (*p == '>')
                break;
        }

        /* Process escape sequences */
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case 'n':  echo_text[text_len++] = '\n'; break;
            case 't':  echo_text[text_len++] = '\t'; break;
            case '\\': echo_text[text_len++] = '\\'; break;
            case '"':  echo_text[text_len++] = '"';  break;
            default:
                /* Keep the backslash and the character */
                if (text_len < INPUT_MAX) echo_text[text_len++] = '\\';
                if (text_len < INPUT_MAX) echo_text[text_len++] = *p;
                break;
            }
            p++;
            continue;
        }

        echo_text[text_len++] = *p++;
    }
    echo_text[text_len] = '\0';

    if (text_len == 0 && pipe_input[0] != '\0') {
        while (pipe_input[text_len] && text_len < INPUT_MAX) {
            echo_text[text_len] = pipe_input[text_len];
            text_len++;
        }
        echo_text[text_len] = '\0';
    }

    /* Trim trailing spaces only when not quoted */
    if (!in_quotes) {
        while (text_len > 0 && echo_text[text_len - 1] == ' ')
            text_len--;
        echo_text[text_len] = '\0';
    }

    /* Skip whitespace before optional '>' */
    while (*p && char_is_space(*p))
        p++;

    if (*p == '>') {
        p++;
        while (*p && char_is_space(*p))
            p++;

        while (*p && !char_is_space(*p) && file_len < INPUT_MAX) {
            filename[file_len++] = *p++;
        }
        filename[file_len] = '\0';

        if (file_len == 0) {
            os_term_puts("Usage: echo <text> > <filename>", OS_ATTR_MUTED);
            return -1;
        }

        if (!os_fs_resolve_path(filename, fullpath)) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
        }

        if (os_fs_dir_exists(fullpath)) {
            os_term_puts("Cannot write, path is directory: ", OS_ATTR_MUTED);
            os_term_puts(fullpath, OS_ATTR_MUTED);
            return -1;
        }

        existed = os_fs_file_exists_any(fullpath);
        if (!os_fs_write_file(fullpath, echo_text)) {
            os_term_puts("Failed to write file (storage full)", OS_ATTR_MUTED);
            return -1;
        }

        os_term_puts(existed ? "Updated: " : "Created: ", OS_ATTR_TEXT);
        os_term_puts(fullpath, OS_ATTR_TEXT);
        return 1;
    }

    /* Print to terminal — honour \n by splitting on it */
    if (echo_text[0]) {
        uint32_t i = 0;
        char line[INPUT_MAX + 1];
        uint32_t llen = 0;

        while (i <= text_len) {
            if (echo_text[i] == '\n' || echo_text[i] == '\0') {
                line[llen] = '\0';
                os_term_puts(line, OS_ATTR_TEXT);
                os_term_newline();
                llen = 0;
            } else {
                if (llen < INPUT_MAX)
                    line[llen++] = echo_text[i];
            }
            i++;
        }
    }

    return 1;
}

static int cmd_mkdir(const char *args)
{
    char dirname[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, dirname);
    if (dirname[0] == '\0') {
        os_term_puts("Usage: mkdir <dirname>", OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_resolve_path(dirname, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_path_name_is_valid(fullpath)) {
        os_term_puts("Invalid directory name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_file_exists_any(fullpath)) {
        os_term_puts("Cannot create directory, file exists: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_add_dir(fullpath)) {
        os_term_puts("Directory already exists: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
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
        return -1;
    }

    if (!os_fs_dir_exists(fullpath)) {
        os_term_puts("Directory not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    shell_set_current_dir(fullpath);
    os_term_puts("Changed to: ", OS_ATTR_TEXT);
    os_term_puts(fullpath, OS_ATTR_TEXT);
    return 1;
}

static int run_lcos_batch_script(const char *fullpath)
{
    const char *script_content;
    char line[INPUT_MAX + 1];
    uint32_t line_len = 0;
    uint32_t i = 0;
    static int nesting_depth = 0;

    if (nesting_depth >= 4) {
        os_term_puts("Script nesting limit exceeded, max 4 levels", OS_ATTR_MUTED);
        return 1;
    }

    script_content = os_fs_get_file_content(fullpath);
    if (!script_content) {
        os_term_puts("Script not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    nesting_depth++;

    while (script_content[i]) {
        char ch = script_content[i];

        if (ch == '\n') {
            char goto_lbl[INPUT_MAX + 1];
            line[line_len] = '\0';

            if (os_check_ctrl_c()) {
                os_term_puts("^C", OS_ATTR_MUTED);
                os_term_newline();
                break;
            }

            if (lcos_batch_extract_goto(line, goto_lbl) && goto_lbl[0] != '\0') {
                uint32_t label_pos;
                if (batch_find_label(script_content, goto_lbl, &label_pos)) {
                    i = label_pos;
                    line_len = 0;
                    continue;
                }
                os_term_puts("Script error: label not found: ", OS_ATTR_MUTED);
                os_term_puts(goto_lbl, OS_ATTR_MUTED);
                break;
            }

            execute_batch_line(line);

            line_len = 0;
            i++;
            continue;
        }

        if (ch == '\r') {
            i++;
            continue;
        }

        if (line_len < INPUT_MAX)
            line[line_len++] = ch;

        i++;
    }

    if (line_len > 0) {
        char goto_lbl[INPUT_MAX + 1];
        line[line_len] = '\0';
        if (!lcos_batch_extract_goto(line, goto_lbl) || goto_lbl[0] == '\0')
            execute_batch_line(line);
    }

    nesting_depth--;
    return 1;
}

static int run_dos_batch_script(const char *fullpath)
{
    const char *script_content;
    char line[INPUT_MAX + 1];
    uint32_t line_len = 0;
    uint32_t i = 0;
    static int nesting_depth = 0;

    if (nesting_depth >= 8) {
        os_term_puts("BAT nesting limit exceeded, max 8 levels", OS_ATTR_MUTED);
        command_exit_code_set(COMMAND_EXIT_FAILURE);
        return -1;
    }

    script_content = os_fs_get_file_content(fullpath);
    if (!script_content) {
        os_term_puts("Script not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        command_exit_code_set(COMMAND_EXIT_FAILURE);
        return -1;
    }

    nesting_depth++;

    while (script_content[i]) {
        char ch = script_content[i];

        if (ch == '\n') {
            int flow;
            line[line_len] = '\0';

            if (os_check_ctrl_c()) {
                os_term_puts("^C", OS_ATTR_MUTED);
                os_term_newline();
                break;
            }

            flow = execute_dos_batch_line(line);
            if (flow == DOS_BATCH_FLOW_EXIT)
                break;
            if (flow == DOS_BATCH_FLOW_GOTO) {
                uint32_t label_pos;
                if (batch_find_label(script_content, batch_goto_label, &label_pos)) {
                    i = label_pos;
                    line_len = 0;
                    continue;
                }
                os_term_puts("BAT error: label not found: ", OS_ATTR_MUTED);
                os_term_puts(batch_goto_label, OS_ATTR_MUTED);
                command_exit_code_set(COMMAND_EXIT_FAILURE);
                break;
            }

            line_len = 0;
            i++;
            continue;
        }

        if (ch == '\r') {
            i++;
            continue;
        }

        if (line_len < INPUT_MAX)
            line[line_len++] = ch;

        i++;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        execute_dos_batch_line(line);
    }

    nesting_depth--;
    return command_succeeded() ? 1 : -1;
}

static int cmd_run(const char *args)
{
    char target[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    const char *pipe_input = os_pipe_input_get();

    parse_first_arg(args, target);
    if (target[0] == '\0' && pipe_input[0] != '\0')
        parse_first_arg(pipe_input, target);
    if (target[0] == '\0') {
        os_term_puts("Usage: run <program>", OS_ATTR_MUTED);
        return -1;
    }

    if (target[0] == '/') {
        if (str_len(target) > INPUT_MAX) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
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
            return -1;
        }
    } else {
        uint32_t i;
        const char *prefix = "/programs/";
        uint32_t prefix_len = str_len(prefix);
        uint32_t target_len = str_len(target);

        if (prefix_len + target_len > INPUT_MAX) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
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
        return -1;
    }

    if (os_fs_get_file_content(fullpath) == 0) {
        os_term_puts("Program not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (path_is_lcos_batch_script(fullpath)) {
        int result;
        if (script_stack_contains(fullpath)) {
            os_term_puts("Script error: self-execution blocked: ", OS_ATTR_MUTED);
            os_term_puts(fullpath, OS_ATTR_MUTED);
            return -1;
        }
        script_stack_push(fullpath);
        result = run_lcos_batch_script(fullpath);
        script_stack_pop();
        return result;
    }

    if (path_is_dos_batch_script(fullpath)) {
        int result;
        if (script_stack_contains(fullpath)) {
            os_term_puts("BAT error: self-execution blocked: ", OS_ATTR_MUTED);
            os_term_puts(fullpath, OS_ATTR_MUTED);
            return -1;
        }
        script_stack_push(fullpath);
        result = run_dos_batch_script(fullpath);
        script_stack_pop();
        return result;
    }

    if (os_game_launch(fullpath))
        return 1;

    if (path_has_suffix(fullpath, ".c") || path_has_suffix(fullpath, ".cc") ||
        path_has_suffix(fullpath, ".cpp") || path_has_suffix(fullpath, ".cxx")) {
        os_term_puts("Shell error: source files are not executable. Compile first.", OS_ATTR_MUTED);
        return -1;
    }

    if (path_has_suffix(fullpath, ".asm") || path_has_suffix(fullpath, ".s")) {
        os_term_puts("Shell error: assembly source is not executable. Assemble/link first.", OS_ATTR_MUTED);
        return -1;
    }

    if (path_has_suffix(fullpath, ".bin") || path_has_suffix(fullpath, ".com") ||
        path_has_suffix(fullpath, ".exe")) {
        os_term_puts("Shell error: binary execution is not implemented yet.", OS_ATTR_MUTED);
        return -1;
    }

    os_term_puts("Shell error: unsupported executable type: ", OS_ATTR_MUTED);
    os_term_puts(fullpath, OS_ATTR_MUTED);
    return -1;
}

static int cmd_read(const char *args)
{
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];
    const char *content;

    parse_first_arg(args, filename);
    if (filename[0] == '\0') {
        os_term_puts("Usage: read <filename>", OS_ATTR_MUTED);
        return -1;
    }

    {
        char dir_path[INPUT_MAX + 1];
        char ext[INPUT_MAX + 1];
        int glob = parse_extension_glob(filename, dir_path, ext);

        if (glob < 0) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
        }

        if (glob > 0) {
            char matches[4096];
            uint32_t i = 0;
            int printed = 0;

            if (!os_fs_dir_exists(dir_path)) {
                os_term_puts("Directory not found: ", OS_ATTR_MUTED);
                os_term_puts(dir_path, OS_ATTR_MUTED);
                return -1;
            }

            if (!os_fs_collect_extension_matches(dir_path, ext, matches, sizeof(matches))) {
                os_term_puts("No files match extension: ", OS_ATTR_MUTED);
                os_term_puts(ext, OS_ATTR_MUTED);
                return -1;
            }

            while (matches[i]) {
                char match_path[INPUT_MAX + 1];
                uint32_t mlen = 0;

                while (matches[i] && matches[i] != '\n' && mlen < INPUT_MAX)
                    match_path[mlen++] = matches[i++];
                match_path[mlen] = '\0';
                if (matches[i] == '\n')
                    i++;

                content = os_fs_get_file_content(match_path);
                if (content == 0)
                    continue;

                if (printed)
                    os_term_newline();
                os_term_puts("== ", OS_ATTR_TEXT);
                os_term_puts(path_basename_local(match_path), OS_ATTR_TEXT);
                os_term_puts(" ==", OS_ATTR_TEXT);
                os_term_newline();

                if (path_is_markdown(match_path))
                    render_markdown_content(content);
                else
                    os_term_puts(content, OS_ATTR_TEXT);
                printed = 1;
            }

            return printed ? 1 : -1;
        }
    }

    if (!os_fs_resolve_path(filename, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_path_name_is_valid(fullpath)) {
        os_term_puts("Invalid file name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
        return -1;
    }

    content = os_fs_get_file_content(fullpath);
    if (content == 0) {
        os_term_puts("File not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (path_is_markdown(fullpath)) {
        render_markdown_content(content);
    } else {
        os_term_puts(content, OS_ATTR_TEXT);
    }
    return 1;
}

static int cmd_edit(const char *args)
{
    char filename[INPUT_MAX + 1];
    char fullpath[INPUT_MAX + 1];

    parse_first_arg(args, filename);
    if (filename[0] == '\0') {
        os_term_puts("Usage: edit <file>", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_resolve_path(filename, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return 1;
    }

    if (!os_fs_path_name_is_valid(fullpath)) {
        os_term_puts("Invalid file name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
        return 1;
    }

    if (os_fs_dir_exists(fullpath)) {
        os_term_puts("Cannot edit a directory: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return 1;
    }

    if (!os_editor_open(fullpath)) {
        os_term_puts("Failed to open editor", OS_ATTR_MUTED);
        return 1;
    }

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

        if (!os_fs_path_name_is_valid(fullpath)) {
            os_term_puts("Invalid file name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
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
        return -1;
    }

    if (!os_fs_resolve_path(dirname, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return -1;
    }

    if (str_eq(fullpath, "/")) {
        os_term_puts("Cannot remove root directory", OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_dir_exists(fullpath)) {
        os_term_puts("Directory not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_dir_is_builtin(fullpath)) {
        os_term_puts("Cannot remove built-in directory: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_dir_has_entries(fullpath)) {
        os_term_puts("Directory not empty: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_remove_dir(fullpath)) {
        os_term_puts("Failed to remove directory: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
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
        return -1;
    }

    {
        char dir_path[INPUT_MAX + 1];
        char ext[INPUT_MAX + 1];
        int glob = parse_extension_glob(filename, dir_path, ext);

        if (glob < 0) {
            os_term_puts("Path too long", OS_ATTR_MUTED);
            return -1;
        }

        if (glob > 0) {
            char matches[4096];
            uint32_t i = 0;
            int removed = 0;

            if (!os_fs_dir_exists(dir_path)) {
                os_term_puts("Directory not found: ", OS_ATTR_MUTED);
                os_term_puts(dir_path, OS_ATTR_MUTED);
                return -1;
            }

            if (!os_fs_collect_extension_matches(dir_path, ext, matches, sizeof(matches))) {
                os_term_puts("No files match extension: ", OS_ATTR_MUTED);
                os_term_puts(ext, OS_ATTR_MUTED);
                return -1;
            }

            while (matches[i]) {
                char match_path[INPUT_MAX + 1];
                uint32_t mlen = 0;

                while (matches[i] && matches[i] != '\n' && mlen < INPUT_MAX)
                    match_path[mlen++] = matches[i++];
                match_path[mlen] = '\0';
                if (matches[i] == '\n')
                    i++;

                if (os_fs_file_exists_builtin(match_path))
                    continue;
                if (os_fs_remove_file(match_path)) {
                    os_term_puts("Removed file: ", OS_ATTR_TEXT);
                    os_term_puts(match_path, OS_ATTR_TEXT);
                    os_term_newline();
                    removed = 1;
                }
            }

            if (!removed) {
                os_term_puts("No removable files matched", OS_ATTR_MUTED);
                return -1;
            }

            return 1;
        }
    }

    if (!os_fs_resolve_path(filename, fullpath)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_dir_exists(fullpath)) {
        os_term_puts("rmfile only removes files: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_file_exists_builtin(fullpath)) {
        os_term_puts("Cannot remove built-in file: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_remove_file(fullpath)) {
        os_term_puts("File not found: ", OS_ATTR_MUTED);
        os_term_puts(fullpath, OS_ATTR_MUTED);
        return -1;
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
        return -1;
    }

    if (!os_fs_resolve_path(src, src_path) || !os_fs_resolve_path(dst, dst_path)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_path_name_is_valid(dst_path)) {
        os_term_puts("Invalid destination file name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_dir_exists(src_path)) {
        os_term_puts("Source is a directory: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_file_exists_any(src_path)) {
        os_term_puts("Source file not found: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_dir_exists(dst_path)) {
        os_term_puts("Destination is a directory: ", OS_ATTR_MUTED);
        os_term_puts(dst_path, OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_copy_file(src_path, dst_path)) {
        os_term_puts("cpfile failed", OS_ATTR_MUTED);
        return -1;
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
        return -1;
    }

    if (!os_fs_resolve_path(src, src_path) || !os_fs_resolve_path(dst, dst_path)) {
        os_term_puts("Path too long", OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_path_name_is_valid(dst_path)) {
        os_term_puts("Invalid destination file name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
        return -1;
    }

    if (os_fs_file_exists_builtin(src_path)) {
        os_term_puts("Cannot move built-in file: ", OS_ATTR_MUTED);
        os_term_puts(src_path, OS_ATTR_MUTED);
        return -1;
    }

    if (!os_fs_move_file(src_path, dst_path)) {
        os_term_puts("mvfile failed", OS_ATTR_MUTED);
        return -1;
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

    if (!os_fs_path_name_is_valid(dst_path)) {
        os_term_puts("Invalid destination directory name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
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

    if (!os_fs_path_name_is_valid(dst_path)) {
        os_term_puts("Invalid destination directory name. Forbidden chars: / \" ' \\ ( ) { } [ ] and spaces", OS_ATTR_MUTED);
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

static int cmd_tasks(const char *args)
{
    uint32_t i;
    char num[12];

    (void)args;
    os_term_puts("Scheduler status", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts("Ticks: ", OS_ATTR_TEXT);
    u32_to_dec_local(os_scheduler_ticks(), num);
    os_term_puts(num, OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts("Current task: ", OS_ATTR_TEXT);
    u32_to_dec_local(os_scheduler_current_task(), num);
    os_term_puts(num, OS_ATTR_TEXT);
    os_term_puts(" (", OS_ATTR_TEXT);
    os_term_puts(os_scheduler_task_name(os_scheduler_current_task()), OS_ATTR_TEXT);
    os_term_puts(")", OS_ATTR_TEXT);
    os_term_newline();
    os_term_puts("Tasks:", OS_ATTR_TEXT);
    os_term_newline();
    for (i = 0; i < os_scheduler_task_count(); i++) {
        os_term_puts(" - ", OS_ATTR_TEXT);
        os_term_puts(os_scheduler_task_name(i), OS_ATTR_TEXT);
        if (i == os_scheduler_current_task())
            os_term_puts(" [running]", OS_ATTR_MUTED);
        os_term_newline();
    }
    return 1;
}

static const struct builtin_command command_table[] = {
    { "help", "show available commands", 0 },
    { "ls", "list files/directories", cmd_ls },
    { "clear", "clear screen", cmd_clear },
    { "echo", "print text or write to file", cmd_echo },
    { "mkdir", "create a directory", cmd_mkdir },
    { "cd", "change directory", cmd_cd },
    { "run", "run program or batch script", cmd_run },
    { "arch", "show kernel/cpu architecture", cmd_arch },
    { "version", "show OS version", cmd_version },
    { "storagetest", "storage probe", cmd_storagetest },
    { "read", "read file contents", cmd_read },
    { "edit", "edit file in TUI", cmd_edit },
    { "touch", "create empty file", cmd_touch },
    { "cpfile", "copy file from src to dst", cmd_cpfile },
    { "mvfile", "move file from src to dst", cmd_mvfile },
    { "cpdir", "copy directory recursively", cmd_cpdir },
    { "mvdir", "move directory recursively", cmd_mvdir },
    { "rmdir", "remove empty directory", cmd_rmdir },
    { "rmfile", "remove non-directory file", cmd_rmfile },
    { "tasks", "show scheduler tasks/status", cmd_tasks },
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

    command_exit_code_set(COMMAND_EXIT_FAILURE);

    for (index = 0; index < count; index++) {
        if (str_eq_ignore_case(commands[index].name, name)) {
            if (commands[index].handler) {
                int result = commands[index].handler(args);
                command_exit_code_set(result < 0 ? COMMAND_EXIT_FAILURE : COMMAND_EXIT_SUCCESS);
                return result;
            }
            return 0;
        }
    }

    return 0;
}