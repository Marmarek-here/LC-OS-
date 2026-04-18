#include "shell.h"

#include "commands/builtin_commands.h"
#include "os_api.h"

static char shell_current_dir_buffer[INPUT_MAX + 1];
static char shell_input_buffer[INPUT_MAX + 1];
static uint32_t shell_input_length_value = 0;
static uint32_t shell_input_cursor_value = 0;

#define SHELL_HISTORY_MAX 16
static char shell_history[SHELL_HISTORY_MAX][INPUT_MAX + 1];
static uint32_t shell_history_count = 0;
static int shell_history_view = -1;
static char shell_history_draft[INPUT_MAX + 1];

static int char_is_space(char ch)
{
    return ch == ' ' || ch == '\t';
}

static uint32_t str_len(const char *s)
{
    uint32_t length = 0;

    while (s[length])
        length++;

    return length;
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

static int str_starts_with(const char *text, const char *prefix)
{
    uint32_t i = 0;

    while (prefix[i]) {
        if (text[i] != prefix[i])
            return 0;
        i++;
    }

    return 1;
}

static int str_has_suffix(const char *text, const char *suffix)
{
    uint32_t tlen = str_len(text);
    uint32_t slen = str_len(suffix);
    uint32_t i;

    if (tlen < slen)
        return 0;

    for (i = 0; i < slen; i++) {
        if (text[tlen - slen + i] != suffix[i])
            return 0;
    }

    return 1;
}

static void str_copy(char *dst, const char *src)
{
    while (*src) {
        *dst = *src;
        dst++;
        src++;
    }

    *dst = '\0';
}

static void shell_input_reset(void)
{
    shell_input_length_value = 0;
    shell_input_cursor_value = 0;
    shell_input_buffer[0] = '\0';
}

static void shell_history_add(const char *line)
{
    uint32_t index;

    if (line[0] == '\0')
        return;

    if (shell_history_count > 0 && str_eq(shell_history[shell_history_count - 1], line))
        return;

    if (shell_history_count < SHELL_HISTORY_MAX) {
        str_copy(shell_history[shell_history_count], line);
        shell_history_count++;
        return;
    }

    for (index = 1; index < SHELL_HISTORY_MAX; index++)
        str_copy(shell_history[index - 1], shell_history[index]);

    str_copy(shell_history[SHELL_HISTORY_MAX - 1], line);
}

static void shell_history_load(int view_index)
{
    uint32_t history_index = shell_history_count - 1 - (uint32_t)view_index;

    str_copy(shell_input_buffer, shell_history[history_index]);
    shell_input_length_value = str_len(shell_input_buffer);
    shell_input_cursor_value = shell_input_length_value;
}

static void shell_history_prev(void)
{
    if (shell_history_count == 0)
        return;

    if (shell_history_view < 0)
        str_copy(shell_history_draft, shell_input_buffer);

    if ((uint32_t)(shell_history_view + 1) >= shell_history_count)
        return;

    shell_history_view++;
    shell_history_load(shell_history_view);
    os_redraw_input_line();
}

static void shell_history_next(void)
{
    if (shell_history_view < 0)
        return;

    if (shell_history_view == 0) {
        shell_history_view = -1;
        str_copy(shell_input_buffer, shell_history_draft);
        shell_input_length_value = str_len(shell_input_buffer);
        shell_input_cursor_value = shell_input_length_value;
        os_redraw_input_line();
        return;
    }

    shell_history_view--;
    shell_history_load(shell_history_view);
    os_redraw_input_line();
}

static void shell_insert_char(char ch)
{
    uint32_t index;

    if (shell_input_length_value >= INPUT_MAX)
        return;

    shell_history_view = -1;

    for (index = shell_input_length_value; index > shell_input_cursor_value; index--)
        shell_input_buffer[index] = shell_input_buffer[index - 1];

    shell_input_buffer[shell_input_cursor_value] = ch;
    shell_input_length_value++;
    shell_input_cursor_value++;
    shell_input_buffer[shell_input_length_value] = '\0';
    os_redraw_input_line();
}

static void shell_backspace(void)
{
    uint32_t index;

    if (shell_input_cursor_value == 0)
        return;

    shell_history_view = -1;

    for (index = shell_input_cursor_value - 1; index < shell_input_length_value - 1; index++)
        shell_input_buffer[index] = shell_input_buffer[index + 1];

    shell_input_cursor_value--;
    shell_input_length_value--;
    shell_input_buffer[shell_input_length_value] = '\0';
    os_redraw_input_line();
}

static void shell_move_left(void)
{
    if (shell_input_cursor_value > 0)
        shell_input_cursor_value--;

    os_cursor_reset_blink();
}

static void shell_move_right(void)
{
    if (shell_input_cursor_value < shell_input_length_value)
        shell_input_cursor_value++;

    os_cursor_reset_blink();
}

static void shell_insert_tab(void)
{
    uint32_t spaces = 4 - (shell_input_cursor_value % 4);
    uint32_t index;

    for (index = 0; index < spaces; index++)
        shell_insert_char(' ');
}

static void shell_print_help(void)
{
    uint32_t count;
    uint32_t i;
    const struct builtin_command *commands = builtin_commands_get(&count);
    uint32_t cols = os_term_cols();

    os_term_puts("Built-in commands:", OS_ATTR_MUTED);
    os_term_newline();

    if (cols < 96) {
        /* Narrow terminals: one command per line to avoid wrap artifacts. */
        for (i = 0; i < count; i++) {
            os_term_puts(commands[i].name, OS_ATTR_TEXT);
            os_term_puts(" : ", OS_ATTR_TEXT);
            os_term_puts(commands[i].help, OS_ATTR_TEXT);
            os_term_newline();
        }
        return;
    }

    {
        uint32_t half = (count + 1) / 2;
        uint32_t left_width = (cols > 3) ? ((cols - 3) / 2) : cols;

        for (i = 0; i < half; i++) {
            char row_buf[INPUT_MAX + 1];
            uint32_t pos = 0;
            uint32_t j;
            const char *lname = commands[i].name;
            const char *lhelp = commands[i].help;

            for (j = 0; lname[j] && pos < INPUT_MAX; j++)
                row_buf[pos++] = lname[j];
            if (pos < INPUT_MAX) row_buf[pos++] = ':';
            if (pos < INPUT_MAX) row_buf[pos++] = ' ';
            for (j = 0; lhelp[j] && pos < INPUT_MAX && pos < left_width; j++)
                row_buf[pos++] = lhelp[j];

            while (pos < left_width && pos < INPUT_MAX)
                row_buf[pos++] = ' ';

            if (pos < INPUT_MAX) row_buf[pos++] = ' ';
            if (pos < INPUT_MAX) row_buf[pos++] = '|';
            if (pos < INPUT_MAX) row_buf[pos++] = ' ';

            if (i + half < count) {
                const char *rname = commands[i + half].name;
                const char *rhelp = commands[i + half].help;

                for (j = 0; rname[j] && pos < INPUT_MAX; j++)
                    row_buf[pos++] = rname[j];
                if (pos < INPUT_MAX) row_buf[pos++] = ':';
                if (pos < INPUT_MAX) row_buf[pos++] = ' ';
                for (j = 0; rhelp[j] && pos < INPUT_MAX; j++)
                    row_buf[pos++] = rhelp[j];
            }

            row_buf[pos] = '\0';
            os_term_puts(row_buf, OS_ATTR_TEXT);
            os_term_newline();
        }
    }
}

static void shell_execute(const char *input)
{
    char command[INPUT_MAX + 1];
    char command_name[INPUT_MAX + 1];
    uint32_t command_length = 0;
    const char *args;

    while (*input && char_is_space(*input))
        input++;

    while (input[command_length] && !char_is_space(input[command_length]) && command_length < INPUT_MAX) {
        command[command_length] = input[command_length];
        command_length++;
    }
    command[command_length] = '\0';

    args = input + command_length;
    while (*args && char_is_space(*args))
        args++;

    if (command[0] == '\0')
        return;

    if (str_starts_with(command, "/programs/")) {
        if (os_game_launch(command))
            return;
        os_term_puts("Direct source execution is disabled. Use run <compiled-file>.", OS_ATTR_MUTED);
        return;
    }

    str_copy(command_name, command);

    if (str_eq(command_name, "help")) {
        shell_print_help();
        return;
    }

    if (!builtin_commands_execute(command_name, args)) {
        /* Try running as a file in the current directory.
           Bare name ending in .lcbat  →  run <curdir>/<name>
           ./name                      →  run <curdir>/<name>        */
        char run_path[INPUT_MAX + 1];
        const char *target = command_name;
        int try_run = 0;

        if (str_starts_with(target, "./")) {
            target = target + 2;
            try_run = 1;
        } else if (str_has_suffix(target, ".lcbat")) {
            try_run = 1;
        }

        if (try_run) {
            const char *cur = shell_current_dir_buffer;
            uint32_t cur_len = str_len(cur);
            uint32_t t_len = str_len(target);
            uint32_t pos = 0;
            uint32_t k;

            for (k = 0; k < cur_len && pos < INPUT_MAX; k++)
                run_path[pos++] = cur[k];

            if (pos > 0 && run_path[pos - 1] != '/' && pos < INPUT_MAX)
                run_path[pos++] = '/';

            for (k = 0; k < t_len && pos < INPUT_MAX; k++)
                run_path[pos++] = target[k];

            run_path[pos] = '\0';
            builtin_commands_execute("run", run_path);
        } else {
            os_term_puts("Unknown command: ", OS_ATTR_MUTED);
            os_term_puts(command_name, OS_ATTR_MUTED);
        }
    }
}

static void shell_submit(void)
{
    char command_line[INPUT_MAX + 1];
    char segment[INPUT_MAX + 1];
    uint32_t seg_len = 0;
    uint32_t i = 0;

    os_cursor_hide();
    str_copy(command_line, shell_input_buffer);
    shell_history_add(command_line);
    shell_history_view = -1;
    os_term_newline();

    /* Split on && and execute each segment in order */
    while (command_line[i]) {
        if (command_line[i] == '&' && command_line[i + 1] == '&') {
            segment[seg_len] = '\0';
            shell_execute(segment);
            os_term_newline();
            seg_len = 0;
            i += 2;
            /* Skip whitespace after && */
            while (command_line[i] == ' ' || command_line[i] == '\t')
                i++;
        } else {
            if (seg_len < INPUT_MAX)
                segment[seg_len++] = command_line[i];
            i++;
        }
    }
    /* Execute final (or only) segment */
    segment[seg_len] = '\0';
    shell_execute(segment);

    os_term_newline();
    shell_input_reset();
    os_redraw_input_line();
}

void shell_init(void)
{
    str_copy(shell_current_dir_buffer, "/");
    shell_input_reset();
    shell_history_count = 0;
    shell_history_view = -1;
    shell_history_draft[0] = '\0';
}

void shell_handle_event(const struct key_event *event)
{
    switch (event->type) {
    case KEY_CHAR:
        shell_insert_char(event->ch);
        break;
    case KEY_CTRL_C:
        break;
    case KEY_ENTER:
        shell_submit();
        break;
    case KEY_BACKSPACE:
        shell_backspace();
        break;
    case KEY_TAB:
        shell_insert_tab();
        break;
    case KEY_LEFT:
        shell_move_left();
        break;
    case KEY_RIGHT:
        shell_move_right();
        break;
    case KEY_UP:
        shell_history_prev();
        break;
    case KEY_DOWN:
        shell_history_next();
        break;
    case KEY_CAPSLOCK:
    case KEY_NONE:
    default:
        break;
    }
}

const char *shell_current_dir(void)
{
    return shell_current_dir_buffer;
}

void shell_set_current_dir(const char *path)
{
    str_copy(shell_current_dir_buffer, path);
}

uint32_t shell_input_length(void)
{
    return shell_input_length_value;
}

uint32_t shell_input_cursor(void)
{
    return shell_input_cursor_value;
}

char shell_input_char_at(uint32_t index)
{
    return shell_input_buffer[index];
}

uint32_t shell_prompt_length(void)
{
    return str_len(shell_current_dir_buffer) + 3;
}