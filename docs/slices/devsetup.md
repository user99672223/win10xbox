status: ready (one open item: delete the `push:` trigger of .github/workflows/dev-setup-check.yml before merging; not done here, the 2-code-push budget was spent)
# Slice `-devsetup`: one command to a green native Windows build for agents
1. `tools/dev/setup-windows-agents.ps1` (PS 5.1): tools via winget/npm, VS components via vswhere + `setup.exe modify`, gh auth, clone, MSVC env, prep, configure, build, ctest, gate, `.claude/settings.local.json`.
2. `tools/dev/prepare-windows.ps1 -Phase Pre|Post` + `tools/dev/build-ntdll-guest.sh`: the ci.yml windows job's prep steps, cached in `%LOCALAPPDATA%\win10xbox\cache` (the tools/ci scripts get `RUNNER_TEMP` pointed at the cache).
3. `.github/workflows/dev-setup-check.yml`: windows-latest, empty dir, script fetched from the branch with `gh api`, `-SkipInstall`, under Windows PowerShell 5.1.
4. Docs: `docs/dev/WINDOWS-AGENTS.md`. No core/ or CMake change.
5. Deviations from the brief: prep is two phases (guest programs need the build tree; ci.yml order); `pwsh` is installed too; the ntdll conformance guest is built (the gate requires winetest_ntdll); the deny rules use `*` wildcard syntax.
6. Cannot be proved by the runner: winget installs, the VS installer `modify`, elevation, `gh auth login --web`, Windows 11 (the runner is Server 2022/2025), a first-time `claude` login, laptop timings.

7. Evidence, code head 4ef2fa8: dev-setup-check https://github.com/user99672223/win10xbox/actions/runs/40034519677 (attempt 2 green, 14 min script time on windows-latest, Windows PowerShell 5.1, VS 18 Enterprise): `ctest: 104 tests, 104 passed, 0 failed, 0 skipped`, gate green incl. winetest_ntdll. ci https://github.com/user99672223/win10xbox/actions/runs/40034519665 (3 jobs green).
8. Attempt 1 of that run failed only integration.guest_window_hooks (`SetTimer 50 ms cadence`, shortest interval 29.8 ms, want >= 40): a timing flake of the test on a loaded runner, not of the setup; passed on rerun. First push failed on a PowerShell parse error (`$name:`), fixed in 4ef2fa8.
