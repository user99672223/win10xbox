// Host half of the audio bridge (docs/STATUS.md, "Audio (measured)").
//
// Why a bridge: Wine's builtin mmdevapi has no audio driver in this layer (its drivers are unix libraries) and
// xaudio2 sits on it, so a guest would only ever see Wine's own "no device" answers, not the machine's. The
// bridge puts the HOST's mmdevapi / xaudio2_9 behind the guest's COM: the guest loads two stub DLLs from
// <machine>\Windows\builtin (core/audio/guest), registered as the InprocServer32 of CLSID_MMDeviceEnumerator
// (and found by name for xaudio2_9.dll); their entry points make a unix call here, which runs the real host
// function and returns the host's HRESULT unchanged.
//
// The host DLLs are loaded in Initialize, before the PEB take-over: afterwards PEB->Ldr is Wine's, and a
// host LoadLibrary would bind their imports to the guest's kernel32. Objects they create are plain in-process
// COM objects; the guest calls their vtables directly (same address space, same x64 ABI).
#include "win10xbox/audio.h"
#include "win10xbox/config.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/trace.h"
#include "audio_abi.h"
#include <windows.h>
#include <winternl.h>
#include <cwchar>
#include <iterator>

#if WIN10XBOX_HAVE_REGISTRY
#include "win10xbox/registry.h"
#endif

WIN10XBOX_DECLARE_CHANNEL(audio);

namespace win10xbox::audio {
namespace {

constexpr NTSTATUS kStatusSuccess = 0;
constexpr HRESULT kEModNotFound = static_cast<HRESULT>(0x8007007E);   // HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
constexpr HRESULT kEProcNotFound = static_cast<HRESULT>(0x8007007F);  // HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)

struct Host {
    bool initialised = false;
    HMODULE mmdevapi = nullptr;
    HMODULE xaudio2 = nullptr;
    HRESULT(WINAPI* get_class_object)(REFCLSID, REFIID, void**) = nullptr;
    HRESULT(WINAPI* xaudio2_create)(void**, UINT32, UINT32) = nullptr;
} g;

struct IFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown*, REFIID, void**) = 0;
};

NTSTATUS WINAPI CreateInstance(void* args) {
    auto* a = static_cast<w10x_audio_create_instance*>(args);
    a->object = nullptr;
    if (!g.get_class_object) { a->hr = g.mmdevapi ? kEProcNotFound : kEModNotFound; return kStatusSuccess; }
    static const IID kIClassFactory = {0x00000001, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    IFactory* factory = nullptr;
    HRESULT hr = g.get_class_object(*reinterpret_cast<const CLSID*>(a->clsid), kIClassFactory,
                                    reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(nullptr, *reinterpret_cast<const IID*>(a->riid), &a->object);
        factory->Release();
    }
    TRACE(audio, "CreateInstance: host hr=%08lx object=%p", static_cast<unsigned long>(hr), a->object);
    a->hr = hr;
    return kStatusSuccess;
}

NTSTATUS WINAPI XAudio2Create(void* args) {
    auto* a = static_cast<w10x_audio_xaudio2_create*>(args);
    a->object = nullptr;
    if (!g.xaudio2_create) { a->hr = g.xaudio2 ? kEProcNotFound : kEModNotFound; return kStatusSuccess; }
    a->hr = g.xaudio2_create(&a->object, a->flags, a->processor);
    TRACE(audio, "XAudio2Create: host hr=%08lx object=%p", static_cast<unsigned long>(a->hr), a->object);
    return kStatusSuccess;
}

const dispatch::UnixCallFn kFuncs[] = {&CreateInstance, &XAudio2Create};   // W10X_AUDIO_* order

#if WIN10XBOX_HAVE_REGISTRY
void SetString(HANDLE key, const wchar_t* name, const wchar_t* value) {
    UNICODE_STRING n;
    n.Buffer = const_cast<PWSTR>(name);
    n.Length = n.MaximumLength = static_cast<USHORT>(wcslen(name) * sizeof(wchar_t));
    registry::Reg_NtSetValueKey(key, &n, 0, REG_SZ, const_cast<wchar_t*>(value),
                                static_cast<ULONG>((wcslen(value) + 1) * sizeof(wchar_t)));
}

// HKLM\Software\Classes\CLSID\{clsid}\InprocServer32: (Default)=server, ThreadingModel=Both.
void RegisterInprocServer(const wchar_t* clsid, const wchar_t* server) {
    std::wstring path = std::wstring(L"\\Registry\\Machine\\Software\\Classes\\CLSID\\") + clsid + L"\\InprocServer32";
    UNICODE_STRING name;
    name.Buffer = path.data();
    name.Length = name.MaximumLength = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    HANDLE key = nullptr;
    NTSTATUS s = registry::Reg_NtCreateKey(&key, KEY_ALL_ACCESS, &oa, 0, nullptr, 0, nullptr);
    if (s) { WARN(audio, "cannot create %ls: %08lx", path.c_str(), static_cast<unsigned long>(s)); return; }
    SetString(key, L"", server);
    SetString(key, L"ThreadingModel", L"Both");
    registry::Close(key);
}
#endif

}  // namespace

void Initialize(const std::wstring& machine_root) {
    if (g.initialised) return;
    const std::wstring stub = machine_root + L"\\Windows\\builtin\\mmdevapi.dll";
    if (GetFileAttributesW(stub.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // no stubs staged: no bridge
    g.initialised = true;
    g.mmdevapi = LoadLibraryW(L"mmdevapi.dll");
    if (g.mmdevapi)
        g.get_class_object = reinterpret_cast<decltype(g.get_class_object)>(GetProcAddress(g.mmdevapi, "DllGetClassObject"));
    else
        WARN(audio, "host mmdevapi.dll: LoadLibrary failed, error %lu", GetLastError());
    g.xaudio2 = LoadLibraryW(L"xaudio2_9.dll");
    if (g.xaudio2)
        g.xaudio2_create = reinterpret_cast<decltype(g.xaudio2_create)>(GetProcAddress(g.xaudio2, "XAudio2Create"));
    else
        WARN(audio, "host xaudio2_9.dll: LoadLibrary failed, error %lu", GetLastError());
    TRACE(audio, "bridge up: host mmdevapi=%p xaudio2_9=%p", static_cast<void*>(g.mmdevapi), static_cast<void*>(g.xaudio2));
    dispatch::RegisterUnixLib(L"mmdevapi", kFuncs, std::size(kFuncs));
    dispatch::RegisterUnixLib(L"xaudio2_9", kFuncs, std::size(kFuncs));
#if WIN10XBOX_HAVE_REGISTRY
    RegisterInprocServer(L"{BCDE0395-E52F-467C-8E3D-C4579291692E}", L"mmdevapi.dll");   // CLSID_MMDeviceEnumerator
#endif
}

}  // namespace win10xbox::audio
