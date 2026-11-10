# Source publication and cache cleanup

The repository is https://github.com/camden-pina/ModernOS.git and development
uses `main`. Git contains kernel/application sources, port patches, pinned
upstream manifests, editable configuration, artwork and license notices.

Generated programs, ISO images, source downloads, object trees, toolchains and
test captures stay in ignored local directories. Private signing identities,
credentials and package/community service state are never source inputs.

## Check the source tree

```sh
make source-check test-source-tree
make source-archive
git status --short
```

The source audit checks tracked and new files, including force-added ignored
files. It rejects compiled programs, archive downloads, generated paths,
unsafe links, oversized files and private-key/token patterns without printing
their contents. `build/coast-source.tar.xz` has deterministic timestamps,
ownership and a SHA-256 inventory; its pinned lwIP source supports offline
kernel builds. The README screenshot is a small PNG from an actual Coast VM.

The GitHub workflow runs publication, source-fetch, assembly, package-trust,
ACPICA/power, EFI and boot-config checks, then compiles the kernel and boot components.
See [contributing](../CONTRIBUTING.md) and [licenses](../LICENSES.md).

## Local build storage

The full desktop builds Electron/Node, GNU ABI libraries, GUI dependencies,
Code OSS, compilers and native ports. These outputs are much larger than the
publishable sources. Multiple output roots retain separate object trees and
ISOs; select a new root only when build inputs change or an independent build
is needed.

```sh
python3 -B tools/clean_cache.py
python3 -B tools/clean_cache.py --apply
python3 -B tools/clean_cache.py --intermediates --apply
```

The default command previews Python bytecode, npm download caches and linker
caches. `--intermediates` additionally selects regenerable Electron/Node
object and split-debug directories only when the completed runtime receipt
verifies and does not reference them. Compiled runtime products, receipt-bound
files and selected image/service directories stay in place. Source downloads
and native compiler SDKs are retained for subsequent builds.

The cleaner protects tracked files, links outside the project, selected image
and service targets, and private state. Each applied cleanup records paths,
logical byte sizes and measured free space in `build/cache-cleanup.json`.
Filesystem hard links or shared extents can make logical size reductions
larger than the disk space actually recovered.

Inspect project size with `du -h --max-depth=1 .` and host free space with
`df -h .`. Avoid deleting all of `build/` while using locally configured
package/community services. `make clean` and `make userland-clean` clean their
specific outputs. [Source builds](source-builds.md) describes complete rebuilds.
