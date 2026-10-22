/* mmdevapi.dll (guest stub): the in-process COM server the guest's combase loads for CLSID_MMDeviceEnumerator.
 * It implements only the class factory; CreateInstance asks the host (core/audio/audio.cpp) to create the object
 * with the HOST's real mmdevapi and returns that object, and the host's HRESULT, unchanged. */
#include "bridge.h"

static const GUID kIID_IUnknown = {0x00000000, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID kIID_IClassFactory = {0x00000001, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
#define E_NOINTERFACE_ ((HRESULT)0x80004002)
#define CLASS_E_CLASSNOTAVAILABLE_ ((HRESULT)0x80040111)
#define CLASS_E_NOAGGREGATION_ ((HRESULT)0x80040110)

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const GUID *clsid, const GUID *riid, void **out);

typedef struct factory factory;
struct factory_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(factory *, const GUID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(factory *);
    ULONG (STDMETHODCALLTYPE *Release)(factory *);
    HRESULT (STDMETHODCALLTYPE *CreateInstance)(factory *, void *, const GUID *, void **);
    HRESULT (STDMETHODCALLTYPE *LockServer)(factory *, BOOL);
};
struct factory { const struct factory_vtbl *vtbl; LONG ref; GUID clsid; };

static HRESULT STDMETHODCALLTYPE f_qi(factory *f, const GUID *riid, void **out)
{
    if (InlineIsEqualGUID(riid, &kIID_IUnknown) || InlineIsEqualGUID(riid, &kIID_IClassFactory)) {
        InterlockedIncrement(&f->ref);
        *out = f;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE_;
}
static ULONG STDMETHODCALLTYPE f_addref(factory *f) { return InterlockedIncrement(&f->ref); }
static ULONG STDMETHODCALLTYPE f_release(factory *f)
{
    LONG r = InterlockedDecrement(&f->ref);
    if (!r) HeapFree(GetProcessHeap(), 0, f);
    return r;
}
static void copy_guid(uint32_t dst[4], const GUID *g)
{
    const uint32_t *src = (const uint32_t *)g;
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
}
static HRESULT STDMETHODCALLTYPE f_create(factory *f, void *outer, const GUID *riid, void **out)
{
    struct w10x_audio_create_instance a;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION_;
    copy_guid(a.clsid, &f->clsid);
    copy_guid(a.riid, riid);
    a.object = NULL;
    a.hr = E_FAIL;
    if (w10x_audio_unix_call((const void *)&DllGetClassObject, W10X_AUDIO_CREATE_INSTANCE, &a))
        return E_FAIL;   /* the bridge itself is missing: nothing the host could answer */
    *out = a.object;
    return a.hr;
}
static HRESULT STDMETHODCALLTYPE f_lock(factory *f, BOOL lock) { (void)f; (void)lock; return S_OK; }

static const struct factory_vtbl kFactoryVtbl = {f_qi, f_addref, f_release, f_create, f_lock};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const GUID *clsid, const GUID *riid, void **out)
{
    static const GUID kEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    factory *f;
    HRESULT hr;
    *out = NULL;
    if (!InlineIsEqualGUID(clsid, &kEnumerator)) return CLASS_E_CLASSNOTAVAILABLE_;
    f = (factory *)HeapAlloc(GetProcessHeap(), 0, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->vtbl = &kFactoryVtbl;
    f->ref = 1;
    f->clsid = *clsid;
    hr = f_qi(f, riid, out);
    f_release(f);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
