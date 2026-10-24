/* guest_closure: the DLL/import closure of a program set, measured through the layer (docs/slices/gameclosure.md).
 *
 * argv[1] is a directory of `<name>_imports.txt` lists written by tools/game/dump_imports.py (default C:\guest\closure).
 * For every list's `module` line: when the module is a DLL that exists in that directory it is loaded
 * (LoadLibraryEx); a program's own DLLs that are not shipped are not needed, only their IMPORTS are. For every
 * `<imported dll> <symbol|#ordinal>` line the DLL is loaded (from the list directory when it is there, else by name from
 * the machine: api-ms-win-* and ext-ms-* names go through the apiset like any import) and GetProcAddress asks for the
 * symbol. Nothing is faked and nothing skipped: a missing DLL or symbol is printed.
 *
 *   closure: MISSING DLL <name> (needed by <module>)
 *   closure: MISSING <dll>!<symbol> (<module>)         also printed for every symbol of a DLL that did not load
 *   closure: <m> modules, <d> dlls, <s> symbols, <md> missing dlls, <ms> missing symbols
 *
 * Exit 42 when nothing is missing, 43 otherwise (also when the directory holds no list at all). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_CLOSED 42
#define EXIT_OPEN 43

struct dll_entry {
    char name[260];
    HMODULE h;
    int missing;
};

static struct dll_entry *g_dlls;
static size_t g_ndlls, g_capdlls;
static unsigned long g_modules, g_symbols, g_missing_dlls, g_missing_syms;
static char g_dir[MAX_PATH];

static int ends_with_i(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && _stricmp(s + a - b, suffix) == 0;
}

static int file_exists_in_dir(const char *name, char *full, size_t full_size)
{
    DWORD attr;
    if (strchr(name, '\\') || strchr(name, '/'))
        return 0;
    if (_snprintf(full, full_size, "%s\\%s", g_dir, name) < 0)
        return 0;
    full[full_size - 1] = 0;
    attr = GetFileAttributesA(full);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static HMODULE load_one(const char *name)
{
    char full[MAX_PATH + 300];
    if (file_exists_in_dir(name, full, sizeof full))
        return LoadLibraryExA(full, NULL, 0);
    return LoadLibraryExA(name, NULL, 0);
}

/* Entry of an imported DLL: loaded once, a failure is remembered. */
static struct dll_entry *find_dll(const char *name)
{
    size_t i;
    struct dll_entry *e;
    for (i = 0; i < g_ndlls; i++)
        if (_stricmp(g_dlls[i].name, name) == 0)
            return &g_dlls[i];
    if (g_ndlls == g_capdlls) {
        size_t cap = g_capdlls ? g_capdlls * 2 : 64;
        struct dll_entry *p = (struct dll_entry *)realloc(g_dlls, cap * sizeof *p);
        if (!p) {
            printf("closure: out of memory\n");
            exit(EXIT_OPEN);
        }
        g_dlls = p;
        g_capdlls = cap;
    }
    e = &g_dlls[g_ndlls++];
    memset(e, 0, sizeof *e);
    strncpy(e->name, name, sizeof e->name - 1);
    e->h = load_one(name);
    if (!e->h) {
        e->missing = 1;
        g_missing_dlls++;
    }
    return e;
}

static void check_list(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024], module[300] = "";
    char reported_dll[300] = "";
    if (!f) {
        printf("closure: cannot open %s\n", path);
        g_missing_dlls++;
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char a[300], b[600];
        unsigned long long size;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n || line[0] == '#')
            continue;
        if (line[0] != ' ') {
            if (sscanf(line, "module %299s %llu", module, &size) < 1)
                continue;
            g_modules++;
            reported_dll[0] = 0;
            if (ends_with_i(module, ".dll")) {
                char full[MAX_PATH + 300];
                if (file_exists_in_dir(module, full, sizeof full)) {
                    struct dll_entry *e = find_dll(module);
                    if (e->missing)
                        printf("closure: MISSING DLL %s (needed by %s: shipped in the list directory, load failed, error %lu)\n",
                               module, module, GetLastError());
                }
            }
            continue;
        }
        if (sscanf(line, " %299s %599s", a, b) != 2 || !module[0])
            continue;
        {
            struct dll_entry *e = find_dll(a);
            FARPROC p = NULL;
            g_symbols++;
            if (e->missing) {
                if (_stricmp(reported_dll, a) != 0) {
                    printf("closure: MISSING DLL %s (needed by %s)\n", a, module);
                    strncpy(reported_dll, a, sizeof reported_dll - 1);
                }
            } else if (b[0] == '#') {
                p = GetProcAddress(e->h, (LPCSTR)(ULONG_PTR)(unsigned)atoi(b + 1));
            } else {
                p = GetProcAddress(e->h, b);
            }
            if (!p) {
                g_missing_syms++;
                printf("closure: MISSING %s!%s (%s)\n", a, b, module);
            }
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    char pattern[MAX_PATH + 32], path[MAX_PATH + 300];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    const char *dir = argc > 1 ? argv[1] : "C:\\guest\\closure";
    size_t n;

    strncpy(g_dir, dir, sizeof g_dir - 1);
    n = strlen(g_dir);
    while (n > 3 && (g_dir[n - 1] == '\\' || g_dir[n - 1] == '/'))
        g_dir[--n] = 0;
    _snprintf(pattern, sizeof pattern, "%s\\*_imports.txt", g_dir);
    pattern[sizeof pattern - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("closure: no *_imports.txt in %s\n", g_dir);
        printf("closure: 0 modules, 0 dlls, 0 symbols, 0 missing dlls, 0 missing symbols\n");
        fflush(stdout);
        return EXIT_OPEN;
    }
    do {
        _snprintf(path, sizeof path, "%s\\%s", g_dir, fd.cFileName);
        path[sizeof path - 1] = 0;
        check_list(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("closure: %lu modules, %lu dlls, %lu symbols, %lu missing dlls, %lu missing symbols\n", g_modules,
           (unsigned long)g_ndlls, g_symbols, g_missing_dlls, g_missing_syms);
    fflush(stdout);
    return (g_missing_dlls || g_missing_syms || !g_modules) ? EXIT_OPEN : EXIT_CLOSED;
}
