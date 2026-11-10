CC ?= cc
LD ?= ld
.DEFAULT_GOAL := all
export SOURCE_MIRRORS

# All upstream port builders depend on bin/nano. Restore their sources before
# running them, including direct port targets invoked from the root Makefile.
.PHONY: sources
sources:
	python3 ../tools/fetch_sources.py
bin/nano: | sources
vendor/%.tar.gz:
	python3 ../tools/fetch_sources.py "$(@F)"
vendor/%.tar.xz:
	python3 ../tools/fetch_sources.py "$(@F)"
vendor/%.tar.bz2:
	python3 ../tools/fetch_sources.py "$(@F)"
vendor/%.tgz:
	python3 ../tools/fetch_sources.py "$(@F)"

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -m64 -mno-avx -mno-avx2
CPPFLAGS := -Iinclude -I../include -I../arch/x86/include/asm
PROGRAMS := passwd login id whoami sudo code-launch profile-session clip coast-sh hello check spin fault exec-test edit gui env stty tty reset build-sh browser-sandbox-check
TEXT_TOOLS := head tail wc grep find hexdump
APPLETS := sh ash printf expr test [ [[ true false cp mv ls mkdir rmdir rm cat echo pwd chmod sleep touch uname ln install yes tee sort uniq tr cut cmp cksum md5sum sha256sum od basename dirname sed diff patch tar gzip gunzip zcat bzip2 bunzip2 bzcat xz unxz xzcat xargs seq readlink realpath mktemp date getopt du
SDK_TOOLS := gcc g++ cc c++ cpp as ld ar ranlib nm objcopy objdump readelf size strings strip bootstrap-toolchain
SDK_EXAMPLE_SOURCES := sdk/hello.c sdk/hello.cpp sdk/Makefile compiler-check.c include/memory_usage.h
SDK_EXAMPLE_OUTPUTS := $(addprefix runtime/usr/src/coast/examples/,hello.c hello.cpp Makefile abi-check.c include/memory_usage.h)
JSC_CHECK_NAMES := engine-check.js module-check.mjs feature-module.mjs agent-check.js
JSC_CHECK_OUTPUTS := $(addprefix runtime/usr/share/modern-browser/,$(JSC_CHECK_NAMES))
# Keep the console shell's established tools available under /bin. The build
# environment selects full upstream versions, with stdin/regex/script options.
BUILD_APPLETS := grep egrep fgrep head tail wc find hexdump stat
TARGETS := $(addprefix bin/,$(PROGRAMS) $(TEXT_TOOLS))
.PHONY: all clean
all: $(TARGETS) $(addprefix bin/,$(APPLETS) $(SDK_TOOLS)) $(addprefix runtime/usr/bin/,$(BUILD_APPLETS)) bin/nano bin/posix-check bin/build-check bin/compiler-check bin/network-check bin/thread-check bin/ipc-check bin/web-capability-check bin/ramfs-check bin/speaker-check bin/vim bin/busybox bin/make bin/awk bin/gawk bin/git bin/curl bin/openssl bin/jsc bin/thread-cxx-check $(JSC_CHECK_OUTPUTS) bin/wallpaper-decode runtime/usr/bin/gcc runtime/usr/bin/bootstrap-toolchain $(SDK_EXAMPLE_OUTPUTS)
.PHONY: native
native: $(TARGETS) $(addprefix bin/,$(APPLETS))
bin/speaker-check: speaker-check.c include/audio_protocol.h ../include/uapi/audio.h bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -lm -o $@
bin/jsc bin/thread-cxx-check $(JSC_CHECK_OUTPUTS) &: ../tools/build_javascriptcore.py ../tools/prepare_browser_host.py ../tools/build_threaded_libgcc.py modern-browser/runtime-check.cpp $(addprefix modern-browser/,$(JSC_CHECK_NAMES)) runtime/usr/bin/gcc
	python3 ../tools/build_javascriptcore.py
	@touch bin/jsc bin/thread-cxx-check $(JSC_CHECK_OUTPUTS)
bin/ipc-check bin/web-capability-check bin/ramfs-check: bin/%: %.c include/web_process.h include/browser_protocol.h bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -pthread -static -no-pie $< -o $@
bin/thread-check: thread-check.c bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -pthread -static -no-pie $< -o $@
bin/wallpaper-decode: wallpaper-decode.c include/wallpaper_protocol.h bin/curl
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie -I../build/web/deps/include $< -L../build/web/deps/lib -lpng16 -ljpeg -lz -lm -o $@
bin/awk bin/gawk &: ../tools/build_gawk.py vendor/gawk-5.4.0.tar.xz bin/nano
	python3 ../tools/build_gawk.py
bin/network-check: network-check.c bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@
bin/git: ../tools/build_git.py ../tools/build_web_deps.py vendor/git-2.56.0.tar.xz vendor/zlib-1.3.2.tar.xz bin/curl bin/nano
	python3 ../tools/build_git.py
bin/curl bin/openssl &: ../tools/build_web_deps.py $(wildcard vendor/openssl-*.tar.* vendor/curl-*.tar.* vendor/libpng-*.tar.* vendor/jpeg*.tar.* vendor/freetype-*.tar.* vendor/expat-*.tar.*) vendor/mozilla-ca-bundle.pem bin/nano
	python3 ../tools/build_web_deps.py
bin/nano: ../tools/build_nano.py $(wildcard vendor/*.tar.*)
	python3 ../tools/build_nano.py
bin/posix-check: posix-check.c bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@
bin/build-check: build-check.c bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@
bin/compiler-check: compiler-check.c include/memory_usage.h bin/nano
	../build/native-musl/bin/musl-gcc -O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@
bin/vim bin/busybox bin/make &: ../tools/build_native_ports.py vendor/busybox-coast.config config/vimrc $(wildcard vendor/*.tar.*) bin/nano
	python3 ../tools/build_native_ports.py
runtime/usr/bin/gcc: ../tools/build_gcc.py $(wildcard vendor/gcc-*.tar.* vendor/binutils-*.tar.* vendor/gmp-*.tar.* vendor/mpfr-*.tar.* vendor/mpc-*.tar.*) bin/nano
	python3 ../tools/build_gcc.py
runtime/usr/bin/bootstrap-toolchain $(SDK_EXAMPLE_OUTPUTS) &: ../tools/package_sdk.py ../tools/build_gcc.py ../tools/build_gawk.py $(wildcard sdk/*) $(SDK_EXAMPLE_SOURCES) vendor/gawk-5.4.0.tar.xz runtime/usr/bin/gcc bin/gawk
	python3 ../tools/package_sdk.py
	@touch runtime/usr/bin/bootstrap-toolchain $(SDK_EXAMPLE_OUTPUTS)
obj/%.o: %.c include/libc.h ../include/uapi/abi.h Makefile
	@mkdir -p obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
obj/crt0.o: crt0.S
	@mkdir -p obj
	$(CC) -m64 -c $< -o $@
bin/%: obj/%.o obj/libc.o obj/crt0.o user.ld
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o $< -o $@
COMPLETION_OBJECTS := obj/shell_completion.o obj/shell_completion_native.o
obj/shell_completion.o obj/sh.o: shell_commands.inc
obj/sudo.o obj/code-launch.o obj/passwd.o obj/login.o obj/profile-session.o obj/sh.o: ../include/uapi/account.h
obj/passwd.o obj/login.o: include/account_secret.h
obj/shell_completion.o obj/shell_completion_native.o obj/shell_line_editor.o obj/shell_terminal_editor.o: include/shell_completion.h desktop/text_input_model.h
bin/coast-sh: obj/sh.o obj/libc.o obj/crt0.o obj/shell_line_editor.o obj/shell_terminal_editor.o $(COMPLETION_OBJECTS) user.ld
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/sh.o obj/shell_line_editor.o obj/shell_terminal_editor.o $(COMPLETION_OBJECTS) -o $@
$(addprefix bin/,$(SDK_TOOLS)): bin/%: obj/sdk-tool.o obj/libc.o obj/crt0.o user.ld runtime/usr/bin/gcc
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/sdk-tool.o -o $@
bin/bootstrap-toolchain: runtime/usr/bin/bootstrap-toolchain $(SDK_EXAMPLE_OUTPUTS)
$(addprefix bin/,$(TEXT_TOOLS)): bin/%: obj/tools.o obj/libc.o obj/crt0.o user.ld
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/tools.o -o $@
$(addprefix bin/,$(APPLETS)): bin/%: obj/applet.o obj/libc.o obj/crt0.o user.ld
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/applet.o -o $@
$(addprefix runtime/usr/bin/,$(BUILD_APPLETS)): runtime/usr/bin/%: obj/applet.o obj/libc.o obj/crt0.o user.ld
	@mkdir -p runtime/usr/bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/applet.o -o $@
GUI_SOURCES := $(wildcard desktop/*.c)
GUI_OBJECTS := $(patsubst desktop/%.c,obj/desktop/%.o,$(GUI_SOURCES))
obj/desktop/%.o: desktop/%.c $(wildcard desktop/*.h) include/libc.h include/browser_protocol.h include/wallpaper_protocol.h include/codeoss_protocol.h ../include/uapi/abi.h Makefile
	@mkdir -p obj/desktop
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
bin/gui: obj/gui.o $(GUI_OBJECTS) $(COMPLETION_OBJECTS) obj/libc.o obj/crt0.o user.ld
	@mkdir -p bin
	$(LD) -nostdlib -static -z max-page-size=4096 -z noexecstack -T user.ld obj/crt0.o obj/libc.o obj/gui.o $(GUI_OBJECTS) $(COMPLETION_OBJECTS) -o $@
.SECONDARY:
clean:
	rm -rf obj bin runtime

# Cached System & Power status parser is shared by its desktop object.
obj/desktop/power.o: include/power_status.h

include dynamic.mk

include unix-tools.mk

include doom.mk

obj/clip.o: ../include/uapi/clipboard.h

# Semantic fixture for the copied software window ABI; never maps a GPU.
bin/window-check: window-check.c ../include/uapi/window.h bin/nano
	../build/native-musl/bin/musl-gcc -O2 -g -std=gnu11 -Wall -Wextra -Werror -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -static -no-pie $< -o $@
all: bin/window-check

runtime/usr/include/coast/window.h: ../include/uapi/window.h
	@mkdir -p $(@D)
	cp $< $@
all: runtime/usr/include/coast/window.h

obj/desktop/game.o: ../include/uapi/window.h
obj/shell_completion.o: ../include/text_utf8.h

# Opt-in genuine upstream text-core component. Uses existing standalone JSC
# archives; does not trigger browser/engine rebuilding or ISO replacement.
.PHONY: codeoss-core
codeoss-core: bin/codeoss-core
bin/codeoss-core: codeoss/runner.cpp codeoss/file-service.cpp codeoss/file-service.h ../tools/build_codeoss_core.py
	python3 ../tools/build_codeoss_core.py

# Experimental native frontend + genuine upstream text core in a separate JSC worker.
.PHONY: codeoss-graphical
codeoss-graphical: bin/gui bin/codeoss-model bin/codeoss-launch
bin/codeoss-model bin/codeoss-launch &: codeoss/model-worker.cpp codeoss/launcher.c codeoss/file-service.cpp codeoss/file-service.h include/codeoss_protocol.h ../tools/build_codeoss_graphical.py ../ports/codeoss/graphical-model.js ../ports/codeoss/GRAPHICAL-SOURCES.json ../ports/codeoss/graphical-shared-check.js
	python3 ../tools/build_codeoss_graphical.py
