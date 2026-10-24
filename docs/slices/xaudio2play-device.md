status: ready
owner: B (A lifts it)
<!-- ci result
ci.yml green on all three jobs for 7d760ae: https://github.com/user99672223/win10xbox/actions/runs/40047932075 (117/117 on windows Debug)
wine-pe.yml green: https://github.com/user99672223/win10xbox/actions/runs/40047932078
Device: VB-CABLE pack 43 installed on the runner (Scream rejected: signer trust prompt). Gate WIN10XBOX_CI_AUDIO_ENDPOINT=1 passes:
guest_wasapi, guest_xaudio2, guest_xaudio2_play print "RESULT played" through the layer; integration.guest_xaudio2_native passes.
Cause of the native failure: the runner's xaudio2_9.dll imports avrt.dll (AvSetMmThreadCharacteristicsA, AvRevertMmThreadCharacteristics,
AvSetMmThreadPriority), absent from the machine folder. Fix: avrt added to wine-pe/dlls.txt (Wine builtin). machine/guest_dlls_d3d.txt unchanged.
Diagnostic: Xl_NtOpenFile WARNs "library <name>.dll not found" when a .dll open fails; the CI step prints the native xaudio2_9 import table.
-->
<!-- handoff
1. Changed: tools/ci/install_virtual_audio.ps1 (new) installs a virtual render endpoint on the Windows runners; ci.yml step "Install a virtual
   audio render endpoint" runs it before the probe. It counts only when guest_wasapi run directly on the host exits 42, then exports
   WIN10XBOX_CI_AUDIO_ENDPOINT=1 (and ..._DEVICE=<name>); with no device it fails the job with each attempt's exact error.
2. Device, in order: Scream 4.0 (https://github.com/duncanthrax/scream/releases/download/4.0/Scream4.0.zip, sha256
   fa33e25f9a46c61e4e0cd83362c51c3d2a45c6fe4091aad7507e240e40f1a520; devcon install Scream.inf *Scream), then VB-CABLE pack 43
   (https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip, sha256 66fd0a4d9f4896ff41632b7e3d53892c085c4561f53e8ae8d0f0bc10eedd1cdd;
   VBCABLE_Setup_x64.exe -i -h). Neither project publishes a checksum: the pin is the hash of the file as downloaded on 2026-10-23.
3. Gate: with the env var, tests/integration/audio_test.cmake fails unless the host exits 42, so integration.guest_wasapi / guest_xaudio2 need
   "RESULT played" and exit 42 on both Windows jobs (layer == host, all steps printed). Linux (PulseAudio null sink) is unchanged.
4. integration.guest_xaudio2_native (new, machine-native-d3d, not registered when cross-compiling): the runner's native xaudio2_9.dll
   (machine/guest_dlls_d3d.txt, "xaudio2_9": "native") over Wine's mmdevapi; exit 42, OnBufferEnd + BuffersQueued lines, trace shows the native
   DLL mapped from the machine folder. run_guest.cmake got -DSKIP_UNLESS_ENV (SKIP 77 unless the env var is 1; CI sets it). It replaces the
   informational "Measure - native xaudio2_9" step.
5. Console: tests/guest/expected_exit.txt already listed guest_wasapi/guest_xaudio2 as 42=endpoint first; 43=no-endpoint stays listed because
   host-uwp's own runner job (no endpoint) asserts the same table; the comment now says 43 on the console is a regression. host-uwp runs on
   main only, so the console verdict arrives with the next package's report.
6. Local: configure OK, YAML parses; integration.guest_wasapi / guest_xaudio2 fail locally only because the local builtins lack
   mmdevapi.dll / xaudio2_9.dll (same before the change); guest_hello/seh/cpuid pass through the edited run_guest.cmake.
7. Untested on Windows (the CI loop measures it): that either driver installs on the hosted image (signing, reboot), that guest_wasapi then
   exits 42, the native xaudio2_9 import chain on the layer, the trace regex for the native DLL, OnBufferEnd time and BuffersQueued line.
8. Commands: ./tools/ci/install_virtual_audio.ps1 -Probe build/bin/guest/guest_wasapi.exe (admin pwsh);
   ctest --test-dir build -R 'guest_wasapi|guest_xaudio2' --output-on-failure with WIN10XBOX_CI_AUDIO_ENDPOINT=1.
9. If neither installs: record the exact error here (job log, step "Install a virtual audio render endpoint"), set status: blocked.
-->

# Slice: xaudio2play-device (a real buffer through XAudio2 2.9 to a virtual endpoint)

Run https://github.com/user99672223/win10xbox/actions/runs/40045351364 (head 1b1b961): linux-cross green; both Windows jobs 114/115,
the one failure is integration.guest_xaudio2_native. Everything else of this slice is measured and green:

- Device installed: VB-CABLE driver pack 43 (pinned URL + sha256 above, hash verified in the job), `VBCABLE_Setup_x64.exe -i -h`, exit 0, then
  AudioEndpointBuilder/Audiosrv restarted. The runner then enumerates "CABLE Input (VB-Audio Virtual Cable)" (render) and
  "CABLE Output"; guest_wasapi run directly on the host exits 42 (`wasapi: RESULT played`, mix 2 ch 48000 Hz, drained=1).
- Scream 4.0 does not install on the hosted runner: its catalog signer (CN=Tom Kistner) has status UnknownError and
  `devcon install Scream.inf *Scream` waits for a driver-trust prompt forever (the first run hung for an hour; the script now kills
  every install process after a time limit, so the fallback to VB-CABLE runs after 120 s).
- Gate (WIN10XBOX_CI_AUDIO_ENDPOINT=1 from the install step): integration.guest_wasapi, guest_xaudio2, guest_xaudio2_play and A's
  guest_xaudio2_play_noendpoint all pass on Windows Debug and RelWithDebInfo. The host-direct xaudio2 probe printed:
  `xaudio2: OnBufferEnd received after about 190 ms; OnStreamEnd received; while playing: BuffersQueued max 1, SamplesPlayed max 9120`, `RESULT played`.
- NOT passing: integration.guest_xaudio2_native, `xaudio2: FAILED LoadLibrary(xaudio2_9.dll) hr=0x8007007e` (module not found). The layer
  maps the native xaudio2_9.dll, then Wine's mmdevapi, oleaut32, version, shcore and kernel.appcore, then a further import of
  xaudio2_9.dll is not found. The loader trace does not name it. Likely avrt.dll (native XAudio2 2.9 imports it; it is neither in
  wine-pe/dlls.txt nor in machine/guest_dlls_d3d.txt) but this is unverified. Next step: run tools/game/dump_imports.py on the runner's
  System32\xaudio2_9.dll, stage every non-api-set import that is neither in Windows/builtin nor in System32 of machine-native-d3d
  (machine/guest_dlls_d3d.txt, or wine-pe/dlls.txt for a Wine build), rerun.
- Console: unchanged from handoff item 5 (host-uwp runs on main only).
