# Browser build tools

`make iso` prepares the browser build tools automatically on an x86-64 Linux
host. This page is background and troubleshooting information; users do not
need to follow a separate setup procedure to include the browser in the ISO.

## What these tools do

| Tool | Purpose |
| --- | --- |
| CMake 4.4.4 | Configures JavaScriptCore and WebCore and generates build rules |
| Ninja 1.13.2 | Executes those build rules and the Meson-generated rules |
| Meson 1.10.2 | Configures GLib/Gio and other browser libraries |
| Ruby 3.4.7 | Runs WebKit's source-code generators |

These programs run on the build computer. They are not browser add-ons or
programs required inside ModernOS, and they are not packaged into the ISO.
The kernel-only build does not need them.

## Automatic preparation

The JavaScriptCore and GLib builders call
[`tools/prepare_browser_host.py`](../tools/prepare_browser_host.py). It fetches
CMake, Ninja and Meson from pinned upstream URLs, checks their SHA-256 hashes,
and extracts them locally. Ruby is compiled from the pinned source archive in
[`userland/vendor/sources.json`](../userland/vendor/sources.json). Downloads use
the same verified cache handling as other source inputs.

The build selects these cached tools directly. No system installation of
them, pip, unzip or PATH editing is required. Normal host requirements such
as GCC/G++, GNU make, Python and curl still apply; see the
[README](../README.md#host-requirements).

Parallel builders share a preparation lock. Once prepared, tools are reused
offline. An incomplete extraction is repaired from its verified archive.
The userland Makefile orders the compiler, JavaScriptCore, browser libraries,
broker and renderer so parallel builds wait for their prerequisites.

You can prepare the tools ahead of time, without compiling the browser:

```sh
make browser-host
```

This command is optional. The normal complete build remains:

```sh
make iso
make run-image
```

## Cache and troubleshooting

All host-tool outputs live in ignored `build/modern-browser/` directories:

| Path | Contents |
| --- | --- |
| `downloads/` | Verified CMake, Ninja and Meson archives |
| `host-tools/cmake-4.4.4-linux-x86_64/` | CMake executable and modules |
| `host-tools/ninja` | Ninja executable |
| `host-tools/meson-1.10.2/` | Meson Python package |
| `host-tools/ruby/` | Ruby executable and standard library |
| `ruby-3.4.7/`, `ruby-host/` | Ruby source and compilation outputs |

Ruby preparation writes `browser-host-ruby-configure.log`,
`browser-host-ruby-build.log` and `browser-host-ruby-install.log` there.
A failing step reports the relevant log. Fix the reported host dependency
and rerun `make iso`; successful preparation is reused.

A checksum mismatch stops preparation and preserves the suspect archive.
Move or remove the reported archive, then rerun the build to fetch it again.
Downloads use configured mirrors first, then the primary URL and available
package mirrors; the final source retains retries and an IPv4 connection fallback.
`SOURCE_MIRRORS` also applies to these host tools. Serve their exact cached archive
filenames (including the `modernos-` prefixes) from a flat HTTPS directory; see
[mirror configuration](build-system.md#commands). Initial downloads need network
access; deleting the cache requires fetching or preparing it again.

`make clean` preserves shared port and host-tool caches. These artifacts do
not belong in GitHub. Pins and upstream links are recorded in the helper and
[`userland/vendor/README.md`](../userland/vendor/README.md).

Builders create their generated output directories automatically. The GLib TLS
and WebCore patchers restore missing or partial source backups from verified
pinned archives. Dependency stages check their installed libraries and metadata
before reusing a completion stamp, and reinstall missing outputs on the next build.

Run `make test-browser-build` for host bootstrap, cold builds, patch-backup
recovery, dependency-cache recovery and parallel Make ordering checks. GitHub
Actions runs these and the source-fetch checks on fresh checkouts for pushes
and pull requests.
