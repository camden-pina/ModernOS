CC = gcc-dynamic
CFLAGS = -O2 -Wall -Wextra

.PHONY: all check clean
all: libdynamic-leaf.so libdynamic-middle.so libdynamic-plugin.so hello transitive
libdynamic-leaf.so: leaf.c
	$(CC) $(CFLAGS) -fPIC -shared $< -Wl,-soname,$@ -o $@
libdynamic-middle.so: middle.c libdynamic-leaf.so
	$(CC) $(CFLAGS) -fPIC -shared $< -L. -ldynamic-leaf -Wl,-soname,$@ -Wl,-rpath,'$$ORIGIN' -o $@
libdynamic-plugin.so: plugin.c
	$(CC) $(CFLAGS) -fPIC -shared $< -Wl,-soname,$@ -o $@
hello: hello.c
	$(CC) $(CFLAGS) $< -o $@
transitive: transitive.c libdynamic-middle.so libdynamic-leaf.so
	$(CC) $(CFLAGS) $< -L. -ldynamic-middle -Wl,-rpath,'$$ORIGIN' -Wl,-rpath-link,. -o $@
check: all
	./hello
	./transitive
clean:
	rm -f libdynamic-leaf.so libdynamic-middle.so libdynamic-plugin.so hello transitive
