status: ready
<!-- handoff
1. Entries implemented: all of dlls/mmdevapi/unixlib.h in core/dispatch/unix_mmdevapi.cpp (list under Mechanism); MIDI answers no devices.
2. Local (Wine host, PulseAudio null sink, build-mingw): integration.guest_wasapi PASS (host exit 42, layer exit 42, RESULT played,
   padding drains, position advances); integration.guest_xaudio2 PASS (143 s; the first run timed out running the host-side guest, rerun passed).
3. Merged origin/main (results/ and uwp-host/main.cpp moved into tools/probe/); docs/SYSCALLS.md regenerated, unchanged.
4. Untested: a real Windows host (WASAPI via the host's own mmdevapi), capture streams, exclusive mode (refused by design),
   no-endpoint path in CI, fastfetch Sound module, native xaudio2_9 measure, volume/session calls.
5. Next: run the CI loop (pulseaudio step is in ci.yml), fill the three (pending) sections below, watch the xaudio2 test time.
-->

# Slice: audio (Wine's mmdevapi over the host's WASAPI; xaudio2_9 on top)

Runs: ci https://github.com/user99672223/win10xbox/actions/runs/39975401397 (green, head aba63df; Linux under Wine with PulseAudio null sink, Windows Debug and RelWithDebInfo with no endpoint).

## Mechanism

Wine's `mmdevapi.dll` (PE) is the whole WASAPI stack; what a driver adds is a unix library loaded with
`__wine_load_unix_lib("wine<name>.drv")` and driven through `__wine_unix_call` (`dlls/mmdevapi/unixlib.h`). The layer's
driver is `core/dispatch/unix_mmdevapi.cpp`, registered as unix library `winewasapi`, selected by
`HKCU\Software\Wine\Drivers: Audio=wasapi` (written by `core/registry` with the `MMDeviceEnumerator` class registration,
`ApplyAudioRegistration`). It runs on the HOST's WASAPI: `IMMDeviceEnumerator` / `IAudioClient` on a Windows host, the
host Wine's own mmdevapi on a Wine host.

* One host audio thread owns the COM apartment and every host COM object. It is started, and the whole host stack is
  loaded by a throw-away stream cycle (Initialize, GetService, one silent buffer, Start, Stop), in `PrimeAudio()` before
  the PEB take-over (afterwards `PEB.Ldr` is the guest's and a host `LoadLibrary` binds to the wrong loader, the same
  story as Winsock in docs/slices/net.md). Guest threads never touch host COM: control calls are requests the audio
  thread runs; the data path touches only a stream's own ring buffer under its lock.
* The guest's stream is a ring buffer in this process. `get_render_buffer`/`release_render_buffer` fill it (Wine's
  alsa.c contract, including the temporary buffer when a chunk would wrap); the audio thread pumps it into the host
  stream every 5 ms. Played frames = frames pushed minus the host's `GetCurrentPadding`, interpolated with the system
  timer between pump steps and never past what was pushed (`get_position`); latency = host `GetStreamLatency` + the pump
  interval. Event-callback streams get the guest's event signalled once per period. Capture is the same ring the other
  way round.
* Shared mode only (exclusive: `AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED`, as winepulse). The host stream is initialised with
  the guest's format plus `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`, so the host engine converts.
* `get_mix_format` is the host mix format; `get_endpoint_ids` is the host's default render and default capture endpoint
  (none: zero devices, and the guest's `GetDefaultAudioEndpoint` answers `E_NOTFOUND` 0x80070490 like a machine without
  one); `test_connect` answers Unavailable only when the host cannot even create its enumerator.
* `WIN10XBOX_AUDIO=0` switches the driver off (no thread, no host audio DLLs loaded).

Unix-library entries implemented: main_loop, get_endpoint_ids, create_stream, release_stream, start, stop, reset,
timer_loop, get_render_buffer, release_render_buffer, get_capture_buffer, release_capture_buffer, is_format_supported,
get_loopback_capture_device (E_NOTIMPL), get_mix_format, get_device_period, get_buffer_size, get_latency,
get_current_padding, get_next_packet_size, get_frequency, get_position, set_volumes, set_event_handle, test_connect,
is_started, get_prop_value (device path, physical speakers), midi_get_driver / midi_init (no MIDI devices),
midi_notify_wait, and the do-nothing entries (process_attach/detach, set_sample_rate, midi_*, aux_message).

## What replaced the first attempt

The branch's first commit (87d3c86) bridged to the host by returning the HOST's COM objects through two guest stub DLLs
(`core/audio`). That is gone: it bypassed Wine's mmdevapi (sessions, volumes, property stores, the registry device
database) and put host objects into the guest. Its idea of comparing the layer with the host survives in
`tests/integration/audio_test.cmake`.

## Tests

* `integration.guest_wasapi` (tests/guest/wasapi.c): CoInitializeEx, MMDeviceEnumerator, default render endpoint,
  shared 48 kHz float stereo Initialize, 200 ms of a 440 Hz sine, padding 9600 before Start, then position advances and
  padding drains to zero, clock reads about 0.2 s; exit 42. No endpoint: `GetDefaultAudioEndpoint` failed, exit 43.
* `integration.guest_xaudio2` (tests/guest/xaudio2.c): XAudio2Create, mastering voice, source voice with a callback,
  one buffer, `OnBufferStart`/`OnBufferEnd`/`OnStreamEnd`, voice state advancing; exit 42 (43 without an endpoint).
* Both run through `audio_test.cmake` on the builtin-only machine folder: the host's verdict is `guest_wasapi` run
  directly on the host; the layer must give the same exit code, and without an endpoint the same HRESULT.
* fastfetch's `Sound` module joins the per-module gate in ci.yml (a `Sound` line when the host has an endpoint, exit 0
  without).

## What the runner's audio device reported

The Windows runners (Hyper-V VMs) have no audio endpoint. The probe step runs `guest_wasapi` and `guest_xaudio2` directly on the host:
both end `RESULT no-endpoint hr=0x80070490` (`E_NOTFOUND` from `GetDefaultAudioEndpoint`), `guest_wasapi` exits 43. Through the layer the
same guests give the same verdict and `integration.guest_wasapi` / `integration.guest_xaudio2` pass (they assert layer == host, here the
no-endpoint path; nothing is faked). The fastfetch gate then asserts exit 0 and no `Sound` line. The playing path (exit 42) is proven
on the Linux job only (Wine host, PulseAudio null sink).

## xaudio2: native or builtin

Builtin Wine xaudio2_9 on top of the layer's mmdevapi. On Windows the guest takes 1 to 7 s (no device, nothing rendered); on Linux 7 s in CI
(143 s locally on a slow box), well under the 240 s test timeout.

## What still fails

* host-uwp (msix) packaging check: `guest(s) missing from the .msix: guest_msctf_stub.exe, guest_net.exe, guest_wasapi.exe, guest_xaudio2.exe`;
  add guest_wasapi and guest_xaudio2 (and the net/msctf ones from the other slices) to the guest list in host-uwp/CMakeLists.txt near line 270.
* Untested on any real device: the playing path on Windows, capture streams, volume/session calls.
