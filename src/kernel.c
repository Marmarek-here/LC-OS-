/*
 * kernel.c — LC(OS) kernel entry
 * Freestanding C; no standard library.
 * VGA terminal with polled keyboard input.
 */

#include "commands/builtin_commands.h"
#include "games.h"
#include "gdt.h"
#include "os_api.h"
#include "programs/program_registry.h"
#include "reboot_state.h"
#include "scheduler.h"
#include "shell.h"
#include "types.h"
#include "vga.h"
#include "filesystem_entries.h"
#include "filesystem_data.h"

#define VGA_ATTR_LOGO      0x0F
#define VGA_ATTR_TEXT      0x07
#define VGA_ATTR_MUTED     0x07
#define VGA_ATTR_STATUS    0x0A
#define VGA_ATTR_PROMPT    0x0E
#define VGA_ATTR_DIR       0x0E
#define VGA_ATTR_FILE      0x0B
#define PROMPT_SIZE        2
#define CURSOR_BLINK_DELAY 3000000
#define PS2_DATA_PORT      0x60
#define PS2_STATUS_PORT    0x64
#define CMOS_INDEX_PORT    0x70
#define CMOS_DATA_PORT     0x71

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289
#define MULTIBOOT2_TAG_TYPE_END          0
#define MULTIBOOT2_TAG_TYPE_MODULE        3
#define MULTIBOOT2_TAG_TYPE_MMAP          6
#define MULTIBOOT2_TAG_TYPE_VBE           7
#define MULTIBOOT2_TAG_TYPE_EFI32         11
#define MULTIBOOT2_TAG_TYPE_EFI64         12
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER   8

#define VGA_CRTC_INDEX     0x3D4
#define VGA_CRTC_DATA      0x3D5

static uint32_t ui_status_row = 10;
static uint32_t term_top_row = 11;
static uint32_t term_bottom_row = 24;

static uint32_t term_row = 11;
static uint32_t term_col = 0;
static uint32_t input_start_row = 11;
static uint32_t input_rendered_rows = 1;
static int caps_lock_enabled = 0;
static uint8_t cursor_visible = 0;
static uint16_t cursor_saved_cell = 0;
static uint32_t cursor_saved_row = 11;
static uint32_t cursor_saved_col = PROMPT_SIZE;
static uint32_t cursor_blink_ticks = 0;
static int cpu_x86_64_capable = 0;
static int cpu_real_mode_compatible = 1;
static int video_vbe_available = 0;
static int booted_via_uefi = 0;
static uint32_t boot_usable_region_base = 0;
static uint32_t boot_usable_region_size = 0;
static uint32_t boot_usable_ram_kib = 0;
static uint32_t boot_reserved_ram_kib = 0;
static uint32_t boot_acpi_ram_kib = 0;

static const uint8_t *fat12_image = 0;
static uint32_t fat12_image_size = 0;
static char fat12_file_buffer[2048];

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

#define LCFS_MAGIC0        'L'
#define LCFS_MAGIC1        'C'
#define LCFS_MAGIC2        'F'
#define LCFS_MAGIC3        'S'
#define LCFS_MAGIC4        'V'
#define LCFS_MAGIC5        '0'
#define LCFS_MAGIC6        '1'
#define LCFS_MAGIC7        '\0'
#define LCFS_VERSION       1u
#define LCFS_REC_DIR       1u
#define LCFS_REC_FILE      2u
#define FS_PERSIST_SECTORS 96u
#define FS_PERSIST_BYTES   (FS_PERSIST_SECTORS * 512u)

struct lcfs_header {
    uint8_t magic[8];
    uint32_t version;
    uint32_t total_size;
    uint32_t dir_count;
    uint32_t file_count;
    uint32_t checksum;
};

static int fs_persist_enabled = 0;
static int fs_persist_ready = 0;
static int fs_persist_faulted = 0;
static uint8_t fs_persist_bytes[FS_PERSIST_BYTES];

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

static volatile int mouse_x = 0;
static volatile int mouse_y = 0;
static volatile uint8_t mouse_buttons = 0;
static volatile uint8_t mouse_prev_buttons = 0;
static volatile uint8_t mouse_packet[3];
static volatile uint32_t mouse_packet_index = 0;
static volatile int mouse_left_click_pending = 0;
static volatile int mouse_click_x = 0;
static volatile int mouse_click_y = 0;

static char pipe_input_buffer[2048];
static int term_capture_active = 0;
static char term_capture_buffer[4096];
static uint32_t term_capture_len = 0;

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

struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct multiboot2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[1];
} __attribute__((packed));

struct multiboot2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint32_t framebuffer_addr_lo;
    uint32_t framebuffer_addr_hi;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type; /* 1 = RGB, 2 = EGA text */
    uint16_t reserved;
} __attribute__((packed));

struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed));

struct multiboot2_mmap_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len_lo;
    uint32_t len_hi;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct fat12_context {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_dir_sectors;
    uint32_t data_sectors;
    uint32_t cluster_count;
    uint32_t first_root_sector;
    uint32_t first_data_sector;
    uint8_t fat_type;
    uint16_t eoc_marker;
};

struct fat12_entry {
    uint8_t attr;
    uint16_t first_cluster;
    uint32_t size;
};

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern void (*exception_stub_table[])(void);
extern void interrupt_ignore_stub(void);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);

static struct key_event keyboard_decode(uint8_t raw_scancode);
static void keyboard_event_push(struct key_event event);
static struct key_event keyboard_poll_event(void);
static const char *fat12_get_file_content(const char *path);
static void cursor_disable(void);
static void u32_to_hex(uint32_t value, char *buffer);
static uint32_t str_len(const char *s);
static void str_copy(char *dst, const char *src);
static void u32_to_dec(uint32_t value, char *buffer);

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait(void)
{
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static int ps2_wait_can_write(void)
{
    uint32_t timeout = 100000u;
    while (timeout--) {
        if ((inb(PS2_STATUS_PORT) & 0x02u) == 0u)
            return 1;
    }
    return 0;
}

static int ps2_wait_can_read(void)
{
    uint32_t timeout = 100000u;
    while (timeout--) {
        if (inb(PS2_STATUS_PORT) & 0x01u)
            return 1;
    }
    return 0;
}

static int ps2_mouse_write(uint8_t value)
{
    if (!ps2_wait_can_write())
        return 0;
    outb(PS2_STATUS_PORT, 0xD4u);
    if (!ps2_wait_can_write())
        return 0;
    outb(PS2_DATA_PORT, value);
    return 1;
}

static int ps2_mouse_read_ack(void)
{
    uint8_t data;
    if (!ps2_wait_can_read())
        return 0;
    data = inb(PS2_DATA_PORT);
    return data == 0xFAu;
}

static int mouse_apply_sensitivity(int delta)
{
    if (delta > 0)
        return (delta + 1) / 2;
    if (delta < 0)
        return (delta - 1) / 2;
    return 0;
}

static void mouse_init(void)
{
    uint8_t status;
    uint32_t cols = vga_is_framebuffer() ? vga_fb_width() : vga_cols();
    uint32_t rows = vga_is_framebuffer() ? vga_fb_height() : vga_rows();

    if (cols == 0)
        cols = 80;
    if (rows == 0)
        rows = 25;

    mouse_x = (int)(cols / 2u);
    mouse_y = (int)(rows / 2u);
    mouse_buttons = 0;
    mouse_prev_buttons = 0;
    mouse_packet_index = 0;
    mouse_left_click_pending = 0;

    if (!ps2_wait_can_write())
        return;
    outb(PS2_STATUS_PORT, 0xA8u);

    if (!ps2_wait_can_write())
        return;
    outb(PS2_STATUS_PORT, 0x20u);
    if (!ps2_wait_can_read())
        return;
    status = inb(PS2_DATA_PORT);

    status |= 0x02u;
    status &= (uint8_t)~0x20u;

    if (!ps2_wait_can_write())
        return;
    outb(PS2_STATUS_PORT, 0x60u);
    if (!ps2_wait_can_write())
        return;
    outb(PS2_DATA_PORT, status);

    if (!ps2_mouse_write(0xF6u))
        return;
    if (!ps2_mouse_read_ack())
        return;

    if (!ps2_mouse_write(0xF4u))
        return;
    ps2_mouse_read_ack();
}

static int ata_wait_not_busy(uint16_t io_base)
{
    uint32_t timeout = 200000u;
    while (timeout--) {
        uint8_t status = inb((uint16_t)(io_base + 7));
        if ((status & 0x80u) == 0)
            return 1;
    }
    return 0;
}

static int ata_wait_drq(uint16_t io_base)
{
    uint32_t timeout = 200000u;
    while (timeout--) {
        uint8_t status = inb((uint16_t)(io_base + 7));
        if (status & 0x01u)
            return 0;
        if ((status & 0x80u) == 0 && (status & 0x08u))
            return 1;
    }
    return 0;
}

static int ata_pio_read_sector(uint32_t lba, uint8_t *out)
{
    uint16_t io = 0x1F0;
    uint32_t i;

    if (lba >= 0x10000000u)
        return 0;

    if (!ata_wait_not_busy(io))
        return 0;

    outb((uint16_t)(io + 6), (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    outb((uint16_t)(io + 2), 1u);
    outb((uint16_t)(io + 3), (uint8_t)(lba & 0xFFu));
    outb((uint16_t)(io + 4), (uint8_t)((lba >> 8) & 0xFFu));
    outb((uint16_t)(io + 5), (uint8_t)((lba >> 16) & 0xFFu));
    outb((uint16_t)(io + 7), 0x20u);

    if (!ata_wait_drq(io))
        return 0;

    for (i = 0; i < 256u; i++) {
        uint16_t w = inw(io);
        out[i * 2u] = (uint8_t)(w & 0xFFu);
        out[i * 2u + 1u] = (uint8_t)((w >> 8) & 0xFFu);
    }

    return 1;
}

static int ata_pio_write_sector(uint32_t lba, const uint8_t *in)
{
    uint16_t io = 0x1F0;
    uint32_t i;

    if (lba >= 0x10000000u)
        return 0;

    if (!ata_wait_not_busy(io))
        return 0;

    outb((uint16_t)(io + 6), (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    outb((uint16_t)(io + 2), 1u);
    outb((uint16_t)(io + 3), (uint8_t)(lba & 0xFFu));
    outb((uint16_t)(io + 4), (uint8_t)((lba >> 8) & 0xFFu));
    outb((uint16_t)(io + 5), (uint8_t)((lba >> 16) & 0xFFu));
    outb((uint16_t)(io + 7), 0x30u);

    if (!ata_wait_drq(io))
        return 0;

    for (i = 0; i < 256u; i++) {
        uint16_t w = (uint16_t)in[i * 2u] | (uint16_t)(in[i * 2u + 1u] << 8);
        outw(io, w);
    }

    outb((uint16_t)(io + 7), 0xE7u);
    if (!ata_wait_not_busy(io))
        return 0;

    return 1;
}

static int ata_identify_total_sectors(uint32_t *total)
{
    uint16_t io = 0x1F0;
    uint16_t words[256];
    uint32_t i;
    uint8_t status;

    outb((uint16_t)(io + 6), 0xA0u);
    outb((uint16_t)(io + 2), 0u);
    outb((uint16_t)(io + 3), 0u);
    outb((uint16_t)(io + 4), 0u);
    outb((uint16_t)(io + 5), 0u);
    outb((uint16_t)(io + 7), 0xECu);

    status = inb((uint16_t)(io + 7));
    if (status == 0u || status == 0xFFu)
        return 0;

    if (!ata_wait_drq(io))
        return 0;

    for (i = 0; i < 256u; i++)
        words[i] = inw(io);

    *total = ((uint32_t)words[61] << 16) | (uint32_t)words[60];
    if (*total == 0u)
        return 0;

    return 1;
}

static int fs_persist_magic_ok(const uint8_t *m)
{
    return m[0] == LCFS_MAGIC0 && m[1] == LCFS_MAGIC1 &&
           m[2] == LCFS_MAGIC2 && m[3] == LCFS_MAGIC3 &&
           m[4] == LCFS_MAGIC4 && m[5] == LCFS_MAGIC5 &&
           m[6] == LCFS_MAGIC6 && m[7] == LCFS_MAGIC7;
}

static int fs_persist_region_is_zero(const uint8_t *blk)
{
    uint32_t i;
    for (i = 0; i < 512u; i++) {
        if (blk[i] != 0u)
            return 0;
    }
    return 1;
}

static void lcfs_write_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t lcfs_read_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static void lcfs_write_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t lcfs_read_u32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint32_t lcfs_checksum(const uint8_t *bytes, uint32_t size)
{
    uint32_t sum = 0;
    uint32_t i;
    for (i = 0; i < size; i++)
        sum = (sum << 5) - sum + (uint32_t)bytes[i];
    return sum;
}

static int fs_persist_read_bytes(uint8_t *dst)
{
    uint32_t total;
    uint32_t start_lba;
    uint32_t i;
    uint8_t first[512];

    if (!ata_identify_total_sectors(&total))
        return 0;
    if (total <= FS_PERSIST_SECTORS)
        return 0;

    start_lba = total - FS_PERSIST_SECTORS;
    if (!ata_pio_read_sector(start_lba, first))
        return 0;

    if (!fs_persist_magic_ok(first)) {
        if (!fs_persist_region_is_zero(first))
            return 0;
        fs_persist_enabled = 1;
        return 1;
    }

    for (i = 0; i < FS_PERSIST_SECTORS; i++) {
        uint32_t j;
        uint8_t blk[512];
        if (!ata_pio_read_sector(start_lba + i, blk))
            return 0;
        for (j = 0; j < 512u; j++)
            dst[i * 512u + j] = blk[j];
    }

    fs_persist_enabled = 1;
    return 1;
}

static int fs_persist_write_bytes(const uint8_t *src)
{
    uint32_t total;
    uint32_t start_lba;
    uint32_t i;
    uint32_t j;

    if (!fs_persist_enabled || fs_persist_faulted)
        return 0;

    if (!ata_identify_total_sectors(&total))
        return 0;
    if (total <= FS_PERSIST_SECTORS)
        return 0;

    start_lba = total - FS_PERSIST_SECTORS;

    for (i = 0; i < FS_PERSIST_SECTORS; i++) {
        uint8_t blk[512];
        for (j = 0; j < 512u; j++)
            blk[j] = src[i * 512u + j];
        if (!ata_pio_write_sector(start_lba + i, blk)) {
            fs_persist_faulted = 1;
            return 0;
        }
    }

    return 1;
}

static int lcfs_serialize(uint8_t *out, uint32_t out_cap)
{
    struct lcfs_header hdr;
    uint32_t off = (uint32_t)sizeof(struct lcfs_header);
    uint32_t i;
    uint32_t j;

    if (out_cap < sizeof(struct lcfs_header))
        return 0;

    for (i = 0; i < out_cap; i++)
        out[i] = 0u;

    hdr.magic[0] = LCFS_MAGIC0;
    hdr.magic[1] = LCFS_MAGIC1;
    hdr.magic[2] = LCFS_MAGIC2;
    hdr.magic[3] = LCFS_MAGIC3;
    hdr.magic[4] = LCFS_MAGIC4;
    hdr.magic[5] = LCFS_MAGIC5;
    hdr.magic[6] = LCFS_MAGIC6;
    hdr.magic[7] = LCFS_MAGIC7;
    hdr.version = LCFS_VERSION;
    hdr.total_size = (uint32_t)sizeof(struct lcfs_header);
    hdr.dir_count = dir_count;
    hdr.file_count = file_count;
    hdr.checksum = 0u;

    for (i = 0; i < dir_count && i < MAX_DIRS; i++) {
        uint32_t path_len = 0;
        while (directories[i][path_len] && path_len < INPUT_MAX)
            path_len++;

        if (off + 2u + path_len > out_cap)
            return 0;

        out[off++] = (uint8_t)LCFS_REC_DIR;
        out[off++] = (uint8_t)path_len;
        for (j = 0; j < path_len; j++)
            out[off++] = (uint8_t)directories[i][j];
    }

    for (i = 0; i < file_count && i < MAX_FILES; i++) {
        uint32_t path_len = 0;
        uint32_t content_len = in_memory_files[i].content_len;

        while (in_memory_files[i].path[path_len] && path_len < INPUT_MAX)
            path_len++;
        if (content_len > sizeof(in_memory_files[i].content))
            content_len = sizeof(in_memory_files[i].content);

        if (off + 4u + path_len + content_len > out_cap)
            return 0;

        out[off++] = (uint8_t)LCFS_REC_FILE;
        out[off++] = (uint8_t)path_len;
        for (j = 0; j < path_len; j++)
            out[off++] = (uint8_t)in_memory_files[i].path[j];
        lcfs_write_u16(&out[off], (uint16_t)content_len);
        off += 2u;
        for (j = 0; j < content_len; j++)
            out[off++] = (uint8_t)in_memory_files[i].content[j];
    }

    hdr.total_size = off;
    hdr.checksum = lcfs_checksum(out + sizeof(struct lcfs_header), off - (uint32_t)sizeof(struct lcfs_header));

    for (i = 0; i < 8u; i++)
        out[i] = hdr.magic[i];
    lcfs_write_u32(out + 8u, hdr.version);
    lcfs_write_u32(out + 12u, hdr.total_size);
    lcfs_write_u32(out + 16u, hdr.dir_count);
    lcfs_write_u32(out + 20u, hdr.file_count);
    lcfs_write_u32(out + 24u, hdr.checksum);

    return 1;
}

static int lcfs_deserialize(const uint8_t *in, uint32_t in_size)
{
    uint32_t off = (uint32_t)sizeof(struct lcfs_header);
    uint32_t total_size;
    uint32_t dir_target;
    uint32_t file_target;
    uint32_t checksum_stored;
    uint32_t checksum_calc;

    if (in_size < sizeof(struct lcfs_header))
        return 0;
    if (!fs_persist_magic_ok(in))
        return 0;

    if (lcfs_read_u32(in + 8u) != LCFS_VERSION)
        return 0;

    total_size = lcfs_read_u32(in + 12u);
    dir_target = lcfs_read_u32(in + 16u);
    file_target = lcfs_read_u32(in + 20u);
    checksum_stored = lcfs_read_u32(in + 24u);

    if (total_size < sizeof(struct lcfs_header) || total_size > in_size)
        return 0;

    checksum_calc = lcfs_checksum(in + sizeof(struct lcfs_header), total_size - (uint32_t)sizeof(struct lcfs_header));
    if (checksum_calc != checksum_stored)
        return 0;

    dir_count = 0;
    file_count = 0;

    while (off < total_size) {
        uint8_t rec_type;
        uint8_t path_len;
        uint32_t path_len_u32;
        uint32_t i;

        if (off + 2u > total_size)
            return 0;

        rec_type = in[off++];
        path_len = in[off++];
        path_len_u32 = (uint32_t)path_len;

        if (path_len_u32 > INPUT_MAX)
            return 0;

        if (rec_type == LCFS_REC_DIR) {
            if (off + path_len_u32 > total_size)
                return 0;
            if (dir_count >= MAX_DIRS)
                return 0;
            for (i = 0; i < path_len_u32; i++)
                directories[dir_count][i] = (char)in[off + i];
            directories[dir_count][path_len_u32] = '\0';
            off += path_len_u32;
            dir_count++;
            continue;
        }

        if (rec_type == LCFS_REC_FILE) {
            uint16_t content_len;
            if (off + path_len_u32 + 2u > total_size)
                return 0;
            if (file_count >= MAX_FILES)
                return 0;
            for (i = 0; i < path_len_u32; i++)
                in_memory_files[file_count].path[i] = (char)in[off + i];
            in_memory_files[file_count].path[path_len_u32] = '\0';
            off += path_len_u32;

            content_len = lcfs_read_u16(in + off);
            off += 2u;

            if ((uint32_t)content_len + off > total_size)
                return 0;
            if (content_len >= sizeof(in_memory_files[file_count].content))
                content_len = (uint16_t)(sizeof(in_memory_files[file_count].content) - 1u);

            for (i = 0; i < (uint32_t)content_len; i++)
                in_memory_files[file_count].content[i] = (char)in[off + i];
            in_memory_files[file_count].content[content_len] = '\0';
            in_memory_files[file_count].content_len = (uint32_t)content_len;
            off += lcfs_read_u16(in + off - 2u);
            file_count++;
            continue;
        }

        return 0;
    }

    if (dir_count != dir_target || file_count != file_target)
        return 0;

    return 1;
}

static void fs_persist_save_current(void)
{
    if (fs_persist_ready == 0 || fs_persist_faulted)
        return;

    if (!lcfs_serialize(fs_persist_bytes, FS_PERSIST_BYTES)) {
        fs_persist_faulted = 1;
        return;
    }

    fs_persist_write_bytes(fs_persist_bytes);
}

static void fs_persist_load_current(void)
{
    uint32_t i;

    dir_count = 0;
    file_count = 0;
    for (i = 0; i < MAX_DIRS; i++)
        directories[i][0] = '\0';
    for (i = 0; i < MAX_FILES; i++) {
        in_memory_files[i].path[0] = '\0';
        in_memory_files[i].content[0] = '\0';
        in_memory_files[i].content_len = 0;
    }

    if (fs_persist_read_bytes(fs_persist_bytes)) {
        if (fs_persist_magic_ok(fs_persist_bytes)) {
            if (!lcfs_deserialize(fs_persist_bytes, FS_PERSIST_BYTES)) {
                dir_count = 0;
                file_count = 0;
            }
        }
    }

    fs_persist_ready = 1;
}

static inline void interrupts_enable(void)
{
    __asm__ volatile ("sti");
}

static inline void interrupts_disable(void)
{
    __asm__ volatile ("cli");
}

static uint32_t term_cols(void)
{
    uint32_t cols = vga_cols();
    if (cols == 0)
        return 80;
    return cols;
}

static uint32_t term_rows(void)
{
    uint32_t rows = vga_rows();
    if (rows == 0)
        return 25;
    return rows;
}

static void init_layout(void)
{
    uint32_t rows = term_rows();

    if (rows >= 18) {
        /* Keep logo visible while giving the shell a larger viewport. */
        ui_status_row = 6;
        term_top_row = 7;
    } else if (rows >= 12) {
        ui_status_row = 4;
        term_top_row = 5;
    } else if (rows >= 6) {
        ui_status_row = rows - 4;
        term_top_row = rows - 3;
    } else {
        ui_status_row = rows - 1;
        term_top_row = rows - 1;
    }

    term_bottom_row = rows - 1;
    if (term_top_row > term_bottom_row)
        term_top_row = term_bottom_row;

    term_row = term_top_row;
    input_start_row = term_top_row;
    cursor_saved_row = term_top_row;
}

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf));
}

static int cpu_has_long_mode(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    cpuid(0x80000000u, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000001u)
        return 0;

    cpuid(0x80000001u, &eax, &ebx, &ecx, &edx);
    return (edx & (1u << 29)) != 0;
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
    uint16_t code_selector;

    __asm__ volatile ("mov %%cs, %0" : "=r"(code_selector));

    for (i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, (uint32_t)exception_stub_table[i], code_selector, 0x8E);

    for (i = 32; i < IDT_ENTRIES; i++)
        idt_set_gate((uint8_t)i, (uint32_t)interrupt_ignore_stub, code_selector, 0x8E);

    idt_set_gate(0x20, (uint32_t)irq0_stub, code_selector, 0x8E);
    idt_set_gate(0x21, (uint32_t)irq1_stub, code_selector, 0x8E);
    idt_set_gate(0x2C, (uint32_t)irq12_stub, code_selector, 0x8E);

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base = (uint32_t)idt;

    __asm__ volatile ("lidt %0" : : "m"(idtp));

    pic_remap();
    /* Unmask IRQ0, IRQ1, IRQ2(cascade) and IRQ12(mouse). */
    outb(PIC1_DATA, 0xF8);
    outb(PIC2_DATA, 0xEF);
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

void irq12_handler_c(void)
{
    uint8_t data = inb(PS2_DATA_PORT);

    if (mouse_packet_index == 0 && (data & 0x08u) == 0u) {
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    mouse_packet[mouse_packet_index++] = data;

    if (mouse_packet_index == 3u) {
        int dx = mouse_apply_sensitivity((int)((signed char)mouse_packet[1]));
        int dy = mouse_apply_sensitivity((int)((signed char)mouse_packet[2]));
        uint8_t buttons = (uint8_t)(mouse_packet[0] & 0x07u);
        int max_x = (int)(vga_is_framebuffer() ? vga_fb_width() : term_cols()) - 1;
        int max_y = (int)(vga_is_framebuffer() ? vga_fb_height() : term_rows()) - 1;

        mouse_packet_index = 0;

        mouse_x += dx;
        mouse_y -= dy;

        if (mouse_x < 0)
            mouse_x = 0;
        if (mouse_y < 0)
            mouse_y = 0;
        if (mouse_x > max_x)
            mouse_x = max_x;
        if (mouse_y > max_y)
            mouse_y = max_y;

        if ((buttons & 0x01u) != 0u && (mouse_prev_buttons & 0x01u) == 0u) {
            mouse_left_click_pending = 1;
            mouse_click_x = mouse_x;
            mouse_click_y = mouse_y;
        }

        mouse_buttons = buttons;
        mouse_prev_buttons = buttons;
    }

    outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void exception_handler_c(uint32_t vector, uint32_t error_code)
{
    static const char *const exception_names[32] = {
        "Divide Error",
        "Debug",
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "BOUND Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 Floating-Point Exception",
        "Alignment Check",
        "Machine Check",
        "SIMD Floating-Point Exception",
        "Virtualization Exception",
        "Control Protection Exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor Injection Exception",
        "VMM Communication Exception",
        "Security Exception",
        "Reserved"
    };
    char vector_hex[11];
    char error_hex[11];

    interrupts_disable();
    cursor_disable();
    vga_clear();

    u32_to_hex(vector, vector_hex);
    u32_to_hex(error_code, error_hex);

    {
        uint32_t row;
        for (row = 0; row < term_rows(); row++)
            vga_clear_row(row, 0x4F);
    }

    vga_puts_at("KERNEL PANIC", 0x4F, 2, 2);
    vga_puts_at("Unhandled CPU exception", 0x4F, 3, 2);
    if (vector < 32)
        vga_puts_at(exception_names[vector], 0x4F, 5, 2);
    else
        vga_puts_at("Unknown exception", 0x4F, 5, 2);
    vga_puts_at("Vector:", 0x4F, 7, 2);
    vga_puts_at(vector_hex, 0x4F, 7, 10);
    vga_puts_at("Error:", 0x4F, 8, 2);
    vga_puts_at(error_hex, 0x4F, 8, 10);
    vga_puts_at("System halted to avoid data corruption.", 0x4F, 10, 2);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static uint32_t str_len(const char *s)
{
    uint32_t length = 0;

    while (s[length])
        length++;

    return length;
}

static uint32_t text_len(const char *s)
{
    uint32_t n = 0;

    while (s[n])
        n++;

    return n;
}

static void u32_to_dec(uint32_t value, char *buffer)
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
        tmp[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    for (i = 0; i < count; i++)
        buffer[i] = tmp[count - i - 1];

    buffer[count] = '\0';
}

static void u32_to_hex(uint32_t value, char *buffer)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    buffer[0] = '0';
    buffer[1] = 'x';

    for (shift = 0; shift < 8; shift++) {
        uint32_t nibble = (value >> ((7 - shift) * 4)) & 0x0Fu;
        buffer[shift + 2] = digits[nibble];
    }

    buffer[10] = '\0';
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX_PORT, (uint8_t)(reg | 0x80));
    return inb(CMOS_DATA_PORT);
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10u) + (value & 0x0Fu));
}

static void write_two_digits(char *out, uint32_t *index, uint8_t value)
{
    out[*index] = (char)('0' + (value / 10u));
    (*index)++;
    out[*index] = (char)('0' + (value % 10u));
    (*index)++;
}

static void rtc_read(uint8_t *second,
                     uint8_t *minute,
                     uint8_t *hour,
                     uint8_t *day,
                     uint8_t *month,
                     uint8_t *year)
{
    uint8_t reg_b;
    uint8_t pm_flag;
    uint8_t sec1;
    uint8_t min1;
    uint8_t hour1;
    uint8_t day1;
    uint8_t month1;
    uint8_t year1;
    uint8_t sec2;
    uint8_t min2;
    uint8_t hour2;
    uint8_t day2;
    uint8_t month2;
    uint8_t year2;

    do {
        while (cmos_read(0x0A) & 0x80)
            ;

        sec1 = cmos_read(0x00);
        min1 = cmos_read(0x02);
        hour1 = cmos_read(0x04);
        day1 = cmos_read(0x07);
        month1 = cmos_read(0x08);
        year1 = cmos_read(0x09);

        while (cmos_read(0x0A) & 0x80)
            ;

        sec2 = cmos_read(0x00);
        min2 = cmos_read(0x02);
        hour2 = cmos_read(0x04);
        day2 = cmos_read(0x07);
        month2 = cmos_read(0x08);
        year2 = cmos_read(0x09);
    } while (sec1 != sec2 || min1 != min2 || hour1 != hour2 || day1 != day2 || month1 != month2 || year1 != year2);

    reg_b = cmos_read(0x0B);

    pm_flag = (uint8_t)(hour1 & 0x80);

    if ((reg_b & 0x04) == 0) {
        sec1 = bcd_to_bin(sec1);
        min1 = bcd_to_bin(min1);
        hour1 = bcd_to_bin((uint8_t)(hour1 & 0x7F));
        day1 = bcd_to_bin(day1);
        month1 = bcd_to_bin(month1);
        year1 = bcd_to_bin(year1);
    } else {
        hour1 = (uint8_t)(hour1 & 0x7F);
    }

    if ((reg_b & 0x02) == 0) {
        if (pm_flag && hour1 < 12)
            hour1 = (uint8_t)(hour1 + 12);
        if (!pm_flag && hour1 == 12)
            hour1 = 0;
    }

    *second = sec1;
    *minute = min1;
    *hour = hour1;
    *day = day1;
    *month = month1;
    *year = year1;
}

static int rtc_values_valid(uint8_t second,
                            uint8_t minute,
                            uint8_t hour,
                            uint8_t day,
                            uint8_t month)
{
    if (second > 59)
        return 0;
    if (minute > 59)
        return 0;
    if (hour > 23)
        return 0;
    if (month == 0 || month > 12)
        return 0;
    if (day == 0 || day > 31)
        return 0;

    return 1;
}

void os_time_get_date(char *out, uint32_t out_size)
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint32_t i = 0;

    if (out_size < 20) {
        if (out_size > 0)
            out[0] = '\0';
        return;
    }

    uint32_t tries = 0;

    while (tries < 8) {
        rtc_read(&second, &minute, &hour, &day, &month, &year);
        if (rtc_values_valid(second, minute, hour, day, month))
            break;
        tries++;
    }

    if (tries == 8) {
        const char *fallback = "RTC unavailable";
        uint32_t k = 0;

        while (fallback[k] && k + 1 < out_size) {
            out[k] = fallback[k];
            k++;
        }
        out[k] = '\0';
        return;
    }

    write_two_digits(out, &i, day);
    out[i++] = '-';
    write_two_digits(out, &i, month);
    out[i++] = '-';
    out[i++] = '2';
    out[i++] = '0';
    write_two_digits(out, &i, year);
    out[i++] = ' ';
    write_two_digits(out, &i, hour);
    out[i++] = ':';
    write_two_digits(out, &i, minute);
    out[i++] = ':';
    write_two_digits(out, &i, second);
    out[i] = '\0';
}

static void cursor_disable(void)
{
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
}

static void input_linear_to_row_col(uint32_t linear_index, uint32_t *row, uint32_t *col)
{
    uint32_t cols = term_cols();

    *row = input_start_row + (linear_index / cols);
    *col = linear_index % cols;

    if (*row > term_bottom_row) {
        *row = term_bottom_row;
        *col = cols - 1;
    }
}

static void cursor_position(uint32_t *row, uint32_t *col)
{
    uint32_t linear = shell_prompt_length() + shell_input_cursor();

    input_linear_to_row_col(linear, row, col);
}

static void cursor_hide(void)
{
    if (!cursor_visible)
        return;

    vga_set_cell(cursor_saved_row, cursor_saved_col, cursor_saved_cell);
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
    cell = vga_get_cell(row, col);
    ch = (uint8_t)(cell & 0xFF);
    attr = (uint8_t)((cell >> 8) & 0xFF);

    cursor_saved_row = row;
    cursor_saved_col = col;
    cursor_saved_cell = cell;

    if (ch == ' ')
        vga_set_cell(row, col, vga_entry(' ', 0x70));
    else
        vga_set_cell(row, col, vga_entry((char)ch, (uint8_t)((attr << 4) | (attr >> 4))));

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
    uint32_t cols = term_cols();

    for (row = term_top_row; row < term_bottom_row; row++) {
        for (col = 0; col < cols; col++)
            vga_set_cell(row, col, vga_get_cell(row + 1, col));
    }

    vga_clear_row(term_bottom_row, 0x00);

    if (term_row > term_top_row)
        term_row--;
}

static void term_newline(void)
{
    term_row++;
    term_col = 0;

    if (term_row > term_bottom_row)
        scroll_terminal();
}

static void term_putc(char c, uint8_t attr)
{
    if (c == '\n') {
        term_newline();
        return;
    }

    if (term_col >= term_cols())
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
    uint32_t cols = term_cols();
    uint32_t rows = term_rows();

    for (row = 0; logo[row] != 0 && row < rows; row++) {
        uint32_t width = str_len(logo[row]);
        uint32_t col = (cols > width) ? ((cols - width) / 2) : 0;

        vga_puts_at(logo[row], VGA_ATTR_LOGO, row, col);
    }
}

static void draw_splash_screen(void)
{
    uint32_t start_tick = scheduler_ticks_get();
    vga_clear();
    draw_logo();
    vga_puts_at("LC(OS)", VGA_ATTR_TEXT, term_rows() / 2, 2);
    vga_puts_at("Booting kernel and restoring VFS...", VGA_ATTR_MUTED, term_rows() / 2 + 2, 2);
    while ((scheduler_ticks_get() - start_tick) < 18u)
        __asm__ volatile ("hlt");
}

static void draw_ui(void)
{
    uint32_t col;
    uint32_t cols = term_cols();
    uint32_t border_row = term_top_row > 0 ? term_top_row - 1 : 0;

    if (term_rows() > border_row) {
        for (col = 0; col < cols; col++)
            vga_put_at('-', VGA_ATTR_TEXT, border_row, col);
    }

    vga_clear_row(ui_status_row, 0x00);
}

static void draw_status(const struct key_event *event)
{
    (void)event;

    cursor_hide();

    vga_clear_row(ui_status_row, 0x00);
    vga_puts_at("Caps: ", VGA_ATTR_STATUS, ui_status_row, 0);
    vga_puts_at(caps_lock_enabled ? "ON" : "OFF", VGA_ATTR_STATUS, ui_status_row, 6);
    cursor_show();
}

static void redraw_input_line(void)
{
    uint32_t prompt_len;
    uint32_t input_len;
    uint32_t linear_len;
    uint32_t required_rows;
    uint32_t available_rows;
    uint32_t clear_rows;
    uint32_t row_offset;
    uint32_t linear = 0;
    uint32_t i;
    uint32_t index;

    if (term_row < term_top_row)
        term_row = term_top_row;
    else if (term_row > term_bottom_row)
        term_row = term_bottom_row;

    if (shell_input_length() == 0 && shell_input_cursor() == 0) {
        input_start_row = term_row;
        input_rendered_rows = 1;
    }

    /* If heavy output/scrolling moved the terminal window, re-anchor prompt safely. */
    if (input_start_row < term_top_row || input_start_row > term_bottom_row)
        input_start_row = term_row;

    prompt_len = shell_prompt_length();
    input_len = shell_input_length();
    linear_len = prompt_len + input_len;
    required_rows = linear_len / term_cols();
    if ((linear_len % term_cols()) != 0 || required_rows == 0)
        required_rows++;

    available_rows = term_bottom_row - term_top_row + 1;
    if (required_rows > available_rows)
        required_rows = available_rows;

    if (input_start_row + required_rows - 1 > term_bottom_row)
        input_start_row = term_bottom_row - required_rows + 1;

    clear_rows = input_rendered_rows;
    if (required_rows > clear_rows)
        clear_rows = required_rows;

    cursor_hide();

    for (row_offset = 0; row_offset < clear_rows; row_offset++) {
        uint32_t row = input_start_row + row_offset;
        if (row > term_bottom_row)
            break;
        vga_clear_row(row, 0x00);
    }

    for (index = 0; shell_current_dir()[index] != '\0'; index++) {
        uint32_t row;
        uint32_t col;

        input_linear_to_row_col(linear, &row, &col);
        vga_put_at(shell_current_dir()[index], VGA_ATTR_TEXT, row, col);
        linear++;
    }

    for (index = 0; index < 2; index++) {
        uint32_t row;
        uint32_t col;
        char prompt_ch = (index == 0) ? '>' : ' ';

        input_linear_to_row_col(linear, &row, &col);
        vga_put_at(prompt_ch, VGA_ATTR_TEXT, row, col);
        linear++;
    }

    for (i = 0; i < input_len; i++) {
        uint32_t row;
        uint32_t col;

        input_linear_to_row_col(linear, &row, &col);
        vga_put_at(shell_input_char_at(i), VGA_ATTR_MUTED, row, col);
        linear++;
    }

    term_row = input_start_row + required_rows - 1;
    if (term_row > term_bottom_row)
        term_row = term_bottom_row;

    term_col = linear % term_cols();
    input_rendered_rows = required_rows;

    cursor_reset_blink();
}

static void term_prompt(void)
{
    term_row = term_top_row;
    term_col = 0;
    input_start_row = term_row;
    input_rendered_rows = 1;
    redraw_input_line();
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

static char to_upper_ascii(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        return (char)(ch - ('a' - 'A'));

    return ch;
}

static int str_contains(const char *text, const char *needle)
{
    uint32_t i = 0;

    if (needle[0] == '\0')
        return 1;

    while (text[i]) {
        uint32_t j = 0;

        while (needle[j] && text[i + j] == needle[j])
            j++;

        if (needle[j] == '\0')
            return 1;

        i++;
    }

    return 0;
}

static uint16_t read_u16_le(const uint8_t *ptr)
{
    return (uint16_t)(ptr[0] | (uint16_t)(ptr[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *ptr)
{
    return (uint32_t)ptr[0] |
           ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[3] << 24);
}

static void boot_memory_init_from_multiboot(uint32_t magic, uint32_t info_addr)
{
    const uint8_t *info;
    uint32_t total_size;
    uint32_t offset;

    booted_via_uefi = 0;
    boot_usable_region_base = 0;
    boot_usable_region_size = 0;
    boot_usable_ram_kib = 0;
    boot_reserved_ram_kib = 0;
    boot_acpi_ram_kib = 0;

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC || info_addr == 0)
        return;

    info = (const uint8_t *)info_addr;
    total_size = read_u32_le(info);
    if (total_size < 16)
        return;

    offset = 8;
    while (offset + 8 <= total_size) {
        const struct multiboot2_tag *tag = (const struct multiboot2_tag *)(info + offset);
        uint32_t aligned_size;

        if (tag->type == MULTIBOOT2_TAG_TYPE_END)
            break;
        if (tag->size < 8)
            break;

        if (tag->type == MULTIBOOT2_TAG_TYPE_EFI32 || tag->type == MULTIBOOT2_TAG_TYPE_EFI64)
            booted_via_uefi = 1;

        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            const struct multiboot2_tag_mmap *mmap_tag = (const struct multiboot2_tag_mmap *)tag;
            uint32_t pos = 16;

            if (mmap_tag->entry_size < sizeof(struct multiboot2_mmap_entry))
                goto next_tag;

            while (pos + mmap_tag->entry_size <= tag->size) {
                const struct multiboot2_mmap_entry *entry =
                    (const struct multiboot2_mmap_entry *)((const uint8_t *)tag + pos);
                uint32_t len_kib = entry->len_hi ? 0xFFFFFFFFu : (entry->len_lo >> 10);

                if (entry->type == 1u) {
                    boot_usable_ram_kib += len_kib;
                    if (entry->addr_hi == 0u && entry->len_hi == 0u &&
                        entry->addr_lo >= 0x00100000u && entry->len_lo > boot_usable_region_size) {
                        boot_usable_region_base = entry->addr_lo;
                        boot_usable_region_size = entry->len_lo;
                    }
                } else if (entry->type == 3u || entry->type == 4u) {
                    boot_acpi_ram_kib += len_kib;
                } else {
                    boot_reserved_ram_kib += len_kib;
                }

                pos += mmap_tag->entry_size;
            }
        }

next_tag:
        aligned_size = (tag->size + 7u) & ~7u;
        if (aligned_size == 0u || offset > total_size - aligned_size)
            break;
        offset += aligned_size;
    }
}

static void fb_init_from_multiboot(uint32_t magic, uint32_t info_addr)
{
    const uint8_t *info;
    uint32_t total_size;
    uint32_t offset;

    video_vbe_available = 0;

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC || info_addr == 0)
        return;

    info = (const uint8_t *)info_addr;
    total_size = read_u32_le(info);
    if (total_size < 16)
        return;

    offset = 8;
    while (offset + 8 <= total_size) {
        const struct multiboot2_tag *tag = (const struct multiboot2_tag *)(info + offset);
        uint32_t aligned_size;

        if (tag->type == MULTIBOOT2_TAG_TYPE_END)
            break;

        if (tag->size < 8)
            break;

        if (tag->type == MULTIBOOT2_TAG_TYPE_VBE)
            video_vbe_available = 1;

        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            const struct multiboot2_tag_framebuffer *fbt =
                (const struct multiboot2_tag_framebuffer *)tag;

            /* Only handle RGB framebuffer (type 1) fitting in 32-bit address space */
            if (fbt->framebuffer_type == 1 &&
                fbt->framebuffer_addr_hi == 0 &&
                (fbt->framebuffer_bpp == 24 || fbt->framebuffer_bpp == 32)) {
                vga_set_framebuffer(
                    fbt->framebuffer_addr_lo,
                    fbt->framebuffer_pitch,
                    fbt->framebuffer_width,
                    fbt->framebuffer_height,
                    fbt->framebuffer_bpp);
                video_vbe_available = 1;
            }
            break;
        }

        aligned_size = (tag->size + 7) & ~7u;
        if (aligned_size == 0 || offset > total_size - aligned_size)
            break;
        offset += aligned_size;
    }
}

static void fat12_init_from_multiboot(uint32_t magic, uint32_t info_addr)
{
    const uint8_t *info;
    uint32_t total_size;
    uint32_t offset;

    fat12_image = 0;
    fat12_image_size = 0;

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC || info_addr == 0)
        return;

    info = (const uint8_t *)info_addr;
    total_size = read_u32_le(info);
    if (total_size < 16)
        return;

    offset = 8;
    while (offset + 8 <= total_size) {
        const struct multiboot2_tag *tag = (const struct multiboot2_tag *)(info + offset);
        uint32_t aligned_size;

        if (tag->type == MULTIBOOT2_TAG_TYPE_END)
            break;

        if (tag->size < 8)
            break;

        if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
            const struct multiboot2_tag_module *module = (const struct multiboot2_tag_module *)tag;
            uint32_t module_size = module->mod_end - module->mod_start;

            if (module->mod_end > module->mod_start &&
                (str_contains(module->cmdline, "fsimg") || fat12_image == 0)) {
                fat12_image = (const uint8_t *)module->mod_start;
                fat12_image_size = module_size;
            }
        }

        aligned_size = (tag->size + 7) & ~7u;
        if (aligned_size == 0 || offset > total_size - aligned_size)
            break;
        offset += aligned_size;
    }
}

static int fat12_load_context(struct fat12_context *ctx)
{
    uint16_t total16;
    uint32_t total32;
    uint32_t data_region_start;

    if (fat12_image == 0 || fat12_image_size < 512)
        return 0;

    ctx->bytes_per_sector = read_u16_le(&fat12_image[11]);
    ctx->sectors_per_cluster = fat12_image[13];
    ctx->reserved_sectors = read_u16_le(&fat12_image[14]);
    ctx->fat_count = fat12_image[16];
    ctx->root_entry_count = read_u16_le(&fat12_image[17]);
    total16 = read_u16_le(&fat12_image[19]);
    ctx->sectors_per_fat = read_u16_le(&fat12_image[22]);
    total32 = read_u32_le(&fat12_image[32]);

    if (ctx->bytes_per_sector == 0 || ctx->sectors_per_cluster == 0 ||
        ctx->fat_count == 0 || ctx->sectors_per_fat == 0)
        return 0;

    ctx->total_sectors = total16 ? total16 : total32;
    if (ctx->total_sectors == 0)
        return 0;

    ctx->root_dir_sectors =
        (uint32_t)((ctx->root_entry_count * 32u + (ctx->bytes_per_sector - 1u)) / ctx->bytes_per_sector);
    data_region_start = ctx->reserved_sectors + ((uint32_t)ctx->fat_count * ctx->sectors_per_fat);
    if (ctx->total_sectors <= data_region_start + ctx->root_dir_sectors)
        return 0;

    ctx->first_root_sector = data_region_start;
    ctx->first_data_sector = ctx->first_root_sector + ctx->root_dir_sectors;
    ctx->data_sectors = ctx->total_sectors - ctx->first_data_sector;
    ctx->cluster_count = ctx->data_sectors / ctx->sectors_per_cluster;

    if (ctx->cluster_count < 4085u) {
        ctx->fat_type = 12u;
        ctx->eoc_marker = 0x0FF8u;
    } else if (ctx->cluster_count < 65525u) {
        ctx->fat_type = 16u;
        ctx->eoc_marker = 0xFFF8u;
    } else {
        return 0;
    }

    return 1;
}

static int fat12_sector_ptr(const struct fat12_context *ctx, uint32_t sector, const uint8_t **ptr)
{
    uint32_t offset = sector * (uint32_t)ctx->bytes_per_sector;

    if (offset >= fat12_image_size)
        return 0;
    if (fat12_image_size - offset < ctx->bytes_per_sector)
        return 0;

    *ptr = fat12_image + offset;
    return 1;
}

static int fat12_name_matches(const uint8_t *entry, const char *segment, uint32_t segment_len)
{
    char built_name[13];
    uint32_t out = 0;
    uint32_t i;
    uint32_t dot_needed = 0;

    if (segment_len == 0 || segment_len > 12)
        return 0;

    for (i = 0; i < 8; i++) {
        if (entry[i] == ' ')
            break;
        built_name[out++] = (char)entry[i];
    }

    for (i = 0; i < 3; i++) {
        if (entry[8 + i] != ' ') {
            dot_needed = 1;
            break;
        }
    }

    if (dot_needed)
        built_name[out++] = '.';

    for (i = 0; i < 3; i++) {
        if (entry[8 + i] == ' ')
            break;
        built_name[out++] = (char)entry[8 + i];
    }
    built_name[out] = '\0';

    if (out != segment_len)
        return 0;

    for (i = 0; i < out; i++) {
        if (to_upper_ascii(segment[i]) != to_upper_ascii(built_name[i]))
            return 0;
    }

    return 1;
}

static int fat12_cluster_is_data(const struct fat12_context *ctx, uint16_t cluster)
{
    return cluster >= 2u && cluster < ctx->eoc_marker;
}

static int fat12_next_cluster(const struct fat12_context *ctx, uint16_t cluster, uint16_t *next)
{
    uint32_t fat_base = (uint32_t)ctx->reserved_sectors * ctx->bytes_per_sector;

    if (ctx->fat_type == 16u) {
        uint32_t fat_byte_offset = fat_base + ((uint32_t)cluster * 2u);
        if (fat_byte_offset + 1u >= fat12_image_size)
            return 0;
        *next = read_u16_le(&fat12_image[fat_byte_offset]);
        return 1;
    }

    {
        uint32_t fat_offset = cluster + (cluster / 2u);
        uint32_t fat_byte_offset = fat_base + fat_offset;
        uint16_t value;

        if (fat_byte_offset + 1u >= fat12_image_size)
            return 0;

        value = (uint16_t)(fat12_image[fat_byte_offset] | ((uint16_t)fat12_image[fat_byte_offset + 1u] << 8));
        if (cluster & 1u)
            value >>= 4;
        else
            value &= 0x0FFFu;

        *next = value;
        return 1;
    }
}

static int fat12_find_in_directory(const struct fat12_context *ctx,
                                   uint16_t dir_cluster,
                                   const char *segment,
                                   uint32_t segment_len,
                                   struct fat12_entry *found)
{
    uint32_t entry_index;

    if (dir_cluster == 0) {
        uint32_t root_entries = ctx->root_entry_count;

        for (entry_index = 0; entry_index < root_entries; entry_index++) {
            uint32_t byte_offset = (ctx->first_root_sector * (uint32_t)ctx->bytes_per_sector) + (entry_index * 32u);
            const uint8_t *entry;

            if (byte_offset + 32 > fat12_image_size)
                break;

            entry = fat12_image + byte_offset;
            if (entry[0] == 0x00)
                break;
            if (entry[0] == 0xE5)
                continue;
            if ((entry[11] & 0x0F) == 0x0F)
                continue;
            if (entry[11] & 0x08)
                continue;

            if (fat12_name_matches(entry, segment, segment_len)) {
                found->attr = entry[11];
                found->first_cluster = read_u16_le(&entry[26]);
                found->size = read_u32_le(&entry[28]);
                return 1;
            }
        }

        return 0;
    }

    while (fat12_cluster_is_data(ctx, dir_cluster)) {
        uint32_t first_sector = ctx->first_data_sector +
                                ((uint32_t)(dir_cluster - 2u) * ctx->sectors_per_cluster);
        uint32_t sector;

        for (sector = 0; sector < ctx->sectors_per_cluster; sector++) {
            const uint8_t *sector_ptr;
            uint32_t offset;

            if (!fat12_sector_ptr(ctx, first_sector + sector, &sector_ptr))
                return 0;

            for (offset = 0; offset < ctx->bytes_per_sector; offset += 32) {
                const uint8_t *entry = sector_ptr + offset;

                if (entry[0] == 0x00)
                    return 0;
                if (entry[0] == 0xE5)
                    continue;
                if ((entry[11] & 0x0F) == 0x0F)
                    continue;
                if (entry[11] & 0x08)
                    continue;

                if (fat12_name_matches(entry, segment, segment_len)) {
                    found->attr = entry[11];
                    found->first_cluster = read_u16_le(&entry[26]);
                    found->size = read_u32_le(&entry[28]);
                    return 1;
                }
            }
        }

        if (!fat12_next_cluster(ctx, dir_cluster, &dir_cluster))
            return 0;
    }

    return 0;
}

static int fat12_find_path(const struct fat12_context *ctx, const char *path, struct fat12_entry *entry)
{
    uint16_t current_cluster = 0;
    const char *p = path;

    if (p[0] != '/')
        return 0;

    while (*p == '/')
        p++;

    if (*p == '\0')
        return 0;

    while (*p) {
        const char *segment_start = p;
        uint32_t segment_len = 0;
        int last_segment;

        while (p[segment_len] && p[segment_len] != '/')
            segment_len++;

        last_segment = (p[segment_len] == '\0');

        if (!fat12_find_in_directory(ctx, current_cluster, segment_start, segment_len, entry))
            return 0;

        if (last_segment)
            return 1;

        if ((entry->attr & 0x10) == 0)
            return 0;

        if (entry->first_cluster < 2)
            return 0;

        current_cluster = entry->first_cluster;
        p += segment_len;
        while (*p == '/')
            p++;
    }

    return 0;
}

static const char *fat12_get_file_content(const char *path)
{
    struct fat12_context ctx;
    struct fat12_entry entry;
    uint32_t remaining;
    uint32_t written = 0;
    uint16_t cluster;

    if (!fat12_load_context(&ctx))
        return 0;

    if (!fat12_find_path(&ctx, path, &entry))
        return 0;

    if (entry.attr & 0x10)
        return 0;

    if (entry.size >= sizeof(fat12_file_buffer))
        remaining = sizeof(fat12_file_buffer) - 1;
    else
        remaining = entry.size;

    cluster = entry.first_cluster;
    while (remaining > 0 && fat12_cluster_is_data(&ctx, cluster)) {
        uint32_t first_sector = ctx.first_data_sector +
                                ((uint32_t)(cluster - 2u) * ctx.sectors_per_cluster);
        uint32_t sector;

        for (sector = 0; sector < ctx.sectors_per_cluster && remaining > 0; sector++) {
            const uint8_t *sector_ptr;
            uint32_t copy_count;
            uint32_t i;

            if (!fat12_sector_ptr(&ctx, first_sector + sector, &sector_ptr))
                return 0;

            copy_count = remaining;
            if (copy_count > ctx.bytes_per_sector)
                copy_count = ctx.bytes_per_sector;

            for (i = 0; i < copy_count; i++)
                fat12_file_buffer[written + i] = (char)sector_ptr[i];

            written += copy_count;
            remaining -= copy_count;
        }

        if (remaining == 0)
            break;

        if (!fat12_next_cluster(&ctx, cluster, &cluster))
            return 0;
    }

    fat12_file_buffer[written] = '\0';
    return fat12_file_buffer;
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
    if (find_file_content(filepath) != 0)
        return 1;
    return fat12_get_file_content(filepath) != 0;
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

    if (str_eq(dirpath, "/"))
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
    uint32_t content_len = 0;
    uint32_t path_len = 0;
    uint32_t i;

    if (filepath == 0 || content == 0)
        return 0;

    while (filepath[path_len] && path_len < INPUT_MAX)
        path_len++;
    if (filepath[path_len] != '\0')
        return 0;

    while (content[content_len] && content_len < (uint32_t)(sizeof(in_memory_files[0].content) - 1))
        content_len++;

    if (file) {
        for (i = 0; i < content_len; i++)
            file->content[i] = content[i];
        file->content[content_len] = '\0';
        file->content_len = content_len;
        return 1;
    }

    /* Create new file */
    if (file_count >= MAX_FILES)
        return 0;

    for (i = 0; i < path_len; i++)
        in_memory_files[file_count].path[i] = filepath[i];
    in_memory_files[file_count].path[path_len] = '\0';

    for (i = 0; i < content_len; i++)
        in_memory_files[file_count].content[i] = content[i];
    in_memory_files[file_count].content[content_len] = '\0';
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

static int path_has_forbidden_name_chars(const char *path)
{
    uint32_t i = 0;

    while (path[i]) {
        char c = path[i];

        if (c != '/') {
            if (c == ' ' || c == '"' || c == '\'' || c == '\\' ||
                c == '(' || c == ')' || c == '{' || c == '}' ||
                c == '[' || c == ']') {
                return 1;
            }
        }

        i++;
    }

    return 0;
}

static int resolve_path(const char *arg, char *out)
{
    uint32_t arg_len = str_len(arg);
    const char *current_dir = shell_current_dir();
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

static void print_file_with_size(const char *name, uint32_t size, int show_sizes)
{
    char size_dec[11];

    term_puts(name, VGA_ATTR_FILE);
    if (show_sizes) {
        term_puts(" (", VGA_ATTR_MUTED);
        u32_to_dec(size, size_dec);
        term_puts(size_dec, VGA_ATTR_MUTED);
        term_puts(" B)", VGA_ATTR_MUTED);
    }
    /* Caller is responsible for term_newline() so suffixes (rights) can follow */
}

static uint32_t path_parent_length(const char *path)
{
    uint32_t i = 0;
    uint32_t last_slash = 0;

    while (path[i]) {
        if (path[i] == '/')
            last_slash = i;
        i++;
    }

    return last_slash;
}

static int path_is_under(const char *prefix, const char *path)
{
    uint32_t i = 0;

    if (str_eq(prefix, "/"))
        return path[0] == '/';

    while (prefix[i]) {
        if (path[i] != prefix[i])
            return 0;
        i++;
    }

    return path[i] == '\0' || path[i] == '/';
}

static int path_build_join(const char *left, const char *right_name, char *out)
{
    uint32_t left_len = str_len(left);
    uint32_t right_len = str_len(right_name);
    uint32_t i;
    uint32_t pos = 0;

    if (str_eq(left, "/")) {
        if (1 + right_len > INPUT_MAX)
            return 0;
        out[pos++] = '/';
    } else {
        if (left_len + 1 + right_len > INPUT_MAX)
            return 0;
        for (i = 0; i < left_len; i++)
            out[pos++] = left[i];
        out[pos++] = '/';
    }

    for (i = 0; i < right_len; i++)
        out[pos++] = right_name[i];

    out[pos] = '\0';
    return 1;
}

static const char *get_file_content_any(const char *path)
{
    struct InMemFile *mem_file = find_memory_file(path);
    const char *builtin_file;

    if (mem_file)
        return mem_file->content;

    builtin_file = find_file_content(path);
    if (builtin_file)
        return builtin_file;

    return fat12_get_file_content(path);
}

/* List files and subdirectories in a given directory */
#define LS_MAX_DIRS  128
#define LS_MAX_FILES 256
#define LS_NAME_MAX  64

struct ls_entry {
    char name[LS_NAME_MAX];
    uint32_t size;
    int writable;
    int executable;
};

static void ls_str_copy_truncate(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int ls_str_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char cb = *b >= 'B' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (ca < cb) return -1;
        if (ca > cb) return 1;
        a++;
        b++;
    }
    if (!*a && !*b) return 0;
    return *a ? 1 : -1;
}

static void ls_sort(struct ls_entry *arr, uint32_t count)
{
    uint32_t i, j;
    struct ls_entry tmp;
    for (i = 1; i < count; i++) {
        tmp = arr[i];
        j = i;
        while (j > 0 && ls_str_cmp(arr[j - 1].name, tmp.name) > 0) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = tmp;
    }
}

static int ls_name_has_suffix(const char *name, const char *suffix)
{
    uint32_t nlen = 0, slen = 0, i;
    while (name[nlen]) nlen++;
    while (suffix[slen]) slen++;
    if (nlen < slen) return 0;
    for (i = 0; i < slen; i++) {
        if (name[nlen - slen + i] != suffix[i]) return 0;
    }
    return 1;
}

static int ls_name_has_suffix_ignore_case(const char *name, const char *suffix)
{
    uint32_t nlen = 0, slen = 0, i;
    while (name[nlen]) nlen++;
    while (suffix[slen]) slen++;
    if (nlen < slen) return 0;
    for (i = 0; i < slen; i++) {
        char a = name[nlen - slen + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int ls_is_executable(const char *name)
{
    return ls_name_has_suffix(name, ".lcbat") ||
           ls_name_has_suffix(name, ".bat") ||
           ls_name_has_suffix(name, "snake") ||
           ls_name_has_suffix(name, "tetris") ||
           ls_name_has_suffix(name, "pingpong");
}

static void list_directory_internal(const char *dirpath, int show_sizes, int show_rights)
{
    static struct ls_entry dir_entries[LS_MAX_DIRS];
    static struct ls_entry file_entries[LS_MAX_FILES];
    uint32_t dir_count_l = 0, file_count_l = 0;
    uint32_t i, j;

    /* Collect built-in subdirectories */
    i = 0;
    while (fs_directories[i].name != 0) {
        if (is_direct_child_path(dirpath, fs_directories[i].path) && dir_count_l < LS_MAX_DIRS) {
            ls_str_copy_truncate(dir_entries[dir_count_l].name, fs_directories[i].name, LS_NAME_MAX);
            dir_entries[dir_count_l].size = 0;
            dir_entries[dir_count_l].writable = 0;
            dir_entries[dir_count_l].executable = 0;
            dir_count_l++;
        }
        i++;
    }

    /* Collect in-memory subdirectories */
    for (i = 0; i < dir_count; i++) {
        const char *mem_dir = directories[i];
        if (is_direct_child_path(dirpath, mem_dir) && dir_count_l < LS_MAX_DIRS) {
            uint32_t start = 1;
            if (!str_eq(dirpath, "/")) {
                uint32_t len = 0;
                while (dirpath[len]) len++;
                start = len + 1;
            }
            ls_str_copy_truncate(dir_entries[dir_count_l].name, &mem_dir[start], LS_NAME_MAX);
            dir_entries[dir_count_l].size = 0;
            dir_entries[dir_count_l].writable = 1;
            dir_entries[dir_count_l].executable = 0;
            dir_count_l++;
        }
    }

    /* Collect built-in files (not shadowed by in-memory) */
    i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].dir_path, dirpath) && find_memory_file(fs_files[i].path) == 0 && file_count_l < LS_MAX_FILES) {
            ls_str_copy_truncate(file_entries[file_count_l].name, fs_files[i].name, LS_NAME_MAX);
            file_entries[file_count_l].size = text_len(fs_files[i].content);
            file_entries[file_count_l].writable = 0;
            file_entries[file_count_l].executable = ls_is_executable(fs_files[i].name);
            file_count_l++;
        }
        i++;
    }

    /* Collect in-memory files */
    for (i = 0; i < file_count; i++) {
        const char *file_path = in_memory_files[i].path;
        uint32_t parent_len = path_parent_length(file_path);
        char parent_dir[INPUT_MAX + 1];

        if (parent_len == 0) {
            parent_dir[0] = '/';
            parent_dir[1] = '\0';
        } else {
            for (j = 0; j < parent_len; j++)
                parent_dir[j] = file_path[j];
            parent_dir[parent_len] = '\0';
        }

        if (str_eq(parent_dir, dirpath) && file_count_l < LS_MAX_FILES) {
            const char *fname = &file_path[parent_len + 1];
            ls_str_copy_truncate(file_entries[file_count_l].name, fname, LS_NAME_MAX);
            file_entries[file_count_l].size = in_memory_files[i].content_len;
            file_entries[file_count_l].writable = 1;
            file_entries[file_count_l].executable = ls_is_executable(fname);
            file_count_l++;
        }
    }

    if (dir_count_l == 0 && file_count_l == 0) {
        term_puts("<empty>", VGA_ATTR_MUTED);
        return;
    }

    /* Sort each group A-Z */
    ls_sort(dir_entries, dir_count_l);
    ls_sort(file_entries, file_count_l);

    /* Print dirs */
    for (i = 0; i < dir_count_l; i++) {
        term_puts(dir_entries[i].name, VGA_ATTR_DIR);
        if (show_sizes)
            term_puts(" <DIR>", VGA_ATTR_MUTED);
        if (show_rights) {
            const char *perm = dir_entries[i].writable ? "  [drw-]" : "  [dr--]";
            term_puts(perm, VGA_ATTR_MUTED);
        }
        term_newline();
    }

    /* Print files */
    for (i = 0; i < file_count_l; i++) {
        print_file_with_size(file_entries[i].name, file_entries[i].size, show_sizes);
        if (show_rights) {
            char perm[10];
            /* Format: [-rw-] — always readable, writable if in-memory, executable if .lcbat/game */
            perm[0] = ' '; perm[1] = ' '; perm[2] = '[';
            perm[3] = '-';
            perm[4] = 'r';
            perm[5] = file_entries[i].writable   ? 'w' : '-';
            perm[6] = file_entries[i].executable ? 'x' : '-';
            perm[7] = ']'; perm[8] = '\0';
            term_puts(perm, VGA_ATTR_MUTED);
        }
        term_newline();
    }
}

static int copy_file_internal(const char *src, const char *dst)
{
    const char *content = get_file_content_any(src);

    if (content == 0)
        return 0;

    return write_memory_file(dst, content);
}

static int copy_directory_tree(const char *src_dir, const char *dst_dir)
{
    uint32_t i;

    if (!dir_exists(dst_dir))
        add_directory(dst_dir);
    if (!dir_exists(dst_dir))
        return 0;

    i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].dir_path, src_dir) && find_memory_file(fs_files[i].path) == 0) {
            char dst_file[INPUT_MAX + 1];

            if (!path_build_join(dst_dir, fs_files[i].name, dst_file))
                return 0;
            if (!copy_file_internal(fs_files[i].path, dst_file))
                return 0;
        }
        i++;
    }

    for (i = 0; i < file_count; i++) {
        const char *file_path = in_memory_files[i].path;
        uint32_t parent_len = path_parent_length(file_path);
        char parent_dir[INPUT_MAX + 1];

        if (parent_len == 0) {
            parent_dir[0] = '/';
            parent_dir[1] = '\0';
        } else {
            uint32_t j;
            for (j = 0; j < parent_len; j++)
                parent_dir[j] = file_path[j];
            parent_dir[parent_len] = '\0';
        }

        if (str_eq(parent_dir, src_dir)) {
            char dst_file[INPUT_MAX + 1];
            const char *name = path_basename(file_path);

            if (!path_build_join(dst_dir, name, dst_file))
                return 0;
            if (!copy_file_internal(file_path, dst_file))
                return 0;
        }
    }

    i = 0;
    while (fs_directories[i].name != 0) {
        if (is_direct_child_path(src_dir, fs_directories[i].path)) {
            char dst_subdir[INPUT_MAX + 1];

            if (!path_build_join(dst_dir, fs_directories[i].name, dst_subdir))
                return 0;
            if (!copy_directory_tree(fs_directories[i].path, dst_subdir))
                return 0;
        }
        i++;
    }

    for (i = 0; i < dir_count; i++) {
        const char *mem_dir = directories[i];

        if (is_direct_child_path(src_dir, mem_dir)) {
            char dst_subdir[INPUT_MAX + 1];

            if (!path_build_join(dst_dir, path_basename(mem_dir), dst_subdir))
                return 0;
            if (!copy_directory_tree(mem_dir, dst_subdir))
                return 0;
        }
    }

    return 1;
}

void os_term_puts(const char *s, uint8_t attr)
{
    (void)attr;
    if (term_capture_active) {
        while (*s && term_capture_len + 1 < sizeof(term_capture_buffer))
            term_capture_buffer[term_capture_len++] = *s++;
        term_capture_buffer[term_capture_len] = '\0';
        return;
    }
    term_puts(s, attr);
}

void os_term_newline(void)
{
    if (term_capture_active) {
        if (term_capture_len + 1 < sizeof(term_capture_buffer)) {
            term_capture_buffer[term_capture_len++] = '\n';
            term_capture_buffer[term_capture_len] = '\0';
        }
        return;
    }
    term_newline();
}

void os_term_clear(void)
{
    if (term_capture_active) {
        term_capture_len = 0;
        term_capture_buffer[0] = '\0';
        return;
    }
    cursor_hide();
    vga_clear();
    draw_logo();
    draw_ui();
    term_prompt();
}

void os_term_capture_begin(void)
{
    term_capture_active = 1;
    term_capture_len = 0;
    term_capture_buffer[0] = '\0';
}

void os_term_capture_end(char *out, uint32_t out_size)
{
    uint32_t i = 0;
    term_capture_active = 0;
    if (out_size == 0)
        return;
    while (term_capture_buffer[i] && i + 1 < out_size) {
        out[i] = term_capture_buffer[i];
        i++;
    }
    out[i] = '\0';
}

void os_redraw_input_line(void)
{
    redraw_input_line();
}

void os_cursor_hide(void)
{
    cursor_hide();
}

void os_cursor_reset_blink(void)
{
    cursor_reset_blink();
}

void os_wait_for_keypress(void)
{
    for (;;) {
        struct key_event event = keyboard_poll_event();

        if (event.type != KEY_NONE)
            return;

        __asm__ volatile ("hlt"); /* sleep until next IRQ */
    }
}

int os_check_ctrl_c(void)
{
    struct key_event event = keyboard_poll_event();
    return event.type == KEY_CTRL_C ? 1 : 0;
}

uint32_t os_term_cols(void)
{
    return term_cols();
}

void os_pipe_input_set(const char *text)
{
    uint32_t i = 0;
    if (text == 0)
        text = "";
    while (text[i] && i + 1 < sizeof(pipe_input_buffer)) {
        pipe_input_buffer[i] = text[i];
        i++;
    }
    pipe_input_buffer[i] = '\0';
}

const char *os_pipe_input_get(void)
{
    return pipe_input_buffer;
}

void os_mouse_get_state(int *x, int *y, uint8_t *buttons)
{
    if (x)
        *x = mouse_x;
    if (y)
        *y = mouse_y;
    if (buttons)
        *buttons = mouse_buttons;
}

int os_mouse_consume_left_click(int *x, int *y)
{
    if (!mouse_left_click_pending)
        return 0;

    mouse_left_click_pending = 0;
    if (x)
        *x = mouse_click_x;
    if (y)
        *y = mouse_click_y;
    return 1;
}

int os_fs_resolve_path(const char *arg, char *out)
{
    return resolve_path(arg, out);
}

int os_fs_dir_exists(const char *path)
{
    return dir_exists(path);
}

int os_fs_dir_is_builtin(const char *path)
{
    return dir_is_builtin(path);
}

int os_fs_dir_has_entries(const char *path)
{
    return dir_has_entries(path);
}

int os_fs_add_dir(const char *path)
{
    if (path_has_forbidden_name_chars(path))
        return 0;

    if (dir_exists(path))
        return 0;

    add_directory(path);
    fs_persist_save_current();
    return dir_exists(path);
}

int os_fs_remove_dir(const char *path)
{
    int ok = remove_memory_dir(path);
    if (ok)
        fs_persist_save_current();
    return ok;
}

void os_fs_list_directory(const char *path)
{
    list_directory_internal(path, 0, 0);
}

void os_fs_list_directory_sizes(const char *path)
{
    list_directory_internal(path, 1, 0);
}

void os_fs_list_directory_rights(const char *path)
{
    list_directory_internal(path, 0, 1);
}

void os_fs_list_directory_ex(const char *path, int show_sizes, int show_rights)
{
    list_directory_internal(path, show_sizes, show_rights);
}

int os_fs_file_exists_any(const char *path)
{
    return file_exists_any(path);
}

int os_fs_file_exists_builtin(const char *path)
{
    return file_exists_builtin(path);
}

const char *os_fs_get_file_content(const char *path)
{
    return get_file_content_any(path);
}

int os_fs_read_extension_matches(const char *dirpath, const char *ext)
{
    uint32_t i;
    int printed = 0;

    i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].dir_path, dirpath) &&
            find_memory_file(fs_files[i].path) == 0 &&
            ls_name_has_suffix_ignore_case(fs_files[i].name, ext)) {
            if (printed)
                term_newline();
            term_puts("== ", VGA_ATTR_TEXT);
            term_puts(fs_files[i].name, VGA_ATTR_FILE);
            term_puts(" ==", VGA_ATTR_TEXT);
            term_newline();
            term_puts(fs_files[i].content, VGA_ATTR_TEXT);
            if (fs_files[i].content[0] != '\0' &&
                fs_files[i].content[text_len(fs_files[i].content) - 1] != '\n')
                term_newline();
            printed = 1;
        }
        i++;
    }

    for (i = 0; i < file_count; i++) {
        const char *file_path = in_memory_files[i].path;
        uint32_t parent_len = path_parent_length(file_path);
        char parent_dir[INPUT_MAX + 1];
        const char *fname;

        if (parent_len == 0) {
            parent_dir[0] = '/';
            parent_dir[1] = '\0';
        } else {
            uint32_t j;
            for (j = 0; j < parent_len; j++)
                parent_dir[j] = file_path[j];
            parent_dir[parent_len] = '\0';
        }

        fname = &file_path[parent_len + 1];
        if (str_eq(parent_dir, dirpath) && ls_name_has_suffix_ignore_case(fname, ext)) {
            if (printed)
                term_newline();
            term_puts("== ", VGA_ATTR_TEXT);
            term_puts(fname, VGA_ATTR_FILE);
            term_puts(" ==", VGA_ATTR_TEXT);
            term_newline();
            term_puts(in_memory_files[i].content, VGA_ATTR_TEXT);
            if (in_memory_files[i].content_len > 0 &&
                in_memory_files[i].content[in_memory_files[i].content_len - 1] != '\n')
                term_newline();
            printed = 1;
        }
    }

    return printed;
}

int os_fs_collect_extension_matches(const char *dirpath, const char *ext, char *out, uint32_t out_size)
{
    uint32_t i;
    uint32_t pos = 0;

    if (out_size == 0)
        return 0;

    out[0] = '\0';

    i = 0;
    while (fs_files[i].name != 0) {
        if (str_eq(fs_files[i].dir_path, dirpath) &&
            find_memory_file(fs_files[i].path) == 0 &&
            ls_name_has_suffix_ignore_case(fs_files[i].name, ext)) {
            uint32_t j = 0;
            while (fs_files[i].path[j] && pos + 1 < out_size)
                out[pos++] = fs_files[i].path[j++];
            if (pos + 1 >= out_size)
                break;
            out[pos++] = '\n';
            out[pos] = '\0';
        }
        i++;
    }

    for (i = 0; i < file_count; i++) {
        const char *file_path = in_memory_files[i].path;
        uint32_t parent_len = path_parent_length(file_path);
        char parent_dir[INPUT_MAX + 1];
        const char *fname;

        if (parent_len == 0) {
            parent_dir[0] = '/';
            parent_dir[1] = '\0';
        } else {
            uint32_t j;
            for (j = 0; j < parent_len; j++)
                parent_dir[j] = file_path[j];
            parent_dir[parent_len] = '\0';
        }

        fname = &file_path[parent_len + 1];
        if (str_eq(parent_dir, dirpath) && ls_name_has_suffix_ignore_case(fname, ext)) {
            uint32_t j = 0;
            while (file_path[j] && pos + 1 < out_size)
                out[pos++] = file_path[j++];
            if (pos + 1 >= out_size)
                break;
            out[pos++] = '\n';
            out[pos] = '\0';
        }
    }

    return pos > 0;
}

int os_fs_write_file(const char *path, const char *content)
{
    char parent[INPUT_MAX + 1];

    if (path == 0 || content == 0)
        return 0;
    if (path[0] != '/' || path[1] == '\0')
        return 0;
    if (str_len(path) > INPUT_MAX)
        return 0;
    if (path_has_forbidden_name_chars(path))
        return 0;
    if (dir_exists(path))
        return 0;

    get_parent_dir(path, parent);
    if (!dir_exists(parent))
        return 0;

    if (path_is_under("/devices", path) || path_is_under("/status", path) ||
        path_is_under("/dev", path) || path_is_under("/proc", path))
        return 0;

    {
        int ok = write_memory_file(path, content);
        if (ok)
            fs_persist_save_current();
        return ok;
    }
}

int os_fs_remove_file(const char *path)
{
    int ok = remove_memory_file(path);
    if (ok)
        fs_persist_save_current();
    return ok;
}

int os_fs_copy_file(const char *src, const char *dst)
{
    if (path_has_forbidden_name_chars(dst))
        return 0;

    {
        int ok = copy_file_internal(src, dst);
        if (ok)
            fs_persist_save_current();
        return ok;
    }
}

int os_fs_move_file(const char *src, const char *dst)
{
    if (path_has_forbidden_name_chars(dst))
        return 0;

    if (!copy_file_internal(src, dst))
        return 0;

    {
        int ok = remove_memory_file(src);
        if (ok)
            fs_persist_save_current();
        return ok;
    }
}

int os_fs_copy_dir(const char *src, const char *dst)
{
    if (path_has_forbidden_name_chars(dst))
        return 0;

    if (!dir_exists(src))
        return 0;

    {
        int ok = copy_directory_tree(src, dst);
        if (ok)
            fs_persist_save_current();
        return ok;
    }
}

int os_fs_move_dir(const char *src, const char *dst)
{
    uint32_t i;
    uint32_t src_len;

    if (!dir_exists(src) || dir_is_builtin(src))
        return 0;

    if (path_has_forbidden_name_chars(dst))
        return 0;

    if (path_is_under(src, dst))
        return 0;

    src_len = str_len(src);

    for (i = 0; i < dir_count; i++) {
        if (path_is_under(src, directories[i])) {
            char new_path[INPUT_MAX + 1];
            const char *suffix = &directories[i][src_len];

            if (str_len(dst) + str_len(suffix) > INPUT_MAX)
                return 0;

            str_copy(new_path, dst);
            str_copy(&new_path[str_len(dst)], suffix);
            str_copy(directories[i], new_path);
        }
    }

    for (i = 0; i < file_count; i++) {
        if (path_is_under(src, in_memory_files[i].path)) {
            char new_path[INPUT_MAX + 1];
            const char *suffix = &in_memory_files[i].path[src_len];

            if (str_len(dst) + str_len(suffix) > INPUT_MAX)
                return 0;

            str_copy(new_path, dst);
            str_copy(&new_path[str_len(dst)], suffix);
            str_copy(in_memory_files[i].path, new_path);
        }
    }

    {
        int ok = remove_memory_dir(src);
        if (ok)
            fs_persist_save_current();
        return ok;
    }
}

int os_fs_path_name_is_valid(const char *path)
{
    return !path_has_forbidden_name_chars(path);
}

int os_game_launch(const char *path)
{
    return programs_launch_path(path);
}

int os_editor_open(const char *path)
{
    return games_start_editor(path);
}

void os_power_shutdown(void)
{
    fs_persist_save_current();
    /* QEMU ACPI power-off. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void os_power_reboot(void)
{
    fs_persist_save_current();
    reboot_request();
}

const char *os_cpu_arch(void)
{
    static char info[160];
    uint32_t pos = 0;
    const char *cpu_text = cpu_x86_64_capable ?
        "CPU: x86_64-capable, kernel mode i386" :
        "CPU: i386-compatible, kernel mode i386";
    const char *video_text = video_vbe_available ?
        " | Video: VBE/framebuffer enabled" :
        " | Video: VBE not detected";
    const char *real_mode_text = cpu_real_mode_compatible ?
        " | Real mode: compatible" :
        " | Real mode: unavailable";
    const char *boot_mode_text = booted_via_uefi ?
        " | Boot: UEFI" :
        " | Boot: BIOS";

    while (cpu_text[pos] && pos + 1 < sizeof(info)) {
        info[pos] = cpu_text[pos];
        pos++;
    }

    {
        uint32_t i = 0;
        while (video_text[i] && pos + 1 < sizeof(info))
            info[pos++] = video_text[i++];
    }

    {
        uint32_t i = 0;
        while (real_mode_text[i] && pos + 1 < sizeof(info))
            info[pos++] = real_mode_text[i++];
    }

    {
        uint32_t i = 0;
        while (boot_mode_text[i] && pos + 1 < sizeof(info))
            info[pos++] = boot_mode_text[i++];
    }

    if (boot_usable_region_size > 0u) {
        const char *ram_policy = " | RAM policy: usable-only (reserved/ACPI excluded)";
        uint32_t i = 0;
        while (ram_policy[i] && pos + 1 < sizeof(info))
            info[pos++] = ram_policy[i++];
    }

    if (vga_is_framebuffer()) {
        const char *mode = " | Display: linear framebuffer";
        uint32_t i = 0;
        while (mode[i] && pos + 1 < sizeof(info))
            info[pos++] = mode[i++];
    }

    info[pos] = '\0';
    return info;
}

void os_storage_probe(int *ata_legacy,
                      int *ahci,
                      int *usb_uhci,
                      int *usb_ohci,
                      int *usb_ehci,
                      int *usb_xhci)
{
    uint8_t status;
    uint32_t bus;
    uint32_t dev;
    uint32_t func;

    if (ata_legacy) *ata_legacy = 0;
    if (ahci) *ahci = 0;
    if (usb_uhci) *usb_uhci = 0;
    if (usb_ohci) *usb_ohci = 0;
    if (usb_ehci) *usb_ehci = 0;
    if (usb_xhci) *usb_xhci = 0;

    /* Quick legacy ATA presence probe (PIO primary status register). */
    status = inb(0x1F7);
    if (status != 0xFF && ata_legacy)
        *ata_legacy = 1;

    /* PCI config-space scan via 0xCF8/0xCFC. */
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (func = 0; func < 8; func++) {
                uint32_t addr = 0x80000000u |
                                (bus << 16) |
                                (dev << 11) |
                                (func << 8);
                uint32_t id;
                uint16_t vendor;
                uint32_t class_reg;
                uint8_t class_code;
                uint8_t subclass;
                uint8_t prog_if;

                outl(0xCF8, addr);
                id = inl(0xCFC);
                vendor = (uint16_t)(id & 0xFFFFu);
                if (vendor == 0xFFFFu)
                    continue;

                outl(0xCF8, addr | 0x08u);
                class_reg = inl(0xCFC);
                class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);

                /* Mass storage */
                if (class_code == 0x01u) {
                    if (subclass == 0x01u && ata_legacy)
                        *ata_legacy = 1; /* IDE */
                    if (subclass == 0x06u && prog_if == 0x01u && ahci)
                        *ahci = 1; /* SATA AHCI */
                }

                /* Serial bus controller / USB */
                if (class_code == 0x0Cu && subclass == 0x03u) {
                    if (prog_if == 0x00u && usb_uhci) *usb_uhci = 1;
                    if (prog_if == 0x10u && usb_ohci) *usb_ohci = 1;
                    if (prog_if == 0x20u && usb_ehci) *usb_ehci = 1;
                    if (prog_if == 0x30u && usb_xhci) *usb_xhci = 1;
                }
            }
        }
    }
}

uint32_t os_scheduler_ticks(void)
{
    return scheduler_ticks_get();
}

uint32_t os_scheduler_current_task(void)
{
    return scheduler_current_task_get();
}

uint32_t os_scheduler_task_count(void)
{
    return scheduler_task_count_get();
}

const char *os_scheduler_task_name(uint32_t index)
{
    if (index == 0)
        return "shell";
    if (index == 1)
        return "idle";
    if (index == 2 && games_editor_is_active())
        return "editor";
    if ((index == 2 && games_is_active() && !games_editor_is_active()) ||
        (index == 3 && games_is_active()))
        return "game";
    return "unknown";
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
    static int ctrl_down = 0;
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

    if (scancode == 0x1D) {
        ctrl_down = released == 0;
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

    if (ctrl_down && scancode == 0x2E) {
        event.type = KEY_CTRL_C;
        event.code = 3;
        event.has_code = 1;
        event.label = "Ctrl+C";
        return event;
    }

    if (ctrl_down && scancode == 0x1F) {
        event.type = KEY_CTRL_S;
        event.code = 19;
        event.has_code = 1;
        event.label = "Ctrl+S";
        return event;
    }

    if (ctrl_down && scancode == 0x12) {
        event.type = KEY_CTRL_E;
        event.code = 5;
        event.has_code = 1;
        event.label = "Ctrl+E";
        return event;
    }

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

static struct key_event keyboard_poll_event(void)
{
    struct key_event event;

    event.type = KEY_NONE;
    event.ch = 0;
    event.code = 0;
    event.has_code = 0;
    event.label = 0;

    if (kb_tail == kb_head)
        return event;

    event = kb_queue[kb_tail];
    kb_tail = (kb_tail + 1) % KB_EVENT_QUEUE_SZ;
    return event;
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr)
{
    struct key_event event;
    int game_was_active = 0;

    interrupts_disable();
    scheduler_init();
    gdt_init();
    cpu_x86_64_capable = cpu_has_long_mode();
    vga_init();
    fb_init_from_multiboot(multiboot_magic, multiboot_info_addr);
    boot_memory_init_from_multiboot(multiboot_magic, multiboot_info_addr);
    init_layout();
    interrupts_init();
    mouse_init();
    fat12_init_from_multiboot(multiboot_magic, multiboot_info_addr);
    fs_persist_load_current();
    interrupts_enable();

    draw_splash_screen();

    vga_clear();
    draw_logo();
    draw_ui();
    cursor_disable();
    shell_init();
    term_prompt();
    draw_status(&(struct key_event){ KEY_NONE, 0, 0, 0, "ready" });

    for (;;) {
        scheduler_refresh_tasks();

        if (reboot_take_requested()) {
            fs_persist_load_current();
            vga_clear();
            draw_logo();
            draw_ui();
            cursor_disable();
            shell_init();
            term_prompt();
            draw_status(&(struct key_event){ KEY_NONE, 0, 0, 0, "ready" });
            game_was_active = 0;
        }

        event = keyboard_poll_event();

        if (event.type == KEY_NONE) {
            if (games_is_active())
                games_tick(scheduler_ticks_get());
            else if (!games_editor_is_active())
                cursor_tick();

            if (game_was_active && !games_is_active() && !games_editor_is_active()) {
                os_redraw_input_line();
                draw_status(&(struct key_event){ KEY_NONE, 0, 0, 0, "ready" });
                game_was_active = 0;
            }

            if (games_is_active() || games_editor_is_active())
                __asm__ volatile ("pause"); /* games need fast ticks */
            else
                __asm__ volatile ("hlt");   /* sleep until next IRQ */
            continue;
        }

        if (games_is_active()) {
            games_handle_event(&event);
            if (!games_is_active()) {
                os_redraw_input_line();
                draw_status(&(struct key_event){ KEY_NONE, 0, 0, 0, "ready" });
                game_was_active = 0;
            }
            continue;
        }

        if (games_editor_is_active()) {
            games_handle_event(&event);
            if (!games_editor_is_active()) {
                os_redraw_input_line();
                draw_status(&(struct key_event){ KEY_NONE, 0, 0, 0, "ready" });
            }
            continue;
        }

        shell_handle_event(&event);
        if (games_is_active())
            game_was_active = 1;
        else if (games_editor_is_active())
            game_was_active = 0;
        else
            draw_status(&event);
    }
}
