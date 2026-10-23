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

## Still failing

(filled in from the CI runs)
