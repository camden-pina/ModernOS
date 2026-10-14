obj-y := cpu.o entry.o io.o tsc.o boot_smp.o boot_ap.o descriptor_tables/ interrupts/

# GAS resolves .include directives separately from C include directories.
asflags-y += -Wa,-I$(src)
$(objdir)/cpu.o: $(src)/kernel.inc
