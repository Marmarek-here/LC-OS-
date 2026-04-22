#include "gdt.h"

/*
 * GDT is currently established in early boot assembly.
 * This module exists so descriptor-table work can be moved out of kernel.c.
 */
void gdt_init(void)
{
}
