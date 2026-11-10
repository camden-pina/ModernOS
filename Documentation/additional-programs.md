# Additional upstream programs

These programs run in Terminal with the active profile's normal account
privileges. The Code OSS desktop image includes them; source builds can opt in
with `make more-programs` before `make iso`.

| Programs | Version | Use |
| --- | --- | --- |
| `ninja` | 1.13.2 | Build dependency graphs, parallel recipes, incremental C/C++ builds |
| `pkgconf`, `pkg-config` | 2.5.1 | Resolve library versions and compiler/linker flags from `.pc` files |
| `m4` | 1.4.21 | GNU macro processing, includes and expressions |
| `gperf` | 3.3 | Generate perfect hash lookup tables in C/C++ |
| `bc`, `dc` | 1.08.2 / 1.5.2 | Arbitrary-precision infix and stack calculators |
| `linux-patch` | 2.8.1 | Full GNU patch, including dry runs and reverse patches |
| `rsync` | 3.5.2 | Recursive local synchronization, checksums, filters and dry runs |
| `zstd`, `unzstd`, `zstdcat`, `zstdmt` | 1.5.7 | Zstandard compression, decompression and threaded operation |
| `lz4`, `unlz4`, `lz4cat` | 1.11.0 | Fast LZ4 compression and decompression |
| `linux-xz`, `linux-unxz`, `linux-xzcat` | 5.8.4 | Full XZ compression and decompression, including threaded operation |

The `linux-` names keep existing BusyBox `patch` and `xz` commands available.
`pkg-config` selects pkgconf. All other aliases retain their upstream behavior.
Use each program's `--help` for its options.

```sh
printf 'scale=30; sqrt(2)\n' | bc -l
printf '2 100 ^ p\n' | dc
zstd -T2 -k notes.txt
unzstd -c notes.txt.zst
lz4 -T2 notes.txt notes.txt.lz4
linux-xz -T2 -k notes.txt
rsync -rlpc --dry-run ~/Documents/ ~/Desktop/backup/
rsync -rlpc ~/Documents/ ~/Desktop/backup/
linux-patch --dry-run -p1 < change.patch
pkg-config --cflags --libs my-library
ninja -j2
```

Rsync requires an existing or creatable destination parent. Start with a dry run
when applying filters or `--delete`; those are real upstream operations. User
files, including these tools' outputs, follow the existing filesystem's
persistence rules. The boot RAM filesystem is lost at reboot.

## Build and packaging

`tools/build_extra_programs.py` downloads the official source archives pinned in
`userland/vendor/extra-program-sources.json`. It uses the existing musl cross
compiler, host CMake/make and Bubblewrap. Builds have no network, a read-only
workspace and source tree, and a writable output directory. Configure explicitly
cross-compiles; target programs never run on the host. GNU bc uses its shipped
math table so the build cannot execute its target bootstrap calculator.

The default output is `build/program-ports-r2`; `--package <name>` selects a
package during port development. Run the full builder to produce the complete
set. `--install-runtime` stages files for ordinary image generation. The receipt
merges the verified earlier Python/jq manifest without rebuilding or changing
those artifacts. Pass the resulting `BUILD.json` to the desktop image builder's
`--program-ports-build` option. Licenses and corresponding source archives are
included in the image. Original sources are inventoried before/after compilation;
packaging verifies the recipe, fixture and binary hashes.

## Scope and validation

The validation workload is installed at
`/usr/share/coast/program-ports/extra-check.py`; run it with `python3` as the
owner account. `tools/test_extra_programs.py` runs that workload in an owned,
diskless, networkless Coast VM, then exercises console and desktop Terminal
interaction. It covers exact versions, arbitrary-precision math, m4 includes,
threaded compression and byte-exact file/pipe round trips, corrupt archive
rejection, patch/reverse/dry-run behavior, and rsync's local recursive updates,
symlinks, checksum mode, exclusions and deletion. It also resolves a `.pc` file,
builds and executes a real C program with Ninja and the native compiler, checks
header dependencies and incremental builds, and compiles and executes a
gperf-generated table. System-write denial remains enforced.

LZ4's normal integrity check exposed a device compatibility issue: seeks on
`/dev/null` were rejected. Null/zero device descriptors now reset their offsets
to zero for valid seek origins, matching
[Linux's device implementation](https://github.com/torvalds/linux/blob/v6.18/drivers/char/mem.c).
The descriptor checks still reject O_PATH/bad descriptors and invalid origins;
pipes remain nonseekable. The credential parity suite compares the same fixture
in isolated Linux and Coast guests, including negative/large offsets and dup
aliases.

The R230 desktop image passes 16 program workflow groups, the 13 Python/jq
regressions, all 93 Linux/Coast parity traces, account-boundary checks and
the 17-case Code OSS desktop workflow. Reports and source inventories are in
`build/program-ports-r2`; `QUALIFICATION.json` records the selected image and
exact evidence. Target executables ran only in the disposable guests.

Rsync uses its bundled zlib and popt; optional ACL, extended-attribute, iconv,
OpenSSL, xxHash, Zstandard and LZ4 integrations are disabled in this port.
Remote shells, daemon service and network transfers are not qualified by the
offline checks. No SSH client is added. Zstandard's optional gzip/LZMA/LZ4
backends are disabled; the separate LZ4 and XZ tools handle those formats.
These are static musl source ports under Coast's existing ABI and account
policy. No new privilege is granted by installing them.

Upstream sources: [Ninja](https://github.com/ninja-build/ninja/releases/tag/v1.13.2),
[pkgconf](https://github.com/pkgconf/pkgconf), [GNU m4](https://www.gnu.org/software/m4/),
[GNU patch](https://www.gnu.org/software/patch/), [gperf](https://www.gnu.org/software/gperf/),
[GNU bc/dc](https://lists.gnu.org/archive/html/info-gnu/2025-05/msg00006.html),
[rsync](https://download.samba.org/pub/rsync/NEWS.html),
[Zstandard](https://github.com/facebook/zstd/releases/tag/v1.5.7),
[LZ4](https://github.com/lz4/lz4/releases/tag/v1.11.0),
[XZ Utils](https://github.com/tukaani-project/xz/releases/tag/v5.8.4).
