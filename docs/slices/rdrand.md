status: ready (head 39e4e59; ci https://github.com/user99672223/win10xbox/actions/runs/40000187575 host-uwp /runs/40000194945 host-win32-smoke /runs/40000243207 host-win32-appcontainer /runs/40000247847)
# Slice: rdrand (D18 RDRAND emulation)

HANDOFF (status: ready)
1. Symptom: on the Xbox Series S guest_identity.exe died 0xc000001d at +0x89d0 (`rdrand eax`), nothing printed.
2. Root cause (from code; the console was not reachable): the #UD handler existed but nothing ever ran it on a host with RDRAND, and its
   first BCryptGenRandom call happened inside the handler, after the PEB take-over, where the RNG provider's lazy load goes through the
   guest's loader state. The handler order was already right (core AddVectoredExceptionHandler(1), crash capture order 0 in both hosts).
3. Fix (core/dispatch/cpu_sites.cpp): InstallRandomInstructionEmulation primes BCryptGenRandom before the take-over and refuses to install
   (ERR "BCryptGenRandom failed while priming") if it cannot, so a missing generator is visible at start-up, not as a fault.
4. Test mode WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1 (latched in Launch beside TEST_LEAK_GUEST_ACTCTX): the scanner (ScanOptions::trap_random)
   rewrites every register-form rdrand/rdseed to `0F 0B` (ud2), keeps the original bytes (loader::RandomSiteBytes), and UdVeh decodes
   those, writes the register, sets CF, advances Rip by the original length. Emulation is also installed when this is set.
5. Tests: integration.guest_rdrand (tests/guest/rdrand.c, prints `rdrand: ok n=16`, exit 42) and integration.guest_identity both run with
   the env var; unit: dispatch_rdrand_decoder_sizes (r16/r32/r64/REX.B), dispatch_rdrand_forced_trap (scanner + VEH end to end).
6. Local: ctest -R 'rdrand|identity' green under Wine; unit.core green. Not run: host-win32 (Windows), host-uwp, the console.
7. CI agent must check: integration.guest_rdrand and guest_identity on host-win32 and host-uwp (guest_rdrand is in HOST_UWP_GUESTS and the
   bundle's expected list; the UWP/Windows runner jobs must export WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1 for them if they run guests outside ctest).
8. On the console: run guest_identity.exe and guest_rdrand.exe WITHOUT the env var; a remaining 0xc000001d means the cause was not the
   priming, and the next suspect is the handler never being reached (log "RDRAND/RDSEED #UD emulation installed" at trace level process).
9. Wine caveat: Wine's ntdll dispatch is not the console's; a pass here proves the decoder, scanner and handler logic, not the console.
10. Nothing else changed: no config field, no new dependency.
