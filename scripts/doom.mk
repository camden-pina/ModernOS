# Native Doomgeneric/Freedoom build and explicit verification targets.
.PHONY: doom doom-sources doom-check-sources test-doom-host test-doom
doom:
	$(MAKE) -C $(srctree)/userland doom-runtime
doom-sources:
	python3 $(srctree)/tools/build_doom.py --fetch-only
doom-check-sources:
	python3 $(srctree)/tools/build_doom.py --check-sources-only
fetch-sources: doom-sources
check-sources: doom-check-sources
test-doom-host:
	python3 $(srctree)/tools/test_doom_host.py
test-doom: _iso-staged
	python3 $(srctree)/tools/test_doom.py --iso $(ISO_IMAGE) --cpus $(SMP) --accel $(QEMU_ACCEL)
