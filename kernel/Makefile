.PHONY: compile_bootloader
compile_bootloader:
	@$(MAKE) -C bootloader compile_bootloader
	
.PHONY: compile_kernel
compile_kernel:
	@$(MAKE) -C x86_64 kernel.elf

.PHONY: clean
clean:
	@$(MAKE) -C x86_64 clean
