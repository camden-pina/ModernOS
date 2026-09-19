################################################################################
# Top-Level Makefile for UEFI OS Build System (Linux/Kbuild style)
################################################################################

# Default architecture; override with "make ARCH=arm" if needed.
ARCH ?= x86_64

export PATH := $(CURDIR)/toolchain/opt/cross/bin:$(PATH)

# Set up architecture-specific variables.
ifeq ($(ARCH),x86_64)
  CROSS_COMPILE_BOOT   ?= x86_64-w64-mingw32-
  CROSS_COMPILE_KERNEL ?= x86_64-elf-
  ARCH_DIR             := arch/x86_64
  TARGET_BOOT          := BOOTX64.EFI
else ifeq ($(ARCH),arm)
  CROSS_COMPILE_BOOT   ?= arm-linux-
  CROSS_COMPILE_KERNEL ?= arm-linux-
  ARCH_DIR             := arch/arm
  TARGET_BOOT          := BOOTARM.EFI
else
  $(error Unsupported architecture "$(ARCH)")
endif

# Build directories and temporary mount points.
BUILD_DIR := build
MNT_DIR   := $(BUILD_DIR)/mnt
LOOPFILE  := $(BUILD_DIR)/loopdev
DISK_IMG  := $(BUILD_DIR)/OS.img

# Final kernel output.
TARGET_KERNEL := kernel.elf
TARGET_TOOLCHAIN := toolchain/opt/cross/bin/x86_64-elf-gcc

# Top-level targets.
.PHONY: all clean image run mount-img umount-img

all: $(TARGET_BOOT) $(TARGET_TOOLCHAIN) $(TARGET_KERNEL) toolchain image

################################################################################
# Build Bootloader
################################################################################
$(TARGET_BOOT):
	@echo "==> Building bootloader for $(ARCH)..."
	$(MAKE) -C bootloader bootloader
	@echo "==> Copying bootloader binary to top-level..."
	cp bootloader/build/BOOTX64.EFI BOOTX64.EFI

################################################################################
# Build Kernel
################################################################################
$(TARGET_KERNEL):
	@echo "==> Building kernel for $(ARCH)..."
	$(MAKE) -C kernel compile_kernel CROSS_COMPILE="$(CROSS_COMPILE_KERNEL)"
	@echo "==> Copying kernel binary to top-level..."
	cp kernel/x86_64/$(TARGET_KERNEL) .

toolchain: $(TARGET_TOOLCHAIN)

$(TARGET_TOOLCHAIN):
	$(MAKE) -C toolchain all

################################################################################
# Create Disk Image with Bootloader Installed
################################################################################
################################################################################
################################################################################
# Create Disk Image with Bootloader & Kernel Installed (Container/Rootless Safe)
################################################################################
image: $(TARGET_BOOT) $(TARGET_KERNEL)
	@echo "==> Creating disk image..."
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=50
	parted $(DISK_IMG) --script mklabel gpt 2>/dev/null
	parted $(DISK_IMG) --script mkpart primary fat32 1MiB 100% 2>/dev/null
	parted $(DISK_IMG) --script set 1 esp on 2>/dev/null
	@echo "==> Formatting FAT32 partition..."
	mformat -i $(DISK_IMG)@@1048576 -F ::
	@echo "==> Installing bootloader, kernel, and configs into disk image..."
	mmd -i $(DISK_IMG)@@1048576 ::/EFI
	mmd -i $(DISK_IMG)@@1048576 ::/EFI/BOOT
	mmd -i $(DISK_IMG)@@1048576 ::/ModernOS
	mmd -i $(DISK_IMG)@@1048576 ::/ModernOS/fonts
	mcopy -i $(DISK_IMG)@@1048576 $(TARGET_BOOT) ::/EFI/BOOT/$(TARGET_BOOT)
	mcopy -i $(DISK_IMG)@@1048576 $(TARGET_KERNEL) ::/ModernOS/$(TARGET_KERNEL)
	mcopy -i $(DISK_IMG)@@1048576 lib/fonts/dfltfont.psf ::/ModernOS/fonts/dfltfont.psf
	mcopy -i $(DISK_IMG)@@1048576 config.ini ::/ModernOS/config.ini
	@touch image
	@echo "Disk image creation complete."

# Run QEMU with the Disk Image
################################################################################
# Ensure an OVMF firmware file exists in build/ (you might generate or download it)
OVMF := ./RELEASE$(shell echo $(ARCH) | tr '[:lower:]' '[:upper:]')_OVMF.fd
run: 
	@echo "==> Launching QEMU for $(ARCH)..."
ifeq ($(ARCH),x86_64)
	qemu-system-x86_64 -cpu qemu64 -m 2G -bios $(OVMF) -drive file=$(DISK_IMG),if=ide -chardev stdio,id=char0,logfile=serial.log,signal=off -serial chardev:char0 -no-reboot
else ifeq ($(ARCH),arm)
	qemu-system-arm -M virt -cpu cortex-a15 -bios $(OVMF) -drive file=$(DISK_IMG),if=sd,format=raw
endif

################################################################################
# Mount Disk Image for Manual Editing
################################################################################
.PHONY: mount-img
mount-img: $(DISK_IMG)
	@echo "==> Mounting disk image for editing..."
	@mkdir -p $(MNT_DIR)
	@bash -c '\
		set -e; \
		LOOPDEV=$$(sudo losetup --find --show -P $(DISK_IMG)); \
		echo $$LOOPDEV > $(LOOPFILE); \
		sudo mount -t vfat "$${LOOPDEV}p1" $(MNT_DIR); \
		echo "Disk image mounted at $(MNT_DIR).";'

################################################################################
# Unmount Disk Image
################################################################################
.PHONY: umount-img
umount-img:
	@echo "==> Unmounting disk image..."
	@bash -c '\
		set -e; \
		sudo umount $(MNT_DIR); \
		LOOPDEV=$$(cat $(LOOPFILE)); \
		sudo losetup -d $$LOOPDEV; \
		rm -f $(LOOPFILE); \
		echo "Disk image unmounted."'

################################################################################
# Clean Up Build Artifacts
################################################################################
clean-bootloader:
	$(MAKE) -C bootloader clean
	rm -f $(TARGET_BOOT) $(DISK_IMG)

clean-kernel:
	$(MAKE) -C kernel clean
	rm -f $(TARGET_KERNEL) $(DISK_IMG)

clean-toolchain:
	$(MAKE) -C toolchain clean

clean: clean-bootloader clean-kernel clean-toolchain
	@echo "==> Cleaning up build artifacts..."
	@rm -rf $(BUILD_DIR) serial.log
	@echo "Cleanup complete."
