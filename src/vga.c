#include "vga.h"
#include "font8x16.h"

/* ── Legacy VGA text mode ────────────────────────────── */
#define VGA_BASE ((volatile uint16_t *)0xB8000)
/* Shadow buffer for framebuffer mode: mirrors what we would have in VGA RAM */
#define FB_SHADOW_COLS 256
#define FB_SHADOW_ROWS 128
static uint16_t fb_shadow[FB_SHADOW_ROWS * FB_SHADOW_COLS];

static uint32_t vga_cols_value = 80;
static uint32_t vga_rows_value = 25;

/* ── VESA linear framebuffer state ──────────────────── */
static int      fb_active  = 0;
static uint8_t *fb_base    = 0;
static uint32_t fb_pitch   = 0;
static uint32_t fb_width   = 0;
static uint32_t fb_height  = 0;
static uint8_t  fb_bpp     = 32;
static uint32_t fb_cell_scale = 1;

#define FB_TARGET_MIN_COLS 80u
#define FB_TARGET_MIN_ROWS 20u
#define FB_MAX_CELL_SCALE  4u

/* Standard 16-colour VGA palette (RGB) */
static const uint32_t vga_palette[16] = {
    0x000000, /* 0 black        */
    0x0000AA, /* 1 blue         */
    0x00AA00, /* 2 green        */
    0x00AAAA, /* 3 cyan         */
    0xAA0000, /* 4 red          */
    0xAA00AA, /* 5 magenta      */
    0xAA5500, /* 6 brown        */
    0xAAAAAA, /* 7 light gray   */
    0x555555, /* 8 dark gray    */
    0x5555FF, /* 9 bright blue  */
    0x55FF55, /* A bright green */
    0x55FFFF, /* B bright cyan  */
    0xFF5555, /* C bright red   */
    0xFF55FF, /* D bright magenta */
    0xFFFF55, /* E bright yellow */
    0xFFFFFF, /* F white        */
};

static uint16_t bda_read_u16(uint32_t addr)
{
    volatile uint16_t *ptr = (volatile uint16_t *)addr;
    return *ptr;
}

static uint8_t bda_read_u8(uint32_t addr)
{
    volatile uint8_t *ptr = (volatile uint8_t *)addr;
    return *ptr;
}

void vga_init(void)
{
    uint32_t cols = (uint32_t)bda_read_u16(0x044A);
    uint32_t rows = (uint32_t)bda_read_u8(0x0484) + 1;

    /*
     * Standard VGA text mode buffer at 0xB8000 is 32 KB.
     * Clamp to known-safe text-mode maximums (160×60 = 9600 cells = 19200 bytes)
     * so we never write past the end of VGA memory regardless of what the BDA says.
     */
    if (cols >= 20 && cols <= 160)
        vga_cols_value = cols;
    else if (cols > 160)
        vga_cols_value = 160;
    else
        vga_cols_value = 80;

    if (rows >= 10 && rows <= 60)
        vga_rows_value = rows;
    else if (rows > 60)
        vga_rows_value = 60;
    else
        vga_rows_value = 25;
}

/* Called after Multiboot2 framebuffer tag is parsed. */
void vga_set_framebuffer(uint32_t addr, uint32_t pitch,
                         uint32_t width, uint32_t height, uint8_t bpp)
{
    uint32_t cols, rows;
    uint32_t scale_w;
    uint32_t scale_h;
    uint32_t i;

    if (!addr || (bpp != 24 && bpp != 32))
        return; /* unsupported; stay in text mode */

    fb_base   = (uint8_t *)addr;
    fb_pitch  = pitch;
    fb_width  = width;
    fb_height = height;
    fb_bpp    = bpp;
    fb_active = 1;

    scale_w = width / (FONT_W * FB_TARGET_MIN_COLS);
    scale_h = height / (FONT_H * FB_TARGET_MIN_ROWS);
    fb_cell_scale = (scale_w < scale_h) ? scale_w : scale_h;
    if (fb_cell_scale < 1u)
        fb_cell_scale = 1u;
    if (fb_cell_scale > FB_MAX_CELL_SCALE)
        fb_cell_scale = FB_MAX_CELL_SCALE;

    cols = width  / (FONT_W * fb_cell_scale);
    rows = height / (FONT_H * fb_cell_scale);

    if (cols == 0u)
        cols = 1u;
    if (rows == 0u)
        rows = 1u;

    if (cols > FB_SHADOW_COLS) cols = FB_SHADOW_COLS;
    if (rows > FB_SHADOW_ROWS) rows = FB_SHADOW_ROWS;

    for (i = 0; i < FB_SHADOW_ROWS * FB_SHADOW_COLS; i++)
        fb_shadow[i] = vga_entry(' ', 0x00);

    vga_cols_value = cols;
    vga_rows_value = rows;
}

int vga_is_framebuffer(void)
{
    return fb_active;
}

uint32_t vga_fb_width(void)
{
    return fb_width;
}

uint32_t vga_fb_height(void)
{
    return fb_height;
}

uint32_t vga_font_scale(void)
{
    return fb_cell_scale;
}

uint32_t vga_cols(void)
{
    return vga_cols_value;
}

uint32_t vga_rows(void)
{
    return vga_rows_value;
}

uint16_t vga_entry(char c, uint8_t attr)
{
    return (uint16_t)(((uint16_t)attr << 8) | (uint8_t)c);
}

/* ── Framebuffer helpers ─────────────────────────────── */

static void fb_write_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint8_t *p = fb_base + y * fb_pitch + x * (fb_bpp >> 3);

    p[0] = (uint8_t)(rgb & 0xFF);         /* B */
    p[1] = (uint8_t)((rgb >> 8)  & 0xFF); /* G */
    p[2] = (uint8_t)((rgb >> 16) & 0xFF); /* R */
    if (fb_bpp == 32)
        p[3] = 0xFF;
}

static void fb_draw_char(char c, uint8_t attr, uint32_t row, uint32_t col)
{
    uint32_t fg_rgb = vga_palette[attr & 0x0F];
    uint32_t bg_rgb = vga_palette[(attr >> 4) & 0x0F];
    uint32_t cell_w = FONT_W * fb_cell_scale;
    uint32_t cell_h = FONT_H * fb_cell_scale;
    uint32_t px     = col * cell_w;
    uint32_t py     = row * cell_h;
    uint32_t gy, gx;
    uint8_t  glyph_idx = (uint8_t)c;

    if (px + cell_w > fb_width || py + cell_h > fb_height)
        return;

    for (gy = 0; gy < (uint32_t)FONT_H; gy++) {
        uint8_t bits = font8x16[glyph_idx][gy];

        for (gx = 0; gx < (uint32_t)FONT_W; gx++) {
            uint32_t color = (bits & (1u << gx)) ? fg_rgb : bg_rgb;
            uint32_t sy;
            uint32_t sx;

            for (sy = 0; sy < fb_cell_scale; sy++) {
                for (sx = 0; sx < fb_cell_scale; sx++)
                    fb_write_pixel(px + gx * fb_cell_scale + sx,
                                   py + gy * fb_cell_scale + sy,
                                   color);
            }
        }
    }
}

static void fb_draw_char_px(char c, uint32_t x, uint32_t y, uint32_t fg_rgb, uint32_t bg_rgb)
{
    uint32_t cell_w = FONT_W * fb_cell_scale;
    uint32_t cell_h = FONT_H * fb_cell_scale;
    uint32_t gy;
    uint32_t gx;
    uint8_t glyph_idx = (uint8_t)c;

    if (!fb_active)
        return;
    if (x + cell_w > fb_width || y + cell_h > fb_height)
        return;

    for (gy = 0; gy < (uint32_t)FONT_H; gy++) {
        uint8_t bits = font8x16[glyph_idx][gy];

        for (gx = 0; gx < (uint32_t)FONT_W; gx++) {
            uint32_t color = (bits & (1u << gx)) ? fg_rgb : bg_rgb;
            uint32_t sy;
            uint32_t sx;

            for (sy = 0; sy < fb_cell_scale; sy++) {
                for (sx = 0; sx < fb_cell_scale; sx++)
                    fb_write_pixel(x + gx * fb_cell_scale + sx,
                                   y + gy * fb_cell_scale + sy,
                                   color);
            }
        }
    }
}

void vga_fill_rect_px(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb)
{
    uint32_t px;
    uint32_t py;

    if (!fb_active || width == 0u || height == 0u)
        return;
    if (x >= fb_width || y >= fb_height)
        return;
    if (x + width > fb_width)
        width = fb_width - x;
    if (y + height > fb_height)
        height = fb_height - y;

    for (py = y; py < y + height; py++) {
        for (px = x; px < x + width; px++)
            fb_write_pixel(px, py, rgb);
    }
}

void vga_draw_rect_px(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb)
{
    if (!fb_active || width == 0u || height == 0u)
        return;

    vga_fill_rect_px(x, y, width, 1u, rgb);
    if (height > 1u)
        vga_fill_rect_px(x, y + height - 1u, width, 1u, rgb);
    if (height > 2u)
        vga_fill_rect_px(x, y + 1u, 1u, height - 2u, rgb);
    if (width > 1u && height > 2u)
        vga_fill_rect_px(x + width - 1u, y + 1u, 1u, height - 2u, rgb);
}

void vga_draw_text_px(const char *s, uint32_t x, uint32_t y, uint32_t fg_rgb, uint32_t bg_rgb)
{
    uint32_t start_x = x;
    uint32_t step_x = FONT_W * fb_cell_scale;
    uint32_t step_y = FONT_H * fb_cell_scale;

    if (!fb_active || s == 0)
        return;

    while (*s) {
        if (*s == '\n') {
            y += step_y;
            x = start_x;
            s++;
            continue;
        }

        fb_draw_char_px(*s, x, y, fg_rgb, bg_rgb);
        x += step_x;
        s++;
    }
}

/* ── Public VGA cell API (dispatches to text or FB) ──── */

uint16_t vga_get_cell(uint32_t row, uint32_t col)
{
    if (fb_active) {
        if (row < FB_SHADOW_ROWS && col < FB_SHADOW_COLS)
            return fb_shadow[row * FB_SHADOW_COLS + col];
        return 0;
    }
    return VGA_BASE[row * vga_cols_value + col];
}

void vga_set_cell(uint32_t row, uint32_t col, uint16_t value)
{
    if (fb_active) {
        if (row < FB_SHADOW_ROWS && col < FB_SHADOW_COLS) {
            fb_shadow[row * FB_SHADOW_COLS + col] = value;
            fb_draw_char((char)(value & 0xFF), (uint8_t)(value >> 8), row, col);
        }
        return;
    }
    VGA_BASE[row * vga_cols_value + col] = value;
}

void vga_put_at(char c, uint8_t attr, uint32_t row, uint32_t col)
{
    vga_set_cell(row, col, vga_entry(c, attr));
}

void vga_clear_row(uint32_t row, uint8_t attr)
{
    uint32_t col;

    for (col = 0; col < vga_cols_value; col++)
        vga_put_at(' ', attr, row, col);
}

void vga_clear(void)
{
    uint32_t row;

    for (row = 0; row < vga_rows_value; row++)
        vga_clear_row(row, 0x00);
}

void vga_puts_at(const char *s, uint8_t attr, uint32_t row, uint32_t col)
{
    while (*s && row < vga_rows_value) {
        if (*s == '\n') {
            row++;
            col = 0;
            s++;
            continue;
        }

        if (col >= vga_cols_value) {
            row++;
            col = 0;
            if (row >= vga_rows_value)
                break;
        }

        vga_put_at(*s, attr, row, col);
        col++;
        s++;
    }
}

