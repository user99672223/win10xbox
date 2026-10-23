status: blocked (2 code pushes used). head a618e7e. Run 40001566624 (host-uwp): msix ok, run ok, launcher two-run assert ok (run=1 hello_cui, run=2 guest_hello, restart works after a guest exits), autoselect-all fails.
Blocker: the chain stops at the first TIMEOUT program (guest_cursor.exe, 5th): the restart requested while the timed-out guest is still running fails with `RequestRestartAsync failed (HRESULT 0x800736b2)` (status.txt `launcher.restart`), so report.txt has 5 blocks instead of 32. Suspected cause: WinRT call from the worker thread while the guest owns the PEB/TEB, or a restart refused for a process with a live guest. Not tried: issue the restart from the UI thread (CoreDispatcher), or end the guest threads first. Fixed: main.cpp lacked launch_config.h and used a non-existent AppRestartFailureReason::None (2cdc221); os_build=unknown after a guest loaded (now resolved before any guest, a618e7e).
Runs: https://github.com/user99672223/win10xbox/actions/runs/39999860417 (2cdc221), https://github.com/user99672223/win10xbox/actions/runs/40001566624 (a618e7e), ci https://github.com/user99672223/win10xbox/actions/runs/40001568579

# Slice `-relaunch`: run program after program from the picker

Handoff (implement agent, not built or run on Windows; selftest built and green with g++ here):
1. Why: D1 allows one guest per host process; a second launch from the picker failed (0xc0000001). Fix = restart the app.
2. After the result screen is dismissed the worker calls `RequestRestartAsync(L"picker run=<N> select=<name>")`
   (`host-uwp/main.cpp` `RequestAppRestart`, wired by `Host::SetRestart`); `OnActivated` parses it (`ParseRestartArgs`).
3. The new process shows the picker with the cursor on `select`; on failure the `AppRestartFailureReason` is on the result screen.
4. `launch.json` `"autoselect"` takes a list; run k = entry k; `launcher_result.txt` gets one `run=N` block per run (append).
5. D1 message names the restart. Added: "Run every program (report)" (picker top entry / `"autoselect": "all"`):
   restart chain `all=K`, `LocalState\report.txt` blocks + `<name>.bmp`, header, TV summary; HANDOFF "Launcher" has it.
6. Untested on Windows: everything in main.cpp/host.cpp/launcher.cpp (compiled only by CI), and RequestRestartAsync itself.
7. CI agent must check, branch `claude/youthful-cerf-l7xxsb-relaunch`, dispatch `host-uwp.yml` by hand:
   `launcher` job: two blocks (run=1 hello_cui.exe, run=2 guest_hello.exe), each ran=yes, exit_code=42; the job polls for run=2
   because `uwp_activate` may report the first process ended. If run=2 is missing read `trace.log` for "launcher: restart".
8. `run` job unchanged (window.exe, RESULT: PASS). `host_uwp.selftest` covers autoselect list/all, restart args, report parse.
   Same `launcher` job, new steps: autoselect all, 20 s each: every bundled program has a block, 5 known-good are PASS.
9. Compile risks to look at first: `RequestAppRestart` (C++/WinRT enum/`ErrorCode`), `OnActivated` launch-args cast.
10. Not proven: gamepad flow and the restart on the Series S; see docs/HANDOFF.md "Launcher". Budget: two pushes, then blocked.
