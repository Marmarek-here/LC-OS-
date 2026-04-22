#ifndef LCOS_OS_API_H
#define LCOS_OS_API_H

#include "types.h"

#define OS_ATTR_TEXT   0x07
#define OS_ATTR_MUTED  0x07

void os_term_puts(const char *s, uint8_t attr);
void os_term_newline(void);
void os_term_clear(void);
void os_term_capture_begin(void);
void os_term_capture_end(char *out, uint32_t out_size);
void os_redraw_input_line(void);
void os_cursor_hide(void);
void os_cursor_reset_blink(void);
void os_wait_for_keypress(void);
int os_check_ctrl_c(void);
uint32_t os_term_cols(void);
void os_pipe_input_set(const char *text);
const char *os_pipe_input_get(void);
void os_mouse_get_state(int *x, int *y, uint8_t *buttons);
int os_mouse_consume_left_click(int *x, int *y);

int os_fs_resolve_path(const char *arg, char *out);
int os_fs_path_name_is_valid(const char *path);
int os_fs_dir_exists(const char *path);
int os_fs_dir_is_builtin(const char *path);
int os_fs_dir_has_entries(const char *path);
int os_fs_add_dir(const char *path);
int os_fs_remove_dir(const char *path);
void os_fs_list_directory(const char *path);
void os_fs_list_directory_sizes(const char *path);
void os_fs_list_directory_rights(const char *path);
void os_fs_list_directory_ex(const char *path, int show_sizes, int show_rights);

int os_fs_file_exists_any(const char *path);
int os_fs_file_exists_builtin(const char *path);
const char *os_fs_get_file_content(const char *path);
int os_fs_read_extension_matches(const char *dirpath, const char *ext);
int os_fs_collect_extension_matches(const char *dirpath, const char *ext, char *out, uint32_t out_size);
int os_fs_write_file(const char *path, const char *content);
int os_fs_remove_file(const char *path);
int os_fs_copy_file(const char *src, const char *dst);
int os_fs_move_file(const char *src, const char *dst);
int os_fs_copy_dir(const char *src, const char *dst);
int os_fs_move_dir(const char *src, const char *dst);
int os_game_launch(const char *path);
int os_editor_open(const char *path);

void os_power_shutdown(void);
void os_power_reboot(void);
void os_time_get_date(char *out, uint32_t out_size);
const char *os_cpu_arch(void);
void os_storage_probe(int *ata_legacy,
					  int *ahci,
					  int *usb_uhci,
					  int *usb_ohci,
					  int *usb_ehci,
					  int *usb_xhci);
uint32_t os_scheduler_ticks(void);
uint32_t os_scheduler_current_task(void);
uint32_t os_scheduler_task_count(void);
const char *os_scheduler_task_name(uint32_t index);

#endif