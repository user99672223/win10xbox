// win10xbox audio bridge (core/audio): lets a guest reach the HOST's real audio stack (WASAPI mmdevapi, XAudio2).
// Wine's builtin mmdevapi/xaudio2 have no driver here, so the machine folder carries two tiny guest stubs
// (core/audio/guest: mmdevapi.dll, xaudio2_9.dll) whose entry points call this module through Wine's unix-call
// mechanism; the module calls the host's own DLLs (loaded before the guest takes over the PEB) and hands the
// resulting COM objects back. See docs/STATUS.md, "Audio (measured)".
#pragma once
#include <string>

namespace win10xbox::audio {

// Call once, after the registry is initialised and BEFORE the PEB take-over. Does nothing unless the machine
// folder holds the guest stubs (<machine>\Windows\builtin\mmdevapi.dll). Idempotent.
void Initialize(const std::wstring& machine_root);

}  // namespace win10xbox::audio
