// win10xbox networking: Wine's ws2_32 talks to \Device\Afd with its own ioctl set (IOCTL_AFD_WINE_*, plus a few
// real AFD codes); on a Windows host the real AFD speaks a different protocol, so this module is the "AFD" the guest
// sees. Each guest socket is an emulated handle backed by a host Winsock SOCKET (docs/DECISIONS.md, N1).
// Owned by core/afd/.
#pragma once
#include <windows.h>
#include <winternl.h>

namespace win10xbox::afd {

// NtOpenFile on "\Device\Afd[\...]": true when the object name is ours.
bool IsAfdDeviceName(const OBJECT_ATTRIBUTES* attr);
// Creates the (still unconfigured) socket handle; IOCTL_AFD_WINE_CREATE then picks family/type/protocol.
NTSTATUS Open(PHANDLE handle, PIO_STATUS_BLOCK io);

// WSAStartup on the host Winsock, once per process (ws2_32's unix calls need it before any socket exists).
void EnsureWinsock();

// Loads everything the host Winsock fetches lazily (mswsock, the name-resolution providers, dnsapi, nsi...) by doing one
// throw-away socket of each kind and one lookup. Must run before the PEB take-over: afterwards the host's loader
// state (PEB.Ldr, activation context) is the guest's and a host LoadLibrary from inside a Winsock call fails, which
// shows up as WSAHOST_NOT_FOUND for "localhost". Failures are ignored here (no network capability is not an error).
void PrimeHost();

// The TEB is shared by host and guest and TEB.WinSockData (0x1738) belongs to whichever ws2_32 is running: the host's
// native ws2_32 keeps its per-thread block there, Wine's guest ws2_32 keeps its event cache there, and each reads the
// other's value as its own (an access violation in the guest's get_sync_event). Every call into the host Winsock
// is therefore bracketed by this scope, which installs the host's value for this thread and puts the guest's back.
class HostWinsockScope {
public:
    HostWinsockScope();
    ~HostWinsockScope();
    HostWinsockScope(const HostWinsockScope&) = delete;
    HostWinsockScope& operator=(const HostWinsockScope&) = delete;
private:
    void* guest_value_;
};

bool IsSocketHandle(HANDLE h);
NTSTATUS Close(HANDLE h);   // closes the host socket and the emulated handle

// NtDeviceIoControlFile: sockets are served here, any other handle goes to the host ntdll.
NTSTATUS NTAPI Afd_NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                         PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out,
                                         ULONG out_len);

}  // namespace win10xbox::afd
