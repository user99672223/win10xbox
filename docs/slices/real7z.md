status: ready

7zr.exe (7-Zip 24.09, built from the pinned source) as a real guest: integration.real7z (a/t/l/x round trip) and
integration.real7z_bench (b -mmt1). First run failed with STATUS_DLL_NOT_FOUND (0xc0000135) while loading the import
chain of oleaut32/ole32/shlwapi; shcore and coml2 added to wine-pe/dlls.txt.

Second run (08bc5a3): the first `7zr a -mmt=4` died with an access violation at the exit of its first worker thread:
LdrShutdownThread -> heap_thread_detach() does RtlEnterCriticalSection(&process_heap->cs) on Wine ntdll's static
process_heap, which is NULL because the process heap is the host's (patch 0001). Fix: wine-pe/patches/0003-heap-thread-detach-host-heap.patch
(returns early when Wine's ntdll owns no heap; same patch file as the net slice, so the merge is identical on both sides).
The retrace now deletes the half-written archive first.

Third run (4ab6912, 39880605699): a, t, l and the bench pass through the layer; all extracted files compare equal, but
`7zr x` exits 2: "Cannot set file attribute : Access denied" for every extracted item. Wine's SetFileAttributesW opens
the file with SYNCHRONIZE only and calls NtSetInformationFile(FileBasicInformation); the host refuses that on a handle
without FILE_WRITE_ATTRIBUTES (c0000022). Fix in core/dispatch/nt_file.cpp: on that refusal the information is set
through a FILE_WRITE_ATTRIBUTES reopen of the same file (relative open of the handle, as ReOpenFile does).

## Result (head ff999a2)

ci.yml run https://github.com/user99672223/win10xbox/actions/runs/39885969605 : linux-cross, windows (Debug) and windows
(RelWithDebInfo) green; "ctest gate: 50 registered, 50 ran and passed, 0 DISABLED" (nothing skipped). wine-pe.yml for the
head (run 39885969579) green.

What runs through the layer: 7zr.exe 24.09 (x64, built from the pinned source by tools/ci/build_7zip.ps1), under
integration.real7z and integration.real7z_bench. Lines copied from the job log of 4ab6912 (same binary and command lines):

    7zr a C:\work7z\lzma2.7z C:\work7z\in\* -r -y -mx=5 -m0=lzma2:d=64k -mmt=4 -> exit 0
    7-Zip (r) 24.09 (x64) : Igor Pavlov : Public domain : 2024-11-29
    Scanning the drive:
    1 folder, 5 files, 4304537 bytes (4204 KiB)
    Archive size: 1152570 bytes (1126 KiB)
    7zr t C:\work7z\lzma2.7z -mmt=4 -> exit 0   (Method = LZMA2:16, Solid = +, Blocks = 1)

then `7zr l`, `7zr x` (needs the FileBasicInformation fix below) with SHA-256 equality of all five extracted files for both
the LZMA2 and the LZMA (-mmt=4 match finder) archives, a `7zr t` of a non-archive that must exit non-zero, and
`7zr b -mmt1` (integration.real7z_bench, passes in about 30 s).

Syscalls changed: NtSetInformationFile (FileBasicInformation on a handle without FILE_WRITE_ATTRIBUTES is set through a
FILE_WRITE_ATTRIBUTES reopen; policy text and docs/SYSCALLS.md updated). Wine patch added:
wine-pe/patches/0003-heap-thread-detach-host-heap.patch (identical to the net slice's file).

DLLs added to wine-pe/dlls.txt: shcore, coml2.

Still failing: nothing in this slice. The first ci.yml run of 4ab6912 had linux-cross fail once in
integration.guest_window_input2 ("another thread's window: delivered on that thread", a Wine-side timing check not
touched by this slice); the rerun on ff999a2 passed.
