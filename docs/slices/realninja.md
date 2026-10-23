status: wip

# Slice: realninja

ninja 1.12.1 (the official prebuilt `ninja-win.zip`, SHA-256 pinned and verified on every run by
`tools/ci/fetch_ninja.ps1`, cached with `actions/cache`) is a real guest program that is not ours and that we do not
build. It runs on the shared machine folder next to the Lua build, gated in ci.yml's windows jobs:

* `integration.ninja_version`: `C:\guest\ninja.exe --version`, exit 0, prints `1.12.1`.
* `integration.ninja_targets`: `ninja -C C:\guest -f ninja_build.ninja -t targets all`, exit 0, lists the three outputs.
* `integration.ninja_build`: the same build.ninja with two rules (`copy`, `concat`) and three edges, exit 0,
  `[1/3]`..`[3/3]` on stdout, `C:\guest\ninja_out\ab.txt` exists afterwards. ninja on Windows does not prepend
  `cmd /c` (src/subprocess-win32.cc) and the layer has no cmd.exe, so the rules run `C:\guest\lua.exe
  C:\guest\ninja_tool.lua` (tests/guest/ninja).
* `integration.ninja_noop`: a second run prints `ninja: no work to do.`

Status: first push is the measurement; nothing measured yet.
