# ─── LC(OS) Makefile ──────────────────────────────────────────────────────────
#
# Requirements (install once):
#   sudo pacman -S nasm grub qemu-system-x86 xorriso mtools
#   # or on Debian/Ubuntu:
#   # sudo apt install nasm grub-pc-bin xorriso mtools qemu-system-x86
#
# Build & run:
#   make        — compile + link kernel ELF
#   make iso    — wrap into a bootable ISO image
#   make run    — launch in QEMU (no window, serial console)
#   make run-uefi — launch in QEMU via OVMF (UEFI firmware)
#   make clean  — remove all build artefacts
# ─────────────────────────────────────────────────────────────────────────────

# ── Toolchain ────────────────────────────────────────────────────────────────
# Use the host GCC targeting i686-elf (32-bit protected mode, Multiboot2
# style) compiled with -m32.  If you have an i686-elf- or x86_64-elf- cross
# compiler on PATH, set CC/LD to it and remove -m32 / -march overrides below.
CC      := gcc
LD      := ld
NASM    := nasm

# ── Flags ────────────────────────────────────────────────────────────────────
# -m32          : generate 32-bit code (Multiboot2 hands control in 32-bit PM)
# -ffreestanding: no host libc/startup files assumed
# -nostdlib     : don't link standard libs
# -mno-red-zone : required for x86_64 kernels (here we're 32-bit so harmless)
CFLAGS  := -m32 -std=c99 -ffreestanding -nostdlib -nostdinc \
           -Wall -Wextra -O2 \
		   -fno-stack-protector -fno-builtin \
		   -mno-mmx -mno-sse -mno-sse2 -msoft-float

NASMFLAGS := -f elf32

LDFLAGS := -m elf_i386 \
           -T linker.ld \
           --nmagic

# ── Paths ─────────────────────────────────────────────────────────────────────
SRC_DIR  := src
BUILD    := build
ISO_DIR  := iso
FS_DIR   := ../fs
FS_IMAGE := $(BUILD)/fs.img
FS_STAGE := $(BUILD)/fs_stage
FS_TREE := $(shell find $(FS_DIR) -mindepth 1 | sort)
FS_FILES := $(shell find $(FS_DIR) -mindepth 1 -type f | sort)

CFLAGS += -I$(BUILD) -I$(SRC_DIR)

KERNEL_ELF := $(BUILD)/lcos.elf
ISO_IMG    := lcos.iso
FS_HEADER   := $(BUILD)/filesystem_entries.h
FS_DATA_HEADER := $(BUILD)/filesystem_data.h

OBJS := $(BUILD)/boot.o $(BUILD)/interrupts.o $(BUILD)/kernel.o $(BUILD)/shell.o $(BUILD)/builtin_commands.o $(BUILD)/vga.o $(BUILD)/games.o $(BUILD)/reboot_state.o $(BUILD)/scheduler.o $(BUILD)/gdt.o $(BUILD)/program_registry.o $(BUILD)/program_snake.o $(BUILD)/program_tetris.o $(BUILD)/program_pingpong.o $(BUILD)/program_desktop.o

# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all iso run run-i386 run-x86_64 clean

OVMF ?= /usr/share/OVMF/OVMF_CODE.fd

all: $(KERNEL_ELF) $(FS_IMAGE)

# ── Compile ───────────────────────────────────────────────────────────────────
$(BUILD)/boot.o: $(SRC_DIR)/boot.asm | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD)/interrupts.o: $(SRC_DIR)/interrupts.asm | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD)/kernel.o: $(SRC_DIR)/kernel.c $(FS_HEADER) $(FS_DATA_HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shell.o: $(SRC_DIR)/shell.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/builtin_commands.o: $(SRC_DIR)/commands/builtin_commands.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vga.o: $(SRC_DIR)/vga.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/games.o: $(SRC_DIR)/games.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/reboot_state.o: $(SRC_DIR)/reboot_state.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/scheduler.o: $(SRC_DIR)/scheduler.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gdt.o: $(SRC_DIR)/gdt.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/program_registry.o: $(SRC_DIR)/programs/program_registry.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/program_snake.o: $(SRC_DIR)/programs/snake.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/program_tetris.o: $(SRC_DIR)/programs/tetris.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/program_pingpong.o: $(SRC_DIR)/programs/pingpong.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/program_desktop.o: $(SRC_DIR)/programs/desktop.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(FS_HEADER): $(FS_TREE) | $(BUILD)
	mkdir -p $(FS_DIR)
	@{ \
	    printf '/* Auto-generated filesystem hierarchy - do not edit */\n\n'; \
	    printf 'struct DirEntry { const char *name; const char *path; };\n'; \
	    printf 'struct FileEntry { const char *name; const char *path; const char *dir_path; const char *content; };\n\n'; \
	    printf 'static const struct DirEntry fs_directories[] = {\n'; \
	    printf '    {"/", "/"},\n'; \
	    find $(FS_DIR) -mindepth 1 -type d | sort | while IFS= read -r dirpath; do \
	        relpath=$$(printf '%s' "$$dirpath" | sed 's|^$(FS_DIR)||'); \
	        esc=$$(printf '%s' "$$relpath" | sed 's/\\/\\\\/g; s/"/\\"/g'); \
	        name=$$(basename "$$relpath"); \
	        printf '    {"%s", "%s"},\n' "$$name" "$$esc"; \
	    done; \
	    printf '    {0, 0}\n};\n\n'; \
	} > $@

$(FS_DATA_HEADER): $(FS_FILES) | $(BUILD)
	mkdir -p $(FS_DIR)
	@{ \
	    printf '/* Auto-generated file contents and entries - do not edit */\n\n'; \
	    find $(FS_DIR) -mindepth 1 -type f | sort | while IFS= read -r filepath; do \
	        name=$$(basename "$$filepath"); \
	        dirpath=$$(dirname "$$filepath"); \
	        reldir=$$(printf '%s' "$$dirpath" | sed 's|^$(FS_DIR)||'); \
	        if [ -z "$$reldir" ]; then reldir="/"; fi; \
	        varname=$$(printf '%s/%s' "$$reldir" "$$name" | sed 's/\./_/g; s/-/_/g; s/\///g; s/ /_/g'); \
	        printf 'static const char file_content_%s[] = "' "$$varname"; \
	        sed 's/\\/\\\\/g; s/"/\\"/g; s/$$/\\n/g' "$$filepath" | tr -d '\n'; \
	        printf '";\n'; \
	    done; \
	    printf '\nstatic const struct FileEntry fs_files[] = {\n'; \
	    find $(FS_DIR) -mindepth 1 -type f | sort | while IFS= read -r filepath; do \
	        name=$$(basename "$$filepath"); \
	        dirpath=$$(dirname "$$filepath"); \
	        reldir=$$(printf '%s' "$$dirpath" | sed 's|^$(FS_DIR)||'); \
	        if [ -z "$$reldir" ]; then reldir="/"; fi; \
	        varname=$$(printf '%s/%s' "$$reldir" "$$name" | sed 's/\./_/g; s/-/_/g; s/\///g; s/ /_/g'); \
	        name_esc=$$(printf '%s' "$$name" | sed 's/\\/\\\\/g; s/"/\\"/g'); \
	        reldir_esc=$$(printf '%s' "$$reldir" | sed 's/\\/\\\\/g; s/"/\\"/g'); \
	        if [ "$$reldir" = "/" ]; then \
	            relpath="/"$$name; \
	        else \
	            relpath=$$(printf '%s/%s' "$$reldir" "$$name"); \
	        fi; \
	        relpath_esc=$$(printf '%s' "$$relpath" | sed 's/\\/\\\\/g; s/"/\\"/g'); \
	        printf '    {"%s", "%s", "%s", file_content_%s},\n' "$$name_esc" "$$relpath_esc" "$$reldir_esc" "$$varname"; \
	    done; \
	    printf '    {0, 0, 0, 0}\n};\n'; \
	} > $@

$(FS_IMAGE): $(FS_TREE) | $(BUILD)
	rm -f $@
	rm -rf $(FS_STAGE)
	mkdir -p $(FS_STAGE)
	if [ -d $(FS_DIR) ]; then cp -a $(FS_DIR)/. $(FS_STAGE)/; fi
	mformat -i $@ -f 1440 -C ::
	mcopy -i $@ -s $(FS_STAGE)/* ::

# ── Link ──────────────────────────────────────────────────────────────────────
$(KERNEL_ELF): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo "[OK] Kernel ELF: $@"

# ── Create bootable ISO ───────────────────────────────────────────────────────
iso: $(KERNEL_ELF) $(FS_IMAGE)
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/lcos.elf
	cp $(FS_IMAGE) $(ISO_DIR)/boot/fs.img
	grub-mkrescue -o $(ISO_IMG) $(ISO_DIR)
	@echo "[OK] ISO image: $(ISO_IMG)"

# ── Run in QEMU ───────────────────────────────────────────────────────────────
run: $(ISO_IMG)
	$(MAKE) run-x86_64

run-x86_64: $(ISO_IMG)
	qemu-system-x86_64 \
	    -boot order=dc \
	    -cdrom $(ISO_IMG) \
	    -drive file=$(FS_IMAGE),format=raw,if=ide,index=0,media=disk \
	    -m 128M \
	    -vga std \
	    -display gtk,zoom-to-fit=off,gl=off \
	    -no-reboot \
	    -no-shutdown

run-i386: $(ISO_IMG)
	qemu-system-i386 \
	    -boot order=dc \
	    -cdrom $(ISO_IMG) \
	    -drive file=$(FS_IMAGE),format=raw,if=ide,index=0,media=disk \
	    -m 128M \
	    -vga std \
	    -display gtk,zoom-to-fit=off,gl=off \
	    -no-reboot \
	    -no-shutdown

run-uefi: $(ISO_IMG)
	@test -f $(OVMF) || (echo "[ERR] OVMF not found at $(OVMF). Set OVMF=/path/to/OVMF_CODE.fd" && exit 1)
	qemu-system-x86_64 \
	    -machine q35 \
	    -bios $(OVMF) \
	    -boot order=dc \
	    -cdrom $(ISO_IMG) \
	    -drive file=$(FS_IMAGE),format=raw,if=ide,index=0,media=disk \
	    -m 128M \
	    -vga std \
	    -display gtk,zoom-to-fit=off,gl=off \
	    -no-reboot \
	    -no-shutdown

# ── Run headless (for CI / SSH sessions) ─────────────────────────────────────
run-nographic: $(ISO_IMG)
	qemu-system-x86_64 \
	    -boot order=dc \
	    -cdrom $(ISO_IMG) \
	    -drive file=$(FS_IMAGE),format=raw,if=ide,index=0,media=disk \
	    -m 128M \
	    -display none \
	    -vga std \
	    -no-reboot \
	    -no-shutdown

# ── Directories ───────────────────────────────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD) $(ISO_IMG)
	rm -f $(ISO_DIR)/boot/lcos.elf
