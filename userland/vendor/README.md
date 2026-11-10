# Upstream source inputs

Source archives are ignored by Git. `sources.json` records HTTPS URLs, optional
mirrors and SHA-256 hashes for the base userland, standalone JavaScriptCore,
NASM, Wayland, Limine and core libraries. Restore them with
`python3 tools/fetch_sources.py` from the repository root or `make fetch-sources`.
Existing valid files work offline; corrupt files are preserved and rejected.

Additional manifests pin QEMU/dependencies, package tools, Python/jq, extra
programs and Linux-compatibility ports. Those builders fetch their selected
archives into ignored output-local caches. Their recipes and patches are in
`tools/`, `ports/` and `userland/`. Source packages in the local Coast repository
also carry exact upstream archives for native/offline workflows.

`ports/electron/sources.lock.json` records the Electron/Chromium/Node graph;
`ports/linux-uapi/` contains the exact C header sources and their notices.
The older Linux-UAPI archive provenance files describe the original local header
snapshot; new QEMU/Qt/bridge builds use the checked-in header sources.

NetSurf and the WebCore browser are retired. Standalone JavaScriptCore remains
for the text-core editor and SDK checks. Chromium is the desktop browser.
See `Documentation/source-builds.md` for the rebuild matrix and remaining
full-image bootstrap requirements. Preserve upstream license notices under
`licenses/`; builders also stage the notices needed by their runtime packages.
