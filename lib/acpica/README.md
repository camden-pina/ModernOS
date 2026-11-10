# ACPICA runtime

Vendored from the official ACPICA 20260930 release at:
https://github.com/open-acpica/acpica/tree/20260930
Upstream commit: e85aa3bebaae81ed1e6d163511ac7e1eafe732cf

This directory includes public/internal headers and the nine runtime component
families; compiler, debugger, disassembler, command-line tools, and upstream
OS service layers are not linked. Preserve the notices in individual files.
The included LICENSE.BSD-3-Clause and LICENSE.GPL-2.0-only describe upstream's
license options; some legacy files retain the original Intel notice.

Coast changes (2026-10-14): add `accoast.h`, select it in `acenv.h` and
`acenvex.h`, mark the public header as a GCC system header for diagnostics (dependencies remain tracked), and add explicit Kbuild manifests. Debug-only resource dump objects are excluded from the non-debug runtime. The new platform configuration
is derived from the Original Intel Code's platform interface. ACPICA's local
C-library symbols are prefixed to avoid ABI collisions with kernel strings.
OS services live in `drivers/acpi/osl.c`. See `Documentation/acpica.md`
for initialization, tests and current platform limits.
