status: ready

# Slice om: the object manager namespace

Run: https://github.com/user99672223/win10xbox/actions/runs/39935668755 (head 07f4891; linux-cross, windows Debug, windows RelWithDebInfo all success; the linux-cross job was re-run once after `apt-get install` timed out three times)

A Windows program sees `\BaseNamedObjects`, `\Sessions\<n>\BaseNamedObjects`, directory objects and symbolic links, and
named events, mutexes, semaphores, sections and timers that a child it starts can open by name. The design, the layout
and the name mapping are in docs/DISPATCHER.md "Object manager namespace"; this file is the measurement log.

## What exists

* `core/dispatch/ob_namespace.{h,cpp}`: the tree (directories, links, kernel leaves), the lookup rules, the
  `NtQueryDirectoryObject` buffer (`PackDirectoryEntries`), the host name encoding. Pure; `tests/unit/test_ob_namespace.cpp`.
* `core/dispatch/nt_object.cpp`: `NtCreate/NtOpen DirectoryObject`, `NtQueryDirectoryObject`,
  `NtCreate/NtOpen/NtQuery SymbolicLinkObject`, `NtQueryObject`, `NtMakeTemporaryObject`, and the named create/open of
  Event, Mutant, Semaphore, Timer, Section (`nt_section.cpp` calls `CreateNamedHostObject`), KeyedEvent, IoCompletion,
  Job, reserve objects. `TranslateOA` (nt_file.cpp) resolves file names that go through the tree.
* `tests/guest/named_objects.c`, `integration.guest_named_objects`: a guest creates `Local\` and `Global\` events and a
  named section, lists both directories, starts itself; the child opens all three by name, reads the section, sets the
  events, exits 7; the parent exits 42.
* `tests/appcontainer/ac_probe.c` `ns.*` probes (printed by host-win32-appcontainer.yml): where a `Local\` object lands in
  an AppContainer and what the container may do in that directory (open to query, create sub-directories and links, open
  the machine-wide `\BaseNamedObjects`).

## Name mapping

Guest `\Sessions\<n>\BaseNamedObjects\foo` (also `Local\foo` relative to it, `\BaseNamedObjects\Local\foo`...) ->
host `<host Local\ directory>\W10X<layer>!|Sessions|<n>|BaseNamedObjects|foo`. `\BaseNamedObjects\foo` (= `Global\foo`) is a
different name: `...!|BaseNamedObjects|foo`. The layer is drawn by the first process of a program tree and passed to the
children in `WIN10XBOX_OBJNS`.

## ntdll `om` unit

Not on `wine-pe/tests/ntdll.allowlist`: it cannot print its summary line through this layer, and where it can it
reports failures that are deliberate. On the Windows runners it still ends in an access violation, first assertion
`om.c:2941` (`NtCreateDebugObject`). Measured under Wine as the host:

* Before: stops at `om.c:2941` (`NtCreateDebugObject`, `STATUS_NOT_SUPPORTED`) and faults at `om.c:2942` (no handle).
  E6 keeps `NtCreateDebugObject` unsupported, so the unit still stops there: everything it does before, in
  `test_null_in_object_name`, now passes (events, mutants, semaphores, timers, sections with names carrying a NUL).
* With the debug-object lines and the window station lines taken out of a private copy of `om.c` (not committed), the
  rest runs: 518 assertions, 108 failures under Wine as the host. Of those, the ones that belong to this slice pass:
  `test_case_sensitive`, `test_name_collisions`, `test_all_kernel_objects` / `test_name_limits` for every object type,
  `test_directory`, `test_symboliclink` (including the file open through a link in `\DosDevices`), `test_query_directory`,
  `test_globalroot`, `test_object_identity`, `test_duplicate_object`.
* `om.c:303` and `om.c:1416` are `todo_wine` in Wine because Windows answers them the way this layer now does:
  "Test succeeded inside todo block" counts as a failure, so a more faithful layer than Wine cannot reach 0 failures.

## Other tests this slice touched

* `kernel32` `drive`: back at its baseline (2). `\??` now lists the machine-wide devices (`GetLogicalDrives` and
  `QueryDosDevice` enumerate `\DosDevices`); the guest sees its own drives (C:), not the host prefix's.
* `kernel32` `volume`: baseline 35 -> 39 (kernel32.baseline says why): `volume.c:165` and `:200` are `todo_wine` that now
  pass, and Wine's harness counts "succeeded inside todo" as failures.

## Still open

* `om`: `NtCreateDebugObject` (E6), window stations (`GetProcessWindowStation` returns nothing), `NtGetNextProcess` /
  `NtGetNextThread`, `\Device\Mailslot` / `\Device\NamedPipe` names of file handles, `IoCompletionReserve` /
  `UserApcReserve` in `ObjectTypesInformation` (Wine host), registry key name limits (`om.c:1005..1126`, core/registry).
* Directories and links a guest creates exist only in that process; named objects inside them are found by a child
  only when the directory exists there too.
* `\Device` and `\Driver` are the host's: the driver host's devices are not listed there (its links are, in `\??`).
* `OBJ_PERMANENT` on directories and links skips the `SeCreatePermanentPrivilege` check.
