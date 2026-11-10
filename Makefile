# Coast: a monolithic x86-64 kernel built with a small Kbuild-style system.
.DEFAULT_GOAL := all

# Opt-in static Qt 6 Core/Gui/Widgets port and native QPA demo.
QT6_BUILD ?= build/qt6/port-r1
.PHONY: qt6
qt6:
	python3 -B "$(srctree)/tools/build_qt6.py" --output-root "$(QT6_BUILD)" --install-runtime
PROGRAM_PORTS_BUILD ?= build/program-ports-r1
.PHONY: program-ports
program-ports:
	python3 -B "$(srctree)/tools/build_program_ports.py" --output-root "$(PROGRAM_PORTS_BUILD)" --install-runtime
MORE_PROGRAMS_BUILD ?= build/program-ports-r2
.PHONY: more-programs
more-programs: program-ports
	python3 -B "$(srctree)/tools/build_extra_programs.py" --output-root "$(MORE_PROGRAMS_BUILD)" --base-build "$(PROGRAM_PORTS_BUILD)/BUILD.json" --install-runtime
QEMU_BUILD ?= build/qemu
GNU_GLIBC_BUILD ?= build/glibc/BUILD.json
GNU_LIBGCC_BUILD ?= build/gnu-libgcc/BUILD.json
NATIVE_GNU_LIBGCC_BUILD ?= build/gnu-libgcc-native/BUILD.json
.PHONY: qemu qemu-native cmake cmake-native gn gn-native bison bison-native grep grep-native glibc glibc-native gnu-libgcc gnu-libgcc-native gnu-cxx gnu-cxx-native llvm llvm-native test-electron-admission test-runtime-fixture
qemu:
	python3 -B "$(srctree)/tools/build_qemu.py" --output-root "$(QEMU_BUILD)" --install-runtime
qemu-native:
	python3 -B "$(srctree)/tools/build_qemu.py" --native
cmake:
	python3 -B "$(srctree)/tools/build_cmake.py"
cmake-native:
	python3 -B "$(srctree)/tools/build_cmake.py" --native
gn:
	python3 -B "$(srctree)/tools/build_gn.py"
gn-native:
	python3 -B "$(srctree)/tools/build_gn.py" --native
bison:
	python3 -B "$(srctree)/tools/build_bison.py"
bison-native:
	python3 -B "$(srctree)/tools/build_bison.py" --native
grep:
	python3 -B "$(srctree)/tools/build_grep.py"
grep-native:
	python3 -B "$(srctree)/tools/build_grep.py" --native
glibc:
	python3 -B "$(srctree)/tools/build_glibc.py"
glibc-native:
	python3 -B "$(srctree)/tools/build_glibc.py" --native
gnu-libgcc:
	python3 -B "$(srctree)/tools/build_gnu_libgcc.py" --glibc-build "$(GNU_GLIBC_BUILD)"
gnu-libgcc-native:
	python3 -B "$(srctree)/tools/build_gnu_libgcc.py" --native --glibc-build "$(GNU_GLIBC_BUILD)"
gnu-cxx: gnu-libgcc
	python3 -B "$(srctree)/tools/build_gnu_cxx.py" --glibc-build "$(GNU_GLIBC_BUILD)" --libgcc-build "$(GNU_LIBGCC_BUILD)"
gnu-cxx-native:
	python3 -B "$(srctree)/tools/build_gnu_cxx.py" --native --glibc-build "$(GNU_GLIBC_BUILD)" --libgcc-build "$(NATIVE_GNU_LIBGCC_BUILD)"
llvm:
	python3 -B "$(srctree)/tools/build_llvm.py"
llvm-native:
	python3 -B "$(srctree)/tools/build_llvm.py" --native
test-electron-admission:
	python3 -B "$(srctree)/tools/test_electron_admission.py"
test-runtime-fixture:
	python3 -B "$(srctree)/tools/test_runtime_fixture.py"
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
# Approved exact-binary compatibility experiment; normal builds stay off.
ELECTRON_PROFILE ?= 0
LARGE_BUILDS ?= 0
ifneq ($(filter $(LARGE_BUILDS),0 1),$(LARGE_BUILDS))
$(error LARGE_BUILDS must be 0 or 1)
endif
ifneq ($(words $(LARGE_BUILDS)),1)
$(error LARGE_BUILDS must be one value)
endif
CREDENTIALS_TEST ?= 0
USERNS_TEST ?= 0
USERNS_VERIFY ?= 0
SOCKET_CREDENTIALS_TEST ?= 0
SOCKET_CREDENTIALS_VERIFY ?= 0
# Approved guest-only no_new_privs contract; never enabled by another flag.
NO_NEW_PRIVS_TEST ?= 0
SECCOMP_TEST ?= 0
ZYGOTE_TRACE ?= 0
PRIVATE_FORK_TEST ?= 0
PRIVATE_FORK_VERIFY ?= 0
RAMFS_PARTIAL_WRITE_TEST ?= 0
ifneq ($(filter $(RAMFS_PARTIAL_WRITE_TEST),0 1),$(RAMFS_PARTIAL_WRITE_TEST))
$(error RAMFS_PARTIAL_WRITE_TEST must be0 or1)
endif
ifneq ($(words $(RAMFS_PARTIAL_WRITE_TEST)),1)
$(error RAMFS_PARTIAL_WRITE_TEST must be one value)
endif
RAMFS_SHARED_WRITE_TEST ?= 0
RAMFS_SHARED_WRITE_VERIFY ?= 0
ifneq ($(filter $(RAMFS_SHARED_WRITE_TEST),0 1),$(RAMFS_SHARED_WRITE_TEST))
$(error RAMFS_SHARED_WRITE_TEST must be0 or1)
endif
ifneq ($(words $(RAMFS_SHARED_WRITE_TEST)),1)
$(error RAMFS_SHARED_WRITE_TEST must be one value)
endif
ifneq ($(filter $(RAMFS_SHARED_WRITE_VERIFY),0 1),$(RAMFS_SHARED_WRITE_VERIFY))
$(error RAMFS_SHARED_WRITE_VERIFY must be0 or1)
endif
ifneq ($(words $(RAMFS_SHARED_WRITE_VERIFY)),1)
$(error RAMFS_SHARED_WRITE_VERIFY must be one value)
endif
FILESYSTEM_SANDBOX_TEST ?= 0
WAITID_COMPAT_TEST ?= 0
EMPTY_DISCARD_TEST ?= 0
ifneq ($(filter $(EMPTY_DISCARD_TEST),0 1),$(EMPTY_DISCARD_TEST))
$(error EMPTY_DISCARD_TEST must be0 or1)
endif
ifneq ($(words $(EMPTY_DISCARD_TEST)),1)
$(error EMPTY_DISCARD_TEST must be one value)
endif
ifeq ($(EMPTY_DISCARD_TEST),1)
ifneq ($(WAITID_COMPAT_TEST):$(FILESYSTEM_SANDBOX_TEST):$(PID_NAMESPACE_TEST):$(ELECTRON_PROFILE):$(CREDENTIALS_TEST):$(USERNS_TEST):$(SOCKET_CREDENTIALS_TEST):$(NO_NEW_PRIVS_TEST):$(SECCOMP_TEST):$(PRIVATE_FORK_TEST),1:1:1:1:1:1:1:1:1:1)
$(error EMPTY_DISCARD_TEST requires all approved profile/C1/C2/C3/C4/F1/G2/P1/S1/W1 gates)
endif
endif
ifneq ($(filter $(WAITID_COMPAT_TEST),0 1),$(WAITID_COMPAT_TEST))
$(error WAITID_COMPAT_TEST must be0 or1)
endif
ifneq ($(words $(WAITID_COMPAT_TEST)),1)
$(error WAITID_COMPAT_TEST must be one value)
endif
FILESYSTEM_SANDBOX_VERIFY ?= 0
ifneq ($(filter $(FILESYSTEM_SANDBOX_TEST),0 1),$(FILESYSTEM_SANDBOX_TEST))
$(error FILESYSTEM_SANDBOX_TEST must be0 or1)
endif
ifneq ($(words $(FILESYSTEM_SANDBOX_TEST)),1)
$(error FILESYSTEM_SANDBOX_TEST must be one value)
endif
ifneq ($(filter $(FILESYSTEM_SANDBOX_VERIFY),0 1),$(FILESYSTEM_SANDBOX_VERIFY))
$(error FILESYSTEM_SANDBOX_VERIFY must be0 or1)
endif
ifneq ($(words $(FILESYSTEM_SANDBOX_VERIFY)),1)
$(error FILESYSTEM_SANDBOX_VERIFY must be one value)
endif
PID_NAMESPACE_TEST ?= 0
PID_NAMESPACE_VERIFY ?= 0
ifneq ($(filter $(PID_NAMESPACE_TEST),0 1),$(PID_NAMESPACE_TEST))
$(error PID_NAMESPACE_TEST must be0 or1)
endif
ifneq ($(words $(PID_NAMESPACE_TEST)),1)
$(error PID_NAMESPACE_TEST must be one value)
endif
ifneq ($(filter $(PID_NAMESPACE_VERIFY),0 1),$(PID_NAMESPACE_VERIFY))
$(error PID_NAMESPACE_VERIFY must be0 or1)
endif
ifneq ($(words $(PID_NAMESPACE_VERIFY)),1)
$(error PID_NAMESPACE_VERIFY must be one value)
endif
ifeq ($(WAITID_COMPAT_TEST),1)
ifneq ($(FILESYSTEM_SANDBOX_TEST):$(PID_NAMESPACE_TEST):$(ELECTRON_PROFILE):$(CREDENTIALS_TEST):$(USERNS_TEST):$(SOCKET_CREDENTIALS_TEST):$(NO_NEW_PRIVS_TEST):$(SECCOMP_TEST):$(PRIVATE_FORK_TEST),1:1:1:1:1:1:1:1:1)
$(error WAITID_COMPAT_TEST requires all approved profile/C1/C2/C3/C4/F1/G2/P1/S1 gates)
endif
endif
ifeq ($(FILESYSTEM_SANDBOX_TEST),1)
ifneq ($(PID_NAMESPACE_TEST):$(ELECTRON_PROFILE):$(CREDENTIALS_TEST):$(USERNS_TEST):$(SOCKET_CREDENTIALS_TEST):$(NO_NEW_PRIVS_TEST):$(SECCOMP_TEST):$(PRIVATE_FORK_TEST),1:1:1:1:1:1:1:1)
$(error FILESYSTEM_SANDBOX_TEST requires all approved profile/C1/C2/C3/C4/F1/G2/P1 gates)
endif
endif
ifeq ($(FILESYSTEM_SANDBOX_VERIFY),1)
ifneq ($(FILESYSTEM_SANDBOX_TEST):$(USERNS_VERIFY):$(PID_NAMESPACE_VERIFY),1:1:1)
$(error FILESYSTEM_SANDBOX_VERIFY requires FILESYSTEM_SANDBOX_TEST=1 USERNS_VERIFY=1 PID_NAMESPACE_VERIFY=1)
endif
endif
ifeq ($(PID_NAMESPACE_TEST),1)
ifneq ($(ELECTRON_PROFILE):$(CREDENTIALS_TEST):$(USERNS_TEST):$(SOCKET_CREDENTIALS_TEST):$(NO_NEW_PRIVS_TEST):$(SECCOMP_TEST):$(PRIVATE_FORK_TEST),1:1:1:1:1:1:1)
$(error PID_NAMESPACE_TEST requires all approved profile/C1/C2/C3/C4/F1/G2 gates)
endif
endif
ifeq ($(PID_NAMESPACE_VERIFY),1)
ifneq ($(PID_NAMESPACE_TEST):$(USERNS_VERIFY),1:1)
$(error PID_NAMESPACE_VERIFY requires PID_NAMESPACE_TEST=1 USERNS_VERIFY=1)
endif
endif
ifneq ($(filter $(PRIVATE_FORK_TEST),0 1),$(PRIVATE_FORK_TEST))
$(error PRIVATE_FORK_TEST must be 0 or 1)
endif
ifneq ($(words $(PRIVATE_FORK_TEST)),1)
$(error PRIVATE_FORK_TEST must be one value)
endif
ifeq ($(PRIVATE_FORK_TEST),1)
ifneq ($(ELECTRON_PROFILE):$(USERNS_TEST):$(NO_NEW_PRIVS_TEST):$(SECCOMP_TEST),1:1:1:1)
$(error PRIVATE_FORK_TEST requires ELECTRON_PROFILE=1 USERNS_TEST=1 NO_NEW_PRIVS_TEST=1 SECCOMP_TEST=1)
endif
endif
ifneq ($(filter $(PRIVATE_FORK_VERIFY),0 1),$(PRIVATE_FORK_VERIFY))
$(error PRIVATE_FORK_VERIFY must be 0 or 1)
endif
ifneq ($(words $(PRIVATE_FORK_VERIFY)),1)
$(error PRIVATE_FORK_VERIFY must be one value)
endif
ifeq ($(PRIVATE_FORK_VERIFY),1)
ifneq ($(PRIVATE_FORK_TEST):$(USERNS_VERIFY),1:1)
$(error PRIVATE_FORK_VERIFY requires PRIVATE_FORK_TEST=1 USERNS_VERIFY=1)
endif
endif
ifneq ($(filter $(ZYGOTE_TRACE),0 1),$(ZYGOTE_TRACE))
$(error ZYGOTE_TRACE must be 0 or 1)
endif
ifneq ($(words $(ZYGOTE_TRACE)),1)
$(error ZYGOTE_TRACE must be one value)
endif
ifeq ($(ZYGOTE_TRACE),1)
ifneq ($(ELECTRON_PROFILE):$(CREDENTIALS_TEST),1:1)
$(error ZYGOTE_TRACE requires ELECTRON_PROFILE=1 CREDENTIALS_TEST=1)
endif
endif
ifneq ($(filter $(SECCOMP_TEST),0 1),$(SECCOMP_TEST))
$(error SECCOMP_TEST must be 0 or 1)
endif
ifneq ($(words $(SECCOMP_TEST)),1)
$(error SECCOMP_TEST must be one value)
endif
ifeq ($(SECCOMP_TEST),1)
ifneq ($(NO_NEW_PRIVS_TEST),1)
$(error SECCOMP_TEST requires NO_NEW_PRIVS_TEST=1)
endif
endif
ifneq ($(filter $(NO_NEW_PRIVS_TEST),0 1),$(NO_NEW_PRIVS_TEST))
$(error NO_NEW_PRIVS_TEST must be 0 or 1)
endif
ifneq ($(words $(NO_NEW_PRIVS_TEST)),1)
$(error NO_NEW_PRIVS_TEST must be one value)
endif
ifeq ($(NO_NEW_PRIVS_TEST),1)
ifneq ($(USERNS_TEST),1)
$(error NO_NEW_PRIVS_TEST requires USERNS_TEST=1)
endif
endif
ifneq ($(filter $(SOCKET_CREDENTIALS_VERIFY),0 1),$(SOCKET_CREDENTIALS_VERIFY))
$(error SOCKET_CREDENTIALS_VERIFY must be 0 or 1)
endif
ifneq ($(words $(SOCKET_CREDENTIALS_VERIFY)),1)
$(error SOCKET_CREDENTIALS_VERIFY must be one value)
endif
ifeq ($(SOCKET_CREDENTIALS_VERIFY),1)
ifneq ($(SOCKET_CREDENTIALS_TEST),1)
$(error SOCKET_CREDENTIALS_VERIFY requires SOCKET_CREDENTIALS_TEST=1)
endif
endif
ifneq ($(filter $(SOCKET_CREDENTIALS_TEST),0 1),$(SOCKET_CREDENTIALS_TEST))
$(error SOCKET_CREDENTIALS_TEST must be 0 or 1)
endif
ifneq ($(words $(SOCKET_CREDENTIALS_TEST)),1)
$(error SOCKET_CREDENTIALS_TEST must be one value)
endif
ifeq ($(SOCKET_CREDENTIALS_TEST),1)
ifneq ($(USERNS_TEST),1)
$(error SOCKET_CREDENTIALS_TEST requires USERNS_TEST=1)
endif
endif
ifneq ($(filter $(USERNS_VERIFY),0 1),$(USERNS_VERIFY))
$(error USERNS_VERIFY must be 0 or 1)
endif
ifneq ($(words $(USERNS_VERIFY)),1)
$(error USERNS_VERIFY must be one value)
endif
ifeq ($(USERNS_VERIFY),1)
ifneq ($(USERNS_TEST),1)
$(error USERNS_VERIFY requires USERNS_TEST=1)
endif
endif
ifneq ($(filter $(USERNS_TEST),0 1),$(USERNS_TEST))
$(error USERNS_TEST must be 0 or 1)
endif
ifneq ($(words $(USERNS_TEST)),1)
$(error USERNS_TEST must be one value)
endif
ifeq ($(USERNS_TEST),1)
ifneq ($(CREDENTIALS_TEST),1)
$(error USERNS_TEST requires CREDENTIALS_TEST=1)
endif
endif
ifneq ($(filter $(CREDENTIALS_TEST),0 1),$(CREDENTIALS_TEST))
$(error CREDENTIALS_TEST must be 0 or 1)
endif
ifneq ($(words $(CREDENTIALS_TEST)),1)
$(error CREDENTIALS_TEST must be one value)
endif
ifeq ($(filter 0 1,$(ELECTRON_PROFILE)),)
$(error ELECTRON_PROFILE must be 0 or 1)
endif
ifneq ($(words $(ELECTRON_PROFILE)),1)
$(error ELECTRON_PROFILE must be one value)
endif
ifeq ($(RAMFS_SHARED_WRITE_TEST),1)
ifneq ($(ELECTRON_PROFILE):$(CREDENTIALS_TEST):$(USERNS_TEST):$(SOCKET_CREDENTIALS_TEST):$(NO_NEW_PRIVS_TEST):$(SECCOMP_TEST):$(PRIVATE_FORK_TEST):$(PID_NAMESPACE_TEST):$(FILESYSTEM_SANDBOX_TEST),1:1:1:1:1:1:1:1:1)
$(error RAMFS_SHARED_WRITE_TEST requires all approved isolation/profile gates)
endif
endif
ifeq ($(RAMFS_SHARED_WRITE_VERIFY),1)
ifneq ($(RAMFS_SHARED_WRITE_TEST),1)
$(error RAMFS_SHARED_WRITE_VERIFY requires RAMFS_SHARED_WRITE_TEST=1)
endif
endif
ifeq ($(RAMFS_PARTIAL_WRITE_TEST),1)
ifneq ($(RAMFS_SHARED_WRITE_TEST),1)
$(error RAMFS_PARTIAL_WRITE_TEST requires RAMFS_SHARED_WRITE_TEST=1)
endif
endif
KBUILD_CPPFLAGS := -DCONFIG_LARGE_BUILDS=$(LARGE_BUILDS) -I$(srctree)/include -I$(srctree)/arch/x86/include \
	-I$(srctree)/arch/x86/include/asm -I$(srctree)/lib/acpica/source/include -D__COAST__ -DCONFIG_ELECTRON_PROFILE=$(ELECTRON_PROFILE) -DCONFIG_CREDENTIALS_TEST=$(CREDENTIALS_TEST) -DCONFIG_USERNS_TEST=$(USERNS_TEST) -DCONFIG_USERNS_VERIFY=$(USERNS_VERIFY) -DCONFIG_SOCKET_CREDENTIALS_TEST=$(SOCKET_CREDENTIALS_TEST) -DCONFIG_SOCKET_CREDENTIALS_VERIFY=$(SOCKET_CREDENTIALS_VERIFY) -DCONFIG_NO_NEW_PRIVS_TEST=$(NO_NEW_PRIVS_TEST) \
	-DCONFIG_RAMFS_PARTIAL_WRITE_TEST=$(RAMFS_PARTIAL_WRITE_TEST) -DCONFIG_RAMFS_SHARED_WRITE_TEST=$(RAMFS_SHARED_WRITE_TEST) -DCONFIG_RAMFS_SHARED_WRITE_VERIFY=$(RAMFS_SHARED_WRITE_VERIFY) -DCONFIG_EMPTY_DISCARD_TEST=$(EMPTY_DISCARD_TEST) -DCONFIG_WAITID_COMPAT_TEST=$(WAITID_COMPAT_TEST) -DCONFIG_FILESYSTEM_SANDBOX_TEST=$(FILESYSTEM_SANDBOX_TEST) -DCONFIG_FILESYSTEM_SANDBOX_VERIFY=$(FILESYSTEM_SANDBOX_VERIFY) -DCONFIG_PID_NAMESPACE_TEST=$(PID_NAMESPACE_TEST) -DCONFIG_PID_NAMESPACE_VERIFY=$(PID_NAMESPACE_VERIFY) -DCONFIG_PRIVATE_FORK_TEST=$(PRIVATE_FORK_TEST) -DCONFIG_PRIVATE_FORK_VERIFY=$(PRIVATE_FORK_VERIFY) -DCONFIG_ZYGOTE_TRACE=$(ZYGOTE_TRACE) -DCONFIG_SECCOMP_TEST=$(SECCOMP_TEST) -I$(srctree)/include/net -I$(srctree)/include/net/freestanding \
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
# One public ISO command; smaller and development images are explicit profiles.
PROFILE ?= full
ifneq ($(words $(PROFILE)),1)
$(error PROFILE must select exactly one of full, core, native, or staged)
endif
ifeq ($(filter full core native staged,$(PROFILE)),)
$(error Unsupported PROFILE=$(PROFILE); choose full, core, native, or staged)
endif
JOBS ?= 4
ISO_BUILD ?= $(if $(filter native,$(PROFILE)),$(BUILD_DIR)/native-image,$(if $(filter core,$(PROFILE)),$(BUILD_DIR)/desktop-source,$(BUILD_DIR)/desktop-source-wide))
ISO_IMAGE ?= $(if $(filter staged,$(PROFILE)),$(objtree)/os.iso,$(if $(filter native,$(PROFILE)),$(ISO_BUILD)/Coast-native.iso,$(ISO_BUILD)/image/Coast-desktop.iso))
ISO_ARGS ?=
REPACK ?= 0
ISO_ROOT := $(objtree)/iso_root
INITRAMFS := $(objtree)/initramfs.tar
# Guest checks without an explicit --iso must boot the selected O= image.
export COAST_ISO = $(abspath $(ISO_IMAGE))
export COAST_INITRAMFS := $(INITRAMFS)
BOOT_DIR := $(srctree)/arch/x86/boot
BOOT_CONFIG ?= $(BOOT_DIR)/boot.conf
BOOTLOADER ?= limine
ifeq ($(filter $(BOOTLOADER),limine grub refind),)
$(error Unsupported BOOTLOADER=$(BOOTLOADER); choose limine, grub, or refind)
endif
ifneq ($(words $(BOOTLOADER)),1)
$(error BOOTLOADER must select exactly one of limine, grub, or refind)
endif
EFI_IMAGE := $(objtree)/arch/x86/boot/efi/coast.efi
BOOT_MODULES ?=
BOOT_ASSETS ?=
XORRISO ?= xorriso
MFORMAT ?= mformat
MCOPY ?= mcopy
include $(srctree)/arch/x86/boot/efi/Makefile
include $(srctree)/scripts/bootloaders/$(BOOTLOADER).mk
AUDIO_FIRMWARE ?= $(wildcard /lib/firmware/aw88399_acf.bin)

.PHONY: all kernel vmlinux prepare kernel-build userland initramfs iso _iso-staged _iso-image clean \
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

.PHONY: fetch-sources check-sources test-source-fetch browser-host test-browser-build test-source-tree test-desktop-assembly source-check source-archive desktop-kernel nasm limine
fetch-sources:
	$(Q)python3 "$(srctree)/tools/fetch_sources.py"
check-sources:
	$(Q)python3 "$(srctree)/tools/fetch_sources.py" --check
test-source-fetch:
	$(Q)python3 "$(srctree)/tools/test_source_fetch.py"
browser-host:
	$(Q)python3 "$(srctree)/tools/prepare_browser_host.py"
test-browser-build: test-source-tree
test-source-tree:
	$(Q)python3 -B "$(srctree)/tools/test_source_tree.py"
test-desktop-assembly:
	$(Q)python3 -B "$(srctree)/tools/test_desktop_assembly.py"

# Publish only sources; keep all archives and generated tools under build/.
source-check:
	$(Q)python3 -B "$(srctree)/tools/source_tree.py"
source-archive:
	$(Q)python3 -B "$(srctree)/tools/source_tree.py" --output "$(BUILD_DIR)/coast-source.tar.xz" --kernel-sources
desktop-kernel:
	$(Q)python3 -B "$(srctree)/tools/build_desktop_kernel.py" --output-root "$(BUILD_DIR)/desktop-kernel"
nasm:
	$(Q)python3 -B "$(srctree)/tools/build_assembler.py"
limine:
	$(Q)python3 -B "$(srctree)/tools/build_limine.py"
.PHONY: setup-host image-tools
HOST_ARGS ?=
setup-host:
	$(Q)python3 -B "$(srctree)/tools/setup_host.py" --install $(if $(filter native,$(PROFILE)),--native) $(HOST_ARGS)
ifeq ($(PROFILE),staged)
iso: _iso-staged
else ifeq ($(PROFILE),native)
iso:
	$(Q)python3 -B "$(srctree)/tools/build_native_iso.py" --output-root "$(ISO_BUILD)" --jobs "$(JOBS)" $(ISO_ARGS)
else
iso:
	$(Q)python3 -B "$(srctree)/tools/build_desktop.py" --output-root "$(ISO_BUILD)" --jobs "$(JOBS)" $(if $(filter full,$(PROFILE)),--with-integrations) $(ISO_ARGS)
endif
image-tools:
	$(Q)python3 -B "$(srctree)/tools/build_image_tools.py"

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

_iso-staged: override PROFILE=staged
_iso-staged: vmlinux $(if $(filter 1,$(REPACK)),,initramfs) $(BOOTLOADER_DEPS)
	$(Q)$(MAKE) --no-print-directory _iso-image PROFILE=staged

# Package existing userland without rebuilding the large shared port caches.
_iso-image: override PROFILE=staged
_iso-image: vmlinux $(BOOTLOADER_DEPS) $(BOOT_CONFIG) $(BOOT_TEMPLATE)
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
		'  make setup-host            Install Ubuntu/Debian host prerequisites' \
		'  make iso                   Build the complete Coast desktop and integrations' \
		'  make run                   Boot that ISO in QEMU without rebuilding' \
		'' \
		'  Build options: PROFILE=full|core|native|staged JOBS=4 ISO_BUILD=path' \
		'                 ISO_ARGS="--plan" (full/core; print the build plan)' \
		'  Run options:   MEMORY=16G SMP=4 QEMU_ACCEL=kvm|tcg QEMU_DISPLAY=gtk,gl=off' \
		'                 ISO_IMAGE=path FIRMWARE=bios|uefi QEMU_EXTRA_ARGS="..."' \
		'  Use the same PROFILE / ISO_BUILD / O= values for iso and run.' \
		'  make -n run                Print the QEMU command without launching' \
		'' \
		'  make [-jN]                 Build only the kernel (build/vmlinux)' \
		'  make source-check          Audit sources for publication' \
		'  make source-archive        Export the offline kernel source kit' \
		'  make clean                 Clean the selected kernel/image output' \
		'  Optional arguments and developer commands: Documentation/build-system.md'

FORCE:

# Host-only regression checks for the ACPICA port and AML consumers.
.PHONY: test-acpi-host
test-acpi-host:
	$(Q)python3 "$(srctree)/tools/test_acpi_boot_interrupts.py"
	$(Q)python3 "$(srctree)/tools/test_acpi_osl.py"
	$(Q)python3 "$(srctree)/tools/test_acpi_osl_mapping.py"
	$(Q)python3 "$(srctree)/tools/test_acpi_runtime_preflight.py"
	$(Q)python3 "$(srctree)/tools/test_acpi_gpe_dispatch.py"
	$(Q)ASAN_OPTIONS=detect_leaks=0 python3 "$(srctree)/tools/test_audio_acpica.py"

.PHONY: test-power-host
test-power-host:
	$(Q)python3 "$(srctree)/tools/test_power_harness.py"
	$(Q)python3 "$(srctree)/tools/test_power_release_host.py"
	$(Q)python3 "$(srctree)/tools/test_power_coordination.py"
	$(Q)python3 "$(srctree)/tools/test_power_scheduler.py"
	$(Q)python3 "$(srctree)/tools/test_power_telemetry.py"
	$(Q)python3 "$(srctree)/tools/test_power_ui.py" --sanitize
	$(Q)python3 "$(srctree)/tools/test_power_workload.py"

.PHONY: test-initramfs
test-initramfs:
	$(Q)python3 -B "$(srctree)/tools/test_initramfs.py"
