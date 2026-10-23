status: wip (implemented, not yet run on CI)

Handoff (implement agent -> CI agent):
* What: Chocolate Doom is in the .msix and listed by the launcher. msix job: cache + `tools/ci/fetch_doom.ps1 -Out $RUNNER_TEMP/doom-guest`,
  Configure passes `-DHOST_UWP_EXTRA_GUEST_DIR`; stage.cmake copies it plus `host-uwp/payload/guest/*` flat into `payload/guest`.
* Sidecars (`host-uwp/guest_sidecar.*`, read in host.cpp next to the exe): `.args`, `.env` (per launch, restored after), `.title`, `.launch`. Format: HANDOFF.md "Launcher".
* Shipped: chocolate-doom.args/.env/.title, doom-timedemo.launch -> picker shows "Doom" and "Doom (timedemo)".
* Local: host_uwp_selftest compiled with g++ (parser files only) and passes, 0 failed. host.cpp/launcher.cpp/stage.cmake edits are NOT compiled (UWP).
* CI agent checks: (1) `msix` job green (both configs), the new fetch step ok; (2) "Verify package contents" / "Assert the package" pass: chocolate-doom.exe, doom1.wad, SDL2.dll, sidecars in payload/guest;
  (3) `launcher` job still green (autoselect hello_cui.exe); (4) package size in the step summary, well under 500 MB (expect about +10 MB); (5) host_uwp.selftest passes on MSVC.
* Compile risks: host.cpp (EnvScope, add_entry lambda,), launcher.h needs <utility>.
* Not run anywhere: picking Doom on a console, the pad, `-fullscreen` under the software renderer; Doom args use a relative iwad (found next to the exe).
