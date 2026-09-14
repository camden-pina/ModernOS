# ============================================
# User Configuration (override these as needed)
# ============================================
PROJECT_DIR   ?= $(shell pwd)
BUILD_DIR     ?= $(PROJECT_DIR)/build
TOOL_ROOT     ?= $(BUILD_DIR)/toolchain
SYS_ROOT      ?= $(PROJECT_DIR)/sysroot

NPROC         ?= $(shell nproc)
MAKE          := make
MAKE_J        := $(MAKE) -j$(NPROC)

# Colors for messages
CYAN          := \033[36m
RESET         := \033[0m

# ============================================
# EDK2 repository settings
# ============================================
EDK2_VERSION  ?= edk2-stable202608
EDK2_REPO     ?= https://github.com/tianocore/edk2.git
EDK2_DIR      ?= $(PROJECT_DIR)/edk2

# ============================================
# Build control flags
# ============================================
SKIP_CONFIGURE ?=
SKIP_BUILD     ?=
SKIP_INSTALL   ?=

# ============================================
# EDK2 build options
# ============================================
BUILD_TYPE     ?= RELEASE
ARCH           ?= X64
TOOLCHAIN      ?= GCC

ARCH := $(strip $(ARCH))

# EDK2 platform
PLATFORM       := Bootloader_$(ARCH)/MyPlatform.dsc

# ============================================
# Phony targets
# ============================================
.PHONY: help setup copy_arch build install bootloader symlink all clean

# --------------------------------------------
# help
# --------------------------------------------
help:
	@echo "$(CYAN)Usage:$(RESET)"
	@echo "  make setup                         - Clone and prepare EDK2 and build BaseTools."
	@echo "  make bootloader [ARCH=<arch>]      - Build the bootloader for the specified architecture."
	@echo "  make build                         - Build the bootloader using MyPlatform.dsc."
	@echo "  make install                       - Copy the generated EFI file to the build directory."
	@echo "  make symlink SRC=<src> DEST=<dest> - Create a relative symlink."
	@echo "  make all                           - Run setup, build, and install sequentially."
	@echo "  make clean                         - Clean build artifacts."

# --------------------------------------------
# setup
# --------------------------------------------
setup:
	@export HOST_WARNINGS="-Wno-error=unused-but-set-variable"; \
	echo "$(CYAN)[Setup] Creating build directory...$(RESET)"; \
	mkdir -p "$(BUILD_DIR)"

	@if [ ! -d "$(EDK2_DIR)" ]; then \
	  echo "$(CYAN)[Setup] Cloning EDK2 repository (tag $(EDK2_VERSION))...$(RESET)"; \
	  git clone --depth 1 -c advice.detachedHead=false \
	    --branch "$(EDK2_VERSION)" "$(EDK2_REPO)" "$(EDK2_DIR)"; \
	  cd "$(EDK2_DIR)" && git submodule update --init; \
	else \
	  echo "$(CYAN)[Setup] EDK2 repository already exists at $(EDK2_DIR).$(RESET)"; \
	fi

	@echo "$(CYAN)[Setup] Building BaseTools...$(RESET)"
	@cd "$(EDK2_DIR)" && . ./edksetup.sh && $(MAKE) -C BaseTools all

# --------------------------------------------
# copy_arch
# --------------------------------------------
copy_arch:
	@if [ ! -d "$(ARCH)" ]; then \
	  echo "$(CYAN)[Error] Directory '$(ARCH)' not found in $(PROJECT_DIR).$(RESET)"; \
	  exit 1; \
	fi

	@echo "$(CYAN)[Copy] Copying $(ARCH) to $(EDK2_DIR)/Bootloader_$(ARCH)...$(RESET)"

	@mkdir -p "$(EDK2_DIR)"
	@rm -rf "$(EDK2_DIR)/Bootloader_$(ARCH)"
	@cp -r "$(ARCH)" "$(EDK2_DIR)/Bootloader_$(ARCH)"

# --------------------------------------------
# build
# --------------------------------------------
build:
	@if [ ! -d "$(EDK2_DIR)" ]; then \
	  echo "$(CYAN)[Build] EDK2 directory not found. Running setup...$(RESET)"; \
	  $(MAKE) setup; \
	fi

	@if [ "$(SKIP_BUILD)" = "1" ]; then \
	  echo "$(CYAN)[Build] Skipping build step as SKIP_BUILD is set.$(RESET)"; \
	else \
	  echo "$(CYAN)[Build] Building bootloader...$(RESET)"; \
	  echo "$(CYAN)         ARCH      = $(ARCH)$(RESET)"; \
	  echo "$(CYAN)         BUILD_TYPE = $(BUILD_TYPE)$(RESET)"; \
	  echo "$(CYAN)         TOOLCHAIN = $(TOOLCHAIN)$(RESET)"; \
	  echo "$(CYAN)         PLATFORM  = $(PLATFORM)$(RESET)"; \
	  cd "$(EDK2_DIR)" && \
	    bash -c '. ./edksetup.sh --reconfig && \
	      build \
	        -p "$(PLATFORM)" \
	        -a "$(ARCH)" \
	        -t "$(TOOLCHAIN)" \
	        -b "$(BUILD_TYPE)"'; \
	fi

	@echo "$(CYAN)[Build] Build step completed.$(RESET)"

	@if [ "$(SKIP_INSTALL)" != "1" ]; then \
	  $(MAKE) install; \
	fi

# --------------------------------------------
# install
# --------------------------------------------
install:
	@if [ "$(SKIP_INSTALL)" = "1" ]; then \
	  echo "$(CYAN)[Install] Skipping install step as SKIP_INSTALL is set.$(RESET)"; \
	else \
	  echo "$(CYAN)[Install] Installing output files...$(RESET)"; \
	  mkdir -p "$(BUILD_DIR)"; \
	  EFI_FILE="$$(find "$(EDK2_DIR)/Build" \
	    -type f \
	    \( -iname 'BOOTX64.EFI' -o -iname 'BOOTX64.efi' \) \
	    -print -quit)"; \
	  if [ -z "$$EFI_FILE" ]; then \
	    echo "$(CYAN)[Error] BOOTX64.EFI was not found in the EDK2 Build directory.$(RESET)"; \
	    echo "$(CYAN)[Error] Search path: $(EDK2_DIR)/Build$(RESET)"; \
	    exit 1; \
	  fi; \
	  echo "$(CYAN)[Install] Found: $$EFI_FILE$(RESET)"; \
	  cp "$$EFI_FILE" "$(BUILD_DIR)/BOOTX64.EFI"; \
	  echo "$(CYAN)[Install] Copied BOOTX64.EFI to $(BUILD_DIR)/BOOTX64.EFI$(RESET)"; \
	fi

# --------------------------------------------
# bootloader
# --------------------------------------------
bootloader: setup copy_arch build

# --------------------------------------------
# all
# --------------------------------------------
all: setup copy_arch build

# --------------------------------------------
# symlink
# --------------------------------------------
symlink:
	@if [ -z "$(SRC)" ] || [ -z "$(DEST)" ]; then \
	  echo "Usage: make symlink SRC=<source file> DEST=<destination file>"; \
	  exit 1; \
	fi

	@mkdir -p "$(dir $(DEST))"
	@ln -sf "$$(realpath --relative-to="$$(dirname "$(DEST)")" "$(SRC)")" "$(DEST)"
	@echo "$(CYAN)[Symlink] Created symlink: $(DEST) -> $(SRC)$(RESET)"

# --------------------------------------------
# clean
# --------------------------------------------
clean:
	@echo "$(CYAN)[Clean] Cleaning project build directory...$(RESET)"
	@rm -rf "$(BUILD_DIR)"

	@if [ -d "$(EDK2_DIR)" ]; then \
	  echo "$(CYAN)[Clean] Cleaning EDK2 build artifacts...$(RESET)"; \
	  cd "$(EDK2_DIR)" && \
	    . ./edksetup.sh --reconfig && \
	    build \
	      -p "$(PLATFORM)" \
	      -a "$(ARCH)" \
	      -t "$(TOOLCHAIN)" \
	      -b "$(BUILD_TYPE)" \
	      clean || true; \
	  rm -rf "$(EDK2_DIR)/Build"; \
	  rm -rf "$(EDK2_DIR)/Bootloader_$(ARCH)"; \
	fi

	@echo "$(CYAN)[Clean] Done.$(RESET)"
