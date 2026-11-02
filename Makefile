
CC := gcc
ASM := nasm

BUILD := build
ISO_DIR := iso
GRUB_EFI_DIR := /usr/lib/grub/x86_64-efi
GRUB_BIOS_DIR := /usr/lib/grub/i386-pc

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-red-zone -Wall -Wextra -O2 -Ikernel
LDFLAGS := -T linker.ld -nostdlib -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

C_SOURCES := \
	kernel/kernel.c \
	kernel/vga.c \
	kernel/framebuffer.c \
	kernel/driverinit.c \
	kernel/fsinit.c \
	kernel/scheduler.c \
	kernel/terminal.c \
	kernel/keyboard.c \
	kernel/blockdev.c \
	kernel/fat.c \
	kernel/panic.c \
	kernel/exceptions.c \
	kernel/pci.c \
	kernel/ata.c \
	kernel/ahci.c \
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

ASM_SOURCES := kernel/entry.asm kernel/trap.asm

OBJECTS := $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SOURCES)) \
           $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ASM_SOURCES))

# ---- Programs ----

PROG_DIR := $(BUILD)/programs
PROG_LIB := programs/libpieg.c

PROG_CFLAGS := -std=gnu11 -ffreestanding -fPIE -fno-stack-protector -mno-red-zone -fno-asynchronous-unwind-tables -fno-plt -fvisibility=hidden -ffunction-sections -fdata-sections -m64 -Wall -Wextra -O2 -Ikernel -Iprograms

PROG_LDFLAGS := -nostdlib -pie -Wl,--no-dynamic-linker -Wl,-e,pieg_start -Wl,-z,norelro -Wl,-z,noseparate-code -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Wl,--gc-sections

APP_NAMES := $(filter-out core libpieg,$(patsubst programs/%.c,%,$(wildcard programs/*.c)))

CORE_NAMES := ls dir cat type mkdir touch rm rmdir del write append cp copy mv move mount df echo pwd clear cls whoami about ver version uname hostname sysinfo fetch ram free hex panic reboot halt shutdown

PROGRAM_ELFS := $(PROG_DIR)/core.elf $(addprefix $(PROG_DIR)/,$(addsuffix .elf,$(APP_NAMES)))

# ---- Filesystem contents ----

FS_README := filesystem/README
FS_HELLO := filesystem/hello.ys
FS_IYS_GUIDE := filesystem/YSGUIDE

FS_IMG := $(BUILD)/pieg-fs.img
DISK_IMG := $(BUILD)/pieg-disk.img
DISK_PART_OFFSET := 1048576

.PHONY: host-test run-uefi all clean iso iso-uefi iso-legacy run run-live disk disk-reset fsimg check-tools programs install-programs

all: $(BUILD)/pieg.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(BUILD)/pieg.elf: $(OBJECTS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) -lgcc

# ---- Program compilation ----

programs: $(PROGRAM_ELFS)

$(PROG_DIR):
	mkdir -p $(PROG_DIR)

$(PROG_DIR)/%.elf: programs/%.c $(PROG_LIB) programs/libpieg.h kernel/api.h | $(PROG_DIR)
	$(CC) $(PROG_CFLAGS) $(PROG_LDFLAGS) $< $(PROG_LIB) -o $@

# Copies every program into /PROGRAMS of the FAT image.
define INSTALL_PROGRAMS
	for n in $(CORE_NAMES); do \
		mcopy -o -i $(1) $(PROG_DIR)/core.elf ::/PROGRAMS/$$(echo $$n | tr a-z A-Z).ELF || exit 1; \
	done
	for n in $(APP_NAMES); do \
		mcopy -o -i $(1) $(PROG_DIR)/$$n.elf ::/PROGRAMS/$$(echo $$n | tr a-z A-Z).ELF || exit 1; \
	done
endef

# ---- Build requirements ----

check-tools:
	@test -f $(GRUB_EFI_DIR)/modinfo.sh || \
		(echo "Missing GRUB x86_64-efi modules. Install grub-efi-amd64-bin."; exit 1)
	@test -f $(GRUB_BIOS_DIR)/modinfo.sh || \
		(echo "Missing GRUB i386-pc modules. Install grub-pc-bin."; exit 1)
	@command -v grub-mkrescue >/dev/null || \
		(echo "Missing grub-mkrescue. Install grub-common."; exit 1)
	@command -v xorriso >/dev/null || \
		(echo "Missing xorriso."; exit 1)
	@command -v mkfs.fat >/dev/null || \
		(echo "Missing mkfs.fat. Install dosfstools."; exit 1)
	@command -v mmd >/dev/null || \
		(echo "Missing mtools."; exit 1)
	@command -v sfdisk >/dev/null || \
		(echo "Missing sfdisk. Install fdisk."; exit 1)

# ---- Live filesystem image ----

fsimg: $(FS_IMG)

$(FS_IMG): $(BUILD)/pieg.elf $(PROGRAM_ELFS) $(FS_README) $(FS_HELLO) $(FS_IYS_GUIDE) check-tools | $(BUILD)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=16 status=none
	mkfs.fat -F 16 -n PIEG $@ >/dev/null

	mmd -i $@ '::/#' ::/PROGRAMS ::/U ::/U/HOME ::/U/HOME/EXAMPLE

	mcopy -i $@ $(BUILD)/pieg.elf '::/#/PIEG.ELF'
	mcopy -i $@ $(FS_README) ::/U/HOME/README.TXT
	mcopy -i $@ $(FS_HELLO) ::/U/HOME/EXAMPLE/HELLO.YS
	mcopy -i $@ $(FS_IYS_GUIDE) ::/U/HOME/YSGUIDE.TXT

	$(call INSTALL_PROGRAMS,$@)

# ---- ISO images ----

iso: iso-uefi iso-legacy

iso-uefi: $(BUILD)/pieg.elf $(FS_IMG) check-tools
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/pieg.elf $(ISO_DIR)/boot/pieg.elf
	cp $(FS_IMG) $(ISO_DIR)/boot/pieg-fs.img
	grub-mkrescue --directory=$(GRUB_EFI_DIR) -o $(BUILD)/pieg-uefi.iso $(ISO_DIR)

iso-legacy: $(BUILD)/pieg.elf $(FS_IMG) check-tools
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/pieg.elf $(ISO_DIR)/boot/pieg.elf
	cp $(FS_IMG) $(ISO_DIR)/boot/pieg-fs.img
	grub-mkrescue --directory=$(GRUB_BIOS_DIR) -o $(BUILD)/pieg-legacy.iso $(ISO_DIR)

# ---- Persistent test disk ----

disk: $(DISK_IMG)

$(DISK_IMG): $(BUILD)/pieg.elf $(FS_README) $(FS_HELLO) $(FS_IYS_GUIDE) check-tools | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	printf 'label: dos\nstart=2048, type=e\n' | sfdisk -q $@
	mkfs.fat -F 16 -n PIEG --offset 2048 $@ 64512 >/dev/null

	mmd -i $@@@$(DISK_PART_OFFSET) '::/#' ::/PROGRAMS ::/U ::/U/HOME ::/U/HOME/EXAMPLE

	mcopy -i $@@@$(DISK_PART_OFFSET) $(BUILD)/pieg.elf '::/#/PIEG.ELF'
	mcopy -i $@@@$(DISK_PART_OFFSET) $(FS_README) ::/U/HOME/README.TXT
	mcopy -i $@@@$(DISK_PART_OFFSET) $(FS_HELLO) ::/U/HOME/EXAMPLE/HELLO.YS
	mcopy -i $@@@$(DISK_PART_OFFSET) $(FS_IYS_GUIDE) ::/U/HOME/YSGUIDE.TXT

# Refreshes programs on the persistent disk.
install-programs: $(PROGRAM_ELFS) $(DISK_IMG)
	$(call INSTALL_PROGRAMS,$(DISK_IMG)@@$(DISK_PART_OFFSET))

disk-reset:
	rm -f $(DISK_IMG)
	$(MAKE) disk

# ---- Live QEMU ----

run-live: iso-legacy
	qemu-system-x86_64 -m 512M -boot order=d -cdrom $(BUILD)/pieg-legacy.iso -monitor stdio

run: iso-legacy disk install-programs
	@printf '\nLaunch PIEG in QEMU? [y/N] '; \
	read answer; \
	case "$$answer" in \
		y|Y|yes|YES) \
			qemu-system-x86_64 \
				-no-reboot \
				-no-shutdown \
				-boot order=d \
				-drive file=$(BUILD)/pieg-disk.img,format=raw,if=ide,index=0 \
				-cdrom $(BUILD)/pieg-legacy.iso \
				-monitor stdio ;; \
		*) echo "Cancelled."; exit 0 ;; \
	esac

# ---- UEFI + SATA test ----

SATA_IMG := $(BUILD)/pieg-sata.img
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

$(SATA_IMG): | $(BUILD)
	truncate -s 2G $@

run-uefi: iso-uefi $(SATA_IMG)
	@test -f $(BUILD)/OVMF_VARS.fd || cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 -machine q35 -m 512M -no-reboot -no-shutdown \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
		-drive file=$(SATA_IMG),format=raw,if=ide,index=0 \
		-cdrom $(BUILD)/pieg-uefi.iso -boot d -monitor stdio

# ---- Host-side tests ----

host-test: | $(BUILD)
	gcc -std=gnu11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Ikernel \
		tests/hosttest.c kernel/gpt.c kernel/mkfat.c kernel/fat.c \
		kernel/blockdev.c kernel/partition.c kernel/persist.c -o $(BUILD)/hosttest
	$(BUILD)/hosttest

# ---- Cleanup ----

clean:
	rm -f $(BUILD)/*.o \
		$(BUILD)/pieg.elf \
		$(PROG_DIR)/*.elf \
		$(BUILD)/pieg.iso \
		$(BUILD)/pieg-uefi.iso \
		$(BUILD)/pieg-legacy.iso
