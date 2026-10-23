status: wip

# Slice wt-ntdll-reg: registry fidelity, measured by Wine's ntdll `reg` unit

Yardstick: `dlls/ntdll/tests/reg.c` (Wine 11.9), run through the layer as `ntdll_test.exe reg` by
`integration.winetest_ntdll` (docs/slices/wt-ntdll.md). Before this slice the unit never finished: it looped on
`NtEnumerateKey` at `reg.c:1987` and the 20 s cap killed it after 134 distinct failing lines.

## Why it looped

`check_enum_value` (reg.c:1376) reads `KeyFullInformation.SubKeys` of a key and then enumerates that many subkeys.
`\Registry\Machine\Software\Classes` did not exist in an overlay-only SOFTWARE hive (the builtin-only machine folder
carries no hives), so `NtOpenKey` failed, the stale stack garbage in the buffer became the subkey count (6619246) and
the loop ran millions of times. `NtEnumerateKey` itself answered correctly for the keys that exist; its one wrong
answer was `STATUS_INVALID_PARAMETER` for `KeyFullInformation`/`KeyCachedInformation`, which Windows serves.

## What changed (core/registry, core/dispatch)

Statuses and semantics, each pinned by a line of `reg.c` and by `tests/unit/test_registry.cpp`
(`registry_nt_name_create_link_statuses`, `registry_notify_requests_accumulate_per_handle`, the enumerate and
open-semantics tests):

* Names. `""` without a root handle is `OBJECT_PATH_SYNTAX_BAD`; `\` is `OBJECT_TYPE_MISMATCH` (the root directory
  object is no key); `\\\` is `OBJECT_NAME_INVALID`; `\Registry\` and `\Registry\\` open `\Registry`. A name outside
  `\Registry` is asked of the object manager the guest shares (`NtOpenDirectoryObject` on the host): `\Foobar` is
  `OBJECT_NAME_NOT_FOUND`, `\Foobar\Machine` and `\Machine\Software\Classes` are `OBJECT_PATH_NOT_FOUND`, `\Device\Null`
  is `OBJECT_TYPE_MISMATCH`. `OBJECT_ATTRIBUTES.Length != sizeof` is `INVALID_PARAMETER` (reg.c:390 family).
* `NtCreateKey` creates only the last component (a missing key on the way is `OBJECT_NAME_NOT_FOUND`; kernelbase's
  `RegCreateKeyEx` builds the path level by level itself); trailing and doubled backslashes are ignored; the access of the
  parent handle does not gate a create (on Windows the parent's ACL does); a stable key under a volatile one is
  `CHILD_MUST_BE_VOLATILE`; `REG_OPTION_CREATE_LINK` on an existing key is `OBJECT_NAME_COLLISION`.
* Links. `REG_OPTION_OPEN_LINK` passed to `NtOpenKeyEx` does not open the link (only `OBJ_OPENLINK` does); creating
  through a link whose target is gone creates the target; a link key takes only `SymbolicLinkValue` as `REG_LINK`
  (`ACCESS_DENIED` otherwise); a target with a terminating NUL does not resolve, a relative target is
  `OBJECT_NAME_INVALID`, a loop is `INVALID_PARAMETER`.
* `NtEnumerateKey`/`NtQueryKey`. Enumerate answers basic, node, full, name (the subkey's full name) and cached for the
  enumerated subkey; flags/virtualization/handle-tags classes are query-only and every other class is
  `INVALID_INFO_CLASS`; an index past the end (including `-1`) is `NO_MORE_ENTRIES`; a buffer under the fixed part is
  `BUFFER_TOO_SMALL`, under the whole answer `BUFFER_OVERFLOW`, both with the size needed. `KeyCachedInformation` now has
  Windows' layout (SubKeys at 12, MaxNameLen 16, Values 20, MaxValueNameLen 24, MaxValueDataLen 28, NameLength 32, 40
  bytes, no name).
* `KEY_VALUE_INFORMATION_CLASS` numbering was wrong in `registry.h` (`PartialAlign64` is 4 and `FullAlign64` is 3, not
  3 and 5); `NtQueryValueKey(KeyValuePartialInformationAlign64)` answered `INVALID_PARAMETER`.
* `NtDeleteKey` on a key that is already deleted succeeds; `NtRenameKey` checks the name before the handle and answers
  `CANNOT_DELETE` for a taken name (a sibling's or the key's own).
* `NtQueryObject` on a key handle (it used to go to the host, which does not know the handle): basic information
  (also for a deleted key) and the object name (`KEY_DELETED` for a deleted key); other classes are `NOT_IMPLEMENTED`.
* `NtNotifyChangeKey`: one notification per handle that keeps the filter and tree flag of its first request, later
  requests on the handle only add their event (Wine's server does the same, and reg.c:2174 pins it); an outstanding
  request resets its event; a filter of 0 never fires.
* A SOFTWARE hive that is not supplied now starts with the key tree every Windows has: `Classes` (`CLSID`,
  `TypeLib`, `Interface` with COM's IUnknown registered, `Applications`, `Directory`, `Folder`, `Wow6432Node` with its own
  `CLSID`/`Interface`/`TypeLib`), `Wow6432Node` and the link `Wow6432Node\Classes` to `Classes\Wow6432Node`. Empty keys
  and the one genuine IUnknown registration (`Interface` is never an empty key, reg.c deletes it and expects
  `CANNOT_DELETE`); a supplied hive has all of it.

## Result

Windows runners, first run of this slice (head 9487143, before the probe slice was merged in), `reg` unit,
Debug and RelWithDebInfo: `reg: 1186 tests executed (13 marked as todo, 0 as flaky, 16 failures), 0 skipped.` (was: killed
by the 20 s cap after millions of failures). 14 of the 16 were pointer assertions (below); the other two were
`\Device\Null` (`reg.c:337`, `:528`): `NtOpenDirectoryObject` answers PATH_NOT_FOUND on Windows for a device as the last
name component, where NtOpenKey answers TYPE_MISMATCH, so the object-namespace answer for a name outside `\Registry`
is now told apart by opening the parent directory. (The host's own `NtOpenKey` would be the better oracle but the
Wine 9 server that the linux-cross job runs on never answers an open of `\Foobar`.)

With the probe slice merged into this branch (origin/claude/youthful-cerf-l7xxsb-probe: `ProbeForRead/Write`, guarded
copies, a failed open leaves the handle NULL) the unit is at **1 failure** on the Wine host, measured here; the
Windows number is the one in "Still failing" below.

## Still failing

`reg` is **not** on `wine-pe/tests/ntdll.allowlist`: one assertion cannot pass while the guest is the unprivileged user
the layer promises (token.h: medium integrity, non-elevated):

* `reg.c:2548: Test failed: Failed to set SE_RESTORE_NAME and SE_BACKUP_NAME privileges, skipping tests`.
  `test_NtRegLoadKeyEx` enables SeRestorePrivilege and SeBackupPrivilege on its token and `win_skip`s when it cannot. On
  Windows (not Wine) a `win_skip` is a skip; under the `wine` platform (this guest is Wine's ntdll) it is a failure, so a
  standard-user token costs exactly one failure. It is not a registry fault and nothing was patched for it.
* If the token were elevated the test would run on, and then it fails differently: its three `NtLoadKeyEx`
  assertions are `todo_wine` (reg.c:2573-2590) and a Windows-correct `NtLoadKeyEx` (INVALID_PARAMETER_7 with a root
  handle and no flags, success with a NULL-free handle for `REG_APP_HIVE`) makes them "succeeded inside todo block",
  so being right is a failure there (measured here with a temporary elevated token: 3 failures). Same class as the
  `rtlstr` U+037F difference in docs/slices/wt-ntdll.md: recorded, not patched.

Not done, noticed on the way: `NtLoadKey*`, `NtUnloadKey`, `NtSaveKey`, `NtRestoreKey` and `NtReplaceKey` do not check
SeRestore/SeBackup the way an unprivileged caller would see them (STATUS_PRIVILEGE_NOT_HELD); no guest reaches them
because every Wine test and program that needs the privilege skips first, so it was left alone.
