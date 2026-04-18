#ifndef LCOS_VGA_H
#define LCOS_VGA_H

#include "types.h"

void vga_init(void);
uint32_t vga_cols(void);
uint32_t vga_rows(void);

uint16_t vga_entry(char c, uint8_t attr);
uint16_t vga_get_cell(uint32_t row, uint32_t col);
void vga_set_cell(uint32_t row, uint32_t col, uint16_t value);
void vga_put_at(char c, uint8_t attr, uint32_t row, uint32_t col);
void vga_clear_row(uint32_t row, uint8_t attr);
void vga_clear(void);
void vga_puts_at(const char *s, uint8_t attr, uint32_t row, uint32_t col);

#endif