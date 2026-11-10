# Coast development handoff

Repository: https://github.com/camden-pina/ModernOS.git. Work on `main`, tracking
`origin/main`. The committed baseline before this cleanup is `944e211`.

## Build and run

```sh
make setup-host
make menuconfig
make iso
make run
```

There is one Coast OS build. `make iso` always builds the kernel, desktop,
Chromium, Code OSS, compiler, Python, Coast Packages/makepkg, Qt and QEMU with
its viewer. The build pipeline includes all 22 component stages unconditionally.
`make run` boots `build/coast/image/Coast.iso` without rebuilding it.

Keep the README quickstart to setup, `make menuconfig`, `make iso`, `make run`. Build parallelism,
output locations, verification and VM settings are optional arguments described
in [the command reference](build-system.md). Keep all guides consistent with
this workflow. Read [source builds](source-builds.md) for component receipts.

`make menuconfig` configures build/VM settings and saves local `.config` as
validated TOML. Make loads it automatically; command-line variables override
saved values. `CONFIG=path` selects another settings file, and the wizard's
load/save controls support reusing configurations. `make config` is the plain
questionnaire; `make defconfig` saves defaults with an `.old` backup.
Configuration does not run builds, install host packages or start VMs.
`make test-build-config` checks persistence, literal arguments and Make overrides.

The setup command explicitly installs Ubuntu/Debian host prerequisites.
Do not install host packages as part of agent verification.
`python3 -B tools/setup_host.py --check` checks prerequisites without installing.

## Validation evidence

The integrated desktop passed all 11 guest qualification phases, including
signed package operations, makepkg, Qt USB/clipboard and nested QEMU through
Chromium. The reports bind their results to exact artifact hashes and record
owned-VM shutdown. The desktop screenshot in `images/desktop.png` is an actual
capture of Coast's Files application.

Qualification reuses verified source-built Electron/Node and GNU ABI receipts.
An empty full Electron graph build is not yet verified end to end. Component
reuse must not be described as an independent clean build of that graph.
Existing receipts describe their own source snapshot; editing current tools
and documentation does not change historical qualification results.

Local evidence and exact artifact paths are in `build/maintenance-checks/`.
`READINESS.json` indexes source/host checks, HTTP/TLS/Git guest checks, desktop
launch checks, boot-component checks and command-plan verification. No builds
or VMs are to be run for this command/documentation cleanup, as requested.
Kernel/userland regression checks manage their own internal fixtures.

## Preserve local state

Preserve selected build outputs, rollback targets, compilers/SDKs, signing
keys and the active package/community services. Do not change unrelated user
applications, VMs, private state or files outside this project.

`build/cleanup-plan-current.json`, `build/cleanup-electron-intermediates.json`
and `build/cleanup-records/` record reviewed cleanup. Local implementation notes
are under ignored `build/local-notes/`.

`tools/clean_cache.py` previews regenerable caches; `--apply` removes its selected
paths. `--intermediates` verifies completed Electron/Node receipts before
selecting object/debug trees. Keep downloads and usable SDKs unless a specific
maintenance task authorizes their deletion.

## Publication

Run `make source-check` and the appropriate host checks for tool changes.
Guest changes require their corresponding VM suites; stop every test-owned VM
and preserve the package/community services. Generated artifacts, downloads,
credentials and local validation records stay out of Git.

Stage reviewed changes. Leave commit and push to the user unless explicitly
asked to execute them. Supply fish-compatible commands when requested.
