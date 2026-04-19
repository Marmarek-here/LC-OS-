#ifndef LCOS_VGA_H
#define LCOS_VGA_H

#include "types.h"

void vga_init(void);
uint32_t vga_cols(void);
uint32_t vga_rows(void);

/* Called by the kernel after parsing the Multiboot2 framebuffer tag.
 * Switches the VGA layer from legacy text mode to a linear framebuffer.
 * addr     - physical address of the framebuffer
 * pitch    - bytes per scanline
 * width    - horizontal resolution in pixels
 * height   - vertical resolution in pixels
 * bpp      - bits per pixel (24 or 32 supported)
 */
void vga_set_framebuffer(uint32_t addr, uint32_t pitch,
                         uint32_t width, uint32_t height, uint8_t bpp);

int vga_is_framebuffer(void);

uint16_t vga_entry(char c, uint8_t attr);
uint16_t vga_get_cell(uint32_t row, uint32_t col);
void vga_set_cell(uint32_t row, uint32_t col, uint16_t value);
void vga_put_at(char c, uint8_t attr, uint32_t row, uint32_t col);
void vga_clear_row(uint32_t row, uint8_t attr);
void vga_clear(void);
void vga_puts_at(const char *s, uint8_t attr, uint32_t row, uint32_t col);

#endif