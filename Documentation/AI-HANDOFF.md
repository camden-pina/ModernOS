# Coast OS development handoff

Repository: https://github.com/camden-pina/ModernOS.git. Work on `main`, tracking
`origin/main`. These pending ISO and recovery changes build on `4bcc314`.

## Build and run

```sh
make setup-host
make menuconfig
make iso
make run
```

There is one Coast OS build. Plain `make iso` includes the kernel, desktop,
Chromium, Code OSS, compiler, Python, Brine/makepkg, Qt and QEMU/viewer.
The default pipeline has 22 component stages plus source/runtime preparation,
assembly and optional qualification. `APPS` and `WITHOUT` select optional apps;
build requirements and installed-app requirements are separate. Keep one public
`make iso`/`make run` interface without named image editions.
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
The app fields explain runtime dependencies. Configuration does not run builds,
install host packages or start VMs.
`make test-build-config` checks persistence, literal arguments and Make overrides.

The Electron source sync fetches depot_tools at depth one and uses gclient
`--no-history` without extra tags/branch heads. Its `.gclient` disables global
Git mirrors. All 170 source revisions, patches, overlays and build arguments
remain pinned. `make test-electron-source-sync` checks real local shallow Git
fetches, patches, hooks and rejection of unexpected revisions.
Source verification accepts an overlaid patch file only when its complete bytes
match the checked-in policy; all other patch files still undergo the Git reverse
check. Verification runs after policy installation before publishing SOURCE.json.
Completed and interrupted overlay phases reuse downloads and completed hooks,
including a completed sync whose source receipt was invalidated by a builder
change. Do not clean the source checkout to recover from a verifier-only fix.
The first Electron build attempt exposed an npm host archive named `.tgz` that
the unpacker treated as an uncompressed tar. The extractor now streams `.tgz`
through gzip, and the host-tool preparation removes abandoned scratch trees
before retrying. It stamps the package before atomic publication so an
interruption immediately afterward leaves a reusable module. The pinned
Rollup archive was verified and extracted locally without changing the source
checkout. Rerun `make iso`; no source cleanup is needed.

`make iso` shows stage progress, elapsed time, tool counters, recent log output
and source Git bytes/rates. Unknown totals use an activity bar. Complete component
logs stay in `ISO_BUILD/logs/`. Ctrl+C stops the stage's process group;
`make test-build-progress` checks logging, counters and child shutdown.

`tools/build_components.py` owns the app/build dependency graphs. Code OSS
requires Bash and shares Electron/Chromium with the browser. The viewer requires
QEMU/Python/Chromium; Brine requires makepkg/Python; makepkg requires
GCC/Python/Bash. Omitting a prerequisite omits dependent apps with an explanation.
An explicit `ADD` restores its app/runtime prerequisites despite saved omissions.
`make build COMPONENT=chromium` builds the app/prerequisites without an ISO;
`make iso ADD=chromium` adds it to the last ISO selection. `SELECTION.json`
preserves that selection when component work invalidates `MANIFEST.json`.
Brine is the package manager name and client command; Coast OS remains the OS
name. The optional app selector accepts saved `packages` selections as Brine,
while the separate `packages` build component still means the pacman backend.

`tools/build_state.py` supplies durable writes and the shared lifecycle lock.
`tools/build_session.py` verifies component/source/dependency identities, records
`STATE.json`, retains compatible object trees and restarts one-shot staging.
Managed Electron sync resumes fetch/patch/hooks/overlays using `SYNC.json`.
Pinned archive downloads retain range-resumable partials; transactional archive
extraction publishes complete source trees. Qualification checkpoints passed
phases against the exact image/manifest/harness and records omitted-app suites
as skipped. This is phase/tool-level recovery, not checkpointing a compiler or
running VM. Keep the documented recovery limits honest.

The setup command explicitly installs Ubuntu/Debian host prerequisites.
Do not install host packages as part of agent verification.
`python3 -B tools/setup_host.py --check` checks prerequisites without installing.

## Validation evidence

The focused recovery/app-selection suite passes host/fixture checks, including
42 lifecycle cases. Coverage includes dependency
closures, selected ISO payloads, package dependency records, static/dynamic ELF
checks, configuration/Make arguments, interrupted download/sync/hooks, archive
extraction, component retries, atomic receipts and selective/full cleanup.
Desktop launcher availability has a production C test; the desktop compiles.
Source-fetch tests retain checksum/TLS behavior. Existing sanitizer-based snap,
damage and transition checks pass. Exact command results and cleanup records
are kept under `build/maintenance-checks/` and indexed by `READINESS.json`.
Electron sync now has 13 local-Git regression cases covering policy/patch
overlap, interrupted policy copies, changed files and invalidated receipts.
The completed 170-repository checkout passes both managed and read-only source
verification; sync reuse took about half a second without changing its files.
The Brine rename passes 87 focused host checks plus 29 repository-server,
27 repository-network and 11 search checks against a disposable publication
fixture. The launcher's C source compiles with warnings treated as errors, and
the default `make iso ISO_ARGS=--plan` selects Brine. Public community-container
qualification remains separate.
Two host-archive regression cases now cover `.tgz` extraction, abandoned
scratch recovery and interruption after package publication. The actual
checksum-pinned Rollup download also passed preparation. The archive reader
now drains compressed streams before closing them; the pinned libksba archive
and a broken-pipe regression test pass.

`make iso JOBS=16` with saved qualification enabled passed all 26 stages. The
integrated desktop passed all 11 guest qualification phases, including
signed package operations, makepkg, Qt USB/clipboard and nested QEMU through
Chromium. The resulting ISO is `build/coast/image/Coast.iso`; its manifest hash
matches the image. The reports bind results to exact artifact hashes and record
owned-VM shutdown. The desktop screenshot in `images/desktop.png` is an actual
capture of Coast's Files application.

Qualification reuses verified source-built Electron/Node and GNU ABI receipts.
An empty full Electron graph build is not yet verified end to end. Component
reuse must not be described as an independent clean build of that graph.
The optional omit/add app selections have host coverage but were not separately
guest-qualified in this run.

Local evidence and exact artifact paths are in `build/maintenance-checks/`.
`READINESS.json` indexes source/host checks, HTTP/TLS/Git guest checks, desktop
launch checks, boot-component checks and command-plan verification. Do not launch a full Electron compile just to check orchestration or naming.
Kernel/userland regression checks manage their own internal fixtures.

## Preserve local state

Preserve selected build outputs, rollback targets, compilers/SDKs, signing
keys and the active package/community services. Do not change unrelated user
applications, VMs, private state or files outside this project.

`build/cleanup-plan-current.json`, `build/cleanup-electron-intermediates.json`
and `build/cleanup-records/` record reviewed cleanup. Local implementation notes
are under ignored `build/local-notes/`.

`make clean PREVIEW=1` shows exact removals and dependency invalidations.
`make clean` retains source/download caches; `COMPONENT=...` selects individual
apps/components or cache/log/test categories. `ALL=1` resets generated storage;
`KEEP='sources downloads toolchains images logs tests'` retains selected classes.
Explicit source cleanup invalidates the shared runtime and its consumers.
The cleaner checks the current ISO app selection, protects selected targets,
public sources/configuration/private state, refuses unsafe external roots and
journals before removal. Pinned upstream key/config fixtures are checked against
Git/source-archive provenance before removal; private edits keep coherent source
trees and their preparation records. FIFO/socket cleanup is covered as well. Build roots and external Kbuild outputs are registered;
named wizard settings under build/ are registered for protection as well.

The canceled shallow sync must keep completed repository objects and its
recovery checkpoint. Clear abandoned Git pack/lock files only after checking
that the build stopped. `COMPONENT=cache` includes those managed sync leftovers.
`tools/clean_cache.py` remains the narrower product-preserving cache cleaner.
See [cleanup/recovery](repository-cleanup.md) for precise scope and KEEP rules.

## Publication

Run `make source-check` and the appropriate host checks for tool changes.
Guest changes require their corresponding VM suites; stop every test-owned VM
and preserve the package/community services. Generated artifacts, downloads,
credentials and local validation records stay out of Git.

Stage reviewed changes. Leave commit and push to the user unless explicitly
asked to execute them. Supply fish-compatible commands when requested.
