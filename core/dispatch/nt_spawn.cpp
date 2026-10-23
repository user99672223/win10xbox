// Process creation (D1) and the refusals around it.
//
//   NtCreateUserProcess   a real child: a new HOST process running the host executable itself on the child's guest
//                         image (host-win32 only, config::child_processes), see "Child processes" in
//                         docs/DISPATCHER.md
//   NtCreateProcessEx, NtCreateThread, NtCreateDebugObject, NtDebugActiveProcess
//                         STATUS_NOT_SUPPORTED with a FIXME: nothing in Windows 10 user mode needs them, and none
//                         can be expressed as "start the host executable on an image"
//   NtSetSystemInformation
//                         the classes that need a privilege answer STATUS_PRIVILEGE_NOT_HELD here, whatever the
//                         host's own token could do
#include "nt_spawn.h"
#include "win10xbox/config.h"
#include <algorithm>
#include <cstring>
#include <cwctype>

namespace win10xbox::dispatch::detail {
namespace {

constexpr NTSTATUS kAccessDenied = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS kPrivilegeNotHeld = static_cast<NTSTATUS>(0xC0000061);
constexpr NTSTATUS kNameTooLong = static_cast<NTSTATUS>(0xC0000106);
constexpr NTSTATUS kTooManyOpenedFiles = static_cast<NTSTATUS>(0xC000011F);

// ---- PS_ATTRIBUTE_LIST / PS_CREATE_INFO (the Windows SDK's winternl.h hides them) -----------------------------
struct PsAttribute {
    ULONG_PTR Attribute;
    SIZE_T Size;
    union { ULONG_PTR Value; PVOID ValuePtr; };
    SIZE_T* ReturnLength;
};
struct PsAttributeList {
    SIZE_T TotalLength;
    PsAttribute Attributes[1];
};
constexpr ULONG_PTR kAttrNumberMask = 0xffff, kAttrInput = 0x20000;
enum : ULONG_PTR {
    kPsParentProcess = 0, kPsDebugPort = 1, kPsToken = 2, kPsClientId = 3, kPsTebAddress = 4, kPsImageName = 5,
    kPsImageInfo = 6, kPsHandleList = 11, kPsJobList = 19, kPsMachineType = 28,
};
struct ClientIdPair { PVOID UniqueProcess; PVOID UniqueThread; };   // CLIENT_ID (not in every SDK's winternl.h)
constexpr std::size_t kCreateInfoSize = 0x58;       // sizeof(PS_CREATE_INFO) on x64
constexpr std::uint32_t kCreateInfoState = 8;       // PS_CREATE_STATE State, after the SIZE_T Size
constexpr std::uint32_t kPsCreateFailOnFileOpen = 1, kPsCreateFailExeFormat = 3, kPsCreateSuccess = 6;
constexpr std::size_t kClientIdSize = sizeof(ClientIdPair), kImageInfoSize = 0x40;

constexpr ULONG kProcessBreakaway = 0x1, kProcessNoDebugInherit = 0x2, kProcessInheritHandles = 0x4,
                kProcessSuspended = 0x200;
constexpr ULONG kThreadCreateSuspended = 0x1, kThreadHideFromDebugger = 0x4;
constexpr USHORT kMachineAmd64 = 0x8664;

// ---- RTL_USER_PROCESS_PARAMETERS (x64 offsets; MSVC's winternl.h hides the fields) --------------------------------
namespace rp {
constexpr std::uint32_t Flags = 0x08, ConsoleHandle = 0x10, ConsoleFlags = 0x18, StdInput = 0x20,   // + 8: out, err
                        CurrentDirectory = 0x38, CommandLine = 0x70, Environment = 0x80, StartX = 0x88, StartFlags = 0xA4, ShowWindow = 0xA8,
                        EnvironmentSize = 0x3F0, Size = 0x410;
constexpr ULONG Normalized = 0x1;          // pointers are absolute; otherwise they are offsets from the block
constexpr ULONG_PTR AllocNoWindow = static_cast<ULONG_PTR>(-2);   // CONSOLE_HANDLE_ALLOC_NO_WINDOW
}  // namespace rp

// ---- guest memory ---------------------------------------------------------------------------------------------
// Guest pointers are untrusted: everything read from or written to them goes through win10xbox/probe.h.
// Readable / Writable are ProbeForRead / ProbeForWrite: the range is touched, not looked up.
bool Readable(const void* p, std::size_t n) { return n == 0 || probe::ProbeRead(p, n) == 0; }
bool Writable(const void* p, std::size_t n) { return n == 0 || probe::ProbeWrite(const_cast<void*>(p), n) == 0; }

// Up to `cap` bytes at p, stopping at the first byte that cannot be read (a block of unknown length).
std::vector<std::uint8_t> ReadAccessible(const void* p, std::size_t cap) {
    std::vector<std::uint8_t> out;
    const auto* src = static_cast<const std::uint8_t*>(p);
    while (out.size() < cap) {
        const auto a = reinterpret_cast<std::uintptr_t>(src + out.size());
        const std::size_t chunk = std::min<std::size_t>(cap - out.size(), 0x1000 - (a & 0xFFF));
        const std::size_t at = out.size();
        out.resize(at + chunk);
        if (probe::CopyFromUser(out.data() + at, src + at, chunk)) { out.resize(at); break; }
    }
    return out;
}

template <class T> T Field(const std::uint8_t* base, std::uint32_t off) {
    T v;
    std::memcpy(&v, base + off, sizeof v);
    return v;
}

// A UNICODE_STRING inside the parameter block; its buffer is a pointer, or an offset when the block is not normalized.
// `params` is our copy of the block, `user_params` where it came from (the offsets are relative to that).
bool ParamString(const std::uint8_t* params, const std::uint8_t* user_params, std::uint32_t off, bool normalized,
                 std::wstring* out) {
    out->clear();
    const auto us = Field<UNICODE_STRING>(params, off);
    if (!us.Length || !us.Buffer) return true;
    const auto* text = normalized ? reinterpret_cast<const std::uint8_t*>(us.Buffer)
                                  : user_params + reinterpret_cast<std::uintptr_t>(us.Buffer);
    out->assign(us.Length / sizeof(wchar_t), L'\0');
    return probe::CopyFromUser(out->data(), text, out->size() * sizeof(wchar_t)) == 0;
}

// ---- the host's process-creation API --------------------------------------------------------------------------
// Resolved dynamically from kernel32 in PrimeChildProcesses: the UWP host's import library does not carry them
// (it never turns child processes on), and after the PEB take-over GetModuleHandleW answers with the guest's.
struct HostProcessApi {
    BOOL(WINAPI* CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                 LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) = nullptr;
    BOOL(WINAPI* InitializeProcThreadAttributeList)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T) = nullptr;
    BOOL(WINAPI* UpdateProcThreadAttribute)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID,
                                            PSIZE_T) = nullptr;
    void(WINAPI* DeleteProcThreadAttributeList)(LPPROC_THREAD_ATTRIBUTE_LIST) = nullptr;
    std::wstring host_exe;
    bool primed = false;
};
HostProcessApi g_api;
std::wstring g_test_host_exe;

// PROC_THREAD_ATTRIBUTE_* numbers (processthreadsapi.h: Number | Thread 0x1 | Input 0x20000 | Additive 0x40000).
constexpr DWORD_PTR kProcThreadParentProcess = 0x00020000, kProcThreadHandleList = 0x00020002,
                    kProcThreadJobList = 0x0002000D;

NTSTATUS StatusFromWin32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return st::ObjectNameNotFound;
    case ERROR_PATH_NOT_FOUND: return st::ObjectPathNotFound;
    case ERROR_ACCESS_DENIED: return kAccessDenied;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_HANDLE: return st::InvalidParameter;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return st::NoMemory;
    case ERROR_BAD_EXE_FORMAT: case ERROR_BAD_FORMAT: return st::InvalidImageFormat;
    case ERROR_TOO_MANY_OPEN_FILES: return kTooManyOpenedFiles;
    case ERROR_FILENAME_EXCED_RANGE: return kNameTooLong;
    default: return st::Unsuccessful;
    }
}

// "\??\C:\x" / "\\?\C:\x" -> "C:\x"; anything else is returned unchanged.
std::wstring NtPathToWin32(std::wstring_view nt) {
    if (nt.size() >= 4 && (nt.substr(0, 4) == L"\\??\\" || nt.substr(0, 4) == L"\\\\?\\")) nt.remove_prefix(4);
    return std::wstring(nt);
}

bool IsDriveAbsolute(const std::wstring& p) {
    return p.size() >= 3 && std::iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
}

// Everything NtCreateUserProcess takes from the guest's arguments.
struct ChildRequest {
    std::wstring image;                   // guest Win32 path, "C:\guest\app.exe"
    std::wstring command_line;            // the child's raw guest command line (may be empty)
    std::wstring current_directory;       // guest path, empty = the host's
    std::vector<std::wstring> env;        // guest environment entries
    bool has_env = false;
    HANDLE std_handles[3] = {};           // guest values (host handles); null = none
    ULONG_PTR console_handle = 0;
    ULONG console_flags = 0;
    STARTUPINFOW startup = {};
    bool inherit_handles = false;
    std::vector<HANDLE> handle_list;      // PS_ATTRIBUTE_HANDLE_LIST (only honoured with inherit_handles)
    bool has_handle_list = false;
    std::vector<HANDLE> job_list;
    HANDLE parent = nullptr;              // PS_ATTRIBUTE_PARENT_PROCESS when it is not this process
};

// The guest's environment block -> entries. `size` is EnvironmentSize in bytes, 0 when the caller did not set it.
bool ParseEnvironment(const wchar_t* user_env, std::size_t size, std::vector<std::wstring>* out) {
    const std::size_t cap = size ? std::min<std::size_t>(size, 1u << 22) : (1u << 22);   // bytes
    const std::vector<std::uint8_t> copy = ReadAccessible(user_env, cap);
    const wchar_t* env = reinterpret_cast<const wchar_t*>(copy.data());
    const std::size_t have = copy.size() / sizeof(wchar_t);                               // wchar_t units
    std::size_t i = 0;
    while (i < have && env[i]) {
        std::size_t j = i;
        while (j < have && env[j]) ++j;
        if (j >= have) return false;   // unterminated entry
        out->emplace_back(env + i, j - i);
        i = j + 1;
    }
    return i < have;                   // found the final terminator
}

// PS_CREATE_INFO { Size; State; ... } written to the caller (best effort: a failing start reports its state).
NTSTATUS WriteCreateInfo(void* create_info, std::uint32_t state) {
    std::uint8_t info[kCreateInfoSize] = {};
    const std::size_t size = kCreateInfoSize;
    std::memcpy(info, &size, sizeof size);
    std::memcpy(info + kCreateInfoState, &state, sizeof state);
    return probe::CopyToUser(create_info, info, sizeof info);
}
void Fail(void* create_info, std::uint32_t state) {
    if (create_info) (void)WriteCreateInfo(create_info, state);
}

// Reads the executable's headers and fills the SECTION_IMAGE_INFORMATION the caller asked for. Statuses are the ones
// Windows gives for a file that is not an image this process can start.
NTSTATUS InspectImage(const std::wstring& host_path, std::uint8_t info[kImageInfoSize]) {
    HANDLE f = CreateFileW(host_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return StatusFromWin32(GetLastError());
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    // Headers, section table and a .NET COR header sit at the start; the whole file is not needed to classify it.
    std::vector<std::uint8_t> head(static_cast<std::size_t>(std::min<LONGLONG>(size.QuadPart, 4 << 20)));
    DWORD got = 0;
    const BOOL ok = head.empty() || ReadFile(f, head.data(), static_cast<DWORD>(head.size()), &got, nullptr);
    CloseHandle(f);
    if (!ok) return StatusFromWin32(GetLastError());
    head.resize(got);
    std::uint32_t image_size = 0;
    NTSTATUS s = BuildImageInformation(head.data(), head.size(), info, &image_size);
    if (!NtSuccess(s)) return s;
    const auto file_size = static_cast<std::uint32_t>(size.QuadPart);
    std::memcpy(info + 0x38, &file_size, sizeof file_size);   // ImageFileSize: the real size, not the prefix read
    USHORT machine, characteristics;
    std::memcpy(&machine, info + 0x30, sizeof machine);
    std::memcpy(&characteristics, info + 0x2C, sizeof characteristics);
    if (machine != kMachineAmd64) {
        WARN(process, "%ls is machine type %04x: only x64 programs run (D6, no 32-bit support)", host_path.c_str(), machine);
        return st::InvalidImageFormat;
    }
    if (characteristics & IMAGE_FILE_DLL) return st::InvalidImageFormat;
    return st::Success;
}

// One inheritable duplicate of a standard handle that was not inheritable itself (closed after the spawn).
struct StdHandle {
    HANDLE value = nullptr;       // what the child is told
    HANDLE temporary = nullptr;   // our duplicate, if value is one
};

}  // namespace

// ---- exported helpers (also used by the tests) --------------------------------------------------------------------

void AppendQuotedArgument(std::wstring* out, std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        out->append(arg);
        return;
    }
    out->push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') out->append(backslashes * 2 + 1, L'\\');
        else out->append(backslashes, L'\\');
        backslashes = 0;
        out->push_back(c);
    }
    out->append(backslashes * 2, L'\\');
    out->push_back(L'"');
}

std::wstring BuildChildCommandLine(const std::wstring& host_exe, const std::wstring& machine_root,
                                   const std::wstring& guest_image, const std::wstring& guest_cmdline,
                                   const std::wstring& guest_cwd, bool pass_environment) {
    std::wstring cl;
    AppendQuotedArgument(&cl, host_exe);
    cl += L" --machine ";
    AppendQuotedArgument(&cl, machine_root);
    if (!guest_cwd.empty()) {
        cl += L" --cwd ";
        AppendQuotedArgument(&cl, guest_cwd);
    }
    if (pass_environment) cl += L" --guest-env";
    if (!guest_cmdline.empty()) {
        cl += L" --cmdline ";
        AppendQuotedArgument(&cl, guest_cmdline);
    }
    cl.push_back(L' ');
    AppendQuotedArgument(&cl, guest_image);
    return cl;
}

bool IsHostOwnedVariable(std::wstring_view entry) {
    auto starts = [&](std::wstring_view prefix) {
        if (entry.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (std::towupper(entry[i]) != prefix[i]) return false;
        return true;
    };
    return starts(L"WIN10XBOX_") || starts(L"WINE");
}

std::vector<wchar_t> BuildChildEnvironmentBlock(const std::vector<std::wstring>& guest_env,
                                                const std::vector<std::wstring>& host_env) {
    std::vector<wchar_t> block;
    auto add = [&](const std::wstring& e) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(0);
    };
    for (const std::wstring& e : guest_env)
        if (!e.empty() && !IsHostOwnedVariable(e)) add(e);
    for (const std::wstring& e : host_env)
        if (!e.empty() && IsHostOwnedVariable(e)) add(e);
    if (block.empty()) block.push_back(0);
    block.push_back(0);
    return block;
}

void PrimeChildProcesses() {
    if (g_api.primed) return;
    g_api.primed = true;
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
#define W10X_K32(fn) g_api.fn = reinterpret_cast<decltype(g_api.fn)>(GetProcAddress(k, #fn))
        W10X_K32(CreateProcessW);
        W10X_K32(InitializeProcThreadAttributeList);
        W10X_K32(UpdateProcThreadAttribute);
        W10X_K32(DeleteProcThreadAttributeList);
#undef W10X_K32
    }
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    if (n && n < MAX_PATH * 2) g_api.host_exe.assign(path, n);
}

void SetChildHostExecutableForTest(const std::wstring& exe) { g_test_host_exe = exe; }

// ---- NtCreateUserProcess --------------------------------------------------------------------------------------------

NTSTATUS NTAPI Xl_NtCreateUserProcess(PHANDLE process_handle, PHANDLE thread_handle, ACCESS_MASK process_access,
                                      ACCESS_MASK thread_access, POBJECT_ATTRIBUTES process_attr,
                                      POBJECT_ATTRIBUTES thread_attr, ULONG process_flags, ULONG thread_flags,
                                      PVOID params_arg, PVOID create_info, PVOID attribute_list) {
    (void)process_access; (void)thread_access;   // the host child's handles carry full access
    if (!config::Current().child_processes) {
        WARN(process, "NtCreateUserProcess: this host cannot start child processes (config::child_processes is off)");
        return st::NotSupported;
    }
    if (!g_api.CreateProcessW || !g_api.InitializeProcThreadAttributeList || !g_api.UpdateProcThreadAttribute ||
        !g_api.DeleteProcThreadAttributeList) {
        ERR(process, "NtCreateUserProcess: kernel32 process creation entry points are missing");
        return st::NotSupported;
    }
    const auto* user_params = static_cast<const std::uint8_t*>(params_arg);
    auto* attrs = static_cast<PsAttributeList*>(attribute_list);
    if (!process_handle || !thread_handle || !user_params || !attrs) return st::InvalidParameter;
    if (!Writable(process_handle, sizeof(HANDLE)) || !Writable(thread_handle, sizeof(HANDLE)) ||
        !Readable(user_params, rp::Size) || !Readable(attrs, sizeof(attrs->TotalLength)))
        return st::AccessViolation;
    // The parameter block and the attribute list are read once; the code below works on these copies.
    std::vector<std::uint8_t> params_copy(rp::Size);
    if (probe::CopyFromUser(params_copy.data(), user_params, rp::Size)) return st::AccessViolation;
    const std::uint8_t* params = params_copy.data();
    SIZE_T total_length = 0;
    if (probe::Load<SIZE_T>(&attrs->TotalLength, &total_length)) return st::AccessViolation;
    if (create_info && !Writable(create_info, kCreateInfoSize)) return st::AccessViolation;
    if (thread_flags & kThreadHideFromDebugger) {
        WARN(process, "NtCreateUserProcess: invalid thread flags %#lx", static_cast<unsigned long>(thread_flags));
        return st::InvalidParameter;
    }
    if (thread_flags & ~kThreadCreateSuspended)
        FIXME(process, "unsupported thread flags %#lx", static_cast<unsigned long>(thread_flags & ~kThreadCreateSuspended));
    if (process_flags & ~(kProcessBreakaway | kProcessNoDebugInherit | kProcessInheritHandles | kProcessSuspended))
        FIXME(process, "unsupported process flags %#lx",
              static_cast<unsigned long>(process_flags & ~(kProcessBreakaway | kProcessNoDebugInherit |
                                                           kProcessInheritHandles | kProcessSuspended)));

    // ---- the attribute list ------------------------------------------------------------------------------------
    if (total_length < sizeof(attrs->TotalLength) || (total_length - sizeof(attrs->TotalLength)) % sizeof(PsAttribute) != 0)
        return st::InvalidParameter;
    const std::size_t count = (total_length - sizeof(attrs->TotalLength)) / sizeof(PsAttribute);
    if (count > 64) return st::InvalidParameter;
    std::vector<PsAttribute> attr_copy(count);
    if (count && probe::CopyFromUser(attr_copy.data(), attrs->Attributes, count * sizeof(PsAttribute)))
        return st::AccessViolation;

    ChildRequest req;
    req.inherit_handles = (process_flags & kProcessInheritHandles) != 0;
    std::wstring image_nt;
    bool have_image = false;
    for (std::size_t i = 0; i < count; ++i) {
        const PsAttribute& a = attr_copy[i];
        switch (a.Attribute & kAttrNumberMask) {
        case kPsParentProcess:
            if (a.ValuePtr && !IsCurrentProcess(static_cast<HANDLE>(a.ValuePtr))) req.parent = static_cast<HANDLE>(a.ValuePtr);
            break;
        case kPsDebugPort:
            if (a.ValuePtr) {
                WARN(process, "NtCreateUserProcess: debugging a child is not supported");
                return st::NotSupported;
            }
            break;
        case kPsToken:
            if (a.ValuePtr) {
                // The guest's tokens are emulated; the host child cannot run under one.
                WARN(process, "NtCreateUserProcess: starting a child under another token is not supported");
                return st::NotSupported;
            }
            break;
        case kPsImageName:
            if (!a.ValuePtr || a.Size < sizeof(wchar_t) || (a.Size & 1) || a.Size > 32767 * sizeof(wchar_t))
                return st::InvalidParameter;
            image_nt.assign(a.Size / sizeof(wchar_t), L'\0');
            if (probe::CopyFromUser(image_nt.data(), a.ValuePtr, a.Size)) return st::AccessViolation;
            have_image = true;
            break;
        case kPsHandleList:
            if (a.Size % sizeof(HANDLE)) return st::InvalidParameter;
            req.has_handle_list = true;
            req.handle_list.resize(a.Size / sizeof(HANDLE));
            if (a.Size && probe::CopyFromUser(req.handle_list.data(), a.ValuePtr, a.Size)) return st::InvalidParameter;
            break;
        case kPsJobList:
            if (a.Size % sizeof(HANDLE)) return st::InvalidParameter;
            req.job_list.resize(a.Size / sizeof(HANDLE));
            if (a.Size && probe::CopyFromUser(req.job_list.data(), a.ValuePtr, a.Size)) return st::InvalidParameter;
            break;
        case kPsMachineType:
            if (a.Value && a.Value != kMachineAmd64) {
                WARN(process, "NtCreateUserProcess: machine type %04lx requested, only x64 runs (D6)", static_cast<unsigned long>(a.Value));
                return st::InvalidImageFormat;
            }
            break;
        case kPsClientId:
        case kPsImageInfo: {
            // Outputs: checked now so that nothing is started when the answer could not be delivered.
            const std::size_t want = (a.Attribute & kAttrNumberMask) == kPsClientId ? kClientIdSize : kImageInfoSize;
            if (a.Size && !Writable(a.ValuePtr, std::min<std::size_t>(a.Size, want))) return st::AccessViolation;
            if (a.ReturnLength && !Writable(a.ReturnLength, sizeof(SIZE_T))) return st::AccessViolation;
            break;
        }
        case kPsTebAddress:
            break;   // an output nobody reads; left untouched
        default:
            if (a.Attribute & kAttrInput)
                FIXME(process, "unhandled input attribute %#llx ignored", static_cast<unsigned long long>(a.Attribute));
            break;
        }
    }
    if (!have_image) return st::InvalidParameter;

    // ---- the parameter block -----------------------------------------------------------------------------------------
    const bool normalized = (Field<ULONG>(params, rp::Flags) & rp::Normalized) != 0;
    req.image = NtPathToWin32(image_nt);
    if (!ParamString(params, user_params, rp::CommandLine, normalized, &req.command_line) ||
        !ParamString(params, user_params, rp::CurrentDirectory, normalized, &req.current_directory))
        return st::AccessViolation;
    if (!IsDriveAbsolute(req.image)) {
        WARN(process, "NtCreateUserProcess: %ls is not a drive-letter path", image_nt.c_str());
        return st::ObjectPathNotFound;
    }
    {
        const auto env = Field<std::uintptr_t>(params, rp::Environment);
        if (env) {
            const auto* block = normalized ? reinterpret_cast<const wchar_t*>(env)
                                           : reinterpret_cast<const wchar_t*>(user_params + env);
            if (!ParseEnvironment(block, Field<SIZE_T>(params, rp::EnvironmentSize), &req.env)) return st::AccessViolation;
            req.has_env = true;
        }
    }
    req.console_handle = Field<std::uintptr_t>(params, rp::ConsoleHandle);
    req.console_flags = Field<ULONG>(params, rp::ConsoleFlags);
    for (int i = 0; i < 3; ++i) req.std_handles[i] = Field<HANDLE>(params, rp::StdInput + 8 * i);

    // ---- the image -----------------------------------------------------------------------------------------------------
    std::wstring host_image;
    NTSTATUS s = TranslateGuestWin32Path(req.image, &host_image);
    if (!NtSuccess(s)) { Fail(create_info, kPsCreateFailOnFileOpen); return s; }
    std::uint8_t image_info[kImageInfoSize];
    s = InspectImage(host_image, image_info);
    if (!NtSuccess(s)) {
        Fail(create_info, s == st::ObjectNameNotFound || s == st::ObjectPathNotFound || s == kAccessDenied
                              ? kPsCreateFailOnFileOpen : kPsCreateFailExeFormat);
        return s;
    }
    std::wstring host_cwd;
    if (!req.current_directory.empty()) {
        s = TranslateGuestWin32Path(req.current_directory, &host_cwd);
        if (!NtSuccess(s)) return s == st::ObjectNameNotFound ? st::ObjectPathNotFound : s;
    }

    // ---- the host command line, environment and flags ------------------------------------------------------------------
    const std::wstring host_exe = g_test_host_exe.empty() ? g_api.host_exe : g_test_host_exe;
    const std::wstring machine_root = config::Current().machine_root;
    if (host_exe.empty() || machine_root.empty()) {
        ERR(process, "NtCreateUserProcess: host executable or machine folder unknown");
        return st::Unsuccessful;
    }
    std::wstring command_line =
        BuildChildCommandLine(host_exe, machine_root, req.image, req.command_line, req.current_directory, req.has_env);
    std::vector<wchar_t> env_block;
    if (req.has_env) env_block = BuildChildEnvironmentBlock(req.env, HostEnvironmentEntries());

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if ((thread_flags & kThreadCreateSuspended) || (process_flags & kProcessSuspended)) flags |= CREATE_SUSPENDED;
    if (process_flags & kProcessBreakaway) flags |= CREATE_BREAKAWAY_FROM_JOB;
    // The guest has no console of its own: the child shares the host's. What the guest asked for maps onto how the
    // host child attaches to it. kernelbase turns "no console" and "new console" both into CONSOLE_HANDLE_ALLOC (-1),
    // which stays "inherit" here; opening a console window for a layer that does not emulate consoles would be wrong.
    if (req.console_handle == rp::AllocNoWindow) flags |= CREATE_NO_WINDOW;
    else if (req.console_handle == 0) flags |= DETACHED_PROCESS;
    if (req.console_flags & 1) flags |= CREATE_NEW_PROCESS_GROUP;   // kernelbase: CREATE_NEW_PROCESS_GROUP

    // Standard handles go to the child as the host's STARTUPINFO std handles. A handle that is not inheritable is
    // duplicated as one for the duration of the call (Windows would hand the child a dead value).
    StdHandle std_handles[3];
    bool any_std = false;
    for (int i = 0; i < 3; ++i) {
        HANDLE h = req.std_handles[i];
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        DWORD info = 0;
        if (!GetHandleInformation(h, &info)) {
            WARN(process, "standard handle %d (%p) is not a valid handle: the child gets none", i, h);
            continue;
        }
        std_handles[i].value = h;
        if (!(info & HANDLE_FLAG_INHERIT)) {
            if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &std_handles[i].temporary, 0, TRUE,
                                 DUPLICATE_SAME_ACCESS)) {
                WARN(process, "standard handle %d (%p) cannot be made inheritable (error %lu): the child gets none", i, h,
                     GetLastError());
                std_handles[i].value = nullptr;
                continue;
            }
            std_handles[i].value = std_handles[i].temporary;
        }
        any_std = true;
    }

    // Which handles the child inherits: with INHERIT_HANDLES every inheritable one (or the caller's list, plus the
    // standard handles); without it only the standard handles.
    std::vector<HANDLE> inherit_list;
    bool use_list = false;
    if (req.inherit_handles && req.has_handle_list) { inherit_list = req.handle_list; use_list = true; }
    else if (!req.inherit_handles && any_std) use_list = true;
    if (use_list)
        for (const StdHandle& sh : std_handles)
            if (sh.value && std::find(inherit_list.begin(), inherit_list.end(), sh.value) == inherit_list.end())
                inherit_list.push_back(sh.value);
    const bool inherit = req.inherit_handles || (use_list && !inherit_list.empty());

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOW);
    si.StartupInfo.dwX = Field<ULONG>(params, rp::StartX);
    si.StartupInfo.dwY = Field<ULONG>(params, rp::StartX + 4);
    si.StartupInfo.dwXSize = Field<ULONG>(params, rp::StartX + 8);
    si.StartupInfo.dwYSize = Field<ULONG>(params, rp::StartX + 12);
    si.StartupInfo.dwXCountChars = Field<ULONG>(params, rp::StartX + 16);
    si.StartupInfo.dwYCountChars = Field<ULONG>(params, rp::StartX + 20);
    si.StartupInfo.dwFillAttribute = Field<ULONG>(params, rp::StartX + 24);
    si.StartupInfo.dwFlags = Field<ULONG>(params, rp::StartFlags) & 0xFF;   // window placement / feedback bits only
    si.StartupInfo.wShowWindow = static_cast<WORD>(Field<ULONG>(params, rp::ShowWindow));
    if (any_std) {
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = std_handles[0].value;
        si.StartupInfo.hStdOutput = std_handles[1].value;
        si.StartupInfo.hStdError = std_handles[2].value;
    }

    std::vector<std::uint8_t> attr_storage;
    LPPROC_THREAD_ATTRIBUTE_LIST host_attrs = nullptr;
    DWORD host_attr_count = (use_list ? 1 : 0) + (req.parent ? 1 : 0) + (req.job_list.empty() ? 0 : 1);
    if (host_attr_count) {
        SIZE_T bytes = 0;
        g_api.InitializeProcThreadAttributeList(nullptr, host_attr_count, 0, &bytes);
        attr_storage.resize(bytes);
        host_attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
        if (!g_api.InitializeProcThreadAttributeList(host_attrs, host_attr_count, 0, &bytes)) {
            const DWORD e = GetLastError();
            for (StdHandle& sh : std_handles) if (sh.temporary) CloseHandle(sh.temporary);
            return StatusFromWin32(e);
        }
        bool ok = true;
        if (use_list)
            ok = ok && g_api.UpdateProcThreadAttribute(host_attrs, 0, kProcThreadHandleList, inherit_list.data(),
                                                       inherit_list.size() * sizeof(HANDLE), nullptr, nullptr);
        if (req.parent)
            ok = ok && g_api.UpdateProcThreadAttribute(host_attrs, 0, kProcThreadParentProcess, &req.parent,
                                                       sizeof(HANDLE), nullptr, nullptr);
        if (!req.job_list.empty())
            ok = ok && g_api.UpdateProcThreadAttribute(host_attrs, 0, kProcThreadJobList, req.job_list.data(),
                                                       req.job_list.size() * sizeof(HANDLE), nullptr, nullptr);
        if (!ok) {
            const DWORD e = GetLastError();
            g_api.DeleteProcThreadAttributeList(host_attrs);
            for (StdHandle& sh : std_handles) if (sh.temporary) CloseHandle(sh.temporary);
            return StatusFromWin32(e);
        }
        si.lpAttributeList = host_attrs;
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    TRACE(process, "spawning %ls (host cwd %ls, %zu env vars, flags %#lx, inherit %d, %zu listed handles)", command_line.c_str(),
          host_cwd.empty() ? L"<host's>" : host_cwd.c_str(), req.env.size(), static_cast<unsigned long>(flags), inherit,
          use_list ? inherit_list.size() : size_t(0));
    PROCESS_INFORMATION pi{};
    const BOOL created = g_api.CreateProcessW(host_exe.c_str(), command_line.data(), nullptr, nullptr, inherit, flags,
                                              req.has_env ? env_block.data() : nullptr,
                                              host_cwd.empty() ? nullptr : host_cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD error = created ? 0 : GetLastError();
    if (host_attrs) g_api.DeleteProcThreadAttributeList(host_attrs);
    for (StdHandle& sh : std_handles) if (sh.temporary) CloseHandle(sh.temporary);
    if (!created) {
        WARN(process, "NtCreateUserProcess: starting %ls failed with Win32 error %lu", host_exe.c_str(), error);
        return StatusFromWin32(error);
    }
    ULONG process_obj_attrs = 0, thread_obj_attrs = 0;   // best effort: attributes we cannot read inherit nothing
    if (process_attr) (void)probe::Load<ULONG>(&process_attr->Attributes, &process_obj_attrs);
    if (thread_attr) (void)probe::Load<ULONG>(&thread_attr->Attributes, &thread_obj_attrs);
    if (process_obj_attrs & OBJ_INHERIT) SetHandleInformation(pi.hProcess, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (thread_obj_attrs & OBJ_INHERIT) SetHandleInformation(pi.hThread, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    // ---- outputs ---------------------------------------------------------------------------------------------------------
    // They were probed before the child was started; a caller that unmaps one meanwhile gets the access violation,
    // and the child it will never learn about is not left running.
    NTSTATUS out_status = probe::Store<HANDLE>(process_handle, pi.hProcess);
    if (out_status == st::Success) out_status = probe::Store<HANDLE>(thread_handle, pi.hThread);
    if (out_status == st::Success && create_info) out_status = WriteCreateInfo(create_info, kPsCreateSuccess);
    for (std::size_t i = 0; i < count && out_status == st::Success; ++i) {
        const PsAttribute& a = attr_copy[i];
        const auto number = a.Attribute & kAttrNumberMask;
        std::size_t written = 0;
        if (number == kPsClientId) {
            ClientIdPair id{};
            id.UniqueProcess = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(pi.dwProcessId));
            id.UniqueThread = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(pi.dwThreadId));
            written = std::min<std::size_t>(a.Size, sizeof id);
            out_status = probe::CopyToUser(a.ValuePtr, &id, written);
        } else if (number == kPsImageInfo) {
            written = std::min<std::size_t>(a.Size, kImageInfoSize);
            out_status = probe::CopyToUser(a.ValuePtr, image_info, written);
        } else {
            continue;
        }
        if (out_status == st::Success && a.ReturnLength) out_status = probe::Store<SIZE_T>(a.ReturnLength, written);
    }
    if (out_status != st::Success) {
        TerminateProcess(pi.hProcess, static_cast<UINT>(out_status));
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return out_status;
    }
    TRACE(process, "started pid %04lx tid %04lx: %ls", pi.dwProcessId, pi.dwThreadId, req.image.c_str());
    return st::Success;
}

// ---- Refusals ---------------------------------------------------------------------------------------------------------

NTSTATUS NTAPI Xl_NtCreateProcessEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG, HANDLE section, HANDLE,
                                    HANDLE, ULONG) {
    // An empty process built from a section, to be filled in and given a thread by the caller (fork-style spawning,
    // process hollowing). A host process can only be started whole, on an image: NtCreateUserProcess is the path
    // Windows 10's CreateProcess takes.
    FIXME(process, "NtCreateProcessEx (section %p): not supported, a child is created whole by NtCreateUserProcess", section);
    return st::NotSupported;
}

NTSTATUS NTAPI Xl_NtCreateThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE process, PVOID /*CLIENT_ID*/, PCONTEXT, PVOID,
                                 BOOLEAN) {
    // The pre-Vista creator: the caller allocates the stack (INITIAL_TEB) and supplies a full CONTEXT to start from.
    // Guest threads are host threads that enter Wine's per-thread loader init (E4), which NtCreateThreadEx
    // arranges around a start routine; an arbitrary CONTEXT has no such entry. Nothing in Windows 10 user mode
    // (kernelbase, Wine's ntdll) calls this.
    FIXME(process, "NtCreateThread (process %p): legacy, not supported; NtCreateThreadEx is the supported path", process);
    return st::NotSupported;
}

NTSTATUS NTAPI Xl_NtCreateDebugObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG) {
    FIXME(process, "NtCreateDebugObject: debugging is not supported (docs/DECISIONS.md E6)");
    return st::NotSupported;
}

NTSTATUS NTAPI Xl_NtDebugActiveProcess(HANDLE process, HANDLE debug_object) {
    FIXME(process, "NtDebugActiveProcess (process %p, debug object %p): debugging is not supported (docs/DECISIONS.md E6)",
          process, debug_object);
    return st::NotSupported;
}

// ---- NtSetSystemInformation -------------------------------------------------------------------------------------------

namespace {
struct SetClass { ULONG klass; const char* name; const char* privilege; };
// SYSTEM_INFORMATION_CLASS values (Wine's numbering = Windows 10's) that change kernel or machine-wide state. Each
// needs a privilege (or is kernel-mode only), and the guest's token holds none: the answer is the one an unprivileged
// Windows user gets. `privilege` is null where the requirement is not a single named privilege.
constexpr SetClass kPrivilegedClasses[] = {
    {9, "SystemFlagsInformation", "SeDebugPrivilege"},
    {21, "SystemFileCacheInformation", "SeIncreaseQuotaPrivilege"},
    {24, "SystemDpcBehaviorInformation", "SeLoadDriverPrivilege"},
    {26, "SystemLoadGdiDriverInformation", "SeLoadDriverPrivilege"},
    {27, "SystemUnloadGdiDriverInformation", "SeLoadDriverPrivilege"},
    {28, "SystemTimeAdjustmentInformation", "SeSystemtimePrivilege"},
    {31, "SystemPerformanceTraceInformation", "SeSystemProfilePrivilege"},
    {34, "SystemCrashDumpStateInformation", "SeDebugPrivilege"},
    {37, "SystemRegistryQuotaInformation", "SeIncreaseQuotaPrivilege"},
    {38, "SystemExtendServiceTableInformation", "SeLoadDriverPrivilege"},
    {39, "SystemPrioritySeparation", "SeTcbPrivilege"},
    {40, "SystemVerifierAddDriverInformation", "SeDebugPrivilege"},
    {41, "SystemVerifierRemoveDriverInformation", "SeDebugPrivilege"},
    {44, "SystemCurrentTimeZoneInformation", "SeTimeZonePrivilege"},
    {46, "SystemTimeSlipNotification", "SeSystemtimePrivilege"},
    {47, "SystemSessionCreate", "SeTcbPrivilege"},
    {48, "SystemSessionDetach", "SeTcbPrivilege"},
    {51, "SystemVerifierInformation", "SeDebugPrivilege"},
    {52, "SystemVerifierThunkExtend", nullptr},
    {54, "SystemLoadGdiDriverInSystemSpace", "SeLoadDriverPrivilege"},
    {56, "SystemPrefetcherInformation", "SeProfileSingleProcessPrivilege"},
    {69, "SystemHotpatchInformation", nullptr},
    {71, "SystemWatchdogTimerHandler", nullptr},
    {75, "SystemRegisterFirmwareTableInformationHandler", nullptr},
    {79, "SystemSuperfetchInformation", "SeProfileSingleProcessPrivilege"},
    {80, "SystemMemoryListInformation", "SeProfileSingleProcessPrivilege"},
    {81, "SystemFileCacheInformationEx", "SeIncreaseQuotaPrivilege"},
    {82, "SystemThreadPriorityClientIdInformation", "SeIncreaseBasePriorityPrivilege"},
    {86, "SystemRefTraceInformation", nullptr},
    {87, "SystemSpecialPoolInformation", "SeDebugPrivilege"},
    {89, "SystemErrorPortInformation", "SeTcbPrivilege"},
    {91, "SystemHypervisorInformation", nullptr},
    {92, "SystemVerifierInformationEx", "SeDebugPrivilege"},
    {93, "SystemTimeZoneInformation", "SeTimeZonePrivilege"},
    {94, "SystemImageFileExecutionOptionsInformation", "SeTcbPrivilege"},
    {95, "SystemCoverageInformation", nullptr},
    {97, "SystemVerifierFaultsInformation", "SeDebugPrivilege"},
    {102, "SystemDynamicTimeZoneInformation", "SeTimeZonePrivilege"},
    {104, "SystemProcessorMicrocodeUpdateInformation", nullptr},
    {109, "SystemStoreInformation", nullptr},
    {110, "SystemRegistryAppendString", nullptr},
    {111, "SystemAitSamplingValue", "SeProfileSingleProcessPrivilege"},
    {113, "SystemCpuQuotaInformation", nullptr},
    {115, "SystemErrorPortTimeouts", "SeTcbPrivilege"},
};
// Classes that reach the host: the one settable class that needs no privilege and changes nothing but a counter.
constexpr SetClass kPassThroughClasses[] = {
    {59, "SystemComPlusPackage", nullptr},
};
}  // namespace

SetSystemInfoAction ClassifySetSystemInformation(ULONG klass, const char** name, const char** privilege) {
    if (name) *name = nullptr;
    if (privilege) *privilege = nullptr;
    for (const SetClass& c : kPassThroughClasses)
        if (c.klass == klass) {
            if (name) *name = c.name;
            return SetSystemInfoAction::PassThrough;
        }
    for (const SetClass& c : kPrivilegedClasses)
        if (c.klass == klass) {
            if (name) *name = c.name;
            if (privilege) *privilege = c.privilege;
        }
    // Every other class is refused too: an unlisted class that does change host state must never reach an elevated
    // host, and the query-only ones have nothing to set. The trace tells the two apart (named, or not).
    return SetSystemInfoAction::PrivilegeNotHeld;
}

NTSTATUS NTAPI Xl_NtSetSystemInformation(ULONG klass, PVOID info, ULONG length) {
    const char* name = nullptr;
    const char* privilege = nullptr;
    if (ClassifySetSystemInformation(klass, &name, &privilege) == SetSystemInfoAction::PassThrough) {
        void* fn = HostExport("NtSetSystemInformation");
        if (!fn) return st::NotImplemented;
        const std::uint64_t args[3] = {klass, reinterpret_cast<std::uint64_t>(info), length};
        return CallWithArgs(fn, args, 3);
    }
    if (name)
        WARN(dispatch, "NtSetSystemInformation(%s): needs %s, which the guest's token does not hold", name,
             privilege ? privilege : "a system privilege");
    else
        FIXME(dispatch, "NtSetSystemInformation(class %lu): not a class the guest may set; refused", static_cast<unsigned long>(klass));
    return kPrivilegeNotHeld;
}

}  // namespace win10xbox::dispatch::detail

namespace win10xbox::process {

std::vector<std::wstring> InheritedGuestEnvironment() {
    std::vector<std::wstring> env;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return env;
    for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
        std::wstring entry = p;
        if (!dispatch::detail::IsHostOwnedVariable(entry)) env.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(block);
    return env;
}

}  // namespace win10xbox::process
