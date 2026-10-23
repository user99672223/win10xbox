/* guest_named_objects: named kernel objects are shared with the children a guest starts, and the object manager
 * namespace lists them.
 *
 * Without arguments it is the parent:
 *   1. creates a named event ("Local\w10x_no_event") and a named section ("Local\w10x_no_section") holding a string,
 *      and an event in the global namespace ("Global\w10x_no_global");
 *   2. lists \Sessions\<session>\BaseNamedObjects and \BaseNamedObjects with NtQueryDirectoryObject and prints the
 *      objects it finds there (the Event and the Section by name and type);
 *   3. starts itself as "child" (CreateProcess: NtCreateUserProcess starts another host process) and waits for the
 *      event the child sets, then for the child's exit code.
 * The child opens the event, the section and the global event by name only, reads the string out of the section,
 * sets both events and exits 7.
 * The parent exits 42 when all of that worked, 1 otherwise. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define EVENT_NAME   L"Local\\w10x_no_event"
#define SECTION_NAME L"Local\\w10x_no_section"
#define GLOBAL_NAME  L"Global\\w10x_no_global"
#define PAYLOAD      "named-objects-section-data-42"

typedef struct { UNICODE_STRING Name, TypeName; } DIR_ENTRY;
typedef NTSTATUS (NTAPI *PFN_OpenDir)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef NTSTATUS (NTAPI *PFN_QueryDir)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_Close)(HANDLE);

static unsigned session_id(void) {
    /* PEB.SessionId (x64 offset 0x2C0): the number kernelbase puts in "\Sessions\<n>\BaseNamedObjects". */
    return *(ULONG*)((char*)NtCurrentTeb()->ProcessEnvironmentBlock + 0x2C0);
}

/* Prints every Event / Section named "w10x_no_*" in directory `path`; returns how many were found. */
static int list_dir(const WCHAR* path) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_OpenDir open_dir = (PFN_OpenDir)GetProcAddress(nt, "NtOpenDirectoryObject");
    PFN_QueryDir query = (PFN_QueryDir)GetProcAddress(nt, "NtQueryDirectoryObject");
    PFN_Close close_h = (PFN_Close)GetProcAddress(nt, "NtClose");
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE dir = NULL;
    static char buffer[65536];
    ULONG context = 0, ret = 0;
    int found = 0;
    NTSTATUS s;
    BOOLEAN restart = TRUE;

    name.Buffer = (PWSTR)path;
    name.Length = (USHORT)(wcslen(path) * sizeof(WCHAR));
    name.MaximumLength = (USHORT)(name.Length + sizeof(WCHAR));
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    s = open_dir(&dir, 1 /* DIRECTORY_QUERY */, &oa);
    if (s) {
        printf("named: NtOpenDirectoryObject(%ls) failed %08lx\n", path, (unsigned long)s);
        return 0;
    }
    for (;;) {
        DIR_ENTRY* e;
        s = query(dir, buffer, sizeof buffer, FALSE, restart, &context, &ret);
        restart = FALSE;
        if (s != 0 && s != 0x105 /* STATUS_MORE_ENTRIES */) break;
        for (e = (DIR_ENTRY*)buffer; e->Name.Buffer; e++) {
            if (e->Name.Length >= 10 * sizeof(WCHAR) && !wcsncmp(e->Name.Buffer, L"w10x_no_", 8)) {
                printf("named: %ls has %.*ls [%.*ls]\n", path, (int)(e->Name.Length / 2), e->Name.Buffer,
                       (int)(e->TypeName.Length / 2), e->TypeName.Buffer);
                found++;
            }
        }
        if (s != 0x105) break;
    }
    close_h(dir);
    return found;
}

static int child_main(void) {
    HANDLE ev, sec, gev;
    char* view;
    int ok = 1;

    ev = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EVENT_NAME);
    if (!ev) { printf("named-child: OpenEvent failed, error %lu\n", GetLastError()); return 1; }
    sec = OpenFileMappingW(FILE_MAP_READ, FALSE, SECTION_NAME);
    if (!sec) { printf("named-child: OpenFileMapping failed, error %lu\n", GetLastError()); return 1; }
    gev = OpenEventW(EVENT_MODIFY_STATE, FALSE, GLOBAL_NAME);
    if (!gev) { printf("named-child: OpenEvent(global) failed, error %lu\n", GetLastError()); return 1; }
    view = (char*)MapViewOfFile(sec, FILE_MAP_READ, 0, 0, 0);
    if (!view) { printf("named-child: MapViewOfFile failed, error %lu\n", GetLastError()); return 1; }
    printf("named-child: data=[%s]\n", view);
    if (strcmp(view, PAYLOAD)) ok = 0;
    /* A name nobody created is not found, and not by accident of some other object. */
    if (OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\w10x_no_such_event") || GetLastError() != ERROR_FILE_NOT_FOUND) {
        printf("named-child: a missing event was found or reported error %lu\n", GetLastError());
        ok = 0;
    }
    /* Creating the same name again opens the parent's object. */
    {
        HANDLE again = CreateEventW(NULL, TRUE, FALSE, EVENT_NAME);
        if (!again || GetLastError() != ERROR_ALREADY_EXISTS) {
            printf("named-child: CreateEvent of an existing name gave error %lu\n", GetLastError());
            ok = 0;
        }
    }
    if (!SetEvent(gev)) ok = 0;
    if (!SetEvent(ev)) ok = 0;
    UnmapViewOfFile(view);
    fflush(stdout);
    return ok ? 7 : 1;
}

int main(int argc, char** argv) {
    HANDLE ev, sec, gev, wait_for_child[1];
    char* view;
    wchar_t path[MAX_PATH], cmdline[MAX_PATH * 2];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD w, code = 0;
    int good = 1;

    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();

    ev = CreateEventW(NULL, TRUE, FALSE, EVENT_NAME);
    if (!ev || GetLastError() == ERROR_ALREADY_EXISTS) { printf("named: CreateEvent failed, error %lu\n", GetLastError()); return 1; }
    gev = CreateEventW(NULL, TRUE, FALSE, GLOBAL_NAME);
    if (!gev) { printf("named: CreateEvent(global) failed, error %lu\n", GetLastError()); return 1; }
    sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, SECTION_NAME);
    if (!sec) { printf("named: CreateFileMapping failed, error %lu\n", GetLastError()); return 1; }
    view = (char*)MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 0);
    if (!view) { printf("named: MapViewOfFile failed, error %lu\n", GetLastError()); return 1; }
    strcpy(view, PAYLOAD);
    printf("named: created event, section and global event\n");

    /* Both directories the names live in answer a listing. */
    swprintf(path, MAX_PATH, L"\\Sessions\\%u\\BaseNamedObjects", session_id());
    if (list_dir(path) < 2) { printf("named: the session directory lists fewer than two objects\n"); good = 0; }
    if (list_dir(L"\\BaseNamedObjects") < 1) { printf("named: \\BaseNamedObjects does not list the global event\n"); good = 0; }

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    GetModuleFileNameW(NULL, path, MAX_PATH);
    swprintf(cmdline, MAX_PATH * 2, L"\"%ls\" child", path);
    fflush(stdout);   /* the child writes to the same pipe: keep the order */
    if (!CreateProcessW(path, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("named: CreateProcess failed, error %lu\n", GetLastError());
        return 1;
    }
    wait_for_child[0] = ev;
    w = WaitForSingleObject(ev, 60000);
    if (w != WAIT_OBJECT_0) { printf("named: the child never set the event (wait %lu)\n", w); good = 0; }
    else printf("named: event set by the child\n");
    if (WaitForSingleObject(gev, 1000) != WAIT_OBJECT_0) { printf("named: the global event was not set\n"); good = 0; }
    w = WaitForSingleObject(pi.hProcess, 60000);
    if (w != WAIT_OBJECT_0 || !GetExitCodeProcess(pi.hProcess, &code)) { printf("named: child wait %lu\n", w); good = 0; }
    else printf("named: child exit %lu\n", code);
    if (code != 7) good = 0;
    (void)wait_for_child;
    printf("named: %s\n", good ? "done" : "FAILED");
    fflush(stdout);
    return good ? 42 : 1;
}
