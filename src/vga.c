#include "vga.h"

#define VGA_BASE ((volatile uint16_t *)0xB8000)
static uint32_t vga_cols_value = 80;
static uint32_t vga_rows_value = 25;

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

    if (cols >= 20 && cols <= 300)
        vga_cols_value = cols;
    else
        vga_cols_value = 80;

    if (rows >= 10 && rows <= 200)
        vga_rows_value = rows;
    else
        vga_rows_value = 25;
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

uint16_t vga_get_cell(uint32_t row, uint32_t col)
{
    return VGA_BASE[row * vga_cols_value + col];
}

void vga_set_cell(uint32_t row, uint32_t col, uint16_t value)
{
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
