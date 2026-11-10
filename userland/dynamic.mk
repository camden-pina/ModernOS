# The shared loader uses its own pinned musl cache; established static ports do
# not rebuild when the dynamic execution profile changes.
.PHONY: dynamic-linker
dynamic-linker: bin/nano runtime/usr/bin/gcc
	python3 ../tools/build_dynamic.py --jobs 1
all: dynamic-linker

.PHONY: dynamic-cpp
dynamic-cpp: dynamic-linker
	python3 ../tools/build_dynamic_cpp.py --jobs 1
all: dynamic-cpp
