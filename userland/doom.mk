# Native Doomgeneric/Freedoom; kept separate from the browser/compiler ports.
.PHONY: doom-runtime
all: bin/doom doom-runtime
bin/doom: ../tools/build_doom.py $(wildcard doom/*) bin/nano
	python3 ../tools/build_doom.py --engine-only
doom-runtime: bin/doom ../tools/build_freedoom.py
	python3 ../tools/build_doom.py

.PHONY: doom-clean-runtime
clean: doom-clean-runtime
doom-clean-runtime:
	rm -rf runtime/usr/share/games/doom
