# Included by the top-level Makefile. This PE32+ loader has no libc/gnu-efi dependency.
EFI_CC ?= clang
EFI_LD ?= lld-link
EFI_TEST_CC ?= cc
EFI_IMAGE := $(objtree)/arch/x86/boot/efi/modernos.efi
EFI_OBJDIR := $(dir $(EFI_IMAGE))
EFI_SOURCES := $(srctree)/arch/x86/boot/efi
EFI_CFLAGS := --target=x86_64-pc-win32-coff -ffreestanding -fshort-wchar \
	-fno-stack-protector -mno-stack-arg-probe -mno-red-zone -fno-builtin \
	-Wall -Wextra -Werror -O2 -std=c11 -I$(srctree)/include
EFI_OBJECTS := $(EFI_OBJDIR)loader.obj $(EFI_OBJDIR)transition.obj

.PHONY: efi efi-clean test-efi-loader
efi: $(EFI_IMAGE)

$(EFI_OBJDIR)loader.obj: $(EFI_SOURCES)/loader.c $(EFI_SOURCES)/efi.h $(srctree)/include/boot/info.h $(EFI_SOURCES)/Makefile
	@mkdir -p "$(EFI_OBJDIR)"
	$(Q)$(EFI_CC) $(EFI_CFLAGS) -c "$<" -o "$@"

$(EFI_OBJDIR)transition.obj: $(EFI_SOURCES)/transition.S $(EFI_SOURCES)/Makefile
	@mkdir -p "$(EFI_OBJDIR)"
	$(Q)$(EFI_CC) --target=x86_64-pc-win32-coff -c "$<" -o "$@"

$(EFI_IMAGE): $(EFI_OBJECTS)
	$(Q)$(EFI_LD) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /dynamicbase /out:"$@" $(EFI_OBJECTS)

efi-clean:
	$(Q)rm -f -- $(EFI_OBJECTS) "$(EFI_IMAGE)" "$(EFI_OBJDIR)loader-test"

$(EFI_OBJDIR)loader-test: $(EFI_SOURCES)/loader_test.c $(EFI_SOURCES)/loader.c $(EFI_SOURCES)/efi.h $(srctree)/include/boot/info.h $(EFI_SOURCES)/Makefile
	@mkdir -p "$(EFI_OBJDIR)"
	$(Q)$(EFI_TEST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-builtin \
		-ffunction-sections -fdata-sections -fsanitize=undefined -fno-sanitize-recover=all \
		-I$(srctree)/include -Wl,--gc-sections -no-pie "$<" -o "$@"

test-efi-loader: $(EFI_OBJDIR)loader-test
	$(Q)"$<"
