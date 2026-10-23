// User-pointer probing for syscall implementations (core/dispatch/probe.cpp).
//
// A syscall that Windows implements in the kernel never faults on a bad caller pointer: it probes, and a bad
// pointer comes back as STATUS_ACCESS_VIOLATION (or STATUS_DATATYPE_MISALIGNMENT where Windows checks
// alignment). A Translate/Emulate implementation here is ordinary user-mode code, so the same pointer would
// fault inside the layer and take the guest down. Every implementation that reads or writes caller memory goes
// through this header instead of dereferencing it.
//
//   * ProbeRead / ProbeWrite: ProbeForRead / ProbeForWrite. Length 0 is accepted without looking at the
//     pointer; otherwise the range must lie below MmUserProbeAddress and be aligned, and every page is actually
//     touched (a read, or an atomic no-op write for ProbeWrite), so a buffer is rejected before anything is
//     written to it.
//   * CopyFromUser / CopyToUser / Load / Store: the access itself, done inside the guard, so a page that goes
//     away between the probe and the access (another guest thread) is reported the same way.
//
// The guard is not SEH: clang-mingw cannot compile __try (it accepts the keyword with -fms-extensions and then
// emits no handler), and a mechanism that differs per toolchain would not be tested where it matters. It is the
// Linux kernel's exception table instead. Every access to caller memory is made by one of the leaf routines in
// core/asm/probe.{S,asm}, which keep nothing on the stack and use volatile registers only. A vectored handler
// (installed first, once) recognises a fault whose instruction lies inside those routines and resumes at their
// common exit with the NTSTATUS in rax; a fault anywhere else is not touched. Consequences:
//   * nothing is unwound, so a guarded access must hold no lock and own nothing: copy in, then work, then copy
//     out, as the kernel does;
//   * the status comes from the fault itself: STATUS_ACCESS_VIOLATION, or the I/O status of an in-page error,
//     or STATUS_GUARD_PAGE_VIOLATION.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace win10xbox::probe {

constexpr NTSTATUS kAccessViolation = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS kDatatypeMisalignment = static_cast<NTSTATUS>(0x80000002);

// First address a user-mode caller may not use (MmUserProbeAddress on x64).
constexpr std::uintptr_t kUserProbeAddress = 0x00007FFFFFFF0000ull;

// Installs the vectored handler. Idempotent and cheap once done; called by dispatch::Initialize, and by every
// function below, so a unit test that calls an implementation directly needs no setup.
void Install();

// ProbeForRead / ProbeForWrite. `align` is a power of two (1 = no alignment requirement).
NTSTATUS ProbeRead(const void* address, std::size_t length, std::size_t align = 1);
NTSTATUS ProbeWrite(void* address, std::size_t length, std::size_t align = 1);

// Copies between our memory and a caller's. The caller's range is checked against kUserProbeAddress first.
// A failed CopyToUser may have written part of the range (probe first with ProbeWrite when that matters).
NTSTATUS CopyFromUser(void* to, const void* user_from, std::size_t length);
NTSTATUS CopyToUser(void* user_to, const void* from, std::size_t length);

template <class T>
NTSTATUS Load(const T* user, T* out) {
    static_assert(std::is_trivially_copyable_v<T>);
    return CopyFromUser(out, user, sizeof(T));
}
template <class T>
NTSTATUS Store(T* user, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return CopyToUser(user, &value, sizeof(T));
}

// Optional output: NULL is accepted (as Windows accepts it for the syscalls that take one), anything else is stored.
template <class T>
NTSTATUS StoreIfPresent(T* user, const T& value) {
    return user ? Store(user, value) : 0;
}

// ---- Captures: a caller's counted strings and OBJECT_ATTRIBUTES, copied once --------------------------------
//
// An implementation that needs a name works on the copy; the caller's memory is read exactly once, here, and
// whatever the caller does to it afterwards cannot make the implementation read garbage.

// A UNICODE_STRING and its characters. `us.Buffer` points into `text`, so the object must not be moved or copied
// once captured. `us.Length` is the caller's (an odd value is the caller's rule to reject; `text` holds
// Length / 2 characters).
struct String {
    UNICODE_STRING us{};
    std::wstring text;
    String() = default;
    String(const String&) = delete;
    String& operator=(const String&) = delete;
    std::wstring_view view() const { return text; }
};
// `user` must not be NULL (the caller decides whether an absent string is allowed). A bad header, or a
// non-empty Buffer that cannot be read, is STATUS_ACCESS_VIOLATION.
NTSTATUS CaptureString(const UNICODE_STRING* user, String* out);

// An OBJECT_ATTRIBUTES, with its ObjectName captured when it has one (`oa.ObjectName` then points at `name.us`;
// it is NULL for an unnamed object). SecurityDescriptor and SecurityQualityOfService are not followed.
struct ObjectAttributes {
    OBJECT_ATTRIBUTES oa{};
    String name;
    ObjectAttributes() = default;
    ObjectAttributes(const ObjectAttributes&) = delete;
    ObjectAttributes& operator=(const ObjectAttributes&) = delete;
};
// `user` must not be NULL. STATUS_ACCESS_VIOLATION for memory that cannot be read, STATUS_INVALID_PARAMETER when
// the structure's Length is not sizeof(OBJECT_ATTRIBUTES) (checked before anything else is read, as Windows does).
NTSTATUS CaptureObjectAttributes(const OBJECT_ATTRIBUTES* user, ObjectAttributes* out);

}  // namespace win10xbox::probe
