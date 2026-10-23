status: wip

# Slice probe: bad caller pointers are STATUS_ACCESS_VIOLATION, not a guest crash

Run: (pending)

On Windows a syscall that is handed a bad pointer returns `STATUS_ACCESS_VIOLATION` (or
`STATUS_DATATYPE_MISALIGNMENT` where it probes alignment); it does not fault. PassThrough syscalls get that from the
host kernel. A Translate/Emulate implementation is ordinary user-mode code, so the same pointer used to fault inside the
layer and end the guest (`ntdll_test.exe` died with `status c0000005` in `time`, `info`, `virtual`, `file`).

## Mechanism (core/include/win10xbox/probe.h, core/dispatch/probe.cpp, core/asm/probe.{S,asm})

* `ProbeRead` / `ProbeWrite` are `ProbeForRead` / `ProbeForWrite`: length 0 is accepted without looking at the pointer;
  otherwise the range must lie below `MmUserProbeAddress` (0x7FFFFFFF0000), be aligned (`STATUS_DATATYPE_MISALIGNMENT`),
  and every page is really touched: a read, or an atomic `lock or byte [p], 0` for write (needs write access, changes
  nothing, cannot lose a concurrent store). Nothing is written to a buffer that failed the probe.
* `CopyFromUser` / `CopyToUser` / `Load` / `Store` / `StoreIfPresent` are the accesses themselves, done under the same
  guard, so a page that disappears between probe and use (another guest thread) is reported the same way.
  `CaptureString` and `CaptureObjectAttributes` copy a `UNICODE_STRING` / `OBJECT_ATTRIBUTES` and the name it points to
  once (`Length != sizeof(OBJECT_ATTRIBUTES)` is `STATUS_INVALID_PARAMETER`, a misaligned name buffer
  `STATUS_DATATYPE_MISALIGNMENT`, as `om.c` expects).
* The guard is not SEH. clang-mingw accepts `__try` with `-fms-extensions` and then emits no handler (checked: the
  unwind info of the function has no handler and the fault is unhandled), so SEH would differ per toolchain and the
  MSVC-only half could not be tested on linux-cross. It is the Linux kernel's exception table instead: every access to
  caller memory is made by one of the leaf routines in `core/asm/probe.{S,asm}` (volatile registers only, no stack). A
  vectored handler, installed first and once (`dispatch::Initialize`, or lazily by the first probe), recognises a fault
  whose `rip` lies inside those routines and resumes at their common exit with the NTSTATUS in `rax`: access violation,
  the I/O status of an in-page error, or `STATUS_GUARD_PAGE_VIOLATION`. A fault anywhere else is not touched. Nothing is
  unwound, so a guarded access holds no lock and owns nothing: implementations copy in, work, copy out. An MSVC
  incremental-link `jmp` thunk in front of the routines is followed when the range is computed.
* No function-local statics, no thread-local state: the handler is reachable from either TEB view.

## Syscall implementations guarded

Every Emulate/Translate implementation that reads or writes caller memory:

* clock: NtQueryPerformanceCounter, NtQuerySystemTime, NtSetSystemTime, NtDelayExecution, NtQueryTimerResolution,
  NtSetTimerResolution, NtQuerySystemInformation(SystemTimeOfDayInformation), NtConvertBetweenAuxiliaryCounterAndPerformanceCounter
  (now Emulate: probes, then `STATUS_NOT_SUPPORTED`, as a machine without an auxiliary counter answers).
  NtSetTimerResolution also keeps whether this process holds a request (`STATUS_TIMER_RESOLUTION_NOT_SET` otherwise).
* registry (26 entries): NtOpenKey(Ex/Transacted/TransactedEx), NtCreateKey, NtDeleteValueKey, NtEnumerateKey,
  NtEnumerateValueKey, NtQueryKey, NtQueryValueKey, NtQueryMultipleValueKey, NtRenameKey, NtSetValueKey,
  NtSetInformationKey, NtNotifyChangeKey/MultipleKeys (the IO_STATUS_BLOCK is written later, from another thread),
  NtLoadKey/Ex/2, NtUnloadKey, NtQueryOpenSubKeys. Attributes, names and data are captured before the registry lock is
  taken. A failed open leaves the handle NULL.
* token (15 entries): NtOpenProcessToken(Ex), NtOpenThreadToken(Ex), NtQueryInformationToken, NtSetInformationToken,
  NtAdjustPrivilegesToken, NtAdjustGroupsToken, NtDuplicateToken, NtFilterToken, NtPrivilegeCheck, NtAccessCheck (the
  security descriptor is flattened into a copy), NtCompareTokens, NtCreateLowBoxToken, NtQuerySecurityObject.
* file/section/process translators: every call that goes through `TranslateOA` (NtCreateFile, NtOpenFile,
  NtQueryAttributesFile, NtQueryFullAttributesFile, NtDeleteFile), NtOpenDirectoryObject, NtOpenSymbolicLinkObject,
  NtQueryInformationFile, NtSetInformationFile (rename/link target), NtLockFile/NtUnlockFile, NtCreateSection(Ex),
  NtMapViewOfSection(Ex), NtQuerySection, NtQueryVirtualMemory (incl. the Wine unix-lib classes),
  NtQueryInformationProcess, NtQueryInformationThread, NtQuerySystemInformation, NtDuplicateObject, NtGetNextThread,
  NtRaiseHardError, NtContinue/NtContinueEx (the CONTEXT is probed, 16-byte aligned, before the TEB view changes),
  NtCreateUserProcess (parameter block, attribute list, strings, outputs), NtGetNlsSectionPtr, NtInitializeNlsFiles,
  NtQueryDefaultLocale, NtQueryDefaultUILanguage, NtQueryObject (ObjectNameInformation of a file handle),
  NtCreateNamedPipeFile (the time-out).
* No caller memory is touched (nothing to guard): the privileged refusals (nt_privileged.cpp), NtCreateProcessEx,
  NtCreateThread, NtCreateDebugObject, NtDebugActiveProcess, NtTerminate*, NtSetContextThread/NtRaiseException/
  NtCreateThreadEx (pointers go to the host kernel unread), NtClose, NtSet*Locale.
* Not done: the win32u surface of core/user (NtUser*/NtGdi*, ids 0x1000+), whose pointers come from Wine's own
  user32/gdi32 only. NULLs that existing unit tests pin as STATUS_INVALID_PARAMETER (NtQueryDefaultLocale(NULL),
  NtLoadKey2 with no source file) were left as they are.

NULL stays NULL-accepting where Windows accepts it: QPC's frequency, NtSetSystemTime's old time, NtDelayExecution's
timeout (infinite), NtCreateKey's disposition and class, NtAdjustPrivilegesToken's previous state and return length,
NtQuerySection's return length, section/file object attributes.

## Tests

* tests/unit/test_probe.cpp: the primitives (read-only, no-access, wrap-around, alignment, zero length, many threads
  faulting at once), the captures, and bad pointers to the clock (NtQueryPerformanceCounter...), registry (NtOpenKey,
  NtCreateKey, NtSetValueKey), token (NtOpenProcessToken, NtQueryInformationToken, NtAdjustPrivilegesToken,
  NtAccessCheck) and the file translator, each next to the NULL that stays accepted.
* Wine ntdll `time` joins `wine-pe/tests/ntdll.allowlist` (floor 5000; 10657 assertions, 0 failures under Wine 9 here)
  and `tests/wine-tests/baseline.cmake` (`run 0`).

## Still failing

* ntdll `reg` is not on the allowlist: its probing failures are fixed (`reg.c:235-266, 390-411` no longer report an
  access violation as `c000000d`), but it still loops forever at `reg.c:1987` (`NtEnumerateKey` answers
  `STATUS_INVALID_PARAMETER`) and fails ~130 registry-semantics assertions that are not about pointers.
* `info`, `virtual`, `file`, `om` still end early; see the table in docs/slices/wt-ntdll.md. What is left there is not
  a bad pointer reaching a Translate/Emulate implementation.
