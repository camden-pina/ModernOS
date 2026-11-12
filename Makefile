# All mutating Make invocations share the cleanup lock. Recursive invocations
# verify the inherited owner's PID/token instead of trying to lock it twice.
coast_guard_root := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
coast_guard_goals := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
coast_guard_flags := $(if $(filter --%,$(firstword $(MAKEFLAGS))),,$(firstword $(MAKEFLAGS)))
coast_guard_readonly := $(or $(findstring n,$(coast_guard_flags)),$(findstring q,$(coast_guard_flags)))
coast_guard_exempt := $(if $(filter-out help menuconfig config defconfig,$(coast_guard_goals)),,yes)
coast_guard_held := $(if $(or $(coast_guard_readonly),$(coast_guard_exempt)),yes,$(shell python3 -B "$(coast_guard_root)/tools/build_state.py" held))
ifneq ($(coast_guard_held),yes)
.DEFAULT_GOAL := all
.PHONY: $(coast_guard_goals)
ifneq ($(filter clean userland-clean,$(coast_guard_goals)),)
ifneq ($(filter-out clean userland-clean,$(coast_guard_goals)),)
$(error Run cleanup and build commands separately)
endif
endif
$(coast_guard_goals) &:
	+@python3 -B "$(coast_guard_root)/tools/build_state.py" guard -- $(MAKE) --no-print-directory -f "$(coast_guard_root)/Makefile" $(coast_guard_goals)
else
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
# Local configuration is parsed as data; generated includes contain literals.
coast_quote = '$(subst ','"'"',$(1))'
CONFIG ?= $(srctree)/.config
coast_config_goals := menuconfig config defconfig
ifneq ($(filter $(coast_config_goals),$(MAKECMDGOALS)),)
ifneq ($(filter-out $(coast_config_goals),$(MAKECMDGOALS)),)
$(error Run configuration and build targets as separate make commands)
endif
else
coast_config_include := $(shell python3 -B $(call coast_quote,$(srctree)/tools/build_config.py) emit-make --config $(call coast_quote,$(CONFIG)))
ifneq ($(.SHELLSTATUS),0)
$(error Cannot load $(CONFIG); fix the configuration or run make menuconfig)
endif
ifneq ($(strip $(coast_config_include)),)
include $(coast_config_include)
endif
endif
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
# The default includes every app; selections control build and install dependencies.
JOBS ?= 4
ISO_BUILD ?= $(BUILD_DIR)/coast
ISO_IMAGE ?= $(ISO_BUILD)/image/Coast.iso
ISO_ARGS ?=
APPS ?= all
WITHOUT ?=
ADD ?=
DIAGNOSTICS ?= 0
QUALIFY ?= 0
WITH_MICROSOFT_AUTH ?= 0
ELECTRON_CHECKOUT ?=
RUNTIME_BUILD ?=
COMPONENTS ?=
PACKAGE_CONFIG ?=
COAST_BUILD_OPTIONS = --apps $(call coast_quote,$(APPS)) \
	$(if $(WITHOUT),--without $(call coast_quote,$(WITHOUT))) \
	$(if $(ADD),--add $(call coast_quote,$(ADD))) \
	$(if $(filter 1,$(DIAGNOSTICS)),--diagnostics) \
	$(if $(filter 1,$(QUALIFY)),--qualify) \
	$(if $(filter 1,$(WITH_MICROSOFT_AUTH)),--with-microsoft-auth) \
	$(if $(ELECTRON_CHECKOUT),--electron-checkout $(call coast_quote,$(ELECTRON_CHECKOUT))) \
	$(if $(RUNTIME_BUILD),--runtime-build $(call coast_quote,$(RUNTIME_BUILD))) \
	$(if $(COMPONENTS),--components $(call coast_quote,$(COMPONENTS))) \
	$(if $(PACKAGE_CONFIG),--repository-config $(call coast_quote,$(PACKAGE_CONFIG)))
TEST_ISO := $(BUILD_DIR)/test-fixtures/os.iso
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

.PHONY: all kernel vmlinux prepare kernel-build userland initramfs iso build _test-iso _test-image clean \
	userland-clean help test-boot-config FORCE
all: vmlinux
kernel vmlinux: $(KERNEL_IMAGE)

prepare:
	@mkdir -p "$(objtree)"
	$(Q)python3 -B "$(srctree)/tools/build_state.py" register-kernel $(call coast_quote,$(objtree))
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

.PHONY: fetch-sources check-sources test-source-fetch browser-host test-browser-build test-source-tree test-desktop-assembly test-build-config test-build-progress test-electron-source-sync test-build-lifecycle test-optional-apps source-check source-archive desktop-kernel nasm limine
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
test-build-config:
	$(Q)python3 -B "$(srctree)/tools/test_build_config.py"
test-build-progress:
	$(Q)python3 -B "$(srctree)/tools/test_build_progress.py"
test-electron-source-sync:
	$(Q)python3 -B "$(srctree)/tools/test_electron_source_sync.py"
test-build-lifecycle:
	$(Q)python3 -B "$(srctree)/tools/test_build_lifecycle.py"
test-optional-apps:
	$(Q)python3 -B "$(srctree)/tools/test_optional_apps.py"

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
.PHONY: setup-host image-tools menuconfig config defconfig
HOST_ARGS ?=
menuconfig:
	$(Q)python3 -B $(call coast_quote,$(srctree)/tools/build_config.py) menu --config $(call coast_quote,$(CONFIG))
config:
	$(Q)python3 -B $(call coast_quote,$(srctree)/tools/build_config.py) text --config $(call coast_quote,$(CONFIG))
defconfig:
	$(Q)python3 -B $(call coast_quote,$(srctree)/tools/build_config.py) defaults --config $(call coast_quote,$(CONFIG))
setup-host:
	$(Q)python3 -B "$(srctree)/tools/setup_host.py" --install $(HOST_ARGS)
iso:
	$(Q)python3 -B "$(srctree)/tools/build_desktop.py" --output-root $(call coast_quote,$(ISO_BUILD)) --jobs $(call coast_quote,$(JOBS)) $(COAST_BUILD_OPTIONS) $(ISO_ARGS)
build:
	$(Q)python3 -B "$(srctree)/tools/build_desktop.py" --output-root $(call coast_quote,$(ISO_BUILD)) --jobs $(call coast_quote,$(JOBS)) $(COAST_BUILD_OPTIONS) --build-only $(if $(COMPONENT),--target $(call coast_quote,$(COMPONENT))) $(ISO_ARGS)
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

_test-iso: ISO_IMAGE=$(TEST_ISO)
_test-iso: vmlinux initramfs $(BOOTLOADER_DEPS)
	@mkdir -p "$(dir $(ISO_IMAGE))"
	$(Q)$(MAKE) --no-print-directory _test-image ISO_IMAGE="$(ISO_IMAGE)"

# Internal fixture assembly for in-tree kernel/userland regression checks.
_test-image: ISO_IMAGE=$(TEST_ISO)
_test-image: vmlinux $(BOOTLOADER_DEPS) $(BOOT_CONFIG) $(BOOT_TEMPLATE)
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

COMPONENT ?=
KEEP ?=
ALL ?= 0
PREVIEW ?= 0
clean:
	$(Q)python3 -B "$(srctree)/tools/clean_project.py" --output-root $(call coast_quote,$(abspath $(ISO_BUILD))) \
		--kernel-output $(call coast_quote,$(objtree)) $(if $(COMPONENT),--component $(call coast_quote,$(COMPONENT))) \
		$(if $(filter 1,$(ALL)),--all) $(if $(KEEP),--keep $(call coast_quote,$(KEEP))) $(if $(filter 1,$(PREVIEW)),--preview)
userland-clean:
	$(Q)python3 -B "$(srctree)/tools/clean_project.py" --output-root $(call coast_quote,$(abspath $(ISO_BUILD))) --component userland $(if $(filter 1,$(PREVIEW)),--preview)

help:
	@printf '%s\n' \
		'  make setup-host            Install Ubuntu/Debian host prerequisites' \
		'  make menuconfig            Configure build and VM settings; save .config' \
		'  make iso                   Build the complete Coast desktop and integrations' \
		'  make run                   Boot that ISO in QEMU without rebuilding' \
		'  make build COMPONENT=app   Build an app and its dependencies, without an ISO' \
		'  make iso ADD=app           Add an app to the last ISO selection' \
		'  make clean PREVIEW=1       Preview dependency-aware cleanup' \
		'  make clean COMPONENT=app   Clean a component and invalidate dependent outputs' \
		'  make clean ALL=1           Reset generated products, sources and downloads' \
		'  make clean KEEP="sources downloads toolchains"  Retain reusable inputs' \
		'' \
		'  Apps:          APPS=all|none|"app app" WITHOUT="app app" ADD="app app"' \
		'                 chromium codeoss bash gcc python packages makepkg qt qemu qemu-viewer' \
		'  Build options: JOBS=4 ISO_BUILD=path O=path ISO_ARGS="--plan"' \
		'                 DIAGNOSTICS=0|1 QUALIFY=0|1 WITH_MICROSOFT_AUTH=0|1' \
		'  Run options:   MEMORY=16G SMP=4 QEMU_ACCEL=auto|kvm|tcg QEMU_DISPLAY=gtk,gl=on' \
		'                 ISO_IMAGE=path FIRMWARE=bios|uefi QEMU_EXTRA_ARGS="..."' \
		'  Use the same ISO_BUILD / O= values for iso and run.' \
		'  Saved .config loads automatically; CONFIG=path selects another file.' \
		'  Command-line NAME=value settings override the saved configuration.' \
		'  make config / defconfig    Text questionnaire / save default settings' \
		'  make -n run                Print the QEMU command without launching' \
		'' \
		'  make [-jN]                 Build only the kernel (build/vmlinux)' \
		'  make source-check          Audit sources for publication' \
		'  make source-archive        Export the offline kernel source kit' \
		'  make clean                 Clean generated products; retain sources and downloads' \
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

endif # project build/cleanup guard
