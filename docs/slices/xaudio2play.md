status: pushed
owner: A
<!-- handoff
1. Guest tests/guest/xaudio2_play.c (XAudio2 2.9, 2 queued 500 ms buffers, exit 42 / 43 no-endpoint / 2..17 failure).
2. Layer option WIN10XBOX_AUDIO_NULL_ENDPOINT=1 (process.cpp -> SetAudioNullEndpoint): fake render endpoint in unix_mmdevapi.cpp.
3. Tests integration.guest_xaudio2_play and integration.guest_xaudio2_play_noendpoint; docs/TESTING.md documents the option.
4. BLOCKED before the first build: the host disk is full (df: 252G size, 85M avail); clang reported "IO failure on output stream: No space left on device". Code is syntax-checked only (clang -fsyntax-only on xaudio2_play.c, unix_mmdevapi.cpp, process.cpp). Nothing committed or pushed.
-->

# Slice: xaudio2play
