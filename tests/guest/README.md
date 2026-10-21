# Guest test programs

Real Windows x64 PE files, built with the same toolchain as the rest of the
repo (MSVC `/MT` on CI, llvm-mingw `-static` locally) and written to
`<build>/bin/guest/`. They do **not** link against `win10xbox_core`: they stand
in for the unmodified programs the layer will eventually load.

| Target | File | What it is for |
|--------|------|----------------|
| `guest_hello` | `hello.c` | Prints `hello from guest` and returns **42**. First end-to-end target for the process bootstrap (exit code propagation). |
| `guest_cpuid` | `cpuid.c` | Prints `cpuid.1.ecx=<hex>` and `vendor=<12 chars>`, returns 0. Shows what CPUID patching makes the guest see (hypervisor bit 31 of ECX, vendor string). |
| `guest_dll` | `dll.c` | `int add(int,int)`, `unsigned long long read_tsc(void)` (`__rdtsc`), `unsigned int cpuid_ecx1(void)` (`__cpuidex`), plus a TLS directory (a `__declspec(thread)` variable and a TLS callback that bumps `g_tls_callback_hits`), exported as `tls_callback_hits()`, `tls_callback_reason()` and `tls_value()`. Used by `tests/unit/test_loader.cpp` to test the mapper (relocation, exports, protections, relocated TLS callback array) and the scanner (the cpuid/rdtsc sites). |

`ctest` smoke-runs `guest_hello` and `guest_cpuid` directly (Wine locally).
`guest_hello` intentionally exits with 42, so its test passes on the output
regex, not on the exit code. The unit test locates the DLL relative to its own
executable: `<dir of test_core.exe>\guest\guest_dll.dll`.

The DLL is never run through `DllMain`/TLS by the loader tests - the layer does
not do that (Wine's `loader.c` does). The test only maps it, patches it and
calls exports.
