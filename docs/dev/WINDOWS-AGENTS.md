# Native Windows setup for non-interactive Claude Code agents

Goal: on a Windows 11 laptop with Visual Studio 2022 installed, ONE command in an empty folder ends with a clone that
builds and passes the full native Windows ctest suite (the `windows` job of `.github/workflows/ci.yml`), ready for
`claude -p` agents.

## The one command

```powershell
mkdir win10xbox-dev; cd win10xbox-dev
iwr https://raw.githubusercontent.com/user99672223/win10xbox/claude/youthful-cerf-l7xxsb/tools/dev/setup-windows-agents.ps1 -OutFile setup-windows-agents.ps1
powershell -ExecutionPolicy Bypass -File .\setup-windows-agents.ps1
```

(The repository is public, so only the script is downloaded first; the script clones the rest into `.\win10xbox`.
A full bootstrap clone, `git clone -b claude/youthful-cerf-l7xxsb https://github.com/user99672223/win10xbox .\win10xbox-bootstrap`,
works as well but leaves a second clone behind.) Run it from an **elevated** PowerShell the first time if Visual Studio
lacks a component; otherwise a normal one is enough. It stops at the first unrecoverable failure and prints the exact next step.

Switches: `-SkipInstall` (never install; a missing prerequisite is a `[fail]` with the command to run), `-SkipBuild`
(stop after clone + prep + settings), `-WithUwp` (also require/add `Microsoft.VisualStudio.ComponentGroup.UWP.VC`, for the
console package), `-SkipWinetest` (no llvm-mingw/Wine ntdll guest; `integration.winetest_ntdll` is then not registered and
not gated), `-Branch`, `-Dir`, `-Config` (default `RelWithDebInfo`). Rerunning is safe: finished steps print `[ok]`.

## What it installs and does

1. Tools, by `winget install --id <id> -e --accept-source-agreements --accept-package-agreements`:
   `Git.Git`, `GitHub.cli`, `Kitware.CMake` (>= 3.25), `Ninja-build.Ninja`, `Python.Python.3.12` (>= 3.10),
   `OpenJS.NodeJS.LTS`, `Microsoft.PowerShell` (the `tools\ci` scripts are written for `pwsh`); the Claude Code CLI by
   `npm install -g @anthropic-ai/claude-code`. PATH is refreshed in the session after each install.
2. Visual Studio, by `vswhere`: requires `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` and a Windows 11 SDK
   (`...Windows11SDK.22621` or newer, or a standalone 10.0.22000+ SDK); missing ones are added with
   `setup.exe modify --installPath <vswhere path> --add <ids> --passive --norestart` (needs elevation; if not elevated the
   script prints the command and asks you to rerun elevated).
3. `gh auth status`; if not logged in, `gh auth login --web` (interactive, once, before any agent) and `gh auth setup-git`.
4. Clone `https://github.com/user99672223/win10xbox` (`claude/youthful-cerf-l7xxsb`, `--recurse-submodules`) into
   `.\win10xbox`, or `git pull --ff-only` + `git submodule update --init`; plus `third_party/zydis` recursively (as CI).
5. MSVC x64 environment (`vcvars64.bat` captured into the session), then `tools\dev\prepare-windows.ps1 -Phase Pre`:
   WDK km headers (`tools\ci\fetch_wdk_km.ps1`), the wine-pe `wine-pe-builtin` and `wine-pe-nls` artifacts
   (`tools\ci\fetch_wine_pe.py`: waits for a wine-pe run of the current commit, else newest successful run of the branch,
   then of the repository), Dear ImGui v1.93.0 (`WIN10XBOX_IMGUI_DIR`), llvm-mingw 20250910 and Wine's ntdll conformance
   guest (`tools\dev\build-ntdll-guest.sh` from Git's bash). Then
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DWIN10XBOX_DDK_INCLUDE=<wdk>`,
   `cmake --build build`, then `-Phase Post` (needs the build tree): `machine\import_windows_dlls.ps1` twice from this
   laptop's `System32`, and every `tools\ci\build_*.ps1` / `fetch_*.ps1` with CI's pins (7-Zip, SQLite, Lua, Python,
   Sysinternals, fastfetch, TCC, ninja, Chocolate Doom, PuTTY/Hello World, SDL2, curl, MinGit).
   Downloads and sources are cached under `%LOCALAPPDATA%\win10xbox\cache`.
6. `ctest --test-dir build --output-on-failure --timeout 300 --output-junit ctest-junit.xml` with `WIN10XBOX_DEBUG=+all`
   (as CI), then `python -I tools/ci/check_ctest_run.py --build build --junit ctest-junit.xml` with CI's
   `--require-label` set; prints `ctest: N tests, P passed, F failed, S skipped`. A skipped test fails the gate.
7. Writes `win10xbox\.claude\settings.local.json` if absent (allow/deny list below).

The wine-pe artifacts come from the branch being built when it has a successful wine-pe run (CI parity), else from the
newest successful run anywhere in the repository; they expire after 30 days, so a stale repository needs
`gh workflow run wine-pe.yml` first.

First-run time (estimate, depends on the laptop and network): winget installs 5 to 15 min, a VS `modify` 10 to 30 min,
the MSVC build 20 to 60 min, the ntdll guest and the real-program builds 15 to 30 min, ctest 15 to 40 min. A rerun with
everything in place is a few minutes plus ctest. The GitHub runner's measured times are in `docs/slices/devsetup.md`.

## Running a one-shot agent

Log in once (`claude`, or set `ANTHROPIC_API_KEY`), then:

```powershell
cd win10xbox
claude -p "<brief>"
```

Unlisted tools are refused in `-p` mode; the allow list covers Read/Edit/Write/Glob/Grep and
`git gh cmake ctest ninja python pwsh powershell dumpbin llvm-objdump`. The deny list blocks `git push` to
`claude/youthful-cerf-l7xxsb` and `main`, force pushes, `rm` and `Remove-Item`.

**Limits of pattern matching, honestly:** Bash rules match the command string, not what it does. `git push origin HEAD:main`,
`git push origin HEAD:refs/heads/main`, a push through `pwsh -c "git push ..."`, or a script that pushes are not caught by
the `git push` rules, and `Bash(pwsh:*)`/`Bash(python:*)` can delete or write anything the user can. `rm`/`Remove-Item` are
denied outright because a path ("only outside the clone") cannot be expressed; an agent can still delete through
`pwsh -c`, `python`, or `git clean`. The settings reduce accidents; they are not a sandbox. The real guards are the brief's
rules (below), `git diff --stat` review before merging, and GitHub branch protection on `main` (turn it on).

## Rules for every agent

* Push only to `claude/youthful-cerf-l7xxsb-<slice>` branches, from a worktree: `git fetch origin && git worktree add
  ..\wt-<slice> -b claude/youthful-cerf-l7xxsb-<slice> origin/claude/youthful-cerf-l7xxsb`. Never touch `main`, never
  force-push.
* Stage by explicit path only (`git add tools/x.ps1`), never `git add -A` / `git add .` (docs/COORDINATION.md rule 8).
* Two code pushes per slice, then `status: blocked` with the first error. Docs-only pushes carry `[skip ci]`; a push
  carrying code must not end with a `[skip ci]` commit.
* Grep before read; `Read` with offset/limit; CI logs only through
  `gh run view <id> --log-failed | grep -E 'error|fail|not found|missing' | head -40`; report in at most 12 lines.
* Commit trailer, exactly the two lines the orchestrator gives (`Co-Authored-By: ...` and `Claude-Session: ...`).

## Brief template (copy the shape of `docs/slices/*.md` handoffs; under 40 lines)

```
Slice `-<slice>` of win10xbox. Worktree: git fetch origin && git worktree add ..\wt-<slice> -b claude/youthful-cerf-l7xxsb-<slice> origin/claude/youthful-cerf-l7xxsb
Read first (grep-first, offset/limit): docs/COORDINATION.md "Agent cost rules", docs/TESTING.md, <the files the slice names>.
Files: <paths to change>.
Contract: <what must be true afterwards, observable: exit codes, output lines, the test names>.
Test: ctest --test-dir build -R <name> --output-on-failure | findstr /R "error FAILED passed"  (local, native Windows);
      then the full suite + python -I tools/ci/check_ctest_run.py --build build --junit ctest-junit.xml.
Stop condition: <test green and gate green> -> write the 10-line handoff at the top of docs/slices/<slice>.md
      (status: line 1, what changed, what is untested, exact commands), push [skip ci] for docs only.
Budget: 2 code pushes, then status: blocked with the exact first error. Stage by explicit path only (never git add -A / .).
Never touch main. Never send repository data anywhere but GitHub. Push only to claude/youthful-cerf-l7xxsb-<slice>.
Commit trailer, exactly:
Co-Authored-By: <as given>
Claude-Session: <as given>
Report <= 12 lines: status first, head sha, run URLs, counts, what is unproven.
```
