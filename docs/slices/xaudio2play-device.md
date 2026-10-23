status: wip
owner: B (A lifts it)
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

# Slice: xaudio2play (a real buffer through XAudio2 2.9 to an endpoint)

(pending the CI loop: run URL, the device that installed, the measured OnBufferEnd time and BuffersQueued line from the job log of
integration.guest_xaudio2_native, and the exact install error if both failed.)
