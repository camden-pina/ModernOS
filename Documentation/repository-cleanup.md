# GitHub repository cleanup

The GitHub repository needs the OS sources, build recipes, tests, resources and
license notices. ISO files and other generated outputs can be rebuilt locally.
The source archive cache is also excluded: the 153.28 MiB GCC archive exceeds
GitHub's 100 MiB regular-file limit. The pinned download manifest and restoration
tool replace the need to commit those archives. See
[GitHub's file-size guidance](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github).

The October 14, 2026 inventory contained about 209 GiB on disk, excluding Git
metadata. Most was already ignored. Before this cleanup, the files available
to add to Git totaled about 505 MiB. With the updated ignore rules they total
about 27 MiB, including the bundled fonts, wallpapers, notices and QEMU firmware.
These figures describe the working tree; Git history has separate storage costs.
The subsequent unused-file cleanup and GitHub documentation leave about
26.8 MiB of source contents.

## What belongs in Git

| Paths | Keep or remove | Reason |
| --- | --- | --- |
| `arch/`, `init/`, `kernel/`, `mm/`, `fs/`, `net/`, `drivers/`, `include/` | Keep | Active x86-64 kernel, shared boot handoff, native EFI stub and syscall interfaces |
| `Kbuild`, all active Makefiles, `scripts/` | Keep | Reproduce the kernel, userland and Limine/GRUB/rEFInd images |
| `scripts/bootloaders/*.conf.in`, `grub.cfg.in` | Keep | Editable native loader settings rendered with shared TOML boot policy |
| `lib/lai/`, `lib/fonts/`, other active `lib/` sources | Keep | Kernel dependencies; preserve LAI's license and authors |
| `tools/`, including all test scripts and C fixtures | Keep | Builders, image packaging, test fixtures and shared test helpers |
| `userland/*.c`, headers, assembly, linker script, `desktop/`, `browser/`, `modern-browser/`, `sdk/` | Keep | Applications, ports, native SDK sources and guest regression checks |
| `userland/vendor/sources.json`, `licenses/`, BusyBox config, CA bundle, README | Keep | Reproducible source inputs, configuration, trust roots and notices |
| `arch/x86/boot/boot.conf`, its background, desktop wallpapers, browser TTF fonts | Keep | Shared boot policy and image assets used by packaging and applications |
| `userland/desktop/font_data.c` and `.h` | Keep | Required atlas; builds do not automatically regenerate it from host fonts |
| `userland/runtime/usr/share/vim/vimrc` | Keep | Small configuration input, rather than the generated Vim runtime |
| `firmware/ovmf/OVMF.fd` | Keep for now | Both QEMU and the shared guest-test harness currently depend on this 4 MiB firmware file |
| `Documentation/`, `README.md` | Keep | Build instructions, ABI/application references and validation evidence |
| `toolchain/Makefile` | Keep | Optional cross-compiler build recipe; the compiler installation is a cache |
| `.vscode/` | Optional | Small editor settings; removal saves only a few KiB and does not affect builds |

Keep the active EFI loader in `arch/x86/boot/efi/`: GRUB and rEFInd use it.
It is separate from the obsolete EDK2 loader removed with the legacy snapshots.

## Unused files removed

The unused-file cleanup removed 61 files, about 341 KiB of source contents.
Build manifests, includes and source references were checked before removal;
all tests, active boot adapters, allocator list macros and lwIP configuration
headers were retained.

| Removed files | Reason |
| --- | --- |
| The 46 files under `legacy/` | Old EDK2 loader, library/node/font copies, experimental USB code and old build files outside the active builds |
| `Documentation/About.rtf` | Unreferenced placeholder containing no current instructions |
| `lib/fonts/dfltfont.psf` | Unreferenced font copy; the active console uses the compiled `font_8x16.c` font |
| `lib/node.c`, `include/node.h` | Commented-out implementation and declarations; no executable behavior or users |
| `lib/fmt.c`, `include/fmt.h` | Formatter without callers, discarded by the kernel link; logging uses the active `vsnprintf` implementation in `lib/lai/core/vsnprintf.c` |
| `include/acpi/aml.h`, `include/acpi/interpreter/methods.h`, `include/acpi/interpreter/namespace.h` | Abandoned interpreter declarations/opcodes with no active include path; the real LAI interpreter remains intact |
| `include/mm_types.h`, `include/str.h` | Unreferenced memory-model scaffold and its otherwise unused string type |
| `arch/x86/include/asm/msr.h` | Unreferenced MSR constant catalog; active CPU/MSR helpers remain in their existing files |
| `drivers/input/mouse/Makefile`, `ps2_mouse.c`, `ps2_mouse.h` | Old PS/2 driver with no callers or initialized path; its code was already discarded by the final kernel link |

The empty node object, unused formatter and PS/2 directory were removed from `obj-y`, the empty
node header include was removed from the ACPI host code, and the unused
`userland/Makefile` rule for `obj/font.o` was removed. Documentation and the
obsolete EDK2 ignore entry were updated with those deletions.

The 73 license and notice files were excluded from cleanup. Their contents,
modification times and modes are checked against the inventory taken before
any deletion. Generated build caches and checkpoints were also retained.

The rebuilt kernel retains its entry point, memory layout and non-executable
loadable bytes. Disassembly differs only in eight panic-message line numbers,
which decreased by one when the unused ACPI include was removed. The host
source-cache, boot-policy, EFI-loader and kernel-memory checks passed, all 68
test scripts remain present, and all 73 protected notices remain unchanged.

The AI handoff and resume documents can be archived outside the public tree,
but several browser documents link to them and they retain failure evidence.
Update those links and move the useful evidence before removing them. Their
local repository paths also need editing if they are intended as public
instructions. Deleting them would save little space.

The repository has no top-level license for the original OS code. Choose that
license separately; retain existing third-party license and attribution files.
OVMF's upstream provenance/notices should also be documented if the firmware
continues to be redistributed. Replacing it with host firmware would first
require changing the shared VM harness, including checkpoint compatibility.

## Generated files excluded from Git

| Paths | Measured disk usage | Can be recreated |
| --- | --- | --- |
| `build/` | About 202 GiB | Kernel objects, ISOs, initramfs, ISO staging, third-party sources/builds, logs, recordings, disk fixtures and checkpoints |
| `toolchain/build/`, `toolchain/src/`, `toolchain/opt/` | About 5.8 GiB combined | Cross-toolchain build, source and installation caches |
| `userland/bin/`, `userland/obj/` | About 364 MiB combined | Compiled guest executables and objects |
| Generated portions of `userland/runtime/` | About 400 MiB | SDK libraries/headers/sources, installed port resources and certificates |
| The 44 source archives in `userland/vendor/` | About 478 MiB of contents | Pinned HTTPS downloads restored by `make fetch-sources` |
| Python caches, Finder metadata, local `.agents/`, `.aws/`, `.codex/`, environment files | Host-specific | Excluded from the source checkout |

Image patterns also cover named candidate/release ISOs outside `build/`, raw
disk images, QCOW2/VDI/VMDK images and VM states. Compiler objects, archives,
dependency files and command records were already ignored. The in-tree EFI
test executable is now ignored too.

Generated copies of modern-browser scripts, Git templates and Git completion
files are now ignored and removed by `make userland-clean`. Their producers
remain in Git: `build_javascriptcore.py`, `build_git.py` and the original
`userland/modern-browser/*.js` and `.mjs` files.

Checkpoints and guest-exported toolchains can contain work that is expensive
or impossible to reconstruct byte for byte. Copy any checkpoint, SDK export,
physical-hardware evidence or test disk you want to retain outside `build/`
before deleting the entire directory. They do not need to be in GitHub.

## Source restoration and offline builds

```sh
make fetch-sources       # Fetch missing archives and verify all 44 hashes
make check-sources       # Verify the prepared source cache without a network
make test-source-fetch   # Check failed downloads, corruption and offline reuse
make test-browser-build  # Check tool bootstrap and parallel build ordering
make -j4                # Kernel; a fresh checkout needs only the lwIP download
make iso BOOTLOADER=limine
make iso BOOTLOADER=grub
make iso BOOTLOADER=refind
```

Existing archives are verified and reused offline. Missing archives are fetched
over HTTPS into unique partial files and renamed into place only after their
SHA-256 matches. An existing corrupt archive is preserved and reported rather
than silently overwritten. Partial transfers are removed on failure.
The original port builders also retain their own source checks.

`make iso` restores userland sources before its upstream port builders run
and prepares the pinned browser host tools automatically.
For direct Python builder invocations, run `make fetch-sources` first.
Offline builds additionally need the appropriate host tools and bootloader
caches described in [build-system.md](build-system.md). A fresh source checkout
with no download cache requires network access.

## Deleting local artifacts

Use the existing build-system commands for routine cleanup:

```sh
make clean                 # Kernel, selected ISO/initramfs, staging and EFI outputs
make O=out clean           # Same cleanup for another selected output directory
make userland-clean        # Generated shared userland; retains vimrc and sources
```

`make clean` deliberately keeps shared port/download caches and test results;
it does not free the full 202 GiB. Preview a broader deletion first:

```sh
git clean -ndX -- build/
git clean -ndX -- toolchain/build/ toolchain/src/ toolchain/opt/
git clean -ndX -- userland/vendor/
```

For each reviewed list, use the matching `git clean -fdX -- PATH...` to remove
those ignored files. Keep the scopes explicit. In particular, the vendor
command preserves its manifest, configuration, CA bundle and license notices.
Removing its archives requires downloads again. Cleaning the toolchain cache
removes the local cross compiler; the documented host compiler remains usable.

Normal `git clean` skips nested Git repositories, including the generated
repositories in `build/web-fixture/`. To delete those fixtures as well, preview
`git clean -ndffX -- build/`, then use `git clean -dffX -- build/` after checking
that every nested repository is disposable. The second `f` permits removing
nested repositories; keep this command confined to `build/`.

The unused legacy sources have already been deleted from the working tree.
The staged migration was left intact; use `git add -A -- .` to include these
additional removals and updated build rules in the next commit. Large caches
and checkpoint files have not been deleted.

## Existing deletions and Git history

The working tree already had 400 tracked deletions before this audit: the old
kernel layout, AArch64, old GNU-EFI tree/libraries, old build entry points,
dependency files, Finder metadata and old root-level binaries/firmware.
The modern source layout and relocated firmware are mostly untracked additions.
Stage both sides of that migration together; staging only modified tracked
files would omit almost the entire current OS.

Ignoring a tracked file does not untrack it. The old tracked generated files
are already absent locally; staging their deletions is sufficient for the
next commit. A generated file still tracked in a future checkout can be
removed from the index with `git rm --cached -- FILE` while retaining its
local copy.

Existing history contains disk-image blobs such as `usb.img` and
`build/OS.img`. All reachable blobs totaled about 443 MiB uncompressed; the
largest was 64 MiB, and none exceeded 100 MiB. New ignore rules and a deletion
commit do not remove those earlier blobs. Preserving history is compatible
with committing this source cleanup. Reducing historical clone size requires
a separate coordinated history rewrite, or a new repository initialized from
the cleaned source tree. Do not remove `.git/` from this working copy to do that.

Before committing, review the full candidate tree:

```sh
git status --short
git diff --check
git add -A -- .
git diff --cached --stat
git diff --cached --name-only -- '*.iso' '*.img' '*.qcow2' '*.o' '*.a' '*.d'
```

The final command should show removal of the old generated tracked files and
no newly added generated artifacts. No commit, push or history rewrite was
performed by this audit.

## Test scripts and shared helpers

Validation for this cleanup passed:

- SHA-256 verification of all 44 local source archives against the existing
  builder pins, and HTTP checks of all 44 final download endpoints. Seven
  GitHub release redirects required IPv4 on this host; the downloader retries
  connection failures over IPv4 while retaining TLS and checksum verification.
- Actual uncached downloads and checksum verification of lwIP, musl and zlib.
- Kernel builds in a separate output directory and in a source-only export
  using the host GCC, with only the verified lwIP archive supplied as a cache.
- Source-only builds of the shell, hello/check programs and native GUI,
  preserving the required embedded font atlas and application headers.
- The source-cache unit checks, eight boot-policy checks, native EFI loader
  fixtures and kernel memory boundary checks in the source-only export.
- During the initial source audit, cleaning the disposable export removed generated kernel/userland files and
  all three newly ignored runtime resource directories while preserving all
  635 source files and the shared lwIP preparation cache.
- Every Python script parsed, every test script appeared once in the inventory,
  local helper imports resolved, and `git diff --check` passed.

The subsequent browser-build automation passed seven bootstrap/dependency tests
and the six source-cache tests. An empty installed-tool cache was populated
from verified archives, including compiling Ruby, and then reused offline.
The complete `make -j4 iso` path also passed using the existing compiler/browser
caches. Its initramfs contains WebCore's renderer, the broker, JavaScriptCore
and NetSurf; the host build tools are excluded. All 73 protected notices retain
their original contents, modes and modification times after verification.

The full third-party userland build and complete guest test suite have not
been rerun from an empty compiler/browser cache. Existing valid source archives
and compiler/browser caches remain available locally.

The subsequent boot-configuration redesign passed 15 TOML/template/asset host
checks and the EFI loader fixtures. Three complete ISOs were packaged from
the existing initramfs. Limine, GRUB and rEFInd passed UEFI boot checks with
four CPUs, and Limine also passed BIOS boot checks. These checks verified
the 2560x1600 framebuffer, syscalls, threads, writable initramfs behavior and
exact reserved boot-file bytes. A GRUB-only template setting and an additional
native asset were verified in the GRUB image. The kernel/module paths and
native EFI handoff settings match those before the redesign.

Keep the 66 original Python test scripts. Together with the new source-cache
and browser-bootstrap tests they occupy little space and cover different regressions. The apparent
overlap between NetSurf and native WebCore tests reflects two active engines:
NetSurf still handles local files, while the native Browser handles remote
pages with WebCore. Older component tests still exercise separately built
JavaScriptCore, IPC and library checks.

`test_runtime.py` is imported by 56 existing tool modules and provides the
VM/checkpoint/disk fixture harness. `test_desktop.py` is imported by 33 tools
for real GUI input and assertions. `web_fixture.py` is imported by 24 modules
for local HTTP/TLS/CORS/media fixtures. Also retain `browser_protocol.py`,
`guest_ramfs.py`, `wayland_pointer.py`, `browser_surface_check.c` and the
diagnostic scripts: tests import these directly or use them as fixtures.
The media tests import helpers from one another, and the compiler test imports
the rebuilt-toolchain check. A script's absence from a Make target does not
make it safe to delete.

Guest tests select the supplied `--iso`, the `MODERNOS_ISO` exported by Make,
or `build/os.iso` through the shared harness. They do not require committed
candidate ISOs. Most test inputs are generated under ignored `build/` paths.
The rebuild/export tests intentionally require checkpoints created by the long
native rebuild; those are local workflow artifacts, rather than source inputs.
Live website tests and physical pointer diagnostics have additional network
or display requirements and are separate from deterministic host checks.

The inventory below lists every current Python test script. The native EFI
loader also has C fixtures in `arch/x86/boot/efi/loader_test.c`, run with
`make test-efi-loader`.

| Script under `tools/` | Coverage |
| --- | --- |
| `test_audio_firmware.py` | Test the real AW88399 parser and driver sequence without host device writes. |
| `test_audio_power.py` | Exercise real speaker _CRS templates and GPIO code without host MMIO writes. |
| `test_boot_config.py` | Host checks for shared TOML policy, native templates, assets, selector errors and fresh ISO staging. |
| `test_boot_files.py` | Check boot-file copy semantics and the exact bytes of a borrowed ELF. |
| `test_bootloaders.py` | Exercise real UEFI boots, SMP, graphics and reserved module backing. |
| `test_browser_guest.py` | Use the real desktop/browser process, USB input and local web fixture. |
| `test_browser_ipc.py` | Exercise real kernel IPC/shared objects and explicit browser capabilities. |
| `test_browser_policy.py` | Remote pages cannot embed or navigate to cached, previously opened files. |
| `test_browser_protocol.py` | Exercise the actual static NetSurf process and its bounded surface protocol. |
| `test_browser_render.py` | Pixel regressions using the complete, real NetSurf process. |
| `test_browser_sandbox.py` | Exercise kernel browser restrictions with an intentionally hostile child. |
| `test_browser_scheduling.py` | Check real engine idle tasks and native input/paint scheduling. |
| `test_browser_script.py` | Exercise document clocks and global aliases in real imported JavaScript. |
| `test_browser_surface.py` | Check the compositor's actual untrusted surface-copy operation with ASan/UBSan. |
| `test_compiler.py` | Compile/link/run real C/C++ programs inside the booted ModernOS SDK. |
| `test_configure_shell.py` | Regression for actual Autoconf shell substitutions and asynchronous signals. |
| `test_console_input.py` | Keep partially typed input through a native fork/stdin-redirection handoff. |
| `test_dark_mode.py` | Validate shared dark surfaces, live switching, saved preferences, popup cleanup. |
| `test_desktop.py` | Exercise the real desktop via emulated USB input and validate guest files. |
| `test_frame_tree_authority.py` | Host-check the real broker tree policy with minimal transport/process stubs. |
| `test_frame_tree_protocol.py` | Exercise bounded frame-tree parsing with malformed input under ASan/UBSan. |
| `test_gcc_generators.py` | Generate actual GCC tables in ModernOS; compare with the same native GNU awk |
| `test_git.py` | Exercise real Git object/ref/index operations inside the booted OS. |
| `test_gui_drag.py` | Measure actual guest composition/upload times during live window dragging. |
| `test_host_mouse.py` | Verify host GTK mouse events through XTest or the Wayland desktop portal. |
| `test_igc_driver.py` | Exercise the actual I226 driver against bounded MMIO/PHY/DMA failure models. |
| `test_javascriptcore.py` | Real modern JS component execution in the guest; this is not a website test. |
| `test_kernel_memory.py` | Check actual kernel integer memory routines against inaccessible boundaries. |
| `test_nano.py` | Boot upstream GNU nano, edit/save using console and desktop PTY, verify bytes. |
| `test_native_activation.py` | Verify genuine WebCore user activation and isolated-frame propagation. |
| `test_native_audio.py` | Native audio-only AAC/MP3/Opus playback, pause/seek and actual device PCM. |
| `test_native_browser.py` | Verify WebKit pixels and USB interaction in the real native Browser window. |
| `test_native_console.py` | Check native console output does not execute page object coercion/getters. |
| `test_native_cookie_context.py` | Check DOM and HTTP cookies use the real isolated frame's site context. |
| `test_native_credentials.py` | Verify actual cross-origin fetch cookies, CORS and Blob-worker requests. |
| `test_native_exception_trace.py` | Check opt-in JSC exception observation without changing actual execution. |
| `test_native_fetch_metadata.py` | Check real GUI navigation provenance reaches WebCore's Fetch Metadata. |
| `test_native_frame_focus.py` | Check document focus across native isolated frames using real guest input. |
| `test_native_frame_graph.py` | Check actual WindowProxy sibling order and names across an isolated frame. |
| `test_native_frame_referrer.py` | Check WebCore referrer policy survives an isolated-frame navigation. |
| `test_native_frames.py` | Exercise real isolated WebCore frames, structured clones and input in UEFI. |
| `test_native_media.py` | Decode actual MP4/WebM frames and PCM in the Limine UEFI guest. |
| `test_native_mse.py` | Exercise genuine MSE coded frames, CPU decoding and HDA PCM in the guest. |
| `test_native_mse_reconfiguration.py` | Verify retained MSE samples decode with their original AVC configuration. |
| `test_native_ports.py` | Exercise upstream Vim, build tools and process/file ABI in the real OS. |
| `test_native_scroll.py` | Measure genuine animated wheel scrolling in the UEFI desktop and WebCore. |
| `test_native_transport.py` | Validate real HTTPS timings, compression, language headers and timing privacy. |
| `test_native_worker_context.py` | Check secure-context inheritance in real workers inside isolated frames. |
| `test_network.py` | Guest socket/DHCP/curl/TLS and genuine Git smart-HTTP/HTTPS tests. |
| `test_osdev_verification.py` | Optional live OSDev check, using only the rendered verification checkbox. |
| `test_pixel_blend.py` | Test the actual renderer's scalar/SSE2 paths against independent arithmetic, |
| `test_ramfs_export.py` | Prove read-only export against real guest data and its guest SHA-256. |
| `test_rebuilt_toolchain.py` | Validate/export the real native compiler from a completed RAM checkpoint. |
| `test_runtime.py` | Boot integration tests using generated disk images, never host block devices. |
| `test_smooth_scroll.py` | Exercise the desktop's shared scroll dynamics at variable frame rates. |
| `test_source_fetch.py` | Check source restoration, offline reuse and failed-download handling. |
| `test_browser_build.py` | Check verified tool extraction, offline reuse, repair and parallel cold-build dependency ordering. |
| `test_speaker_check.py` | Capture the actual speaker-check PCM sequence in QEMU; no physical claim. |
| `test_threads.py` | Test real musl pthreads/futexes and group teardown in the custom guest. |
| `test_toolchain_rebuild.py` | Exercise the bundled source rebuild inside ModernOS (long-running TCG test). |
| `test_vm_checkpoint.py` | Recover a real RAM-only ModernOS guest and its live processes from a file. |
| `test_wallpaper.py` | Check real image decoding, ISO assets and wallpaper settings through USB input. |
| `test_web_live.py` | Optional public-network smoke test for the real guest resolver/TLS/apps. |
| `test_webcore_deps.py` | Guest checks for new real WebCore libraries; these do not assert browser support. |
| `test_webcore_network.py` | Actual brokered guest WebCore pages and pixels, including CORS/preflight. |
| `test_webcore_render.py` | Actual guest WebCore pixels and click in bounded, immutable-origin fixtures. |
| `test_webkit_deps.py` | Real guest GLib/GObject/Gio execution; this is not a webpage renderer test. |
| `test_youtube_playback.py` | Optional live YouTube test: real guest player pixels and captured HDA sound. |
