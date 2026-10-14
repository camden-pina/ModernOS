# GRUB loads the OS-owned EFI stub directly, independent of Limine.
BOOT_TEMPLATE ?= $(srctree)/scripts/bootloaders/grub.cfg.in
GRUB_HOST_DIR ?= $(srctree)/build/grub-host
GRUB_MKSTANDALONE ?= $(or $(shell command -v grub-mkstandalone 2>/dev/null),$(shell command -v grub2-mkstandalone 2>/dev/null),$(if $(wildcard $(GRUB_HOST_DIR)/usr/bin/grub-mkstandalone),$(GRUB_HOST_DIR)/usr/bin/grub-mkstandalone,grub-mkstandalone))
GRUB_EFI_DIR ?= $(if $(findstring $(GRUB_HOST_DIR),$(GRUB_MKSTANDALONE)),$(GRUB_HOST_DIR)/usr/lib/grub/x86_64-efi,)
GRUB_FONT ?= $(firstword $(wildcard $(GRUB_HOST_DIR)/usr/share/grub/unicode.pf2 /usr/share/grub/unicode.pf2 /usr/share/grub2/unicode.pf2))
BOOTLOADER_DEPS := $(EFI_IMAGE)
BOOTLOADER_ARGS := --grub-mkstandalone "$(GRUB_MKSTANDALONE)" \
	--grub-efi-dir "$(GRUB_EFI_DIR)" $(if $(GRUB_FONT),--grub-font "$(GRUB_FONT)")
