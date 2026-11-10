# ACPICA integration

Coast uses the full hardware-enabled ACPICA 20260930 runtime, pinned to
upstream commit `e85aa3bebaae81ed1e6d163511ac7e1eafe732cf` from
<https://github.com/open-acpica/acpica/tree/20260930>. Runtime sources are
vendored in `lib/acpica`; no interpreter download is needed during a kernel
build. The OS services are implemented in `drivers/acpi/osl.c`.

## Initialization and interfaces

- Early ACPI discovery validates the supplied RSDP, root, FADT and DSDT. This
  preserves table access for HPET, APIC, SMP and PCI initialization.
- The ACPICA subsystem and table manager are initialized using the physical
  RSDP supplied by either boot adapter.
- After APIC and PCI discovery, ACPICA loads the AML namespace before creating GPE blocks, enables
  hardware/events, evaluates optional `_PIC(1)` and completes object/device
  initialization (`_REG`, `_STA`, `_INI`), then enables runtime GPEs. The
  initialization-pass log is not a claim that every firmware device method
  succeeded: upstream reports individual failures and continues. Boot logs
  report device count and the result of `_S5` evaluation.
- SCI interrupts are allowed once its handler is installed, including during
  boot AML and the later audio resource queries, so firmware global-lock
  releases can complete. Boot logs check IF before `_PIC` and after the boot
  consumers. The LAPIC timer remains masked; CLI is restored before SMP and
  scheduler initialization.
- SCI routing uses the MADT's I/O APIC and interrupt-source overrides, with
  vector 0x52 reserved by this port. A level-triggered source is serviced
  before LAPIC EOI. The fixed power-button event is acknowledged and counted;
  the desktop asks for confirmation rather than automatically powering off.
  See [System & Power](system-power.md) for the cached status and asynchronous
  coordinated shutdown/reset interface and its validation limits.
- Four scheduled event workers and one notification worker execute deferred
  ACPICA callbacks. They are created after the initial user shell, preserving
  PID 1. GPE admission is reserved from the complete boot GPE topology before
  `_PIC`/object-initialization AML can enable newly bound GPE methods.
- After object initialization, BSP preparation maps discoverable static
  SystemMemory regions and fixed FADT memory registers before SMP. Only
  already-resolved regions or exact literal-integer AML declarations are
  evaluated; computed or method-created regions are not speculatively executed.
  Preparation counts, failures, deferred-region counts and reset/sleep register
  readiness are available through `acpi_os_get_runtime_status()`.
- The AW88399 consumer uses ACPICA objects for `_HID`, `_SUB` and `_CRS` and
  releases every returned allocation. Device identity, resource, GPIO
  ownership and lock checks remain mandatory before amplifier reset.

OS services are in `drivers/acpi/osl.c`: allocation, physical mappings,
interrupt-safe spinlocks, count semaphores/timeouts, atomic FACS global lock,
deferred execution, timer/delays, I/O ports, PCI ECAM and serialized CF8/CFC
fallback, and SCI installation/removal. ACPICA uses its local object cache and
binary semaphores for mutexes. Its freestanding C-library symbols are prefixed
so that they cannot replace kernel string or formatting implementations.

The kernel formatter lives independently in `lib/vsnprintf.c` with its MIT notice.

## Build

The existing commands are unchanged:

```sh
make -j4
make iso
make LLVM=1 O=build/clang -j4
```

A complete image still requires the project's native GCC and browser ports;
a kernel ELF alone is not a complete OS. See [build-system.md](build-system.md)
and the README for host tools, pinned source fetching and bootloader choices.

## Tests

Host regression checks compile the actual OSL and audio consumer against the
vendored public headers. They use ASan/UBSan; privileged hardware boundaries
are replaced by explicit test shims. They are not physical hardware tests.

```sh
make test-acpi-host
python3 tools/test_acpica.py --help
python3 tools/test_acpica_negative.py --help
```

The OSL tests cover concurrent spin/global locks, semaphore counts and limits,
timeouts and wakeups, PCI register access widths, deferred execution, queue
limits and OSL queue shutdown. They saturate both ordinary rings while admitting
1024 configured GPEs, exercise ordered completion under notification pressure,
re-trigger a GPE before its original callback returns, and verify progress when
three GPE-like callbacks await a signal from a fourth event worker. A separate
scenario demonstrates the finite boundary: a fifth queued GPE cannot progress
while all four event workers wait, until an external signal releases them.
Every test-owned allocation is counted. LeakSanitizer is disabled because some
sandbox ptrace supervisors do not support it; AddressSanitizer and
UndefinedBehaviorSanitizer remain enabled.

`tools/test_acpi_osl_mapping.py` retains the real OSL page walk and mapping
policy. Synthetic page tables cover 4 KiB/2 MiB/1 GiB leaves, PAT selection,
write permission propagation, canonical/LA57 rejection, mixed RAM/MMIO spans
and immutable runtime mapping reuse. Its first runtime memory touch succeeds
after boot-only preparation without new page-table writes. Synthetic MCFG/ECAM
tests cover real memory accesses, split-DWORD 64-bit PCI operations, nonzero
bus starts, table reference lifetime and malformed/missing/error paths.

`tools/test_acpi_runtime_preflight.py` tests the actual BSP helper, including
literal AML bounds/negative forms, optional deferred/failed regions, preparation
budgets, fixed-register readiness and GPE topology reservation. Its ACPICA
parser and hardware calls are spies; it does not establish AML interpreter or
physical-device behavior.

`tools/test_acpi_gpe_dispatch.py` combines the real OSL with the upstream
GPE dispatch, asynchronous method, completion and finish bodies. Emulated
hardware and AML boundaries prove that saturation does not leave configured
GPEs disabled, and that completion remains behind earlier notifications.
This is an upstream-core host regression, not injected SCI/GPE hardware or AML
execution. Deliberate source mutations test sensitivity to the former initial
queue-loss and inline-completion-ordering failure modes.

`tools/test_acpi_boot_interrupts.py` also compiles the actual bootstrap functions
and upstream FACS acquisition/global-lock SCI handler with host-safe interrupt
and scheduler boundaries. It models firmware ownership, release and SCI
delivery during `_PIC`, object initialization and audio queries, checks CLI
before SMP setup, and verifies that removing STI or CLI is detected. The
firmware release and SCI dispatch are deterministic host mocks, not injected
guest contention or physical hardware. QEMU boot logs separately record
the actual IF-enabled boot condition; the host model does not establish a
hardware global-lock handshake.

The guest test injects a synthetic SSDT into a disposable QEMU q35 VM and
uses the opt-in `acpi.selftest` boot argument. It verifies actual AML execution
and deferred notification behavior, UEFI boot with one and four CPUs,
ring-3 regression checks, and repeated fixed power-button SCI events. This
parameter is absent from normal boot configuration. GPE method binding and
software/hardware enable bits are checked; synthetic GPE interrupt injection
is not performed. Fixed power-button SCI delivery is exercised through QMP.
The fixture also declares a read-only literal SystemMemory region in an
unassigned q35 MMIO hole. Its Field is first accessed after scheduling, proving
that BSP preparation supports a runtime-first access without changing global
page tables. The bus-hole fill value is not asserted. This SSDT is strictly for
the disposable q35 test configuration, never physical hardware.

`tools/test_acpica_negative.py --iso build/coast/image/Coast.iso` separately checks that the
fixture is inert without the boot token, a missing fixture reports failure
without losing the shell, and removing the AML mutex produces a detected lost
update. It preserves the original fixture and source ISO; the deliberately
unlocked SSDT is generated only under the ignored test-output directory.

## Current platform limits

These are explicit limits rather than successful hardware coverage:

- Supported architecture is four-level x86-64 paging. SCI destination APIC IDs
  must fit the I/O APIC's 8-bit physical destination; interrupt remapping is
  not implemented. Only one SCI registration is supported.
- The VM subsystem has no SMP TLB shootdown facility. OSL establishes global
  mappings during BSP initialization. Once scheduling starts, it reuses
  existing valid mappings and rejects new/retyped ranges. A single mapping
  is limited to 64 MiB. Borrowed shared direct mappings are not unmapped.
  BSP region preparation is limited to 64 MiB per region and 256 MiB of total
  preparation work. Optional oversized, failed or computed/deferred regions
  remain unprepared and are counted. Temporary method-created regions are
  absent from the public namespace walk. Later accesses work only if a safe
  mapping already exists; otherwise they fail visibly. Fixed FADT reset/sleep
  register readiness is a mapping capability, not proof that the firmware can
  successfully shut down or reset a physical machine.
- Known firmware tables, RSDP and FACS are registered as RAM so that reserved
  firmware pages retain RAM cache attributes. Boot-map RAM/ACPI NVS is also
  preserved; device mappings use uncacheable attributes. Mixed RAM/MMIO spans
  are classified per complete page; ambiguous partly-RAM pages are rejected.
- ACPICA core support does not by itself add drivers for firmware-specific
  embedded-controller, GPIO, GenericSerialBus or platform operation regions.
  Unsupported operation regions report ACPICA errors. Suspend/resume,
  thermal policy, batteries and physical laptop audio are not established by
  the QEMU tests.
- Ordinary event and notification rings are bounded to 256 queued items each.
  Their rejection counts and high-water marks, mapping rejections and GPE
  admission rejections are queryable through `acpi_os_get_diagnostics()`.
  Queue rejections also produce rate-limited diagnostics.
- Every configured boot GPE has separate reserved dispatch and completion
  slots. Ordinary notifications cannot consume that capacity. Completion is
  merged into the notification FIFO by enqueue sequence, so a full ordinary
  ring does not force upstream's premature inline GPE re-enable fallback.
  Per GPE, one running and one pending generation of each kind are supported
  and serialized; upstream disables hardware until completion, preventing a
  second pending generation during normal dispatch. Unknown contexts or
  duplicate pending requests return explicit errors. Runtime hardware GPE
  topology mutation/removal is unsupported; new method binding within an
  already-reserved block does not require more admission capacity.
- Four event workers support independent/dependent GPE progress without an
  unbounded task-per-event policy. Firmware dependency chains occupying all
  four event workers can still stall. One notification worker preserves FIFO
  ordering; firmware notification handlers must not wait for a later handler
  on that same queue. These are explicit liveness boundaries, not established
  arbitrary-firmware concurrency support.
- Full `AcpiTerminate` and runtime subsystem unload are unsupported. The OSL
  termination hook drains its own queues, but upstream namespace destruction
  precedes that hook; draining from SCI removal could deadlock an ACPICA worker
  on itself. Coordinated power keeps the boot-lifetime namespace intact and
  quiesces clients/deferred work; it never uses termination as a shortcut.
  SCI removal synchronizes the
  top-half and leaves vector 0x52 masked so a delayed LAPIC EOI can still clear
  I/O APIC remote-IRR. Host tests model removal/reinstallation with a delayed
  EOI; they do not establish physical-hardware teardown support.

Licenses are retained under `lib/acpica` and packaged by the normal initramfs
builder from `userland/vendor/licenses`.
