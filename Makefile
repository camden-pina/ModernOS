# ModernOS: a monolithic x86-64 kernel built with a small Kbuild-style system.
.DEFAULT_GOAL := all
.SUFFIXES:
.DELETE_ON_ERROR:

srctree := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
ARCH ?= x86
ifneq ($(filter $(ARCH),x86 x86_64),$(ARCH))
$(error Unsupported ARCH=$(ARCH); this kernel supports x86-64 only (ARCH=x86))
endif
SRCARCH := x86

# O= takes precedence over the equivalent environment variable KBUILD_OUTPUT.
ifeq ($(origin O),undefined)
O := $(if $(KBUILD_OUTPUT),$(KBUILD_OUTPUT),build)
endif
ifeq ($(strip $(O)),)
$(error O= must name an output directory)
endif
objtree := $(abspath $(O))
BUILD_DIR := $(objtree)
export srctree objtree SRCARCH
export SOURCE_MIRRORS

export PATH := $(srctree)/toolchain/opt/cross/bin:$(PATH)
CROSS_COMPILE ?= $(if $(CROSS_COMPILE_KERNEL),$(CROSS_COMPILE_KERNEL),$(if $(shell command -v x86_64-elf-gcc 2>/dev/null),x86_64-elf-,))
TOOLCHAIN_PREFIX ?= $(CROSS_COMPILE)
ifneq ($(filter llvm,$(TOOLCHAIN)),)
LLVM := 1
else ifneq ($(strip $(TOOLCHAIN)),)
TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
endif
ifeq ($(origin CC),default)
CC := $(TOOLCHAIN_PREFIX)gcc
endif
ifeq ($(origin LD),default)
LD := $(TOOLCHAIN_PREFIX)ld
endif
ifeq ($(origin AR),default)
AR := $(TOOLCHAIN_PREFIX)ar
endif
ifeq ($(LLVM),1)
CC := clang
LD := ld.lld
AR := llvm-ar
endif
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
CLANG_TARGET := --target=x86_64-unknown-none-elf
endif

V ?= 0
ifeq ($(V),1)
Q :=
else
Q := @
endif
export Q V CC LD AR

include $(srctree)/arch/$(SRCARCH)/Makefile
CFLAGS ?= -O2 -g -pipe
LWIP_ROOT := $(srctree)/build/lwip
KBUILD_CPPFLAGS := -I$(srctree)/include -I$(srctree)/arch/x86/include \
	-I$(srctree)/arch/x86/include/asm -I$(srctree)/lib/lai/include \
	-I$(srctree)/include/net -I$(srctree)/include/net/freestanding \
	-I$(LWIP_ROOT)/src/include $(CPPFLAGS) $(KCPPFLAGS)
KBUILD_CFLAGS := $(CLANG_TARGET) $(CFLAGS) -Wall -Wextra -std=gnu11 \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC \
	-ffunction-sections -fdata-sections $(KBUILD_ARCH_CFLAGS) $(KCFLAGS)
KBUILD_AFLAGS := $(CLANG_TARGET) -g -ffreestanding -fno-PIC \
	$(KBUILD_ARCH_CFLAGS) $(AFLAGS) $(KAFLAGS)
KBUILD_LDFLAGS := $(KBUILD_ARCH_LDFLAGS) -nostdlib -static -z noexecstack \
	--gc-sections $(LDFLAGS)
export KBUILD_CPPFLAGS KBUILD_CFLAGS KBUILD_AFLAGS KBUILD_LDFLAGS LWIP_ROOT

KERNEL_IMAGE := $(objtree)/vmlinux
TARGET_KERNEL := $(KERNEL_IMAGE)
ISO_IMAGE ?= $(objtree)/os.iso
ISO_ROOT := $(objtree)/iso_root
INITRAMFS := $(objtree)/initramfs.tar
# Guest checks without an explicit --iso must boot the selected O= image.
export MODERNOS_ISO := $(abspath $(ISO_IMAGE))
export MODERNOS_INITRAMFS := $(INITRAMFS)
BOOT_DIR := $(srctree)/arch/x86/boot
BOOT_CONFIG ?= $(BOOT_DIR)/boot.conf
BOOTLOADER ?= limine
ifeq ($(filter $(BOOTLOADER),limine grub refind),)
$(error Unsupported BOOTLOADER=$(BOOTLOADER); choose limine, grub, or refind)
endif
ifneq ($(words $(BOOTLOADER)),1)
$(error BOOTLOADER must select exactly one of limine, grub, or refind)
endif
EFI_IMAGE := $(objtree)/arch/x86/boot/efi/modernos.efi
BOOT_MODULES ?=
BOOT_ASSETS ?=
XORRISO ?= xorriso
MFORMAT ?= mformat
MCOPY ?= mcopy
include $(srctree)/arch/x86/boot/efi/Makefile
include $(srctree)/scripts/bootloaders/$(BOOTLOADER).mk
AUDIO_FIRMWARE ?= $(wildcard /lib/firmware/aw88399_acf.bin)
WEBCORE_INTERPRETER ?= llint
export WEBCORE_INTERPRETER

.PHONY: all kernel vmlinux prepare kernel-build userland initramfs iso iso-image clean \
	userland-clean help test-boot-config FORCE
all: vmlinux
kernel vmlinux: $(KERNEL_IMAGE)

prepare:
	@mkdir -p "$(objtree)"
ifneq ($(objtree),$(srctree))
	@if [ -f "$(objtree)/Makefile" ] && [ "$$(head -n 1 "$(objtree)/Makefile")" != '# Generated output directory wrapper.' ]; then \
		echo 'Output directory contains a source Makefile; choose another O= directory.' >&2; exit 1; \
	fi
	@{ printf '%s\n' '# Generated output directory wrapper.' '.PHONY: all FORCE' 'all:'; \
	   printf '\t$$%s -C "%s" O="%s"\n' '(MAKE)' '$(srctree)' '$(objtree)'; \
	   printf '%s\n' '%: FORCE'; \
	   printf '\t$$%s -C "%s" O="%s" $$@\n' '(MAKE)' '$(srctree)' '$(objtree)'; \
	   printf '%s\n' 'FORCE:'; } > "$(objtree)/Makefile"
	@printf '%s\n' '*' > "$(objtree)/.gitignore"
endif

$(LWIP_ROOT)/.prepared: $(srctree)/tools/prepare_lwip.py $(srctree)/userland/vendor/lwip-2.2.1.tar.gz
	$(Q)python3 $(srctree)/tools/prepare_lwip.py

# A source-only checkout restores just lwIP for a kernel build.
$(srctree)/userland/vendor/lwip-2.2.1.tar.gz:
	$(Q)python3 "$(srctree)/tools/fetch_sources.py" lwip-2.2.1.tar.gz

.PHONY: fetch-sources check-sources test-source-fetch browser-host test-browser-build
fetch-sources:
	$(Q)python3 "$(srctree)/tools/fetch_sources.py"
check-sources:
	$(Q)python3 "$(srctree)/tools/fetch_sources.py" --check
test-source-fetch:
	$(Q)python3 "$(srctree)/tools/test_source_fetch.py"
browser-host:
	$(Q)python3 "$(srctree)/tools/prepare_browser_host.py"
test-browser-build:
	$(Q)python3 "$(srctree)/tools/test_browser_build.py"

kernel-build: prepare $(LWIP_ROOT)/.prepared
	$(Q)$(MAKE) --no-print-directory -f $(srctree)/scripts/Makefile.build obj=.

$(KERNEL_IMAGE): kernel-build $(KERNEL_LDS) FORCE
	@set -e; \
	command='$(LD) $(KBUILD_LDFLAGS) -T $(KERNEL_LDS) --whole-archive $(objtree)/built-in.a --no-whole-archive -o $@'; \
	previous=$$(cat "$@.cmd" 2>/dev/null || true); \
	if [ ! -f "$@" ] || [ "$$command" != "$$previous" ] || \
	   [ "$(objtree)/built-in.a" -nt "$@" ] || [ "$(KERNEL_LDS)" -nt "$@" ]; then \
		if [ "$(V)" = 1 ]; then printf '%s\n' "$$command"; else printf '  LD      %s\n' '$@'; fi; \
		$(LD) $(KBUILD_LDFLAGS) -T $(KERNEL_LDS) --whole-archive $(objtree)/built-in.a --no-whole-archive -o "$@"; \
		printf '%s\n' "$$command" > "$@.cmd"; \
	fi

userland:
	$(Q)$(MAKE) -C $(srctree)/userland
initramfs: userland
	@mkdir -p "$(objtree)"
	$(Q)python3 $(srctree)/tools/mkinitramfs.py $(INITRAMFS) $(srctree)/userland/bin \
		--font-license $(srctree)/userland/desktop/assets/FONT-LICENSE.txt \
		--native-licenses $(srctree)/userland/vendor/licenses --runtime $(srctree)/userland/runtime \
		--wallpapers $(srctree)/userland/desktop/assets/wallpapers \
		$(if $(AUDIO_FIRMWARE),--audio-firmware "$(AUDIO_FIRMWARE)")

iso: vmlinux initramfs $(BOOTLOADER_DEPS)
	$(Q)$(MAKE) --no-print-directory iso-image

# Package existing userland without rebuilding the large shared port caches.
iso-image: vmlinux $(BOOTLOADER_DEPS) $(BOOT_CONFIG) $(BOOT_TEMPLATE)
	$(Q)python3 "$(srctree)/tools/build_boot_iso.py" --bootloader "$(BOOTLOADER)" \
		--config "$(BOOT_CONFIG)" --kernel "$(KERNEL_IMAGE)" --initramfs "$(INITRAMFS)" \
		--template "$(BOOT_TEMPLATE)" \
		--source-root "$(srctree)" \
		--efi-image "$(EFI_IMAGE)" --root "$(ISO_ROOT)" --output "$(ISO_IMAGE)" \
		--xorriso "$(XORRISO)" --mformat "$(MFORMAT)" --mcopy "$(MCOPY)" \
		$(BOOTLOADER_ARGS) $(foreach module,$(BOOT_MODULES),--module-file "$(module)") \
		$(foreach asset,$(BOOT_ASSETS),--asset-file "$(asset)")

test-boot-config:
	$(Q)python3 "$(srctree)/tools/test_boot_config.py"

# Port builders and guest tests use shared build/ caches and userland outputs.
include $(srctree)/scripts/Makefile.targets

clean: efi-clean
	$(Q)$(MAKE) --no-print-directory -f $(srctree)/scripts/Makefile.clean obj=.
	$(Q)rm -f $(KERNEL_IMAGE) $(KERNEL_IMAGE).cmd $(INITRAMFS) $(ISO_IMAGE)
	$(Q)rm -rf $(ISO_ROOT)
userland-clean:
	$(Q)$(MAKE) -C $(srctree)/userland clean

help:
	@printf '%s\n' \
		'  make [-jN]                 Build the kernel (build/vmlinux)' \
		'  make O=out                 Build kernel objects and images under out/' \
		'  make V=1                   Show full compiler and linker commands' \
		'  make CROSS_COMPILE=prefix- Use a GCC cross toolchain' \
		'  make LLVM=1                Use clang, ld.lld, and llvm-ar' \
		'  make iso BOOTLOADER=limine  Build the default BIOS/UEFI Limine ISO' \
		'  make iso BOOTLOADER=grub    Build an x86-64 UEFI GRUB 2 ISO' \
		'  make iso BOOTLOADER=refind  Build an x86-64 UEFI rEFInd ISO' \
		'  BOOT_CONFIG= / BOOT_TEMPLATE=  Select shared TOML / native loader template' \
		'  BOOT_ASSETS=               Extra GUEST_PATH=HOST_PATH files for templates' \
		'  make iso-image             Package the selected loader and existing initramfs' \
		'  make test-boot-config      Check shared config and loader selection/staging' \
		'  make fetch-sources         Download/verify pinned upstream source archives' \
		'  make check-sources         Verify cached sources without network access' \
		'  make browser-host          Prepare the browser host-tool cache explicitly' \
		'  make test-browser-build    Check tool bootstrap and parallel port ordering' \
		'  make run / run-image       Boot a rebuilt / existing ISO in QEMU' \
		'  make clean                 Clean kernel/images in selected output' \
		'  make userland-clean        Also remove shared generated userland' \
		'  See Documentation/build-system.md for layout and build details.'

FORCE:
