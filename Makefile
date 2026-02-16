OUTPUT := fifi
BUILD  := build
ISO_ROOT := $(BUILD)/iso_root
KERNEL := $(BUILD)/$(OUTPUT)
ISO    := $(BUILD)/$(OUTPUT).iso

LIMINE_DIR := /usr/share/limine

CC := clang
LD := ld.lld

CFLAGS := \
	-std=c11 -O2 -pipe -Wall -Wextra \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
	-mno-red-zone -mcmodel=kernel \
	--target=x86_64-elf \
	-Ikernel/include

LDFLAGS := -T kernel/linker.lds -nostdlib

OBJS := $(BUILD)/main.o $(BUILD)/serial.o

.PHONY: all kernel iso clean run

all: iso

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/main.o: kernel/src/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/serial.o: kernel/src/serial.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

kernel: $(KERNEL)

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $(KERNEL)

iso: $(ISO)

$(ISO): $(KERNEL) limine.conf
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot $(ISO_ROOT)/EFI/BOOT

	cp $(KERNEL) $(ISO_ROOT)/boot/$(OUTPUT)
	cp limine.conf $(ISO_ROOT)/limine.conf

	cp $(LIMINE_DIR)/limine-bios.sys      $(ISO_ROOT)/
	cp $(LIMINE_DIR)/limine-bios-cd.bin   $(ISO_ROOT)/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin   $(ISO_ROOT)/
	cp $(LIMINE_DIR)/BOOTX64.EFI          $(ISO_ROOT)/EFI/BOOT/

	xorriso -as mkisofs -R -r -J \
		-b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		-o $(ISO) \
		$(ISO_ROOT)

	limine bios-install $(ISO)

run: iso
	qemu-system-x86_64 -m 1024 -cdrom $(ISO) -serial stdio

clean:
	rm -rf $(BUILD)
