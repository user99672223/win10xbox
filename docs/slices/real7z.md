status: wip

7zr.exe (7-Zip 24.09, built from the pinned source) as a real guest: integration.real7z (a/t/l/x round trip) and
integration.real7z_bench (b -mmt1). First run failed with STATUS_DLL_NOT_FOUND (0xc0000135) while loading the import
chain of oleaut32/ole32/shlwapi; shcore and coml2 added to wine-pe/dlls.txt.
