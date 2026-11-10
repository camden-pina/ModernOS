# Local continuation handoff — 2026-10-15

> Latest handoff: [PASSOFF-2026-11-09.md](PASSOFF-2026-11-09.md). Source-build work
> in `/home/camdenp/Desktop/ModernOS` is in progress. The paths and selections
> below describe an older snapshot.

The working project is `/home/camdenp/Desktop/Coast`. It was an extracted
ACPICA handoff, without Git metadata. The separate clean checkout at
`/home/camdenp/Documents/test` remains on `refine-build-system-stable`, commit
`4e213f90c2990fcb91ea15e22d20b7fdf7cf54b1`. No new changes have been pushed.
This snapshot continues that handoff; it is not described as a new remote commit.

## Preserved input

Before edits, the source and completed browser-containing build were copied to
`/home/camdenp/Documents/Codex/2026-10-14/task/preservation/20261015-0600/`.
Its `manifest.json` and `SHA256SUMS` record the exact baseline. The original
`build/os.iso` was not replaced. Its SHA-256 is
`a89d7bcaa86543c253857e5da2f71d60be472d13b65fc0e335bf23dbdd6238b9`.

The source download matched the supplied checksum
`c3c9163c9b603a58289c3c8a34c45c71dedc7fb29df3de5b55bf74f6450d53e6`.
Other named downloaded handoff artifacts were not found in the inspected project
and download locations; existing project documentation supplied the test workflow.

## Candidate and features

The current frozen image is
`build/continuation-20261015/coast-candidate05.iso` (802,138,112 bytes), SHA-256
`f31feec46af5a05264066888aadf2a534fe2e0134d70fb3ce6601fb1963e4fe6`.
The kernel is `build/continuation-20261015/kernel/vmlinux`, SHA-256
`a6dda1b59bf8bb8c716ae13aba6e831bf50798b789193ca1b0ef89598306e48e`.
The initramfs SHA-256 is
`02a600ed8a51ae9bace345603f9b0c0e7c77f0a4860632e85196f7c2e633ad90`.
Earlier candidate images and failed test evidence are retained separately.

* [Dynamic linking](dynamic-linking.md): ET_DYN/PIE and dynamic ET_EXEC, the musl
  loader/shared C runtime, transitive libraries, dlopen/dlsym, TLS/pthreads,
  constructors/destructors, `gcc-dynamic`, `ldd`, and a native shared-library SDK.
  The existing default GCC/G++ and large static browser caches are preserved.
* [Doom](doom.md): native Doomgeneric with source-built Freedoom 0.13.0, framebuffer,
  USB keyboard/relative mouse, native HDA sound effects, cleanup and save paths.
  All engine/data source bundles, notices and provenance are included.
* [Unix tools](unix-tools.md): less, nl, tac, rev, paste, comm, fold, base64,
  split, clear and which; `cd -`; precise native-shell guidance for POSIX syntax;
  PTY-aware paging with slow-reader and terminal-state restoration fixes. Direct
  console file paging is supported; console pipelines are refused by the current
  foreground policy. Desktop PTY file/pipeline verification is a separate gate. The console
  has no userspace foreground-reassignment API; no kernel permissions were broadened.
* Initramfs packaging is atomic and emits unique directory entries. Validators
  accept only identical historical repeated directories, while rejecting duplicate
  files, links and conflicts. A failed package preserves the prior archive.
* The local network/HDA power supervisor verifies process ownership and cleanup.
  Terminal serial evidence is drained after QMP stop within the original deadline;
  explicitly selected audio captures use 48 kHz stereo without changing defaults.

## Verification

The preserved full browser image passed the 14-command power/ACPICA matrix,
including 20 ACPICA stages at 1/4 CPUs, fixed SCI events and negative controls.
Four AHCI/NVMe shutdown/reset cases verified all 131,072 dirty bytes and EOF in a
fresh guest. Actual e1000e/HDA poweroff/reset runs passed device MMIO/bus-master,
PCM, filesystem, fresh-boot and unchanged-input checks. See
`build/continuation-20261015/baseline-validation.json` and the separately preserved
`baseline-evidence.tar.gz`.

Dynamic kernel-loader host tests passed 27 actual-C sanitizer cases; patched musl
path helpers passed 11. Candidate03 passed the full dynamic suite on TCG 1/4 CPUs
and KVM 4 CPUs. Candidate04 changes only the Doom save-parent startup binary and
its source package relative to candidate03. Candidate05 adds the pager integration
fix while retaining identical kernel, libc and Doom binaries.

Candidate04 Doom passed the full gameplay oracle on KVM 4 CPUs and TCG 1 CPU:
USB menus started MAP01; player coordinates and captured frames changed; relative
mouse turned the player; firing consumed ammunition and produced real sound
samples; captured HDA PCM was nonzero; menu quit restored the shell and display
could be reacquired. JSON, serial logs, PPMs and WAVs are under
`build/runtime-tests/doom-c04-*`; verified cleanup reports are under
`build/continuation-20261015/doom-c04-*.runner.json`.

The final candidate05 passed every phase in
`build/continuation-20261015/final-validation.json`:

* All 14 power/ACPICA matrix commands, including the direct unchanged ISO,
  enriched rollback/desktop cases, both CPU counts and negative controls.
* All four AHCI/NVMe dirty-file shutdown/reset cases, each verifying all 131,072
  bytes and EOF from a fresh guest, with unchanged source inputs.
* Both actual e1000e/HDA terminal-stop cases, with packet/PCM activity, stopped
  register/trace state, persistence, fresh boots and verified helper/process cleanup.
* Dynamic ELF/native shared-compiler SDK and pthread/futex checks on TCG 1/4 CPUs.
* The full Doom gameplay/input/HDA/quit/display oracle on final-image KVM 4 CPUs.
* Unix command/console-file/PTY pipeline/pressure/cleanup checks on TCG 1/4 CPUs.
  Unsupported console pipelines are checked as explicit EPERM refusal and recovery.
* The complete native WebKit browser regression on TCG 4 CPUs: real pixels and
  USB interaction, scrolling/repaint equality, HTTP/TLS, forms/history/storage,
  downloads, scheduling and graceful child cleanup.

The final aggregation checks each phase's exact ISO hash, unchanged kernel and
runtime inputs, owned descendant cleanup and absent guest/recovery markers. It
also rechecks that the independent Git checkout is clean and the original user
ISO is unchanged. Earlier failed runs remain available with their failure scopes.

## Reproduction and boot

The source delivery includes the complete source tree, pinned vendor archives,
notices, existing generated `userland/runtime/` staging, the pinned Limine archive,
and the three browser host-tool downloads. It excludes generated kernel/compiler
objects, `userland/bin/`, `userland/obj/`, images and temporary disks. Preserve
symlinks and executable bits when extracting. Host dependencies and cache layout
are documented in [the build guide](build-system.md),
[browser build tools](browser-build-tools.md), and [native tools](native-tools.md).
The bundled OVMF firmware came from the existing handoff; its original adjacent
license/provenance metadata was absent.

Use normal GCC/Ninja and `COAST_LOW_MEMORY_BUILD=0`. Expensive browser builders
accept `--jobs 1`; top-level `make -j1` does not override every third-party builder's
own job count. No clean full browser rebuild was performed during continuation:
the user's completed browser build was retained, the new kernel used a separate
output directory, and new features were compiled with focused one-job commands.
A clean build remains expensive and requires the documented host tools.

To boot the tested image without rebuilding or altering the original image:

```sh
make run-image ISO_IMAGE=build/continuation-20261015/coast-candidate05.iso
```

The exact focused packaging commands and inputs are recorded in
`build/continuation-20261015/packaging-recipe.json`,
`candidate05-package.log`, and `candidate05-manifest.json`. The serial final regression driver is
`build/continuation-20261015/run-final-regressions.py`; each phase writes its own
input hashes and result/cleanup reports. It operates only on disposable virtual
disks and requires the other compiler/guest work to have finished.

## Practical limits

Physical hardware has not been tested. ACPICA does not imply complete platform
support: S1/S3/S4/resume, EC/GPIO/GenericSerialBus regions, thermal/fan policy,
CPU C/P/T/CPPC policy, PCI _PRT integration, coordinated igc/external AW88399 stop
remain unsupported. Arbitrary firmware preparation may block synchronously;
there is no forced live AML abort. The device supplement proves actual queued
HDA playback and network work before shutdown, but instantaneous active DMA at
the exact stop boundary remains unproven.

Dynamic load biases are deterministic (no ASLR); hardware entropy is reported
only when actually obtained. Shared C++ runtime support, general glibc/Linux
binary compatibility, runtime unloading and a full hostile-ELF validator are
outside the supported profile. Doom provides sound effects without music.
The native shell remains small; use `build-sh` for pipes/redirection/compound
POSIX commands. Console `cat | less` cannot acquire raw input ownership under
the current foreground policy; use `less FILE` on the console. This limitation
is tested as a clean refusal, not claimed as interactive pipeline support.

The user-requested activity helper overwrote one owned timestamp every 600
seconds during active integration/testing. It was stopped at 07:31:13 UTC after
all guest gates passed; its owned temporary timestamp was removed, and the final
aggregation confirmed the worker is dead. Its controller is retained under
`build/continuation-20261015/task-activity-writer.json`. It changed no host
power/security/network settings and was not a guarantee against host sleep.

## Delivered artifacts

`/home/camdenp/Documents/Codex/2026-10-14/task/deliverables/` contains the tested
ISO, full source/runtime archive, validation evidence archive, complete source
file manifest, final validation JSON, delivery index and SHA256SUMS. The ISO is a
copy of candidate05 with the exact same checksum. `SOURCE-MANIFEST.json` records
every source/runtime file or symlink. The source archive contains generated
runtime staging alongside source and all required pinned downloads; it does not
contain the large generated compiler/browser object caches.

Nothing was pushed or published remotely. All boot, storage and power tests used
disposable virtual disks; no boot image was written to a physical disk.
