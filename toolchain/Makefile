# Makefile for building Binutils, GCC, and dependencies (GMP, MPFR, MPC) for ModernOS

# -----------------------
# Versions (modify as needed)
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
# Source tarball URLs
GCC_URL       := https://ftp.gnu.org/gnu/gcc/gcc-$(GCC_VERSION)/gcc-$(GCC_VERSION).tar.gz
BINUTILS_URL  := https://ftp.gnu.org/gnu/binutils/binutils-$(BINUTILS_VERSION).tar.gz
GMP_URL       := https://ftp.gnu.org/gnu/gmp/gmp-$(GMP_VERSION).tar.xz
MPFR_URL      := https://www.mpfr.org/mpfr-$(MPFR_VERSION)/mpfr-$(MPFR_VERSION).tar.xz
MPC_URL       := https://ftp.gnu.org/gnu/mpc/mpc-$(MPC_VERSION).tar.gz

TOOLCHAIN := $(PREFIX)/bin/$(TARGET)-gcc

# -----------------------
# Detect if aria2c is available
DOWNLOAD_TOOL := $(shell command -v aria2c 2>/dev/null)

# -----------------------
# Default target: final GCC binary installed
all: $(TOOLCHAIN)

# -----------------------
# Download rules
$(SRC_DIR)/gcc-$(GCC_VERSION).tar.gz:
	@mkdir -p $(SRC_DIR)
ifeq ($(DOWNLOAD_TOOL),)
	wget -O $@ $(GCC_URL)
else
	cd $(SRC_DIR) && aria2c -x 16 -s 16 -o $(notdir $@) $(GCC_URL)
endif

$(SRC_DIR)/binutils-$(BINUTILS_VERSION).tar.gz:
	@mkdir -p $(SRC_DIR)
ifeq ($(DOWNLOAD_TOOL),)
	wget -O $@ $(BINUTILS_URL)
else
	cd $(SRC_DIR) && aria2c -x 16 -s 16 -o $(notdir $@) $(BINUTILS_URL)
endif

$(SRC_DIR)/gmp-$(GMP_VERSION).tar.xz:
	@mkdir -p $(SRC_DIR)
ifeq ($(DOWNLOAD_TOOL),)
	wget -O $@ $(GMP_URL)
else
	cd $(SRC_DIR) && aria2c -x 16 -s 16 -o $(notdir $@) $(GMP_URL)
endif

$(SRC_DIR)/mpfr-$(MPFR_VERSION).tar.xz:
	@mkdir -p $(SRC_DIR)
ifeq ($(DOWNLOAD_TOOL),)
	wget -O $@ $(MPFR_URL)
else
	cd $(SRC_DIR) && aria2c -x 16 -s 16 -o $(notdir $@) $(MPFR_URL)
endif

$(SRC_DIR)/mpc-$(MPC_VERSION).tar.gz:
	@mkdir -p $(SRC_DIR)
ifeq ($(DOWNLOAD_TOOL),)
	wget -O $@ $(MPC_URL)
else
	cd $(SRC_DIR) && aria2c -x 16 -s 16 -o $(notdir $@) $(MPC_URL)
endif

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
