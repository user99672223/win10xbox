/* msctf_stub: stands in for msctf.dll in the native-GUI machine folder (docs/GUI.md, Route B).
 * The genuine imm32.dll delay-loads these Ctf* entry points from msctf.dll; the genuine msctf.dll cannot be loaded
 * on Wine's builtins (it needs api-ms-win-core-com and the C++ EH runtime), and without any msctf.dll Wine's
 * delay-load hook raises EXCEPTION_WINE_STUB on the first IME message. Every export does nothing and returns 0
 * (FALSE / S_OK / NULL), which is "no text service framework" for the IME dispatch paths. */
#include <windows.h>
#include <stddef.h>

#define CTF_STUB(name) __declspec(dllexport) size_t name(void) { return 0; }

CTF_STUB(CtfImeAssociateFocus)
CTF_STUB(CtfImeConfigure)
CTF_STUB(CtfImeConversionList)
CTF_STUB(CtfImeCreateInputContext)
CTF_STUB(CtfImeCreateThreadMgr)
CTF_STUB(CtfImeDestroy)
CTF_STUB(CtfImeDestroyInputContext)
CTF_STUB(CtfImeDestroyThreadMgr)
CTF_STUB(CtfImeDispatchDefImeMessage)
CTF_STUB(CtfImeEnumRegisterWord)
CTF_STUB(CtfImeEscape)
CTF_STUB(CtfImeEscapeEx)
CTF_STUB(CtfImeGetGuidAtom)
CTF_STUB(CtfImeGetRegisterWordStyle)
CTF_STUB(CtfImeInquire)
CTF_STUB(CtfImeInquireExW)
CTF_STUB(CtfImeIsGuidMapEnable)
CTF_STUB(CtfImeIsIME)
CTF_STUB(CtfImeNotifyIME)
CTF_STUB(CtfImeProcessCicHotkey)
CTF_STUB(CtfImeProcessKey)
CTF_STUB(CtfImeRegisterWord)
CTF_STUB(CtfImeSelect)
CTF_STUB(CtfImeSelectEx)
CTF_STUB(CtfImeSetActiveContext)
CTF_STUB(CtfImeSetCompositionString)
CTF_STUB(CtfImeSetFocus)
CTF_STUB(CtfImeToAsciiEx)
CTF_STUB(CtfImeUnregisterWord)
CTF_STUB(CtfImmCoInitialize)
CTF_STUB(CtfImmCoUninitialize)
CTF_STUB(CtfImmDispatchDefImeMessage)
CTF_STUB(CtfImmEnterCoInitCountSkipMode)
CTF_STUB(CtfImmGenerateMessage)
CTF_STUB(CtfImmGetCompatibleKeyboardLayout)
CTF_STUB(CtfImmGetGuidAtom)
CTF_STUB(CtfImmHideToolbarWnd)
CTF_STUB(CtfImmIsCiceroEnabled)
CTF_STUB(CtfImmIsCiceroStartedInThread)
CTF_STUB(CtfImmIsGuidMapEnable)
CTF_STUB(CtfImmIsTextFrameServiceDisabled)
CTF_STUB(CtfImmLastEnabledWndDestroy)
CTF_STUB(CtfImmLeaveCoInitCountSkipMode)
CTF_STUB(CtfImmNotify)
CTF_STUB(CtfImmRestoreToolbarWnd)
CTF_STUB(CtfImmSetAppCompatFlags)
CTF_STUB(CtfImmSetCiceroStartInThread)
CTF_STUB(CtfImmSetDefaultRemoteKeyboardLayout)
CTF_STUB(CtfImmTIMActivate)
CTF_STUB(CtfImmTIMDeactivate)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
