// Force-included (/FI) into the unmodified Tutorial07 sources of guest_d3dsample (docs/slices/d3dsample.md round 3): the
// sample throws away the HRESULT of D3DCompileFromFile, so this header routes the call through a wrapper that sends the
// result and the compiler's error blob to OutputDebugStringA, which the layer's NtRaiseException trace prints. The call
// itself is passed through unchanged.
#pragma once
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>

static inline HRESULT W10xCompileFromFile(LPCWSTR file, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                                          LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code,
                                          ID3DBlob** errors) {
    const HRESULT hr = D3DCompileFromFile(file, defines, include, entry, target, flags1, flags2, code, errors);
    char line[240];
    _snprintf_s(line, sizeof line, _TRUNCATE, "d3dsample: D3DCompileFromFile(%s, %s, flags 0x%x) = 0x%08lx", entry, target,
                flags1, static_cast<unsigned long>(hr));
    OutputDebugStringA(line);
    if (errors && *errors && (*errors)->GetBufferSize()) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "d3dsample: error blob: %s",
                    static_cast<const char*>((*errors)->GetBufferPointer()));
        OutputDebugStringA(line);
    }
    return hr;
}
#define D3DCompileFromFile W10xCompileFromFile
