# Small, source-backed BusyBox tools for both the console and POSIX shells.
# Kept separate because the main Makefile also owns the larger upstream ports.
UNIX_APPLETS := less nl tac rev paste comm fold base64 split clear which
APPLETS += $(UNIX_APPLETS)
all: $(addprefix bin/,$(UNIX_APPLETS)) bin/unix-tools-check

$(addprefix bin/,$(UNIX_APPLETS)): bin/%: obj/applet.o obj/libc.o obj/crt0.o user.ld
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/applet.o -o $@

bin/unix-tools-check: unix-tools-check.c bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@

# Native foreground group/terminal permission and controller-exit fixture.
all: bin/foreground-check
bin/foreground-check: foreground-check.c bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@

all: bin/clipboard-check
bin/clipboard-check: clipboard-check.c ../include/uapi/clipboard.h bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@

# Public native SDK headers are independent of the freestanding syscall enum.
all: runtime/usr/include/coast/clipboard.h runtime/usr/include/coast/pointer_caps.h runtime/usr/src/coast/examples/clipboard-example.c
runtime/usr/include/coast/clipboard.h: ../include/uapi/clipboard.h
	@mkdir -p $(@D)
	cp $< $@
runtime/usr/include/coast/pointer_caps.h: ../include/uapi/pointer_caps.h
	@mkdir -p $(@D)
	cp $< $@
runtime/usr/src/coast/examples/clipboard-example.c: sdk/clipboard-example.c
	@mkdir -p $(@D)
	cp $< $@
