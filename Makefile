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
    -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fno-vectorize -fno-slp-vectorize \
    --target=x86_64-elf \
    -Ikernel/include \
    -Ikernel/arch/x86_64/idt

ASFLAGS := --target=x86_64-elf
LDFLAGS := -T kernel/linker.lds -nostdlib

OBJS := \
    $(BUILD)/shell.o \
    $(BUILD)/workqueue.o \
    $(BUILD)/timer.o \
    $(BUILD)/elf.o \
    $(BUILD)/main.o \
    $(BUILD)/serial.o \
    $(BUILD)/keyboard.o \
    $(BUILD)/pmm.o \
    $(BUILD)/heap.o \
    $(BUILD)/vmm.o \
    $(BUILD)/panic.o \
    $(BUILD)/kprintf.o \
    $(BUILD)/console.o \
    $(BUILD)/pic.o \
    $(BUILD)/pit.o \
    $(BUILD)/idt.o \
    $(BUILD)/isr.o \
    $(BUILD)/initrd.o \
    $(BUILD)/acpi.o \
    $(BUILD)/vfs.o \
    $(BUILD)/isr_asm.o

.PHONY: all kernel iso clean run

all: iso

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/main.o: kernel/src/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/timer.o: kernel/src/timer.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/serial.o: kernel/src/serial.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/keyboard.o: kernel/src/keyboard.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: kernel/src/pmm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/heap.o: kernel/src/heap.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vmm.o: kernel/src/vmm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/panic.o: kernel/src/panic.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/console.o: kernel/src/console.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/idt.o: kernel/arch/x86_64/idt/idt.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/isr.o: kernel/arch/x86_64/idt/isr.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/isr_asm.o: kernel/arch/x86_64/idt/isr.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

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

	python3 tools/mkcpio.py initrd/rootfs initrd/initrd.cpio
	cp initrd/initrd.cpio $(ISO_ROOT)/boot/initrd.cpio

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
	qemu-system-x86_64 -m 1024 -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

clean:
	rm -rf $(BUILD)

$(BUILD)/pic.o: kernel/src/pic.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pit.o: kernel/src/pit.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kprintf.o: kernel/src/kprintf.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Optional page fault test (off by default)
ifeq ($(PF_TEST),1)
CFLAGS += -DFIFI_PF_TEST=1
endif

# Optional VMM API self-test (off by default)
ifeq ($(VMM_TEST),1)
CFLAGS += -DFIFI_VMM_API_TEST=1
endif

# Optional heap tests (off by default)
ifeq ($(HEAP_TEST),1)
CFLAGS += -DFIFI_HEAP_TEST=1
endif

# Optional heap overflow/guard-page fault test (requires HEAP_TEST=1)
ifeq ($(HEAP_OVF),1)
CFLAGS += -DFIFI_HEAP_OVF_TEST=1
endif

# Optional heap poisoning (debug)
ifeq ($(HEAP_POISON),1)
CFLAGS += -DFIFI_HEAP_POISON=1
endif

# initrd module object
$(BUILD)/initrd.o: kernel/src/initrd.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# vfs object

# acpi object
$(BUILD)/acpi.o: kernel/src/acpi.c | $(BUILD)
		$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/vfs.o: kernel/src/vfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# elf object
$(BUILD)/elf.o: kernel/src/elf.c | $(BUILD)
# shell object
$(BUILD)/shell.o: kernel/src/shell.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: kernel/src/%.c
	$(CC) $(CFLAGS) -c $< -o $@


	clang --target=x86_64-elf -c kernel/arch/x86_64/ctx_switch.S -o build/ctx_switch.o
	clang -std=c11 -O2 -pipe -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fno-vectorize -fno-slp-vectorize --target=x86_64-elf -Ikernel/include -Ikernel/arch/x86_64/idt -c kernel/src/thread.c -o build/thread.o


# === FiFi override: link all build objects ===
# We override ONLY the recipe for build/fifi. The original prerequisites still apply.
# This guarantees new objects (thread.o, ctx_switch.o, etc) always get linked.
build/fifi:
	ld.lld -T kernel/linker.lds -nostdlib $(wildcard build/*.o) -o $@
# === end override ===
