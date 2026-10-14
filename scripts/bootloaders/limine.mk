# Limine retains the existing BIOS + x86-64 UEFI boot paths.
LIMINE_VERSION ?= 12.9.0
BOOT_TEMPLATE ?= $(srctree)/scripts/bootloaders/limine.conf.in
LIMINE_DIR ?= $(srctree)/build/limine-binary
LIMINE_ARCHIVE ?= $(srctree)/build/limine-$(LIMINE_VERSION)-binary.tar.gz
BOOTLOADER_DEPS := $(LIMINE_DIR)/limine
BOOTLOADER_ARGS := --limine-dir "$(LIMINE_DIR)"

$(LIMINE_DIR)/limine:
	@mkdir -p "$(dir $(LIMINE_ARCHIVE))" "$(dir $(LIMINE_DIR))"
	@if [ ! -f "$(LIMINE_ARCHIVE)" ]; then \
		curl -fL -o "$(LIMINE_ARCHIVE).tmp" https://github.com/Limine-Bootloader/Limine/releases/download/v$(LIMINE_VERSION)/limine-binary.tar.gz && \
		mv "$(LIMINE_ARCHIVE).tmp" "$(LIMINE_ARCHIVE)"; \
	fi
	$(Q)tar -xf "$(LIMINE_ARCHIVE)" -C "$(dir $(LIMINE_DIR))"
	$(Q)$(MAKE) -C "$(LIMINE_DIR)"
