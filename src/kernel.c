/*
 * kernel.c — LC(OS) kernel entry
 * Freestanding C; no standard library.
 * VGA terminal with polled keyboard input.
 */

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

#include "filesystem_entries.h"
#include "filesystem_data.h"

#define VGA_BASE           ((volatile uint16_t *)0xB8000)
#define VGA_COLS           80
#define VGA_ROWS           25
#define VGA_ATTR_LOGO      0x0F
#define VGA_ATTR_TEXT      0x07
#define VGA_ATTR_MUTED     0x07
#define VGA_ATTR_STATUS    0x0A
#define VGA_ATTR_PROMPT    0x0E
#define STATUS_ROW         10
#define TERM_TOP           11
#define TERM_BOTTOM        24
#define INPUT_MAX          63
#define PROMPT_SIZE        2
#define CURSOR_BLINK_DELAY 3000000

#define PS2_DATA_PORT      0x60
#define PS2_STATUS_PORT    0x64

#define VGA_CRTC_INDEX     0x3D4
#define VGA_CRTC_DATA      0x3D5

enum key_type {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_CAPSLOCK
};

struct key_event {
    enum key_type type;
    char ch;
    uint8_t code;
    uint8_t has_code;
    const char *label;
};

static uint32_t term_row = TERM_TOP;
static uint32_t term_col = 0;
static char input_buffer[INPUT_MAX + 1];
static uint32_t input_length = 0;
static uint32_t input_cursor = 0;
static int caps_lock_enabled = 0;
static uint8_t cursor_visible = 0;
static uint16_t cursor_saved_cell = 0;
static uint32_t cursor_saved_row = TERM_TOP;
static uint32_t cursor_saved_col = PROMPT_SIZE;
static uint32_t cursor_blink_ticks = 0;

/* Filesystem and directory management */
static char current_dir[INPUT_MAX + 1] = "/";
#define MAX_DIRS 16
#define MAX_FILES 32
static char directories[MAX_DIRS][INPUT_MAX + 1];
static uint32_t dir_count = 0;

struct InMemFile {
    char path[INPUT_MAX + 1];
    char content[256];
    uint32_t content_len;
};
static struct InMemFile in_memory_files[MAX_FILES];
static uint32_t file_count = 0;

#define IDT_ENTRIES        256
#define PIC1_CMD           0x20
#define PIC1_DATA          0x21
#define PIC2_CMD           0xA0
#define PIC2_DATA          0xA1
#define PIC_EOI            0x20

#define KB_EVENT_QUEUE_SZ  32
static volatile struct key_event kb_queue[KB_EVENT_QUEUE_SZ];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;

static volatile uint32_t scheduler_ticks = 0;
static volatile uint32_t scheduler_current_task = 0;
static volatile uint32_t scheduler_task_count = 1;

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t first_page_table[1024] __attribute__((aligned(4096)));

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern void interrupt_ignore_stub(void);
extern void irq0_stub(void);
extern void irq1_stub(void);

static struct key_event keyboard_decode(uint8_t raw_scancode);
static void keyboard_event_push(struct key_event event);
static int keyboard_event_pop(struct key_event *event);

static inline uint16_t vga_entry(char c, uint8_t attr)
{
    return (uint16_t)(((uint16_t)attr << 8) | (uint8_t)c);
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait(void)
{
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static inline void interrupts_enable(void)
{
    __asm__ volatile ("sti");
}

static inline void interrupts_disable(void)
{
    __asm__ volatile ("cli");
}

static void scheduler_tick(void)
{
    scheduler_ticks++;
    if (scheduler_task_count > 1)
        scheduler_current_task = (scheduler_current_task + 1) % scheduler_task_count;
}

static void scheduler_init(void)
{
    scheduler_ticks = 0;
    scheduler_current_task = 0;
    /* Minimal round-robin scaffold (shell + idle logical tasks). */
    scheduler_task_count = 2;
}

static void paging_init(void)
{
    uint32_t i;
    uint32_t cr0;

    for (i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002;
        first_page_table[i] = (i * 0x1000) | 3;
    }

    page_directory[0] = ((uint32_t)first_page_table) | 3;

    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

static void idt_set_gate(uint8_t vec, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[vec].offset_low = (uint16_t)(base & 0xFFFF);
    idt[vec].selector = sel;
    idt[vec].zero = 0;
    idt[vec].type_attr = flags;
    idt[vec].offset_high = (uint16_t)((base >> 16) & 0xFFFF);
}

static void pic_remap(void)
{
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 4);
    io_wait();
    outb(PIC2_DATA, 2);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

static void interrupts_init(void)
{
    uint32_t i;

    for (i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate((uint8_t)i, (uint32_t)interrupt_ignore_stub, 0x08, 0x8E);

    idt_set_gate(0x20, (uint32_t)irq0_stub, 0x08, 0x8E);
    idt_set_gate(0x21, (uint32_t)irq1_stub, 0x08, 0x8E);

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base = (uint32_t)idt;

    __asm__ volatile ("lidt %0" : : "m"(idtp));

    pic_remap();
    /* Unmask IRQ0 (timer) and IRQ1 (keyboard); mask others. */
    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}

void irq0_handler_c(void)
{
    scheduler_tick();
    outb(PIC1_CMD, PIC_EOI);
}

void irq1_handler_c(void)
{
    uint8_t raw = inb(PS2_DATA_PORT);
    struct key_event event = keyboard_decode(raw);

    if (event.type != KEY_NONE)
        keyboard_event_push(event);

    outb(PIC1_CMD, PIC_EOI);
}

static inline uint16_t vga_get_at(uint32_t row, uint32_t col)
{
    return VGA_BASE[row * VGA_COLS + col];
}

static void vga_put_at(char c, uint8_t attr, uint32_t row, uint32_t col)
{
    VGA_BASE[row * VGA_COLS + col] = vga_entry(c, attr);
}

static void vga_clear_row(uint32_t row, uint8_t attr)
{
    uint32_t col;

    for (col = 0; col < VGA_COLS; col++)
        vga_put_at(' ', attr, row, col);
}

static void vga_clear(void)
{
    uint32_t row;

    for (row = 0; row < VGA_ROWS; row++)
        vga_clear_row(row, 0x00);
}

static void vga_puts_at(const char *s, uint8_t attr, uint32_t row, uint32_t col)
{
    while (*s && row < VGA_ROWS) {
        if (*s == '\n') {
            row++;
            col = 0;
            s++;
            continue;
        }

        if (col >= VGA_COLS) {
            row++;
            col = 0;
            if (row >= VGA_ROWS)
                break;
        }

        vga_put_at(*s, attr, row, col);
        col++;
        s++;
    }
}

static void boot_marker(uint32_t row, const char *text)
{
    vga_clear_row(row, 0x00);
    vga_puts_at(text, VGA_ATTR_STATUS, row, 0);
}

static uint32_t str_len(const char *s)
{
    uint32_t length = 0;

    while (s[length])
        length++;

    return length;
}

static void u32_to_dec(uint32_t value, char *buffer)
{
    char reversed[11];
    uint32_t count = 0;
    uint32_t index;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0) {
        reversed[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    for (index = 0; index < count; index++)
        buffer[index] = reversed[count - index - 1];

    buffer[count] = '\0';
}

static void cursor_disable(void)
{
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
}

static void cursor_position(uint32_t *row, uint32_t *col)
{
    uint32_t prompt_len = 0;
    
    /* Calculate actual prompt length: current_dir + " > " */
    while (current_dir[prompt_len])
        prompt_len++;
    prompt_len += 3; /* for " > " */
    
    *row = term_row;
    *col = prompt_len + input_cursor;
    if (*col >= VGA_COLS)
        *col = VGA_COLS - 1;
}

static void cursor_hide(void)
{
    if (!cursor_visible)
        return;

    VGA_BASE[cursor_saved_row * VGA_COLS + cursor_saved_col] = cursor_saved_cell;
    cursor_visible = 0;
}

static void cursor_show(void)
{
    uint32_t row;
    uint32_t col;
    uint16_t cell;
    uint8_t ch;
    uint8_t attr;

    if (cursor_visible)
        return;

    cursor_position(&row, &col);
    cell = vga_get_at(row, col);
    ch = (uint8_t)(cell & 0xFF);
    attr = (uint8_t)((cell >> 8) & 0xFF);

    cursor_saved_row = row;
    cursor_saved_col = col;
    cursor_saved_cell = cell;

    if (ch == ' ')
        VGA_BASE[row * VGA_COLS + col] = vga_entry(' ', 0x70);
    else
        VGA_BASE[row * VGA_COLS + col] = vga_entry((char)ch, (uint8_t)((attr << 4) | (attr >> 4)));

    cursor_visible = 1;
}

static void cursor_reset_blink(void)
{
    cursor_blink_ticks = 0;
    cursor_hide();
    cursor_show();
}

static void cursor_tick(void)
{
    cursor_blink_ticks++;
    if (cursor_blink_ticks < CURSOR_BLINK_DELAY)
        return;

    cursor_blink_ticks = 0;
    if (cursor_visible)
        cursor_hide();
    else
        cursor_show();
}

static void scroll_terminal(void)
{
    uint32_t row;
    uint32_t col;

    for (row = TERM_TOP; row < TERM_BOTTOM; row++) {
        for (col = 0; col < VGA_COLS; col++)
            VGA_BASE[row * VGA_COLS + col] = VGA_BASE[(row + 1) * VGA_COLS + col];
    }

    vga_clear_row(TERM_BOTTOM, 0x00);

    if (term_row > TERM_TOP)
        term_row--;
}

static void term_newline(void)
{
    term_row++;
    term_col = 0;

    if (term_row > TERM_BOTTOM)
        scroll_terminal();
}

static void term_putc(char c, uint8_t attr)
{
    if (c == '\n') {
        term_newline();
        return;
    }

    if (term_col >= VGA_COLS)
        term_newline();

    vga_put_at(c, attr, term_row, term_col);
    term_col++;
}

static void term_puts(const char *s, uint8_t attr)
{
    while (*s)
        term_putc(*s++, attr);
}

static void draw_logo(void)
{
    static const char *logo[] = {
        " _     ____  _____  ______  ",
        "| |   / ___|/ / _ \\/ ___\\ \\ ",
        "| |  | |   | | | | \\___ \\| |",
        "| |__| |___| | |_| |___) | |",
        "|_____\\____| |\\___/|____/| |",
        "            \\_\\         /_/  ",
        0
    };
    uint32_t row;

    for (row = 0; logo[row] != 0; row++) {
        uint32_t width = str_len(logo[row]);
        uint32_t col = (VGA_COLS - width) / 2;

        vga_puts_at(logo[row], VGA_ATTR_LOGO, row, col);
    }
}

static void draw_ui(void)
{
    uint32_t col;

    vga_puts_at("LC(OS) polling terminal", VGA_ATTR_TEXT, 7, 2);
    vga_puts_at("Keys: arrows move, Tab inserts spaces, Caps Lock toggles case.",
                VGA_ATTR_TEXT, 8, 2);

    for (col = 0; col < VGA_COLS; col++)
        vga_put_at('-', VGA_ATTR_TEXT, 9, col);

    vga_clear_row(STATUS_ROW, 0x00);
}

static void draw_status(const struct key_event *event)
{
    char decimal[11];

    cursor_hide();

    vga_clear_row(STATUS_ROW, 0x00);
    vga_puts_at("Last key:", VGA_ATTR_STATUS, STATUS_ROW, 0);

    if (event->type == KEY_CHAR) {
        vga_put_at('\'', VGA_ATTR_STATUS, STATUS_ROW, 10);
        vga_put_at(event->ch, VGA_ATTR_STATUS, STATUS_ROW, 11);
        vga_put_at('\'', VGA_ATTR_STATUS, STATUS_ROW, 12);
    } else if (event->label != 0) {
        vga_puts_at(event->label, VGA_ATTR_STATUS, STATUS_ROW, 10);
    } else {
        vga_puts_at("none", VGA_ATTR_STATUS, STATUS_ROW, 10);
    }

    vga_puts_at(" Code:", VGA_ATTR_STATUS, STATUS_ROW, 24);
    if (event->has_code) {
        u32_to_dec((uint32_t)event->code, decimal);
        vga_puts_at(decimal, VGA_ATTR_STATUS, STATUS_ROW, 31);
    } else {
        vga_puts_at("N/A", VGA_ATTR_STATUS, STATUS_ROW, 31);
    }

    vga_puts_at(" Caps:", VGA_ATTR_STATUS, STATUS_ROW, 40);
    vga_puts_at(caps_lock_enabled ? "ON" : "OFF", VGA_ATTR_STATUS, STATUS_ROW, 47);
    cursor_show();
}

static void prompt_reset(void)
{
    input_length = 0;
    input_cursor = 0;
    input_buffer[0] = '\0';
}

static void redraw_input_line(void)
{
    uint32_t index;

    cursor_hide();

    vga_clear_row(term_row, 0x00);
    term_col = 0;
    
    /* Show current directory before prompt */
    term_puts(current_dir, VGA_ATTR_PROMPT);
    term_puts(" > ", VGA_ATTR_PROMPT);

    for (index = 0; index < input_length; index++)
        term_putc(input_buffer[index], VGA_ATTR_MUTED);

    cursor_reset_blink();
}

static void term_prompt(void)
{
    term_row = TERM_TOP;
    term_col = 0;
    prompt_reset();
    redraw_input_line();
}

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

static void str_copy(char *dst, const char *src)
{
    while (*src) {
        *dst = *src;
        dst++;
        src++;
    }

    *dst = '\0';
}

/* Find file content by full path */
static const char *find_file_content(const char *filepath)
{
    uint32_t i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].path, filepath))
            return fs_files[i].content;
        i++;
    }
    return 0;
}

static struct InMemFile *find_memory_file(const char *filepath);

static int file_exists_builtin(const char *filepath)
{
    return find_file_content(filepath) != 0;
}

static int file_exists_any(const char *filepath)
{
    if (find_memory_file(filepath) != 0)
        return 1;
    return file_exists_builtin(filepath);
}

/* Check if directory path exists (from filesystem or in-memory added dirs) */
static int dir_exists(const char *dirpath)
{
    uint32_t i;

    if (str_eq(dirpath, "/") || str_eq(dirpath, "/commands") || str_eq(dirpath, "/programs"))
        return 1;
    
    /* Check built-in directories */
    i = 0;
    while (fs_directories[i].name != 0) {
        if (str_eq(fs_directories[i].path, dirpath))
            return 1;
        i++;
    }
    
    /* Check in-memory added directories */
    if (str_eq(dirpath, "/"))
        return 1;
    for (i = 0; i < dir_count; i++) {
        if (str_eq(directories[i], dirpath))
            return 1;
    }
    return 0;
}

/* Add a directory to in-memory list */
static void add_directory(const char *dirpath)
{
    if (dir_count >= MAX_DIRS)
        return;
    if (dir_exists(dirpath))
        return;
    str_copy(directories[dir_count], dirpath);
    dir_count++;
}

/* Get parent directory from a path */
static void get_parent_dir(const char *dirpath, char *parent)
{
    uint32_t i = 0;
    uint32_t last_slash = 0;
    
    while (dirpath[i]) {
        if (dirpath[i] == '/')
            last_slash = i;
        i++;
    }
    
    if (last_slash == 0) {
        /* Already at root or only one segment */
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        /* Copy everything up to last slash */
        for (i = 0; i < last_slash; i++)
            parent[i] = dirpath[i];
        parent[last_slash] = '\0';
    }
}

/* Find in-memory file by full path */
static struct InMemFile *find_memory_file(const char *filepath)
{
    uint32_t i;
    for (i = 0; i < file_count; i++) {
        if (str_eq(in_memory_files[i].path, filepath))
            return &in_memory_files[i];
    }
    return 0;
}

/* Create or overwrite in-memory file */
static int write_memory_file(const char *filepath, const char *content)
{
    struct InMemFile *file = find_memory_file(filepath);

    /* Count content length */
    uint32_t content_len = 0;
    
    /* Count content length */
    while (content[content_len] && content_len < 255)
        content_len++;
    
    if (file) {
        /* Overwrite existing file */
        str_copy(file->content, content);
        file->content_len = content_len;
        return 1;
    }
    
    /* Create new file */
    if (file_count >= MAX_FILES)
        return 0;
    
    str_copy(in_memory_files[file_count].path, filepath);
    str_copy(in_memory_files[file_count].content, content);
    in_memory_files[file_count].content_len = content_len;
    file_count++;
    return 1;
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

static int resolve_path(const char *arg, char *out)
{
    uint32_t arg_len = str_len(arg);
    uint32_t cwd_len = str_len(current_dir);

    if (str_eq(arg, "..")) {
        get_parent_dir(current_dir, out);
        return 1;
    }

    if (arg[0] == '/') {
        if (arg_len > INPUT_MAX)
            return 0;
        str_copy(out, arg);
        return 1;
    }

    if (str_eq(current_dir, "/")) {
        if (arg_len + 1 > INPUT_MAX)
            return 0;
        out[0] = '/';
        str_copy(&out[1], arg);
        return 1;
    }

    if (cwd_len + 1 + arg_len > INPUT_MAX)
        return 0;

    str_copy(out, current_dir);
    {
        uint32_t len = 0;
        while (out[len])
            len++;
        out[len] = '/';
        str_copy(&out[len + 1], arg);
    }

    return 1;
}

static int dir_is_builtin(const char *path)
{
    uint32_t i = 0;

    while (fs_directories[i].name != 0) {
        if (str_eq(fs_directories[i].path, path))
            return 1;
        i++;
    }

    return 0;
}

static int dir_has_entries(const char *dirpath)
{
    uint32_t i;
    uint32_t len = 0;
    char prefix[INPUT_MAX + 1];

    while (dirpath[len]) {
        prefix[len] = dirpath[len];
        len++;
    }

    prefix[len] = '/';
    prefix[len + 1] = '\0';

    i = 0;
    while (fs_directories[i].name != 0) {
        const char *p = fs_directories[i].path;
        if (!str_eq(p, dirpath) && str_starts_with(p, prefix))
            return 1;
        i++;
    }

    i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].dir_path, dirpath))
            return 1;
        i++;
    }

    for (i = 0; i < dir_count; i++) {
        const char *p = directories[i];
        if (!str_eq(p, dirpath) && str_starts_with(p, prefix))
            return 1;
    }

    for (i = 0; i < file_count; i++) {
        const char *path = in_memory_files[i].path;
        uint32_t k = 0;
        uint32_t last_slash = 0;
        char parent[INPUT_MAX + 1];

        while (path[k]) {
            if (path[k] == '/')
                last_slash = k;
            k++;
        }

        if (last_slash == 0) {
            parent[0] = '/';
            parent[1] = '\0';
        } else {
            for (k = 0; k < last_slash; k++)
                parent[k] = path[k];
            parent[last_slash] = '\0';
        }

        if (str_eq(parent, dirpath))
            return 1;
    }

    return 0;
}

static int remove_memory_dir(const char *path)
{
    uint32_t i;

    for (i = 0; i < dir_count; i++) {
        if (str_eq(directories[i], path)) {
            uint32_t j;
            for (j = i; j + 1 < dir_count; j++)
                str_copy(directories[j], directories[j + 1]);
            dir_count--;
            return 1;
        }
    }

    return 0;
}

static int remove_memory_file(const char *path)
{
    uint32_t i;

    for (i = 0; i < file_count; i++) {
        if (str_eq(in_memory_files[i].path, path)) {
            uint32_t j;
            for (j = i; j + 1 < file_count; j++)
                in_memory_files[j] = in_memory_files[j + 1];
            file_count--;
            return 1;
        }
    }

    return 0;
}

/* Return 1 if child is a direct child directory of parent. */
static int is_direct_child_path(const char *parent, const char *child)
{
    uint32_t parent_len = 0;
    uint32_t i;
    uint32_t name_start;

    if (str_eq(parent, child))
        return 0;

    while (parent[parent_len])
        parent_len++;

    if (str_eq(parent, "/")) {
        if (child[0] != '/')
            return 0;
        name_start = 1;
    } else {
        for (i = 0; i < parent_len; i++) {
            if (parent[i] != child[i])
                return 0;
        }

        if (child[parent_len] != '/')
            return 0;

        name_start = parent_len + 1;
    }

    if (child[name_start] == '\0')
        return 0;

    for (i = name_start; child[i]; i++) {
        if (child[i] == '/')
            return 0;
    }

    return 1;
}

/* List files and subdirectories in a given directory */
static void list_directory(const char *dirpath)
{
    uint32_t i, j;
    int found_any = 0;

    if (str_eq(dirpath, "/")) {
        term_puts("[commands]", VGA_ATTR_TEXT);
        term_newline();
        term_puts("[programs]", VGA_ATTR_TEXT);
        term_newline();
        found_any = 1;
    }

    if (str_eq(dirpath, "/commands")) {
        term_puts("help", VGA_ATTR_TEXT);
        term_newline();
        term_puts("ls", VGA_ATTR_TEXT);
        term_newline();
        term_puts("echo", VGA_ATTR_TEXT);
        term_newline();
        term_puts("mkdir", VGA_ATTR_TEXT);
        term_newline();
        term_puts("rmdir", VGA_ATTR_TEXT);
        term_newline();
        term_puts("rmfile", VGA_ATTR_TEXT);
        term_newline();
        term_puts("cd", VGA_ATTR_TEXT);
        term_newline();
        term_puts("read", VGA_ATTR_TEXT);
        term_newline();
        term_puts("touch", VGA_ATTR_TEXT);
        found_any = 1;
    }

    /* List built-in subdirectories. */
    i = 0;
    while (fs_directories[i].name != 0) {
        const char *subdir_path = fs_directories[i].path;

        if (is_direct_child_path(dirpath, subdir_path)) {
            term_puts("[", VGA_ATTR_TEXT);
            term_puts(fs_directories[i].name, VGA_ATTR_TEXT);
            term_puts("]", VGA_ATTR_TEXT);
            term_newline();
            found_any = 1;
        }

        i++;
    }

    /* List built-in files. */
    i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].dir_path, dirpath) && find_memory_file(fs_files[i].path) == 0) {
            term_puts(fs_files[i].name, VGA_ATTR_TEXT);
            term_newline();
            found_any = 1;
        }
        i++;
    }

    /* List in-memory created directories. */
    for (i = 0; i < dir_count; i++) {
        const char *mem_dir = directories[i];

        if (is_direct_child_path(dirpath, mem_dir)) {
            uint32_t start = 1;

            if (!str_eq(dirpath, "/")) {
                uint32_t len = 0;
                while (dirpath[len])
                    len++;
                start = len + 1;
            }

            term_puts("[", VGA_ATTR_TEXT);
            term_puts(&mem_dir[start], VGA_ATTR_TEXT);
            term_puts("]", VGA_ATTR_TEXT);
            term_newline();
            found_any = 1;
        }
    }

    /* List in-memory created files. */
    for (i = 0; i < file_count; i++) {
        const char *file_dir = in_memory_files[i].path;
        uint32_t last_slash = 0;
        uint32_t k = 0;
        
        /* Find parent directory from file path */
        while (file_dir[k]) {
            if (file_dir[k] == '/')
                last_slash = k;
            k++;
        }
        
        /* Extract parent dir */
        char parent_dir[INPUT_MAX + 1];
        if (last_slash == 0) {
            parent_dir[0] = '/';
            parent_dir[1] = '\0';
        } else {
            for (j = 0; j < last_slash; j++)
                parent_dir[j] = file_dir[j];
            parent_dir[last_slash] = '\0';
        }
        
        if (str_eq(parent_dir, dirpath)) {
            const char *filename = &file_dir[last_slash + 1];
            term_puts(filename, VGA_ATTR_TEXT);
            term_newline();
            found_any = 1;
        }
    }
    
    if (!found_any) {
        term_puts("<empty>", VGA_ATTR_MUTED);
    }
}

static void execute_command(const char *input)
{
    char command[INPUT_MAX + 1];
    char command_name[INPUT_MAX + 1];
    uint32_t command_length = 0;
    const char *args;

    while (*input && char_is_space(*input))
        input++;

    while (input[command_length] && !char_is_space(input[command_length])) {
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
        term_puts("Program execution not implemented yet: ", VGA_ATTR_MUTED);
        term_puts(command, VGA_ATTR_MUTED);
        return;
    }

    if (str_starts_with(command, "/commands/")) {
        str_copy(command_name, command + 10);
    } else {
        str_copy(command_name, command);
    }

    if (str_eq(command_name, "help")) {
        term_puts("Built-in commands:", VGA_ATTR_MUTED);
        term_newline();
        term_puts("help      - show available commands", VGA_ATTR_TEXT);
        term_newline();
        term_puts("ls        - list files/directories", VGA_ATTR_TEXT);
        term_newline();
        term_puts("echo      - print text or write to file", VGA_ATTR_TEXT);
        term_newline();
        term_puts("mkdir     - create a directory", VGA_ATTR_TEXT);
        term_newline();
        term_puts("cd        - change directory", VGA_ATTR_TEXT);
        term_newline();
        term_puts("cd ..     - go to parent directory", VGA_ATTR_TEXT);
        term_newline();
        term_puts("read      - read file contents", VGA_ATTR_TEXT);
        term_newline();
        term_puts("touch     - create empty file", VGA_ATTR_TEXT);
        term_newline();
        term_puts("rmdir     - remove empty directory", VGA_ATTR_TEXT);
        term_newline();
        term_puts("rmfile    - remove non-directory file", VGA_ATTR_TEXT);
        term_newline();
        term_puts("Use /commands/<name> form or bare name", VGA_ATTR_TEXT);
        return;
    }

    if (str_eq(command_name, "ls")) {
        list_directory(current_dir);
        return;
    }

    if (str_eq(command_name, "mkdir")) {
        char dirname[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        uint32_t dlen = 0;
        const char *arg_start = args;
        
        while (*arg_start && !char_is_space(*arg_start)) {
            dirname[dlen] = *arg_start;
            dlen++;
            arg_start++;
        }
        dirname[dlen] = '\0';

        if (dlen == 0) {
            term_puts("Usage: mkdir <dirname>", VGA_ATTR_MUTED);
            return;
        }

        if (!resolve_path(dirname, fullpath)) {
            term_puts("Path too long", VGA_ATTR_MUTED);
            return;
        }

        if (file_exists_any(fullpath)) {
            term_puts("Cannot create directory, file exists: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (dir_exists(fullpath)) {
            term_puts("Directory already exists: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        add_directory(fullpath);
        term_puts("Created directory: ", VGA_ATTR_TEXT);
        term_puts(fullpath, VGA_ATTR_TEXT);
        return;
    }

    if (str_eq(command_name, "cd")) {
        char dirname[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        uint32_t dlen = 0;
        const char *arg_start = args;
        
        while (*arg_start && !char_is_space(*arg_start)) {
            dirname[dlen] = *arg_start;
            dlen++;
            arg_start++;
        }
        dirname[dlen] = '\0';

        if (dlen == 0) {
            term_puts("Usage: cd <dirname>", VGA_ATTR_MUTED);
            return;
        }

        if (!resolve_path(dirname, fullpath)) {
            term_puts("Path too long", VGA_ATTR_MUTED);
            return;
        }

        if (!dir_exists(fullpath)) {
            term_puts("Directory not found: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        str_copy(current_dir, fullpath);
        term_puts("Changed to: ", VGA_ATTR_TEXT);
        term_puts(current_dir, VGA_ATTR_TEXT);
        return;
    }

    if (str_eq(command_name, "read")) {
        char filename[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        const char *content;
        struct InMemFile *mem_file;
        uint32_t flen = 0;
        const char *arg_start = args;
        
        while (*arg_start && !char_is_space(*arg_start)) {
            filename[flen] = *arg_start;
            flen++;
            arg_start++;
        }
        filename[flen] = '\0';

        if (flen == 0) {
            term_puts("Usage: read <filename>", VGA_ATTR_MUTED);
            return;
        }

        if (!resolve_path(filename, fullpath)) {
            term_puts("Path too long", VGA_ATTR_MUTED);
            return;
        }

        /* Check in-memory files first */
        mem_file = find_memory_file(fullpath);
        if (mem_file) {
            term_puts(mem_file->content, VGA_ATTR_TEXT);
            return;
        }

        /* Then check built-in files */
        content = find_file_content(fullpath);
        if (!content) {
            term_puts("File not found: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        term_puts(content, VGA_ATTR_TEXT);
        return;
    }

    if (str_eq(command_name, "touch")) {
        char filename[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        uint32_t flen = 0;
        const char *arg_start = args;
        
        while (*arg_start && !char_is_space(*arg_start)) {
            filename[flen] = *arg_start;
            flen++;
            arg_start++;
        }
        filename[flen] = '\0';

        if (flen == 0) {
            term_puts("Usage: touch <filename>", VGA_ATTR_MUTED);
            return;
        }

        if (!resolve_path(filename, fullpath)) {
            term_puts("Path too long", VGA_ATTR_MUTED);
            return;
        }

        if (dir_exists(fullpath)) {
            term_puts("Cannot create file, directory exists: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (find_memory_file(fullpath) || file_exists_builtin(fullpath)) {
            term_puts("File already exists: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (!write_memory_file(fullpath, "")) {
            term_puts("Failed to create file (storage full)", VGA_ATTR_MUTED);
            return;
        }

        term_puts("Created: ", VGA_ATTR_TEXT);
        term_puts(fullpath, VGA_ATTR_TEXT);
        return;
    }

    if (str_eq(command_name, "rmdir")) {
        char dirname[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        uint32_t dlen = 0;
        const char *arg_start = args;

        while (*arg_start && !char_is_space(*arg_start)) {
            dirname[dlen] = *arg_start;
            dlen++;
            arg_start++;
        }
        dirname[dlen] = '\0';

        if (dlen == 0) {
            term_puts("Usage: rmdir <dirname>", VGA_ATTR_MUTED);
            return;
        }

        if (!resolve_path(dirname, fullpath)) {
            term_puts("Path too long", VGA_ATTR_MUTED);
            return;
        }

        if (str_eq(fullpath, "/")) {
            term_puts("Cannot remove root directory", VGA_ATTR_MUTED);
            return;
        }

        if (!dir_exists(fullpath)) {
            term_puts("Directory not found: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (dir_is_builtin(fullpath)) {
            term_puts("Cannot remove built-in directory: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (dir_has_entries(fullpath)) {
            term_puts("Directory not empty: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (!remove_memory_dir(fullpath)) {
            term_puts("Failed to remove directory: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        term_puts("Removed directory: ", VGA_ATTR_TEXT);
        term_puts(fullpath, VGA_ATTR_TEXT);
        return;
    }

    if (str_eq(command_name, "rmfile")) {
        char filename[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        uint32_t flen = 0;
        const char *arg_start = args;

        while (*arg_start && !char_is_space(*arg_start)) {
            filename[flen] = *arg_start;
            flen++;
            arg_start++;
        }
        filename[flen] = '\0';

        if (flen == 0) {
            term_puts("Usage: rmfile <filename>", VGA_ATTR_MUTED);
            return;
        }

        if (!resolve_path(filename, fullpath)) {
            term_puts("Path too long", VGA_ATTR_MUTED);
            return;
        }

        if (dir_exists(fullpath)) {
            term_puts("rmfile only removes files: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (file_exists_builtin(fullpath)) {
            term_puts("Cannot remove built-in file: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        if (!remove_memory_file(fullpath)) {
            term_puts("File not found: ", VGA_ATTR_MUTED);
            term_puts(fullpath, VGA_ATTR_MUTED);
            return;
        }

        term_puts("Removed file: ", VGA_ATTR_TEXT);
        term_puts(fullpath, VGA_ATTR_TEXT);
        return;
    }

    /* Handle echo with redirection: echo text > file.txt */
    if (str_eq(command_name, "echo")) {
        char echo_text[INPUT_MAX + 1];
        char filename[INPUT_MAX + 1];
        char fullpath[INPUT_MAX + 1];
        const char *redirect_ptr;
        int existed = 0;
        uint32_t text_len = 0;
        uint32_t file_len = 0;
        
        /* Look for > character */
        redirect_ptr = args;
        while (*redirect_ptr && *redirect_ptr != '>') {
            echo_text[text_len] = *redirect_ptr;
            text_len++;
            redirect_ptr++;
        }
        echo_text[text_len] = '\0';
        
        /* Trim trailing space from text */
        while (text_len > 0 && echo_text[text_len - 1] == ' ')
            text_len--;
        echo_text[text_len] = '\0';
        
        if (*redirect_ptr == '>') {
            /* Redirection mode: echo text > file */
            redirect_ptr++; /* Skip the > */
            while (*redirect_ptr && char_is_space(*redirect_ptr))
                redirect_ptr++; /* Skip spaces */
            
            /* Extract filename */
            while (*redirect_ptr && !char_is_space(*redirect_ptr)) {
                filename[file_len] = *redirect_ptr;
                file_len++;
                redirect_ptr++;
            }
            filename[file_len] = '\0';
            
            if (file_len == 0) {
                term_puts("Usage: echo <text> > <filename>", VGA_ATTR_MUTED);
                return;
            }
            
            if (!resolve_path(filename, fullpath)) {
                term_puts("Path too long", VGA_ATTR_MUTED);
                return;
            }

            if (dir_exists(fullpath)) {
                term_puts("Cannot write, path is directory: ", VGA_ATTR_MUTED);
                term_puts(fullpath, VGA_ATTR_MUTED);
                return;
            }

            existed = file_exists_any(fullpath);
            
            if (!write_memory_file(fullpath, echo_text)) {
                term_puts("Failed to write file (storage full)", VGA_ATTR_MUTED);
                return;
            }

            if (existed)
                term_puts("Updated: ", VGA_ATTR_TEXT);
            else
                term_puts("Created: ", VGA_ATTR_TEXT);
            term_puts(fullpath, VGA_ATTR_TEXT);
        } else {
            /* Normal echo without redirection */
            if (*args)
                term_puts(args, VGA_ATTR_TEXT);
        }
        return;
    }

    term_puts("Unknown command: ", VGA_ATTR_MUTED);
    term_puts(command_name, VGA_ATTR_MUTED);
}

static void submit_input(void)
{
    char command_line[INPUT_MAX + 1];

    cursor_hide();
    str_copy(command_line, input_buffer);
    term_newline();
    execute_command(command_line);
    term_newline();
    prompt_reset();
    redraw_input_line();
}

static void input_insert_char(char ch)
{
    uint32_t index;

    if (input_length >= INPUT_MAX)
        return;

    for (index = input_length; index > input_cursor; index--)
        input_buffer[index] = input_buffer[index - 1];

    input_buffer[input_cursor] = ch;
    input_length++;
    input_cursor++;
    input_buffer[input_length] = '\0';
    redraw_input_line();
}

static void input_backspace(void)
{
    uint32_t index;

    if (input_cursor == 0)
        return;

    for (index = input_cursor - 1; index < input_length - 1; index++)
        input_buffer[index] = input_buffer[index + 1];

    input_cursor--;
    input_length--;
    input_buffer[input_length] = '\0';
    redraw_input_line();
}

static void input_move_left(void)
{
    if (input_cursor > 0)
        input_cursor--;

    cursor_reset_blink();
}

static void input_move_right(void)
{
    if (input_cursor < input_length)
        input_cursor++;

    cursor_reset_blink();
}

static void input_move_home(void)
{
    input_cursor = 0;
    cursor_reset_blink();
}

static void input_move_end(void)
{
    input_cursor = input_length;
    cursor_reset_blink();
}

static void input_tab(void)
{
    uint32_t spaces = 4 - (input_cursor % 4);
    uint32_t index;

    for (index = 0; index < spaces; index++) {
        if (input_length >= INPUT_MAX)
            break;
        input_insert_char(' ');
    }
}

static char alpha_with_case(char lower, int shift)
{
    int uppercase = shift ^ caps_lock_enabled;

    if (uppercase)
        return (char)(lower - ('a' - 'A'));

    return lower;
}

static char scancode_to_ascii(uint8_t scancode, int shift)
{
    switch (scancode) {
    case 0x02: return shift ? '!' : '1';
    case 0x03: return shift ? '@' : '2';
    case 0x04: return shift ? '#' : '3';
    case 0x05: return shift ? '$' : '4';
    case 0x06: return shift ? '%' : '5';
    case 0x07: return shift ? '^' : '6';
    case 0x08: return shift ? '&' : '7';
    case 0x09: return shift ? '*' : '8';
    case 0x0A: return shift ? '(' : '9';
    case 0x0B: return shift ? ')' : '0';
    case 0x0C: return shift ? '_' : '-';
    case 0x0D: return shift ? '+' : '=';
    case 0x10: return alpha_with_case('q', shift);
    case 0x11: return alpha_with_case('w', shift);
    case 0x12: return alpha_with_case('e', shift);
    case 0x13: return alpha_with_case('r', shift);
    case 0x14: return alpha_with_case('t', shift);
    case 0x15: return alpha_with_case('y', shift);
    case 0x16: return alpha_with_case('u', shift);
    case 0x17: return alpha_with_case('i', shift);
    case 0x18: return alpha_with_case('o', shift);
    case 0x19: return alpha_with_case('p', shift);
    case 0x1A: return shift ? '{' : '[';
    case 0x1B: return shift ? '}' : ']';
    case 0x1E: return alpha_with_case('a', shift);
    case 0x1F: return alpha_with_case('s', shift);
    case 0x20: return alpha_with_case('d', shift);
    case 0x21: return alpha_with_case('f', shift);
    case 0x22: return alpha_with_case('g', shift);
    case 0x23: return alpha_with_case('h', shift);
    case 0x24: return alpha_with_case('j', shift);
    case 0x25: return alpha_with_case('k', shift);
    case 0x26: return alpha_with_case('l', shift);
    case 0x27: return shift ? ':' : ';';
    case 0x28: return shift ? '"' : '\'';
    case 0x29: return shift ? '~' : '`';
    case 0x2B: return shift ? '|' : '\\';
    case 0x2C: return alpha_with_case('z', shift);
    case 0x2D: return alpha_with_case('x', shift);
    case 0x2E: return alpha_with_case('c', shift);
    case 0x2F: return alpha_with_case('v', shift);
    case 0x30: return alpha_with_case('b', shift);
    case 0x31: return alpha_with_case('n', shift);
    case 0x32: return alpha_with_case('m', shift);
    case 0x33: return shift ? '<' : ',';
    case 0x34: return shift ? '>' : '.';
    case 0x35: return shift ? '?' : '/';
    case 0x39: return ' ';
    default:   return 0;
    }
}

static struct key_event keyboard_decode(uint8_t raw_scancode)
{
    static int shift_down = 0;
    static int extended = 0;
    struct key_event event;
    uint8_t released;
    uint8_t scancode;

    event.type = KEY_NONE;
    event.ch = 0;
    event.code = 0;
    event.has_code = 0;
    event.label = 0;

    if (raw_scancode == 0xE0) {
        extended = 1;
        return event;
    }

    released = (uint8_t)(raw_scancode & 0x80);
    scancode = (uint8_t)(raw_scancode & 0x7F);

    if (scancode == 0x2A || scancode == 0x36) {
        shift_down = released == 0;
        return event;
    }

    if (extended) {
        extended = 0;
        if (released)
            return event;

        switch (scancode) {
        case 0x4B:
            event.type = KEY_LEFT;
            event.label = "Left";
            return event;
        case 0x4D:
            event.type = KEY_RIGHT;
            event.label = "Right";
            return event;
        case 0x48:
            event.type = KEY_UP;
            event.label = "Up";
            return event;
        case 0x50:
            event.type = KEY_DOWN;
            event.label = "Down";
            return event;
        default:
            return event;
        }
    }

    if (released)
        return event;

    if (scancode == 0x3A) {
        caps_lock_enabled = !caps_lock_enabled;
        event.type = KEY_CAPSLOCK;
        event.label = caps_lock_enabled ? "CapsLock ON" : "CapsLock OFF";
        return event;
    }

    if (scancode == 0x1C) {
        event.type = KEY_ENTER;
        event.code = 13;
        event.has_code = 1;
        event.label = "Enter";
        return event;
    }

    if (scancode == 0x0E) {
        event.type = KEY_BACKSPACE;
        event.code = 8;
        event.has_code = 1;
        event.label = "Backspace";
        return event;
    }

    if (scancode == 0x0F) {
        event.type = KEY_TAB;
        event.code = 9;
        event.has_code = 1;
        event.label = "Tab";
        return event;
    }

    event.ch = scancode_to_ascii(scancode, shift_down);
    if (event.ch != 0) {
        event.type = KEY_CHAR;
        event.code = (uint8_t)event.ch;
        event.has_code = 1;
    }

    return event;
}

static void keyboard_event_push(struct key_event event)
{
    uint32_t next = (kb_head + 1) % KB_EVENT_QUEUE_SZ;

    if (next == kb_tail)
        return;

    kb_queue[kb_head] = event;
    kb_head = next;
}

static int keyboard_event_pop(struct key_event *event)
{
    int has_event = 0;
    uint32_t flags;

    __asm__ volatile ("pushf; pop %0" : "=r"(flags));
    interrupts_disable();
    if (kb_tail != kb_head) {
        *event = kb_queue[kb_tail];
        kb_tail = (kb_tail + 1) % KB_EVENT_QUEUE_SZ;
        has_event = 1;
    }
    if (flags & (1u << 9))
        interrupts_enable();

    return has_event;
}

static struct key_event keyboard_poll_event(void)
{
    struct key_event event;
    uint8_t status;

    event.type = KEY_NONE;
    event.ch = 0;
    event.code = 0;
    event.has_code = 0;
    event.label = 0;

    status = inb(PS2_STATUS_PORT);
    if ((status & 0x01) == 0)
        return event;

    return keyboard_decode(inb(PS2_DATA_PORT));
}

void kernel_main(void)
{
    struct key_event event;

    interrupts_disable();
    /* Keep advanced subsystems compiled but disabled until IRQ path is stabilized. */
    scheduler_init();

    vga_clear();
    boot_marker(0, "BOOT 1: kernel_main entered");
    boot_marker(1, "BOOT 2: scheduler initialized");

    draw_logo();
    boot_marker(2, "BOOT 3: logo drawn");
    draw_ui();
    boot_marker(3, "BOOT 4: ui drawn");
    cursor_disable();
    boot_marker(4, "BOOT 5: cursor disabled");
    term_prompt();
    boot_marker(5, "BOOT 6: prompt ready");
    draw_status(&(struct key_event){ KEY_NONE, 0, 0, 0, "ready" });
    boot_marker(6, "BOOT 7: entering main loop");

    for (;;) {
        event = keyboard_poll_event();

        switch (event.type) {
        case KEY_CHAR:
            input_insert_char(event.ch);
            break;
        case KEY_ENTER:
            submit_input();
            break;
        case KEY_BACKSPACE:
            input_backspace();
            break;
        case KEY_TAB:
            input_tab();
            break;
        case KEY_LEFT:
            input_move_left();
            break;
        case KEY_RIGHT:
            input_move_right();
            break;
        case KEY_UP:
            input_move_home();
            break;
        case KEY_DOWN:
            input_move_end();
            break;
        case KEY_CAPSLOCK:
            break;
        case KEY_NONE:
        default:
            cursor_tick();
            __asm__ volatile ("pause");
            continue;
        }

        draw_status(&event);
    }
}
