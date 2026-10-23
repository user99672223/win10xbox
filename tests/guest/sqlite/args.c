/* guest_args: the command line a guest sees. Found by running the SQLite shell: `sqlite3.exe -batch :memory: "select
 * 6*7;"` parsed its last argument as "select 6*7;C:\guest\sqlite3.exeWinSta0\Default...". The process parameters
 * block packs its strings back to back and the layer built every UNICODE_STRING with MaximumLength == Length, so
 * nothing in the block was NUL-terminated and GetCommandLineW() (which returns CommandLine.Buffer as is) ran into
 * the next string. No earlier guest took an argument, so nothing noticed.
 *
 * Run as:  guest_args.exe one "two words" three      Exit code 42 when everything holds, 1 otherwise. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winternl.h>

static int g_bad;

static void check_terminated(const char* what, const UNICODE_STRING* s) {
    size_t len = s->Buffer ? wcsnlen(s->Buffer, (size_t)s->Length / 2 + 64) : 0;
    int ok = len == (size_t)s->Length / 2;
    printf("args: %s length=%u strlen=%u terminated=%d\n", what, (unsigned)(s->Length / 2), (unsigned)len, ok);
    if (!ok) g_bad = 1;
}

int main(int argc, char** argv) {
    PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    RTL_USER_PROCESS_PARAMETERS* pp = peb->ProcessParameters;
    int i;

    printf("args: argc=%d\n", argc);
    for (i = 1; i < argc; ++i) printf("args: argv[%d]=%s\n", i, argv[i]);
    printf("args: cmdline=%ls\n", GetCommandLineW());
    check_terminated("CommandLine", &pp->CommandLine);
    check_terminated("ImagePathName", &pp->ImagePathName);
    if (GetCommandLineW() != pp->CommandLine.Buffer) {
        printf("args: GetCommandLineW() is not the PEB's CommandLine.Buffer\n");
        g_bad = 1;
    }
    if (argc != 4 || strcmp(argv[1], "one") || strcmp(argv[2], "two words") || strcmp(argv[3], "three")) g_bad = 1;
    fflush(stdout);
    return g_bad ? 1 : 42;
}
