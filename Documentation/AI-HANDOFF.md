# Coast development handoff

Repository: https://github.com/camden-pina/ModernOS.git. The working tree is on
`main`, tracking `origin/main`. The published baseline is commit `af60ecd`.
The source tree, build tools and documentation are maintained together here.

## Current build workflow

```sh
make setup-host
make iso
make run
```

The setup command explicitly installs Ubuntu/Debian host prerequisites. Do not
install host packages as part of agent verification. `python3 -B
tools/setup_host.py --check` performs a read-only prerequisite check. The
smaller native workflow uses `make iso PROFILE=native` and `make run PROFILE=native`.
Plain `make iso` always selects the full source pipeline with all integrations;
`make run` boots its existing image. Full/core/native/staged variants use
`PROFILE`, and `JOBS`, `ISO_BUILD`, `ISO_ARGS` and the QEMU variables are described
in [the command reference](build-system.md). Keep the README quickstart to the
setup, `make iso`, `make run` sequence.
See [the README](../README.md), [host setup](host-setup.md) and
[source builds](source-builds.md) for the complete current interfaces.

## Images and evidence

`build/desktop-wide-source-r1/image/Coast-desktop.iso` is a qualified wide image
containing Coast Packages/makepkg, Qt and QEMU alongside Chromium/Code OSS.
SHA-256: `4e6726ccb614de56b8884cf018a9cfd932b17b3ec3183b3804d84bbc5160d9d3`.
Its `qualification/QUALIFICATION.json` records all 11 passed guest phases and
owned-VM shutdown. The source-only native export is under
`build/github-source-check-r4/`; its native image passes UEFI/BIOS at 1/4 CPUs.

The desktop qualification uses verified source-built Electron/Node and GNU ABI
components. An empty full Electron graph build is not yet verified end to end.
Do not describe component reuse as an independent clean build of that graph.
The image's source/receipt identities describe its own assembly snapshot;
new build tooling and documentation do not rewrite those receipts.

The latest screenshot in `images/desktop.png` comes from the qualified wide
image's actual native Files window. It is a captured frame, not a mockup.

## Local services and cleanup

The selected image/service symlinks under `build/Coast-*`, their rollback
targets, native compilers/SDKs, signing keys and the active package/community
services are preserved. Do not change user applications, unrelated VMs,
private state or files outside this project while doing repository maintenance.

Reviewed generated candidates and duplicate ISOs are cleaned up locally.
`build/cleanup-plan-current.json` and
`build/cleanup-electron-intermediates.json` record the cleanup. Compiled
Electron/Node products and their receipt verify after pruning regenerable
objects, split-debug and linker caches. Compact candidate receipts are in
`build/cleanup-records/`; local implementation notes are under
`build/local-notes/` and are excluded from publication.

`tools/clean_cache.py` previews regenerable caches. `--apply` removes only its
selected paths; `--intermediates` also checks completed Electron/Node receipts
before selecting object/debug trees. Keep source downloads and usable SDKs
unless a specific maintenance task authorizes their deletion.

## Verification and publication

Run `make source-check`, source-tree/fetch/assembly tests, the package-trust
tests and `tools/test_build_workflows.py` for build-tool changes. Guest changes
need their appropriate actual-VM suites. All test-owned VMs must stop; active
package/community services must remain available.

Current maintenance simplifies host setup and launch, uses the actual GitHub
URL, adds the desktop screenshot, consolidates current documentation and
prunes unused generated material. Prepare the reviewed source changes in the
Git index; leave commit and push commands to the user unless explicitly asked
to execute them. Fish uses quoted multiline `git commit -m` arguments rather
than a POSIX here-document.

The maintenance checks pass: source/fetch/assembly, runtime admission/fixture,
ACPICA/power/EFI/boot configuration, package trust and 11 host-setup/cleanup
protection tests. `build/maintenance-checks/launcher/report.json` records a real
wide-image GUI launch through the BIOS launcher. The networking report covers
real HTTP/TLS data, certificate rejection and Git clone/fetch/push in the guest.
A clean source export builds the native ISO; `native-boot/report.json` records
all four UEFI/BIOS 1/4-CPU cases, desktop/Files USB input and owned-VM shutdown.
All 12 selected links and their image hashes are unchanged; both local services
respond. Host package installation is not part of this verification.

The unified Make interface is checked through command dry runs and full/core
JSON plans only. No source builds or VMs were rerun for this command-routing
change, as requested. Existing in-tree guest tests retain staged assembly.
