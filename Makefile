
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

CC := gcc
ASM := nasm

BUILD := build
ISO_DIR := iso

GRUB_EFI_DIR := /usr/lib/grub/x86_64-efi
GRUB_BIOS_DIR := /usr/lib/grub/i386-pc

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE.fd

# --------------------------------------------------
# COMPILER FLAGS
# --------------------------------------------------

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-red-zone -Wall -Wextra -O2 -Ikernel

LDFLAGS := -T linker.ld -nostdlib -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

PROG_CFLAGS := -std=gnu11 -ffreestanding -fPIE -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables -fno-plt -fvisibility=hidden -ffunction-sections -fdata-sections -m64 -Wall -Wextra -O2 -Ikernel -Iprograms

PROG_LDFLAGS := -nostdlib -pie -Wl,--no-dynamic-linker -Wl,-e,pieg_start -Wl,-z,norelro -Wl,-z,noseparate-code -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Wl,--gc-sections

# --------------------------------------------------
# KERNEL SOURCES
# --------------------------------------------------

KERNEL_C := \
	kernel/kernel.c \
	kernel/vga.c \
	kernel/gfx.c \
	kernel/framebuffer.c \
	kernel/driverinit.c \
	kernel/fsinit.c \
	kernel/scheduler.c \
	kernel/terminal.c \
	kernel/keyboard.c \
	kernel/mouse.c \
	kernel/mousecore.c \
	kernel/kbdlayout.c \
	kernel/sound.c \
	kernel/blockdev.c \
	kernel/fat.c \
	kernel/panic.c \
	kernel/exceptions.c \
	kernel/pci.c \
	kernel/ata.c \
	kernel/ahci.c \
	kernel/usb.c \
	kernel/gpt.c \
	kernel/mkfat.c \
	kernel/persist.c \
	kernel/mem.c \
	kernel/ramdisk.c \
	kernel/partition.c \
	kernel/shellfs.c \
	kernel/program.c \
	kernel/usermode.c \
	kernel/login.c

KERNEL_ASM := \
	kernel/entry.asm \
	kernel/trap.asm

KERNEL_OBJ := \
	$(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_C)) \
	$(patsubst kernel/%.asm,$(BUILD)/%.o,$(KERNEL_ASM))

# --------------------------------------------------
# PROGRAMS
# --------------------------------------------------

CORE_NAMES := \
	ls dir cat type mkdir touch rm rmdir del \
	write append cp copy mv move mount df echo pwd \
	clear cls whoami about ver version uname hostname \
	sysinfo fetch ram free hex panic reboot halt shutdown

APP_NAMES := $(filter-out libpieg,$(basename $(notdir $(wildcard programs/*.c))))

PROGRAM_ELFS := $(addprefix $(BUILD)/programs/,$(addsuffix .elf,$(APP_NAMES)))

# --------------------------------------------------
# FILESYSTEM CONFIGURATION
# --------------------------------------------------

FS_IMG := $(BUILD)/pieg-fs.img
DISK_IMG := $(BUILD)/pieg-disk.img
PART_IMG := $(BUILD)/pieg-partition.img

# The FS image is loaded into RAM whole by GRUB (boot module), so every MB here
# is a MB of RAM gone. Left empty it is sized from the content: files + 4 MB of
# headroom, at least 4 MB. Set FS_SIZE_MB=N to force a size.
FS_SIZE_MB ?=
FS_MIN_MB := 4
FS_HEADROOM_MB := 4
FS_MAX_MB := 32
DISK_SIZE_MB := 256

DISK_PART_OFFSET := 1048576
DISK_PART_SECTORS := 522240

IMAGE_DIR := Images

KEYBOARD_FILES := \
	Keyboard/colemak.kbd \
	Keyboard/de.kbd \
	Keyboard/dvorak.kbd \
	Keyboard/fr.kbd \
	Keyboard/se.kbd \
	Keyboard/uk.kbd \
	Keyboard/us.kbd

IMAGE_FILES := $(shell find $(IMAGE_DIR) -type f 2>/dev/null)
IMAGE_DIRS := $(shell find $(IMAGE_DIR) -type d 2>/dev/null)

# --------------------------------------------------
# TARGETS
# --------------------------------------------------

.PHONY: all clean run iso iso-legacy iso-uefi \
	disk-image fs-image install-programs \
	copy-programs copy-keyboard copy-images \
	check-tools microuefi microlegacy

all: $(BUILD)/pieg.elf fs-image disk-image iso-legacy

# --------------------------------------------------
# BUILD DIRECTORIES
# --------------------------------------------------

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/programs:
	mkdir -p $(BUILD)/programs

# --------------------------------------------------
# KERNEL COMPILATION
# --------------------------------------------------

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(BUILD)/pieg.elf: $(KERNEL_OBJ) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(KERNEL_OBJ) -lgcc

# --------------------------------------------------
# PROGRAM COMPILATION
# --------------------------------------------------

$(BUILD)/programs/%.elf: programs/%.c programs/libpieg.c programs/libpieg.h | $(BUILD)/programs
	$(CC) $(PROG_CFLAGS) $(PROG_LDFLAGS) $< programs/libpieg.c -o $@

# --------------------------------------------------
# COMMON FILESYSTEM DIRECTORIES
# --------------------------------------------------

define CREATE_DIRECTORIES
	mmd -i "$(1)" '::/#' ::/PROGRAMS ::/U ::/U/HOME ::/U/HOME/EXAMPLE
	mmd -i "$(1)" ::/INPUTDEV ::/INPUTDEV/KEYBOARD ::/INPUTDEV/MOUSE ::/INPUTDEV/CUSTOM
endef

# --------------------------------------------------
# COMMON FILES
# --------------------------------------------------

define COPY_COMMON_FILES
	mcopy -i "$(1)" $(BUILD)/pieg.elf '::/#/PIEG.ELF'
	mcopy -i "$(1)" filesystem/README ::/U/HOME/README.TXT
	mcopy -i "$(1)" filesystem/hello.ys ::/U/HOME/EXAMPLE/HELLO.YS
	mcopy -i "$(1)" filesystem/YSGUIDE ::/U/HOME/YSGUIDE.TXT
endef

# --------------------------------------------------
# FILESYSTEM IMAGE
# --------------------------------------------------

fs-image: $(FS_IMG)

$(FS_IMG): $(BUILD)/pieg.elf $(PROGRAM_ELFS) \
	filesystem/README filesystem/hello.ys filesystem/YSGUIDE \
	$(KEYBOARD_FILES) $(IMAGE_FILES) $(IMAGE_DIRS)

	@echo "[FS] Creating filesystem..."
	rm -f $@
	mb="$(FS_SIZE_MB)"; \
	if [ -z "$$mb" ]; then \
		bytes=$$(du -sbc filesystem Keyboard $(IMAGE_DIR) $(PROGRAM_ELFS) 2>/dev/null | tail -n 1 | cut -f1 || true); \
		mb=$$(( bytes / 1048576 + $(FS_HEADROOM_MB) )); \
		if [ $$mb -lt $(FS_MIN_MB) ]; then mb=$(FS_MIN_MB); fi; \
	fi; \
	if [ $$mb -gt $(FS_MAX_MB) ]; then \
		echo "[FS] needs $$mb MB, over the $(FS_MAX_MB) MB RAM budget (set FS_MAX_MB to allow)"; exit 1; \
	fi; \
	echo "[FS] RAM disk size: $$mb MB"; \
	dd if=/dev/zero of=$@ bs=1M count=$$mb status=none; \
	spc=""; \
	if [ $$mb -le 24 ]; then spc="-s 1"; fi; \
	mkfs.fat -F 16 $$spc -n PIEG $@ >/dev/null

	$(call CREATE_DIRECTORIES,$@)
	$(call COPY_COMMON_FILES,$@)

	@$(MAKE) copy-programs IMAGE="$@"
	@$(MAKE) copy-keyboard IMAGE="$@"
	@$(MAKE) copy-images IMAGE="$@"

	@echo "[FS] Filesystem complete."

# --------------------------------------------------
# PERSISTENT DISK IMAGE
# --------------------------------------------------

disk-image: $(BUILD)/pieg.elf $(PROGRAM_ELFS) \
	$(KEYBOARD_FILES) $(IMAGE_FILES) $(IMAGE_DIRS)

	@echo "[DISK] Creating disk image..."

	rm -f $(DISK_IMG) $(PART_IMG)

	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_SIZE_MB) status=none

	@echo "[DISK] Creating partition table..."

	printf 'label: dos\nstart=2048, size=$(DISK_PART_SECTORS), type=e\n' | sfdisk -q $(DISK_IMG)

	@echo "[DISK] Creating partition filesystem..."

	dd if=/dev/zero of=$(PART_IMG) bs=512 count=$(DISK_PART_SECTORS) status=none
	mkfs.fat -F 16 -n PIEG $(PART_IMG) >/dev/null

	dd if=$(PART_IMG) of=$(DISK_IMG) bs=512 seek=2048 conv=notrunc status=none

	rm -f $(PART_IMG)

	@echo "[DISK] Creating directories..."

	$(call CREATE_DIRECTORIES,$(DISK_IMG)@@$(DISK_PART_OFFSET))
	$(call COPY_COMMON_FILES,$(DISK_IMG)@@$(DISK_PART_OFFSET))

	@$(MAKE) copy-programs IMAGE="$(DISK_IMG)@@$(DISK_PART_OFFSET)"
	@$(MAKE) copy-keyboard IMAGE="$(DISK_IMG)@@$(DISK_PART_OFFSET)"
	@$(MAKE) copy-images IMAGE="$(DISK_IMG)@@$(DISK_PART_OFFSET)"

	@echo "[DISK] Disk image complete."

# --------------------------------------------------
# PROGRAM INSTALLATION
# --------------------------------------------------

copy-programs:
	@echo "[PROGRAMS] Installing core commands..."

	@for n in $(CORE_NAMES); do \
		echo "[PROGRAMS] Installing $$n"; \
		mcopy -o -i "$(IMAGE)" "$(BUILD)/programs/core.elf" "::/PROGRAMS/$$(echo $$n | tr a-z A-Z).ELF" || exit 1; \
	done

	@for n in $(APP_NAMES); do \
		[ "$$n" = "core" ] && continue; \
		echo "[PROGRAMS] Installing $$n"; \
		mcopy -o -i "$(IMAGE)" "$(BUILD)/programs/$$n.elf" "::/PROGRAMS/$$(echo $$n | tr a-z A-Z).ELF" || exit 1; \
	done

# --------------------------------------------------
# KEYBOARD LAYOUTS
# --------------------------------------------------

copy-keyboard:
	@echo "[KEYBOARD] Installing layouts..."

	@for f in $(KEYBOARD_FILES); do \
		echo "[KEYBOARD] Copying $$f"; \
		mcopy -o -i "$(IMAGE)" "$$f" "::/INPUTDEV/KEYBOARD/$$(basename "$$f" | tr a-z A-Z)" || exit 1; \
	done

# --------------------------------------------------
# IMAGE DIRECTORY
# --------------------------------------------------

copy-images:
	@if [ -d "$(IMAGE_DIR)" ]; then \
		echo "[IMAGES] Preparing Images directory..."; \
		mmd -i "$(IMAGE)" ::/U/HOME/Images 2>/dev/null || true; \
		find "$(IMAGE_DIR)" -type d -print0 | while IFS= read -r -d '' d; do \
			rel=$${d#"$(IMAGE_DIR)"}; \
			if [ -n "$$rel" ]; then \
				echo "[IMAGES] Creating directory $$rel"; \
				mmd -i "$(IMAGE)" "::/U/HOME/Images$$rel" 2>/dev/null || true; \
			fi; \
		done; \
		find "$(IMAGE_DIR)" -type f -print0 | while IFS= read -r -d '' f; do \
			rel=$${f#"$(IMAGE_DIR)/"}; \
			echo "[IMAGES] Copying $$rel"; \
			mcopy -o -i "$(IMAGE)" "$$f" "::/U/HOME/Images/$$rel" || exit 1; \
		done; \
		echo "[IMAGES] Copy complete."; \
	else \
		echo "[IMAGES] No Images directory found."; \
	fi

# --------------------------------------------------
# ISO CREATION
# --------------------------------------------------

iso: iso-legacy

iso-legacy: $(BUILD)/pieg.elf $(FS_IMG)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/pieg.elf $(ISO_DIR)/boot/pieg.elf
	cp $(FS_IMG) $(ISO_DIR)/boot/pieg-fs.img
	grub-mkrescue --directory=$(GRUB_BIOS_DIR) -o $(BUILD)/pieg-legacy.iso $(ISO_DIR)

iso-uefi: $(BUILD)/pieg.elf $(FS_IMG)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/pieg.elf $(ISO_DIR)/boot/pieg.elf
	cp $(FS_IMG) $(ISO_DIR)/boot/pieg-fs.img
	grub-mkrescue --directory=$(GRUB_EFI_DIR) -o $(BUILD)/pieg-uefi.iso $(ISO_DIR)

# --------------------------------------------------
# NORMAL BOOT
# --------------------------------------------------

run: disk-image iso-legacy
	qemu-system-x86_64 \
		-m 512M \
		-machine pc \
		-cpu qemu64 \
		-smp 2 \
		-display sdl \
		-audiodev pa,id=audio0 \
		-device ich9-intel-hda \
		-device hda-duplex,audiodev=audio0 \
		-drive file=$(DISK_IMG),format=raw,if=ide \
		-cdrom $(BUILD)/pieg-legacy.iso \
		-boot d

# --------------------------------------------------
# MICRO UEFI: 29 MB
# --------------------------------------------------

microuefi: disk-image iso-uefi
	@test -f "$(OVMF_CODE)" || { echo "[MICROUEFI] OVMF firmware not found: $(OVMF_CODE)"; exit 1; }

	@echo "[MICROUEFI] Starting PIEG with 29 MB RAM..."

	qemu-system-x86_64 \
		-m 29M \
		-machine q35 \
		-cpu qemu64 \
		-smp 1 \
		-display sdl \
		-bios "$(OVMF_CODE)" \
		-drive file=$(DISK_IMG),format=raw,if=ide \
		-cdrom $(BUILD)/pieg-uefi.iso \
		-boot d

# --------------------------------------------------
# MICRO LEGACY: 29 MB
# --------------------------------------------------

microlegacy: disk-image iso-legacy
	@echo "[MICROLEGACY] Starting PIEG with 29 MB RAM..."

	qemu-system-x86_64 \
		-m 29M \
		-machine pc \
		-cpu qemu64 \
		-smp 1 \
		-display sdl \
		-drive file=$(DISK_IMG),format=raw,if=ide \
		-cdrom $(BUILD)/pieg-legacy.iso \
		-boot d

# --------------------------------------------------
# INSTALL PROGRAMS INTO EXISTING FS IMAGE
# --------------------------------------------------

install-programs: $(FS_IMG) $(PROGRAM_ELFS)
	@$(MAKE) copy-programs IMAGE="$(FS_IMG)"

# --------------------------------------------------
# TOOL CHECK
# --------------------------------------------------

check-tools:
	@command -v gcc >/dev/null || { echo "Missing gcc"; exit 1; }
	@command -v nasm >/dev/null || { echo "Missing nasm"; exit 1; }
	@command -v grub-mkrescue >/dev/null || { echo "Missing grub-mkrescue"; exit 1; }
	@command -v mcopy >/dev/null || { echo "Missing mcopy"; exit 1; }
	@command -v mmd >/dev/null || { echo "Missing mmd"; exit 1; }
	@command -v mkfs.fat >/dev/null || { echo "Missing mkfs.fat"; exit 1; }
	@command -v sfdisk >/dev/null || { echo "Missing sfdisk"; exit 1; }
	@command -v qemu-system-x86_64 >/dev/null || { echo "Missing QEMU"; exit 1; }
	@echo "All required tools found."

# --------------------------------------------------
# CLEAN
# --------------------------------------------------

clean:
	rm -f $(BUILD)/*.o
	rm -f $(BUILD)/programs/*.elf
	rm -f $(BUILD)/pieg.elf
	rm -f $(BUILD)/pieg.iso
	rm -f $(BUILD)/pieg-fs.img
	rm -f $(BUILD)/pieg-disk.img
	rm -f $(BUILD)/pieg-partition.img
	rm -f $(BUILD)/pieg-uefi.iso
	rm -f $(BUILD)/pieg-legacy.iso
