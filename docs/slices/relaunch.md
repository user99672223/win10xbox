status: wip

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
