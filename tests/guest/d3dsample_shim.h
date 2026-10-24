// Force-included (/FI) into the unmodified Tutorial07 sources of guest_d3dsample (docs/slices/d3dsample.md round 3): the
// sample throws away the HRESULT of D3DCompileFromFile, so the call goes through a wrapper that reports the result, the
// compiler's error blob and a CryptoAPI probe (the debug path hashes with PROV_RSA_AES). The wrapper calls the real
// function with the sample's own arguments and returns its result unchanged.
//
// Channel: the layer traces every registry open that fails, with the key path, so each message is sent as the name of a
// key that does not exist (HKCU\Software\win10xbox-diag\<text>); "trace:registry:OpenCommon open Software\win10xbox-diag\..."
// in the test log. (OutputDebugStringA is handled inside the guest's ntdll and never reaches the layer.)
#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static inline void W10xDiag(const char* fmt, ...) {
    char text[200], path[260];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    for (char* c = text; *c; ++c)
        if (*c == '\\' || *c == '\n' || *c == '\r' || *c == '/') *c = ' ';
    _snprintf_s(path, sizeof path, _TRUNCATE, "Software\\win10xbox-diag\\%s", text);
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) == ERROR_SUCCESS) RegCloseKey(k);
    OutputDebugStringA(text);
}

static inline void W10xProbeCrypto() {
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        W10xDiag("crypto: CryptAcquireContext(PROV_RSA_AES) failed, error 0x%08lx", GetLastError());
        return;
    }
    HCRYPTHASH hash = 0;
    BOOL ok = CryptCreateHash(prov, CALG_MD5, 0, 0, &hash);
    DWORD err = ok ? 0 : GetLastError();
    if (ok) {
        static const BYTE data[4] = {1, 2, 3, 4};
        ok = CryptHashData(hash, data, sizeof data, 0);
        err = ok ? 0 : GetLastError();
        CryptDestroyHash(hash);
    }
    W10xDiag("crypto: acquired PROV_RSA_AES, MD5 hash %s, error 0x%08lx", ok ? "ok" : "failed", err);
    CryptReleaseContext(prov, 0);
}

static inline HRESULT W10xCompileFromFile(LPCWSTR file, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                                          LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code,
                                          ID3DBlob** errors) {
    W10xProbeCrypto();
    SetLastError(0);
    const HRESULT hr = D3DCompileFromFile(file, defines, include, entry, target, flags1, flags2, code, errors);
    W10xDiag("D3DCompileFromFile %s %s flags 0x%x = 0x%08lx, last error 0x%08lx", entry, target, flags1,
             static_cast<unsigned long>(hr), GetLastError());
    if (errors && *errors && (*errors)->GetBufferSize()) {
        const char* blob = static_cast<const char*>((*errors)->GetBufferPointer());
        for (size_t i = 0, n = strlen(blob); i < n && i < 800; i += 150) W10xDiag("error blob %u: %.150s", static_cast<unsigned>(i / 150), blob + i);
    }
    return hr;
}
#define D3DCompileFromFile W10xCompileFromFile
