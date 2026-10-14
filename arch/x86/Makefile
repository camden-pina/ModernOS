# x86-64 platform flags and built-in directories. Included by the top Makefile
# and by scripts/Makefile.build while descending into arch/x86.
obj-y := boot/ kernel/ mm/

KBUILD_ARCH_CFLAGS := -m64 -march=x86-64 -mabi=sysv -mno-80387 -mno-mmx \
	-mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel
KBUILD_ARCH_LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000
KERNEL_LDS := $(srctree)/arch/x86/kernel/vmlinux.lds
