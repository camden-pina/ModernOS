# Makefile for building Binutils, GCC, and dependencies (GMP, MPFR, MPC) for ModernOS

# -----------------------
# Versions (must match the pins in userland/vendor/sources.json)
GCC_VERSION       := 14.2.0
BINUTILS_VERSION  := 2.37
GMP_VERSION       := 6.2.1
MPFR_VERSION      := 4.1.0
MPC_VERSION       := 1.2.1

# -----------------------
# Directories (all relative to toolchain/)
PREFIX    := $(CURDIR)/opt/cross
TARGET    := x86_64-elf
SRC_DIR   := $(CURDIR)/src
BUILD_DIR := $(CURDIR)/build

# -----------------------
# Number of parallel jobs (defaults to number of cores)
JOBS ?= $(shell nproc)

# -----------------------
# Use the same pinned sources, mirrors and atomic downloads as the ISO build.
SOURCE_FETCHER := $(abspath ../tools/fetch_sources.py)
SOURCE_ARCHIVES := gcc-$(GCC_VERSION).tar.gz binutils-$(BINUTILS_VERSION).tar.gz \
                   gmp-$(GMP_VERSION).tar.xz mpfr-$(MPFR_VERSION).tar.xz mpc-$(MPC_VERSION).tar.gz
export SOURCE_MIRRORS

TOOLCHAIN := $(PREFIX)/bin/$(TARGET)-gcc

# -----------------------
# Default target: final GCC binary installed
all: $(TOOLCHAIN)

# -----------------------
# Download rules
$(addprefix $(SRC_DIR)/,$(SOURCE_ARCHIVES)):
	python3 "$(SOURCE_FETCHER)" "$(@F)" --destination "$(SRC_DIR)"

# -----------------------
# Extraction rules with stamp files
$(SRC_DIR)/gcc-$(GCC_VERSION)/.extracted: $(SRC_DIR)/gcc-$(GCC_VERSION).tar.gz
	tar -xzf $< -C $(SRC_DIR)
	touch $@

$(SRC_DIR)/binutils-$(BINUTILS_VERSION)/.extracted: $(SRC_DIR)/binutils-$(BINUTILS_VERSION).tar.gz
	tar -xzf $< -C $(SRC_DIR)
	touch $@

$(SRC_DIR)/gmp-$(GMP_VERSION)/.extracted: $(SRC_DIR)/gmp-$(GMP_VERSION).tar.xz
	tar -xf $< -C $(SRC_DIR)
	touch $@

$(SRC_DIR)/mpfr-$(MPFR_VERSION)/.extracted: $(SRC_DIR)/mpfr-$(MPFR_VERSION).tar.xz
	tar -xf $< -C $(SRC_DIR)
	touch $@

$(SRC_DIR)/mpc-$(MPC_VERSION)/.extracted: $(SRC_DIR)/mpc-$(MPC_VERSION).tar.gz
	tar -xzf $< -C $(SRC_DIR)
	touch $@

# -----------------------
# Link GCC dependencies (create symlinks for GMP, MPFR, MPC)
$(SRC_DIR)/gcc-$(GCC_VERSION)/.deps_done: \
	$(SRC_DIR)/gcc-$(GCC_VERSION)/.extracted \
	$(SRC_DIR)/gmp-$(GMP_VERSION)/.extracted \
	$(SRC_DIR)/mpfr-$(MPFR_VERSION)/.extracted \
	$(SRC_DIR)/mpc-$(MPC_VERSION)/.extracted
	cd $(SRC_DIR)/gcc-$(GCC_VERSION) && \
	rm -rf gmp mpfr mpc && \
	ln -s ../gmp-$(GMP_VERSION) gmp && \
	ln -s ../mpfr-$(MPFR_VERSION) mpfr && \
	ln -s ../mpc-$(MPC_VERSION) mpc
	touch $@

# -----------------------
# Build and install Binutils (stamp: $(BUILD_DIR)/binutils/.build_done)export PATH="$HOME/opt/cross/bin:$PATH"

$(BUILD_DIR)/binutils/.build_done: $(SRC_DIR)/binutils-$(BINUTILS_VERSION)/.extracted
	@mkdir -p $(BUILD_DIR)/binutils
	cd $(BUILD_DIR)/binutils && \
	$(SRC_DIR)/binutils-$(BINUTILS_VERSION)/configure \
		--target=$(TARGET) \
		--prefix=$(PREFIX) \
		--with-sysroot \
		--disable-nls \
		--disable-werror && \
	$(MAKE) -j$(JOBS) && \
	$(MAKE) install
	touch $@

# Final target for Binutils installation
$(PREFIX)/bin/$(TARGET)-as: $(BUILD_DIR)/binutils/.build_done

# -----------------------
# Build and install GCC (stamp: $(BUILD_DIR)/gcc/.build_done)
$(BUILD_DIR)/gcc/.build_done: $(PREFIX)/bin/$(TARGET)-as $(SRC_DIR)/gcc-$(GCC_VERSION)/.deps_done
	@mkdir -p $(BUILD_DIR)/gcc
	cd $(BUILD_DIR)/gcc && \
	$(SRC_DIR)/gcc-$(GCC_VERSION)/configure \
		--target=$(TARGET) \
		--prefix=$(PREFIX) \
		--disable-nls \
		--enable-languages=c,c++ \
		--without-headers && \
	$(MAKE) -j$(JOBS) all-gcc && \
	$(MAKE) -j$(JOBS) all-target-libgcc && \
	$(MAKE) -j$(JOBS) install-gcc && \
	$(MAKE) -j$(JOBS) install-target-libgcc
	touch $@

# Final target for GCC installation
$(PREFIX)/bin/$(TARGET)-gcc: $(BUILD_DIR)/gcc/.build_done
# Set the path
	@echo 'export PATH="$(PREFIX)/bin:$$PATH"'

# -----------------------
# Clean target
clean:
	rm -rf $(BUILD_DIR) $(PREFIX) opt/ src/

.PHONY: all clean
