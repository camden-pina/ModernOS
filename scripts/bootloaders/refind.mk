# A pinned official binary is cached locally; REFIND_EFI can override it.
BOOT_TEMPLATE ?= $(srctree)/scripts/bootloaders/refind.conf.in
REFIND_VERSION ?= 0.14.2
REFIND_CACHE ?= $(srctree)/build
REFIND_DIR ?= $(REFIND_CACHE)/refind-bin-$(REFIND_VERSION)
REFIND_EFI ?= $(REFIND_DIR)/refind/refind_x64.efi
BOOTLOADER_DEPS := $(EFI_IMAGE) $(REFIND_EFI)
BOOTLOADER_ARGS := --refind-efi "$(REFIND_EFI)"

$(REFIND_EFI):
	$(Q)python3 "$(srctree)/tools/fetch_refind.py" --version "$(REFIND_VERSION)" --cache "$(REFIND_CACHE)"
	@test -f "$@" || { printf 'Missing REFIND_EFI=%s; supply an existing x86-64 rEFInd EFI binary.\n' "$@" >&2; exit 1; }
