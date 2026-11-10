# Compile the pinned source release for BIOS and x86-64 UEFI.
BOOT_TEMPLATE ?= $(srctree)/scripts/bootloaders/limine.conf.in
LIMINE_BUILD ?= $(srctree)/build/limine-source
LIMINE_DIR ?= $(LIMINE_BUILD)/output/bin
BOOTLOADER_DEPS := $(LIMINE_DIR)/limine
BOOTLOADER_ARGS := --limine-dir "$(LIMINE_DIR)"

$(LIMINE_DIR)/limine: $(srctree)/tools/build_limine.py $(srctree)/userland/vendor/sources.json
	$(Q)python3 -B "$(srctree)/tools/build_limine.py" --output-root "$(LIMINE_BUILD)"
