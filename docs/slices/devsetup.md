status: wip (first push; dev-setup-check not yet run)
# Slice `-devsetup`: one command to a green native Windows build for agents
1. `tools/dev/setup-windows-agents.ps1` (PS 5.1): tools via winget/npm, VS components via vswhere + `setup.exe modify`, gh auth, clone, MSVC env, prep, configure, build, ctest, gate, `.claude/settings.local.json`.
2. `tools/dev/prepare-windows.ps1 -Phase Pre|Post` + `tools/dev/build-ntdll-guest.sh`: the ci.yml windows job's prep steps, cached in `%LOCALAPPDATA%\win10xbox\cache` (the tools/ci scripts get `RUNNER_TEMP` pointed at the cache).
3. `.github/workflows/dev-setup-check.yml`: windows-latest, empty dir, script fetched from the branch with `gh api`, `-SkipInstall`, under Windows PowerShell 5.1.
4. Docs: `docs/dev/WINDOWS-AGENTS.md`. No core/ or CMake change.
5. Deviations from the brief: prep is two phases (guest programs need the build tree; ci.yml order); `pwsh` is installed too; the ntdll conformance guest is built (the gate requires winetest_ntdll); the deny rules use `*` wildcard syntax.
6. Cannot be proved by the runner: winget installs, the VS installer `modify`, elevation, `gh auth login --web`, Windows 11 (the runner is Server 2022/2025), a first-time `claude` login, laptop timings.
7. Evidence: see the run URLs below once filled in.
