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


build/.dir:
	mkdir -p build
	touch $@


OBJS := \
    build/userdemo.o \
    build/gdt.o \
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
    $(BUILD)/exec.o \
    $(BUILD)/fork.o \
    $(BUILD)/pci.o \
    $(BUILD)/virtio_blk.o \
    $(BUILD)/virtio_net.o \
    $(BUILD)/net.o \
    $(BUILD)/arp.o \
    $(BUILD)/ip.o \
    $(BUILD)/udp.o \
    $(BUILD)/dhcp.o \
    $(BUILD)/dns.o \
    $(BUILD)/tcp.o \
    $(BUILD)/http.o \
    $(BUILD)/statusbar.o \
    $(BUILD)/splash.o \
    $(BUILD)/rtl8168.o \
    $(BUILD)/ext2.o \
    $(BUILD)/xhci.o \
    $(BUILD)/ramfs.o \
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
	$(LD) $(LDFLAGS) $(OBJS) build/thread.o build/ctx_switch.o build/syscall.o -o $(KERNEL)

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

	rm -rf initrd/rootfs_build && cp -a initrd/rootfs initrd/rootfs_build
	OUTDIR=initrd/rootfs_build bash tools/user/build_all.sh
	python3 tools/mkcpio.py initrd/rootfs_build initrd/initrd.cpio
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

DISK := build/disk.img
DISK_MB := 32

$(DISK): | $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=$(DISK_MB) 2>/dev/null
	mkfs.ext2 -b 1024 -L "fifidisk" $(DISK)
	@# Mount, populate test files, unmount
	mkdir -p /tmp/fifi_mnt
	sudo mount -o loop $(DISK) /tmp/fifi_mnt
	echo "Hello from ext2!" | sudo tee /tmp/fifi_mnt/hello.txt > /dev/null
	sudo mkdir -p /tmp/fifi_mnt/docs
	echo "FiFi OS ext2 filesystem" | sudo tee /tmp/fifi_mnt/docs/readme.txt > /dev/null
	python3 -c "print('A'*1200)" | sudo tee /tmp/fifi_mnt/big.txt > /dev/null
	sudo umount /tmp/fifi_mnt
	@echo "[disk] created $(DISK) with ext2 ($(DISK_MB) MiB)"

disk: $(DISK)

QEMU_COMMON := -M q35 -m 256M -smp 1 -cdrom $(ISO) -no-reboot
QEMU_DISK   := -drive file=$(DISK),format=raw,if=virtio
QEMU_NET    := -netdev user,id=net0 -device virtio-net-pci,netdev=net0

run: iso $(DISK)
	qemu-system-x86_64 $(QEMU_COMMON) $(QEMU_DISK) $(QEMU_NET) -serial file:serial.log -no-shutdown

rundbg: iso $(DISK)
	qemu-system-x86_64 $(QEMU_COMMON) $(QEMU_DISK) $(QEMU_NET) -serial stdio -no-shutdown

# Boot from an image written by install.sh (via test_install.sh)
TEST_USB := build/test_usb.img
runinstall: $(DISK)
	@test -f $(TEST_USB) || (echo "Run ./test_install.sh first to create the installed image." && exit 1)
	qemu-system-x86_64 -M q35 -m 256M -smp 1 \
	    -drive file=$(TEST_USB),format=raw,if=ide,index=0 \
	    $(QEMU_DISK) \
	    -serial stdio -no-reboot -no-shutdown

clean:
	rm -rf $(BUILD)

# Package a release zip: fifi.iso + all platform installers
RELEASE_DIR := $(BUILD)/release
RELEASE_ZIP := $(BUILD)/fifi-os-release.zip

release: iso
	rm -rf $(RELEASE_DIR)
	mkdir -p $(RELEASE_DIR)
	cp $(ISO)      $(RELEASE_DIR)/fifi.iso
	cp install.sh  $(RELEASE_DIR)/install.sh
	cp install.ps1 $(RELEASE_DIR)/install.ps1
	cp install.bat $(RELEASE_DIR)/install.bat
	printf "FiFi OS\n\nLinux/macOS: chmod +x install.sh && ./install.sh\nWindows:     right-click install.bat -> Run as administrator\n" \
	    > $(RELEASE_DIR)/README.txt
	cd $(BUILD) && zip -r fifi-os-release.zip release/
	@echo ""
	@echo "Release ready: $(RELEASE_ZIP)"
	@echo "Upload to: https://github.com/Hensonam23/FiFi_OS/releases/new"

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

$(BUILD)/exec.o: kernel/src/exec.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ramfs.o: kernel/src/ramfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pci.o: kernel/src/pci.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/virtio_blk.o: kernel/src/virtio_blk.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/virtio_net.o: kernel/src/virtio_net.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/net.o: kernel/src/net.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arp.o: kernel/src/arp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ip.o: kernel/src/ip.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/udp.o: kernel/src/udp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dhcp.o: kernel/src/dhcp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dns.o: kernel/src/dns.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tcp.o: kernel/src/tcp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/http.o: kernel/src/http.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/statusbar.o: kernel/src/statusbar.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/splash.o: kernel/src/splash.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/rtl8168.o: kernel/src/rtl8168.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/xhci.o: kernel/src/xhci.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# elf object
$(BUILD)/elf.o: kernel/src/elf.c | $(BUILD)
# shell object
$(BUILD)/shell.o: kernel/src/shell.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: kernel/src/%.c build/.dir
	$(CC) $(CFLAGS) -c $< -o $@


	clang --target=x86_64-elf -c kernel/arch/x86_64/ctx_switch.S -o build/ctx_switch.o
	clang -std=c11 -O2 -pipe -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fno-vectorize -fno-slp-vectorize --target=x86_64-elf -Ikernel/include -Ikernel/arch/x86_64/idt -c kernel/src/thread.c -o build/thread.o
	clang -std=c11 -O2 -pipe -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fno-vectorize -fno-slp-vectorize --target=x86_64-elf -Ikernel/include -Ikernel/arch/x86_64/idt -c kernel/src/userdemo.c -o build/userdemo.o
	clang -std=c11 -O2 -pipe -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fno-vectorize -fno-slp-vectorize --target=x86_64-elf -Ikernel/include -Ikernel/arch/x86_64/idt -c kernel/src/syscall.c -o build/syscall.o

build/ctx_switch.o: kernel/arch/x86_64/ctx_switch.S
	clang --target=x86_64-elf -c $< -o $@

