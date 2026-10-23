status: wip

kernel32 conformance slice: wine-pe/tests/kernel32.sh, wine-pe/tests/kernel32.baseline, integration.winetest_kernel32 (label wine_conformance).

The baseline holds on linux-cross for 2ec9d69 (run 39878161631: integration.winetest_kernel32 passed in 265 s, ctest gate
"nothing skipped, nothing missing"). The per-unit table is filled from the kernel32.tsv that the linux-cross-bin artifact now carries.
