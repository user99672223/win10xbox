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

bool IsSocketHandle(HANDLE h);
NTSTATUS Close(HANDLE h);   // closes the host socket and the emulated handle

// NtDeviceIoControlFile: sockets are served here, any other handle goes to the host ntdll.
NTSTATUS NTAPI Afd_NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                         PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out,
                                         ULONG out_len);

}  // namespace win10xbox::afd
