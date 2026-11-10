# System & Power

Coast exposes a narrow, asynchronous coordinated power request interface.
Firmware advertisement alone does not establish OS backend readiness, and a
successful request return does not prove physical poweroff/reset. The trusted
native environment is single-user; this is not a multiuser authorization boundary.
Browser renderer/broker roles must remain denied access to power controls.

## Shell

- `acpi` and `battery` print the same cached ACPI/runtime/adapter/battery report.
  They never evaluate AML in the calling task. Unknown values, unsupported
  methods, device errors and stale data stay visible.
- `power` prints the coordinated transition snapshot, including backend
  readiness, state, request ID, last error and failure stage.
- `poweroff` requests shutdown; `reboot` requests restart. Save application work
  first. Success means the request was accepted or coalesced with an identical
  pending request. Inspect `power` for progress or a recoverable failure.
- `power cancel` asks to cancel during `pending` or `freezing`. The request can
  race preparation and return busy. An accepted cancel does not establish that
  arbitrary firmware `_PTS` side effects were undone.
- Argument mistakes, query/output failures and rejected actions return nonzero
  status from `coast-sh -c`. `reset` still resets terminal settings.

## Desktop

Open **System & Power** from the registered application launcher. It displays
OS/architecture, monotonic uptime, CPU inventory, mount/storage persistence,
cached ACPI/telemetry, power readiness and the raw latest power request/error.
The diagnostic text hard-wraps long identifiers and supports wheel, arrows,
Page Up/Down, Home and End. Tab/Shift+Tab select controls; Enter/Space activate;
F5 or Ctrl+R refresh. Each sysinfo section is bounded by the 8192-byte ABI limit;
possible truncation is disclosed. Missing/failed/unfamiliar power data disables
actions. Cached sections refresh every 500 ms while the desktop is runnable.

Shut down and Restart are enabled only with explicit backend-ready flags and an
idle/failed state. They first ask for overall confirmation, then use the actual
editor dirty-document and terminal foreground-program checks. An unreadable
terminal activity query also requires confirmation. Canceling any review clears
all consent; a subsequent attempt reviews the applications again.

Review approval records consent for that immediate request. It does not mark a
document clean, close windows, signal terminals or terminate their programs.
All windows remain open through the reversible request, so rejected requests,
cancellation and recoverable failure preserve their state. Save first: accepting
poweroff can still lose unsaved application work when the terminal transition
succeeds. Other applications do not yet expose dirty-state hooks.

Repeated action clicks are disabled while a transition is active. Cancel is
enabled only for `pending`/`freezing`; its real syscall failure is shown rather
than reported as successful. The coordinator provides a one-second pending
window before software freeze. Input, applications and this UI pause during
actual freeze. Only an already-admitted cancellation can win the locked
FREEZING-to-PREPARING boundary; the dependable interactive cancellation window
is `pending`. Continuous stage animation or cancellation from the frozen
desktop is not promised. Cached stage/error details become readable again after
a recoverable failure. `firmware_prep_attempted=1` warns that firmware preparation
was attempted; software/AP rollback does not prove firmware rollback.

The fixed ACPI power button increments `fixed_button_events`. The desktop polls
this count every 250 ms, establishes a startup baseline and coalesces repeated
events during a review/transition. A new event opens/focuses System & Power and
asks the same explicit confirmation. A button event never automatically sends a
power syscall. If readiness is unavailable, it explains that no request was sent.
**Return to console** remains an independent desktop-exit action.

## ABI

`SYS_sysinfo=401` retains `(kind, buffer, capacity)` and bounded textual results.
`INFO_ACPI=6` contains the cached runtime/telemetry report. `INFO_POWER=7` contains
complete key/value lines such as:

```
power_state=idle
poweroff_ready=0
reset_ready=0
request_id=0
last_error=0
failure_stage=none
fixed_button_events=0
firmware_prep_attempted=0
```

States include `idle`, `pending`, `freezing`, `preparing`, `rendezvous`, `flushing`,
`committed` and `failed`. Unknown states fail closed in the UI. Readiness means
software preflight is available; it is not a hardware-success indicator.

`SYS_power=413` uses action in `rdi`, reserved flags in `rsi` (must be zero):
`OS_POWER_OFF=1`, `OS_POWER_RESET=2`, `OS_POWER_CANCEL=3`. A zero return is only
asynchronous acceptance/coalescing. The unused third argument (`rdx`) must also
be zero. Negative errors are exposed unchanged.
`recovering` means precommit release or firmware-service restoration did not
finish; new transitions are blocked. Firmware preparation can itself wait
indefinitely inside AML (for example, `Wait(event, 0xFFFF)`). The software-drain
deadlines cannot preempt that live method safely; there is no claim of a
universal preparation timeout or rollback of firmware side effects.

There is no arbitrary AML, suspend/resume, charge-limit, thermal or CPU-policy
control exposed by this ABI.

## Targeted UI validation

Run `python3 tools/test_power_ui.py` (optionally `--sanitize` for ASan/UBSan).
It compiles actual shell/System & Power,
editor and terminal source against mocked syscall/render boundaries. It checks
bounded/fail-closed parsing, query/action error exits, invalid arguments,
unsupported states, wrap/scroll, sequential dirty/active-terminal consent,
cancellation preserving windows/flags, late cancellation rejection, async/repeat
semantics and fixed-button confirmation without automatic power actions.

This host test does not establish graphical appearance, real ACPICA evaluation,
SMP quiescence, storage flushing, QMP SHUTDOWN/RESET or physical hardware success.
Those require their own coordinator/backend and guest/hardware validation.

See [capabilities and focused validation](system-power-validation.md) for the
implementation/test matrix, known limits and reproducible guest checks.

## Desktop diagnostic views

The View menu selects Power and battery, Processor inventory, Storage mounts,
Processes, Network diagnostics, Audio device, or Full diagnostics. Each uses the
existing kernel snapshot/PCM information APIs and shows unavailable data as such.
These views provide read-only observations; there is no Wi-Fi configuration or
master-volume service. Full diagnostics retains the detailed original power,
ACPI, CPU and mount information and includes the additional views. Reports have
room for every bounded snapshot; each individual API limit remains explicit.

Power and battery and Full diagnostics show the established power buttons.
Other views expose Refresh and retain shutdown, restart and pending cancellation
in the System menu. The same readiness checks, dirty-application consent and
reversible cancellation paths apply to menu actions. Diagnostic navigation
submits no power request. F5 refreshes; Tab selects visible controls; the report
scrolls with the wheel, arrows and Page Up/Down.
