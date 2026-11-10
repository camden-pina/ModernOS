This directory records the upstream VSCodium application provenance and early
platform probes. The current full Code OSS workbench runs through Coast's
ported Electron runtime and native desktop integration. Its guest checks cover
editing, real file saves, sandbox permissions, relaunch and complete cleanup.
See [current source coverage](../../Documentation/source-builds.md) and
[native build tools](../../Documentation/native-build-tools.md).

`UPSTREAM.json` records the original release import. Its early execution note
describes that historical probe, before the application port worked. A fresh
source rebuild of the entire workbench and its native modules remains pending.
Keep upstream licenses and notices with redistributed application payloads.

`SOURCES.json` pins the original VSCodium source release, its VS Code commit,
both public archive checksums and the release-string build identity. Use
`tools/build_codeoss_source.py` for the new source preparation and workbench
compilation path; it still needs full compilation, runtime packaging and guest
qualification.
