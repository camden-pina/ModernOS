# Approval proposal: bounded, hash-pinned Electron compatibility probe

2026-10-25, revision 2: shared aggregate accounting. DESIGN ONLY. No cap, accounting rule, kernel source, sandbox, fuse or UID behavior was changed. No new image or large source build was run. Current tested desktop release remains A2.

This revision corrects an ambiguity in the initial draft: the requested 1 GiB is one shared aggregate for the entire profile descendant tree, not a fresh per-process allowance. No version of this policy was implemented.

## Exact action proposed for approval

Implement and test an opt-in compatibility profile in a separate candidate kernel/ISO, admitting only the unchanged official Electron 43.7.3 x86-64 executable at `/opt/electron-43.7.3/electron`, SHA-256:

`b315f26fdb0db1764ae6184125686aa5ed4f92a755abc01d95277b91994eb3c3`

This is byte-identical to the selected VSCodium 1.140.06880 executable. The first milestone is Electron's own Node-mode/jitless initialization and basic guest file/event-loop checks in one owned, diskless, network-disabled VM. It is not graphical workbench approval or proof of application compatibility. No default or earlier ISO is overwritten.

| Budget or rule | Proposed behavior |
| --- | --- |
| Approved main plus PT_INTERP image | **256 MiB combined**, summed page-rounded PT_LOAD memory, including BSS; checked before mapping |
| PT_INTERP subcap | Existing **128 MiB** retained, also counted in the combined 256 MiB |
| Approved executable file bytes | At most **256 MiB**, bounding full-file identity hashing work |
| All other executables | Existing **128 MiB per-image** behavior retained; no global constant increase |
| Shared profile-tree charged mappings | **1 GiB / 262,144 pages total**, summed across distinct descendant address spaces; imposed before loading |
| Each descendant address space | Existing smaller per-process/role limits also apply; no descendant receives a fresh 1 GiB pool |
| Existing inherited or role budget | Any smaller nonzero limit wins; never raise the current 512/768 MiB sandbox limits |
| Individual mmap/brk bounds | Existing **512 MiB** limit retained |
| Stack, user addresses, ELF metadata | Existing 8 MiB stack, address limits, 64 program-header limit and 128 MiB TLS bound retained |
| Executable permissions and access | W^X, NX, executable-stack/text-relocation rejection, file permissions and sandbox checks retained |

The shared 1 GiB ceiling sums the existing charged leaf-page accounting over distinct member VMs, including eager PROT_NONE/EOF pages and shared mappings as currently charged. Threads/vfork references to the same VM are counted once. A fork copy or a separate child VM adds its own full charge; shared memfd aliases across VMs remain conservatively charged according to the existing per-VM rules rather than being deduplicated as physical RSS. It is not a claim that page tables/kernel metadata are included in RSS accounting. Those retain existing PMM enforcement and must be covered by allocation-failure tests. No sparse/overcommit reservation support is part of this proposal.

The new ceiling tightens the currently unlimited ordinary CLI context (`page_limit=0`). It must not be used to enlarge an inherited sandbox ceiling: enforce both the shared group budget and every smaller inherited/per-VM/role ceiling before allocation. Retain group membership during exec, and ensure later role assignment cannot reset a per-VM ceiling upward or mint another group budget. Changing the main-image admission remains a resource-policy change requiring specific approval even though it is narrowly scoped and other controls remain intact.

## Why this is required and why the numbers are finite

Verified official Electron 43.7.3 maps 226,131,968 bytes (215.7 MiB). Its verified glibc interpreter maps 217,088 bytes, for 226,349,056 bytes combined, below 256 MiB. The current 128 MiB main-image cap necessarily rejects it before runtime startup. The maintained 43.8.3 artifact is slightly larger, not a smaller workaround. Stripping debug/section data would not reduce PT_LOAD memory, and running it indirectly through ld.so would evade the intended exec check; neither is proposed.

Saved A2 traces show standalone Node's successful jitless arithmetic reaches 156,687 charged pages, about 612.1 MiB, under the existing eager-backing model. A 512 MiB ceiling would fail that measured baseline. One GiB is a provisional finite probe ceiling, allowing bounded headroom for the larger Electron image/dependencies. Electron's real peak is not measured; exceeding the ceiling must fail with proper cleanup rather than trigger an automatic increase. Existing stricter roles may still fail, as intended. Two independently backed 612 MiB descendants would exceed the shared 1 GiB and must fail; this is a deliberately bounded single-runtime probe, not a promise that multiprocess workbench startup fits.

Electron also uses V8's memory cage. Even jitless mode may request much larger virtual reservations than the current 512 MiB mapping limit permits. That is an explicit likely later blocker, not permission to disable the cage or raise mmap limits. This proposal authorizes only a bounded image-admission/charged-ceiling experiment to observe the next real failure. Separate finite reservation/commit accounting would need its own design and approval if required.

## Admission and accounting design

Use a default-off build option and an immutable in-kernel allowlist containing the one canonical path and complete SHA-256 above. No user-supplied flag, environment value, writable manifest, basename or path prefix can grant a larger budget. All normal executable/file access checks run unchanged. Resolve/open the actual executable once; hash and validate the same open content under the loader's existing VFS serialization, with no identity cache that can go stale. Reject incomplete reads and identity mismatch. Preserve symlink/path validation and hold file identity consistent through mapping.

The kernel currently has no existing SHA-256 implementation found in its core library inventory. A real bounded hash implementation and known-answer/streaming/error tests are prerequisites; a userspace-supplied digest or hash stub is unacceptable. Hash identity is an admission gate, not a claim that an official application is vulnerability-free.

Pass an internal per-exec budget into validation; do not replace USER_MAX_IMAGE globally, as that also controls TLS and placement assumptions. Validate every segment, overlap, metadata range, interpreter and aggregate before allocation. Create a refcounted shared budget object for the initial admitted probe tree and attach each member VM before its first image/stack page. Every process-creation path must inherit that same object from the actual initiating task/context, not from a caller-supplied PPID or environment. Re-executing the approved hash within the tree must reuse the existing object, not create a second pool. Unapproved descendant executables retain the 128 MiB image cap and remain charged to the same group.

Threads and vfork sharing an existing VM take references without charging its pages again. Fork/spawn with a distinct VM must reserve sufficient group capacity before physical allocation, then convert reservations to owned charges or fully roll them back on failure. Exec within the group must charge both old and new VMs while they coexist; the old charge remains until its last reference/physical cleanup, including a vfork parent still owning it. An initial admission from outside the group must not retag an unrelated parent's borrowed VM; any transient old outside-group VM keeps its original limits, and all newly constructed profile VM pages are charged before allocation.

Use atomic/locked group accounting and lifetime references for allocation reservations and asynchronous creation. Unmap, failed map/clone/exec, crash and final VM destruction release exactly their owned charges once; task/thread exit alone must not release a VM still shared. No individual profile child or exec can detach from the group to recover a fresh allowance. Existing per-VM limit queries report that VM's hard upper bound; add diagnostic group usage/limit reporting rather than presenting remaining shared capacity as a private entitlement.

## Risks and required regression plan

* A larger admitted image consumes more memory and load time. Whole-file hashing plus loading about 224 MiB under the existing VFS lock may cause a noticeable cold-start stall; measure it and responsiveness honestly. No performance claim is made yet.
* Profile identity must not be vulnerable to pathname aliases, mutation during hash/load, short reads, integer overflow or caller-supplied policy. Exercise same-name modified files, another path, truncated files and exact hash mismatch. Verify ordinary exec remains capped at 128 MiB.
* Reuse the actual ELF parser sanitizer harness: malformed PHDRs, filesz/memsz overflow, unaligned/overlapping/out-of-range segments, W+X/executable stack/text relocations, malformed TLS/PHDR/dynamic/RELRO and interpreter overlap must still fail. Add synthetic boundary tests at 128/256 MiB and the combined interpreter limit without admitting those test hashes in the production profile.
* Inject PMM/map/read failures at multiple points and verify complete rollback, counters, file references and old process survival on failed exec. Test repeated loads/teardown, private/PROT_NONE/EOF accounting, shared objects, thread/clone paths and quota exhaustion followed by unmap/reuse.
* Verify cumulative shared-budget exhaustion across separate child VMs; threads/vfork must not double-count the same VM, while fork copies must count. Test concurrent alloc/free, sibling races, nested exec, old/new exec peak overlap, failed partial clone, child crashes, parent closure, kill/reap and last-reference cleanup. Re-exec or alternate spawn routes must not create a fresh pool.
* Verify sticky minimum limits before and after nested exec/fork/role assignment. Keep the existing 512/768 MiB sandbox quota and denial suites passing. The profile must never grant extra filesystem, process, network or credential authority.
* Run A1/A2 mapping guards/hints, epoll, userland/POSIX, dynamic musl/C++, job-control and native/HiDPI GUI regressions. Preserve all existing releases and the original frozen kernel by hash.
* Only then attempt the genuine verified Electron entry with interpreter settings, capture embedded runtime versions, startup time/peak accounting and the exact next failing API. Keep sandboxing enabled and stop at unsupported security/platform requirements. No full editor/window claim follows from Node mode.

Approval of this proposal does **not** authorize larger anonymous mappings, disabling V8's memory cage, relaxing W^X/memfd executable restrictions, disabling Chromium sandboxing, fake UID behavior, host changes or a large Chromium build.
