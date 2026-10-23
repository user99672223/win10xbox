/*
 * nowine: nothing in the system DLLs the layer loads for this process may say
 * "Wine". For every loaded module (EnumProcessModules, plus the loader's own
 * list in the PEB, which must agree):
 *   - the export directory: no exported name contains "wine", case-insensitive.
 *     The genuine Windows names WinExec, LsaNtStatusToWinError and the
 *     *WinEvent* hook functions contain it by accident and are not counted;
 *   - the version resource: no "Wine" in any of its strings (and the
 *     FileDescription / FileVersion / ProductName are printed);
 *   - the read-only data sections (.rdata, .rsrc, ... everything that is
 *     neither writable nor executable): the ASCII and UTF-16 substrings
 *     "Wine ", "wine_" and "WINE" do not occur (WINEVENT, the Windows hook
 *     name, is not counted).
 * ntdll must not resolve wine_get_version. One line per module is printed;
 * the program returns 42 when everything is clean, 1 otherwise, after printing
 * every hit as "nowine: HIT ...". This executable itself is not scanned: it
 * has to spell the patterns.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include <winternl.h>

#define MAX_MODS 512

static int g_hits;

typedef BOOL(WINAPI* PFN_EnumProcessModules)(HANDLE, HMODULE*, DWORD, LPDWORD);

static void show(const unsigned char* p, size_t n, int wide, char* out, size_t outsz) {
  size_t i, o = 0;
  for (i = 0; i < n && o + 1 < outsz; i++) {
    unsigned char c = wide ? p[i * 2] : p[i];
    out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
  }
  out[o] = 0;
}

static void hit(const char* module, const char* where, const char* what, const char* ctx) {
  g_hits++;
  printf("nowine: HIT %s %s %s \"%s\"\n", module, where, what, ctx);
}

static int is_legit_wine_name(const char* name) {
  /* strip the genuine Windows names that contain the letters; whatever is left must not contain "wine" */
  static const char* const legit[] = {"WinExec", "WinError", "WinEvent"};
  char buf[520];
  size_t i, n = strlen(name);
  size_t k;
  if (n > 512) n = 512;
  for (i = 0; i < n; i++) buf[i] = name[i];
  buf[n] = 0;
  for (k = 0; k < sizeof legit / sizeof legit[0]; k++) {
    char* at;
    while ((at = strstr(buf, legit[k])) != NULL) memset(at, '_', strlen(legit[k]));
  }
  for (i = 0; buf[i]; i++)
    if ((buf[i] | 0x20) == 'w' && (buf[i + 1] | 0x20) == 'i' && (buf[i + 2] | 0x20) == 'n' && (buf[i + 3] | 0x20) == 'e') return 0;
  return 1;
}

static int readable(const void* p, size_t n) {
  /* the whole range [p, p+n) is committed and readable */
  const unsigned char* a = (const unsigned char*)p;
  const unsigned char* end = a + n;
  while (a < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & 0xFF)) return 0;
    a = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  }
  return 1;
}

/* value of a "key" string of a VS_VERSIONINFO blob (UTF-16), or NULL */
static const WCHAR* version_string(const unsigned char* blob, DWORD size, const WCHAR* key) {
  size_t klen = wcslen(key), i;
  for (i = 6; i + (klen + 1) * 2 + 4 <= size; i += 2) {
    if (memcmp(blob + i, key, (klen + 1) * 2) == 0) {
      size_t v = (i + (klen + 1) * 2 + 3) & ~(size_t)3;   /* the value follows the key, DWORD aligned */
      return v < size ? (const WCHAR*)(blob + v) : NULL;
    }
  }
  return NULL;
}

static void scan_section(const char* mname, const char* sname, const unsigned char* p, size_t n) {
  static const char* const pat[] = {"Wine ", "wine_", "WINE"};
  size_t k, i;
  for (k = 0; k < 3; k++) {
    size_t pl = strlen(pat[k]);
    int wide;
    for (wide = 0; wide < 2; wide++) {
      size_t step = wide ? 2 : 1, plen = pl * step;
      if (n < plen) continue;
      for (i = 0; i + plen <= n; i++) {
        size_t j;
        if (p[i] != (unsigned char)pat[k][0]) continue;
        for (j = 0; j < pl; j++)
          if (p[i + j * step] != (unsigned char)pat[k][j] || (wide && p[i + j * 2 + 1] != 0)) break;
        if (j != pl) continue;
        if (k == 2) { /* WINEVENT is the Windows hook name */
          const unsigned char* q = p + i + pl * step;
          if (i + plen + 4 * step <= n && q[0] == 'V' && q[step] == 'E' && q[2 * step] == 'N' && q[3 * step] == 'T') continue;
        }
        {
          char ctx[96], where[64];
          size_t from = i > 24 * step ? i - 24 * step : 0, len = (n - from) / step;
          if (len > 64) len = 64;
          show(p + from, len, wide, ctx, sizeof ctx);
          sprintf(where, "%s/%s", sname, wide ? "utf16" : "ascii");
          hit(mname, where, pat[k], ctx);
        }
      }
    }
  }
}

static void check_module(HMODULE mod, int is_self) {
  const unsigned char* base = (const unsigned char*)mod;
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  const IMAGE_NT_HEADERS64* nt;
  const IMAGE_SECTION_HEADER* sec;
  char mname[MAX_PATH], shortname[64];
  WCHAR wpath[MAX_PATH];
  unsigned i, nnames = 0, nfuncs = 0, nsec_scanned = 0;
  size_t scanned = 0;
  const WCHAR *fdesc = NULL, *fver = NULL, *pname = NULL;
  const char* slash;
  int hits_before = g_hits;

  wpath[0] = 0;
  GetModuleFileNameW(mod, wpath, MAX_PATH);
  WideCharToMultiByte(CP_ACP, 0, wpath, -1, mname, sizeof mname, NULL, NULL);
  slash = strrchr(mname, '\\');
  strncpy(shortname, slash ? slash + 1 : mname, sizeof shortname - 1);
  shortname[sizeof shortname - 1] = 0;
  if (!shortname[0]) strcpy(shortname, "?");

  if (!readable(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
    printf("nowine: FAIL %s: no DOS header at %p\n", shortname, (const void*)mod);
    g_hits++;
    return;
  }
  nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
  if (!readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) {
    printf("nowine: FAIL %s: no NT header\n", shortname);
    g_hits++;
    return;
  }

  /* export names */
  if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size) {
    const IMAGE_EXPORT_DIRECTORY* ed =
        (const IMAGE_EXPORT_DIRECTORY*)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    if (readable(ed, sizeof *ed)) {
      const DWORD* names = (const DWORD*)(base + ed->AddressOfNames);
      nnames = ed->NumberOfNames;
      nfuncs = ed->NumberOfFunctions;
      if (nnames && readable(names, nnames * sizeof(DWORD))) {
        for (i = 0; i < nnames; i++) {
          const char* nm = (const char*)(base + names[i]);
          if (!readable(nm, 1)) continue;
          if (!is_legit_wine_name(nm)) hit(shortname, "export-name", "wine", nm);
        }
      }
    }
  }

  /* version resource */
  if (!is_self) {
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    if (r) {
      DWORD sz = SizeofResource(mod, r);
      HGLOBAL g = LoadResource(mod, r);
      const unsigned char* blob = g ? (const unsigned char*)LockResource(g) : NULL;
      if (blob && sz > 40) {
        DWORD o;
        fdesc = version_string(blob, sz, L"FileDescription");
        fver = version_string(blob, sz, L"FileVersion");
        pname = version_string(blob, sz, L"ProductName");
        for (o = 0; o + 8 <= sz; o += 2)
          if (blob[o] == 'W' && blob[o + 1] == 0 && blob[o + 2] == 'i' && blob[o + 3] == 0 && blob[o + 4] == 'n' && blob[o + 5] == 0 &&
              blob[o + 6] == 'e' && blob[o + 7] == 0) {
            char ctx[96];
            show(blob + (o > 40 ? o - 40 : 0), 64, 1, ctx, sizeof ctx);
            hit(shortname, "version-resource", "Wine", ctx);
          }
      }
    }
  }

  /* read-only data */
  sec = IMAGE_FIRST_SECTION(nt);
  if (!is_self) {
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
      DWORD ch = sec[i].Characteristics;
      size_t len = sec[i].Misc.VirtualSize;
      char sname[9];
      if ((ch & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) || !(ch & IMAGE_SCN_MEM_READ)) continue;
      if (sec[i].SizeOfRawData > len && !len) len = sec[i].SizeOfRawData;
      if (!len || !readable(base + sec[i].VirtualAddress, len)) continue;
      memcpy(sname, sec[i].Name, 8);
      sname[8] = 0;
      scan_section(shortname, sname, base + sec[i].VirtualAddress, len);
      scanned += len;
      nsec_scanned++;
    }
  }

  {
    char d[96] = "-", v[48] = "-", p[64] = "-";
    if (fdesc) WideCharToMultiByte(CP_ACP, 0, fdesc, -1, d, sizeof d, NULL, NULL);
    if (fver) WideCharToMultiByte(CP_ACP, 0, fver, -1, v, sizeof v, NULL, NULL);
    if (pname) WideCharToMultiByte(CP_ACP, 0, pname, -1, p, sizeof p, NULL, NULL);
    printf("nowine: %s%s exports=%u named=%u sections=%u bytes=%lu version=%s desc=[%s] product=[%s] hits=%d\n", shortname,
           is_self ? " (self, not scanned)" : "", nfuncs, nnames, nsec_scanned, (unsigned long)scanned, v, d, p, g_hits - hits_before);
  }
}

/* "nowine.exe all": load every other system DLL first, to list what the rest of the set still says (not gated) */
static const WCHAR* const k_more[] = {
    L"advapi32.dll", L"sechost.dll", L"msvcrt.dll", L"user32.dll", L"gdi32.dll", L"win32u.dll", L"imm32.dll", L"shell32.dll",
    L"shlwapi.dll", L"shcore.dll", L"comctl32.dll", L"comdlg32.dll", L"rpcrt4.dll", L"ole32.dll", L"oleaut32.dll", L"combase.dll",
    L"ws2_32.dll", L"winmm.dll", L"version.dll", L"bcrypt.dll", L"cryptbase.dll", L"mpr.dll", L"netapi32.dll", L"netutils.dll",
    L"iphlpapi.dll", L"dnsapi.dll", L"nsi.dll", L"pdh.dll", L"cfgmgr32.dll", L"hid.dll", L"secur32.dll", L"winbrand.dll",
    L"wtsapi32.dll", L"coml2.dll", L"wbemprox.dll"};

int main(int argc, char** argv) {
  HMODULE mods[MAX_MODS];
  int n = 0, i, j;
  DWORD needed = 0;
  HMODULE self = GetModuleHandleW(NULL);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  PFN_EnumProcessModules enum_mods =
      k32 ? (PFN_EnumProcessModules)(void*)GetProcAddress(k32, "K32EnumProcessModules") : NULL;
  int from_api = 0, from_peb = 0;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc > 1 && strcmp(argv[1], "all") == 0) {
    size_t k;
    for (k = 0; k < sizeof k_more / sizeof k_more[0]; k++)
      if (!LoadLibraryW(k_more[k])) printf("nowine: %ls did not load (error %lu)\n", k_more[k], GetLastError());
  }

  if (enum_mods && enum_mods(GetCurrentProcess(), mods, sizeof mods, &needed)) {
    from_api = (int)(needed / sizeof(HMODULE));
    if (from_api > MAX_MODS) from_api = MAX_MODS;
    n = from_api;
  } else {
    printf("nowine: EnumProcessModules unavailable or failed (error %lu), using the loader list only\n", GetLastError());
  }
  {
    /* the loader's own list: it must name the same modules */
    PEB* peb = (PEB*)__readgsqword(0x60);
    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    LIST_ENTRY* e;
    for (e = head->Flink; e != head && n < MAX_MODS; e = e->Flink) {
      LDR_DATA_TABLE_ENTRY* ent = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
      HMODULE m = (HMODULE)ent->DllBase;
      int known = 0;
      if (!m) continue;
      from_peb++;
      for (j = 0; j < n; j++)
        if (mods[j] == m) known = 1;
      if (!known) {
        printf("nowine: module %p is in the loader list but not in EnumProcessModules\n", (void*)m);
        mods[n++] = m;
      }
    }
  }
  printf("nowine: %d modules from EnumProcessModules, %d from the loader list\n", from_api, from_peb);

  for (i = 0; i < n; i++) check_module(mods[i], mods[i] == self);

  if (!ntdll) {
    printf("nowine: FAIL no ntdll.dll\n");
    g_hits++;
  } else {
    /* the name is assembled so that it does not sit in this image as one string */
    char name[24];
    memcpy(name, "wine_", 5);
    strcpy(name + 5, "get_version");
    if (GetProcAddress(ntdll, name)) {
      printf("nowine: HIT ntdll.dll GetProcAddress resolves %s\n", name);
      g_hits++;
    } else {
      printf("nowine: ntdll.dll does not resolve %s\n", name);
    }
  }

  if (g_hits) {
    printf("nowine: FAIL %d hit(s) in %d modules\n", g_hits, n);
    return 1;
  }
  printf("nowine: done modules=%d hits=0\n", n);
  return 42;
}
