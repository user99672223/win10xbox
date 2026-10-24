status: wip
owner: unassigned

# -unload-slist: unloading a guest/builtin image leaves a host SList entry pointing into the freed image

Found by `-d3dsample` round 3 (docs/slices/d3dsample.md). Not part of that slice.

## First error

In the Debug build of the d3dsample fixture (ci run 40133570491 @ 3046eba, job windows Debug), after
`CryptAcquireContext(PROV_RSA_AES)` fails (docs/slices/crypto-rsaaes.md) the layer unmaps rsaenh.dll, crypt32.dll, ncrypt.dll and
bcrypt.dll. The next `InitializeCriticalSectionEx` in the process (called from `D3DCompileFromFile`, i.e. the host's
`RtlInitializeCriticalSectionEx`) raises 0xc0000005 inside the host `ntdll!ExpInterlockedPopEntrySListFault`, reading an address
inside the just-freed crypt32 image (crypt32 base 0x...ef30000, fault address 0x...ef94da0 = base + 0x64da0).

Likely cause (not proven): the host ntdll keeps a lock-free free list (an SList) of cached `RTL_CRITICAL_SECTION_DEBUG` records.
A DLL's static critical section was initialized at load and deleted at DllMain(DETACH) (`RtlDeleteCriticalSection` pushes its
debug record onto that SList; the record lives in the DLL's image data). The layer then frees the image without the host ever
popping that record, so the SList head still points into freed memory; the next `RtlInitializeCriticalSectionEx` pops it and
dereferences the freed page.

## Reproducer (to build)

A guest DLL with a static `CRITICAL_SECTION` (`InitializeCriticalSection` in `DllMain(PROCESS_ATTACH)`,
`DeleteCriticalSection` in `DETACH`), loaded then freed by a guest EXE (`LoadLibrary`, `FreeLibrary`), after which the EXE calls
`InitializeCriticalSectionEx` on a local critsec (several times). Expected: no fault. Add `tests/guest/unload_slist.c` plus
`tests/guest/unload_slist_dll.c`, registered as an integration test (Windows jobs; machine-native-d3d). Also try with the DLL's
critsec never deleted (no push) to confirm that the delete is the trigger, and with `RtlInitializeCriticalSectionEx` called
before the unload (an entry already popped).

## Next steps

* Find where the layer unmaps an image (grep the unload path in core/user, `FreeLibrary` / `LdrUnloadDll` handling) and, before
  freeing, make the host forget any pointers into it: drain the host SList (pop entries and re-push only those outside the
  image), or never free images that registered critical-section debug records, or keep the image mapped (leak it) as a first fix.
* Alternatively initialize the DLL's critical sections with `RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO` (no debug record, no SList).
* Native control: on Windows the loader runs the same DllMain detach and the SList entry is part of the process heap, not the image,
  so it never dangles: the defect is in how the layer's images are placed/freed.
