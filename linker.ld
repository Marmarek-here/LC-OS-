/* linker.ld — LC(OS) linker script
 *
 * Links the Multiboot2 header section first so GRUB can find it within the
 * first 32 KiB of the binary, followed by .text, .rodata, .data, .bss.
 *
 * Load address: 1 MiB (0x100000) — standard for Multiboot kernels.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Multiboot2 header must be early in the binary */
    .multiboot2 :
    {
        *(.multiboot2)
    }

    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* Discard DWARF / comment sections from the binary */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
    }
}
