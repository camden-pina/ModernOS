# Accounts, root and sudo

The physical console and desktop Terminal start as UID/GID 1000, rather than
root. Settings opens to **Profiles**, where each profile has a stable UID/GID,
private home, password, standard/administrator role, sudo timeout, terminal
shell, avatar and description. The first profile is the owner/administrator;
additional profiles start as standard users. Set an owner password before
creating other accounts or using sudo. Password dialogs mask input and clear
their buffers on completion or cancellation.

`passwd` changes the current account's password. Administrators use `sudo
command`, `sudo -v`, `sudo -k`, `sudo -n command`, `sudo -u USER command`, or
`sudo -i`. `-S` reads the password from standard input. The normal prompt turns
terminal echo off and restores it on interruption. Sudo discards inherited
environment variables, supplies a fixed command search path, closes extra file
descriptors and sets the command's actual credentials before execution.
`sudo passwd root` sets the initially locked root password; `login root` then
authenticates at the physical console. `login 1000` returns to the owner account.

The kernel maintains real, effective, saved and filesystem UID/GID values,
supplementary groups, owner/group/other mode checks, sticky directories and
set-ID ELF execution. `/bin/sudo` is root-owned mode **4755**, including in the
standard initramfs builder. Authentication checks the real caller against
kernel-owned policy. An ordinary program cannot obtain elevation by calling
the account syscall directly, changing preferences, supplying environment
variables or changing its UID. `no_new_privs` is inherited and irreversible;
it suppresses set-ID elevation. Writes by an unprivileged process and changes
of file ownership clear executable set-ID bits. `id`, `whoami`, `chmod` and
`stat` expose the real state.

Passwords use PBKDF2-HMAC-SHA256 with 600,000 iterations, independently generated
128-bit salts and constant-time digest comparison. `/etc/coast-accounts`
is a root-owned, mode-0600 kernel-managed file. Its descriptor is never placed
in a user process's descriptor table. Invalid policy fails closed. Five failed
attempts lock authentication for 30 seconds. A cached sudo approval is bound
to the account, original session, exact terminal endpoint, policy generation
and expiry. Changing policy or switching profiles invalidates approvals.

The native desktop is a trusted root broker. Ordinary Files/editor operations
use the authenticated profile's filesystem identity. Only the protected native
`/bin/gui` executable can enter that broker role; interpreters, aliases,
namespace processes and `no_new_privs` processes cannot. Kernel login identity
takes precedence over the preference registry's `active` field. A profile
switch checks the destination password, reviews unsaved work, closes apps,
terminates the old account's terminal descendants and clears the clipboard.
Code OSS currently runs only in the owner profile, because its existing private
namespace is pinned to UID 1000. Other profiles are denied rather than given
access to the owner's home.

The `code [folder]` command uses a separate static setuid frontend. Kernel
admission checks that protected executable and the active owner account. It
accepts only a folder, checks it with the caller's filesystem identity, closes
extra descriptors and execs one fixed display bridge with a fresh environment.
The dynamic loader never sees inherited `LD_*` variables. The existing bridge
then drops Electron to UID 1000 and applies NNP/seccomp/Chromium sandboxing.
This permits an ordinary Terminal to open the editor without granting the
shell root privileges or accepting arbitrary privileged command arguments.

## Scope and remaining differences from Linux

This is a native account backend on Coast's kernel, with Linux credential
semantics for the operations above. It is not upstream sudo/PAM, and does not
implement the full Linux capabilities, NSS, sudoers, SELinux or general chroot
interfaces. Supplementary groups are currently limited to 32. The existing
Chromium namespace, seccomp and restricted-proc policies continue to apply.

**Passwords and account policy currently last for the live boot.** The writable
boot RAM filesystem supplies the required ownership/mode enforcement. FAT32
preferences do not provide a secure credential store and never receive password
records. Unprivileged filesystem access currently fails closed on mounted
filesystems without the kernel's validated DAC path. Persistent Linux-style
accounts need a writable filesystem with validated ownership and permissions;
a saved desktop preference file alone does not provide that security.

## Validation

`tools/test_credential_parity.py` compiles one static fixture and runs its exact
bytes in an isolated Linux guest and an isolated Coast guest. It compares
all credential traces from 15 fork-isolated cases: real/effective/saved IDs,
permanent and temporary drops, denied root regain, sorted supplementary groups,
filesystem IDs, setuid/setgid ELF execution and auxiliary vectors, `no_new_privs`,
saved-ID reset on exec, set-ID scripts, mode-bit clearing, FUTEX_WAKE_OP and
per-descriptor FIOCLEX/FIONCLEX flags. The device-seek cases compare null/zero
offsets, valid and invalid origins, dup aliases, O_PATH and pipe rejection.
Neither fixture executes on the host.

`tools/test_guest_accounts.py` checks protected files/homes, direct-syscall and
forged-actor denials, real set-ID sudo execution, standard-user denial,
environment sanitization, `no_new_privs`, failed authentication and lockout.
`tools/test_desktop_profiles.py` drives the actual profile/password UI and
verifies private homes, role authorization, password timeouts, session closure,
console identity and forged preference rejection. The KDF is independently
compared with Python's PBKDF2 implementation by `test_account_kdf_host.py`.
`tools/test_code_launcher.py` checks ordinary CLI launch before setting any
password, loader-environment sanitization and the unprivileged shell identity
after editor shutdown.

References: [Linux credentials](https://www.kernel.org/doc/html/latest/security/credentials.html),
[setresuid](https://man7.org/linux/man-pages/man2/setresuid.2.html), and
[setreuid](https://man7.org/linux/man-pages/man2/setreuid.2.html).
