# Qt 6 native port

Coast has an initial **Qt 6.8.3 Core, Gui and Widgets** port. Open **Qt 6 Demo**
from Spotlight to try the actual Qt controls, text editor and file save. It
runs with the active profile's UID/GID and private home. The desktop passes
that profile's UI scale to Qt; ordinary resizes change the Qt layout without
scaling its retained pixels.

The `coast` QPA plugin uses the native copied-pixel window service. It
implements raster backing stores, partial frame damage, configure/focus/close
events, keyboard input, pointer buttons/wheel and cursor bitmaps. The desktop
provides decorations, movement, resizing, maximize and snapping. The backend
uses FreeType with the installed DejaVu fonts and Qt's Unix event dispatcher.
Qt's worker pool and semaphore handoff use the kernel's Linux-compatible
`FUTEX_WAKE_OP` implementation, including atomic operations, signed comparisons
and conditional wakeups.

Native window clients can submit their own frames and read their own input
queues. An ordinary client cannot operate the display server or inspect peer
windows. Admission requires the currently signed-in account; browser and
Electron namespaces retain their existing restrictions. Switching profiles
closes the Qt process together with other apps and sessions.

Qt's normal copy/paste shortcuts share the native **UTF-8 text clipboard** with
Files, Text Editor and Terminal. Text is limited to 64 KiB; images, arbitrary
MIME data and primary-selection mode are not implemented. A native client
without terminal descriptors may access the clipboard only while its owned
window has focus. Inheriting the desktop's process group does not grant access.
The backend detects changes when focus returns and uses generation-checked
reads to avoid mixing two clipboard versions.
The `clip` copy/paste command briefly retries a denied operation while Bash
hands a new foreground job its terminal. Each retry still requires kernel
authorization; background commands remain denied.

## Fresh source desktop

`make iso` builds this port with the source pipeline's POSIX musl
compiler and includes its binary, source archive and notices in the fresh image.
It does not need an existing `build/modern-browser` C++ prefix. Its Qt SDK and
host generators remain in the selected component output. A standalone fresh
build can select the same compiler explicitly:

```sh
python3 -B tools/build_qt6.py --compiler-build build/my-desktop/compiler/BUILD.json \
  --output-root build/my-qt --jobs 4
```

The development commands below use the existing Coast musl/GCC toolchain and
install into the staged userland.

## Build

On the Linux development host, with the existing Coast musl/GCC and threaded
C++ runtime available:

```sh
make qt6
```

The Qt target needs CMake, Ninja and Bubblewrap. It downloads QtBase from the
official archive and verifies SHA-256
`56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80`.
Host `moc`/resource generators are built separately from the target libraries.
Target programs are compiled for static musl and are never executed on the
host. Compilation has no network and a read-only source/workspace mount; only
the selected build output is writable. Linux UAPI headers are copied into an
isolated include directory and inventoried. Host pkg-config libraries are
excluded from target dependency discovery.

`make qt6` installs the demo, port sources, upstream source archive and licenses
into `userland/runtime` for the next normal ISO build. The cross-development
SDK, static libraries, generated metadata and build receipt remain under
`build/qt6/port-r1`. A custom build directory is supported with `QT6_BUILD=...`.
For compilation without installing runtime files:

```sh
python3 -B tools/build_qt6.py --output-root build/qt6/port-r1
```

To keep an earlier build intact while reusing its verified static SDK:

```sh
python3 -B tools/build_qt6.py --output-root build/qt6/port-r2 --sdk-build build/qt6/port-r1 --install-runtime
```

The Code OSS overlay builder accepts `--qt-build <directory>/BUILD.json` and
verifies the binary, source and archive hashes before packaging. The completed
desktop image includes the native Qt launcher entry.

## Current scope

This is a raster Widgets port, with **one native top-level window per process**.
The demo uses embedded controls and keyboard editing; native popup windows,
menus, dialogs, multiple top-level windows,
input-method composition and multiple displays need further QPA work. The
initial backend exposes one virtual 1920×1080 screen while the desktop supplies
the actual client size. Qt Quick/QML, Qt Network, multimedia and hardware
OpenGL/Vulkan rendering are not part of this build. It does not make arbitrary
Qt applications compatible automatically.

The QPA implementation is pinned to Qt 6.8.3 because Qt's private platform APIs
require validation for each Qt release. Its CMake project requires that exact
version. The supplied static libraries and corresponding source support
relinking; Qt and bundled dependencies retain their upstream licenses.

## Validation

`tools/test_qt6_desktop.py` launches the real app through Spotlight in an owned,
diskless and networkless QEMU guest. It checks UID 1000, the first raster frame,
USB keyboard input, pixel-identical title text across native resizing,
maximize, tab navigation, editing, undo/redo, saving the expected bytes and
process exit. The credential/futex parity fixture runs identical ELF bytes on
Linux and Coast. Host-side execution of a target binary is never used as
evidence of a guest port.

`tools/test_qt6_clipboard.py` verifies UTF-8 paste from the native clipboard into
Qt, copy from Qt into Terminal, replacement after focus changes, and a native
client's denial before focus and immediately after focus is revoked.

References: [Qt QPA](https://doc.qt.io/qt-6/qpa.html),
[pinned Qt source](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz.mirrorlist),
and [Linux FUTEX_WAKE_OP](https://man7.org/linux/man-pages/man2/FUTEX_WAKE_OP.2const.html).
