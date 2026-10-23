status: wip

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
