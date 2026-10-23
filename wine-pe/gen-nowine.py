#!/usr/bin/env python3
"""Regenerate patches/0006 and patches/0007 from dlls.txt and version_strings.txt.

    wine-pe/gen-nowine.py [--winebuild PATH]

Both patches touch a few dozen files of Wine's tree in the same mechanical way
(one line per exported Wine-internal function, one description per module), so
they are generated, not written by hand; the checked-in patches are what the
build applies and this script is how they are rebuilt when the Wine pin moves.

It needs wine-src with patches/0001..0005 applied (fetch-wine.sh) and a
configured build tree whose tools/winebuild/winebuild exists (build.sh built at
least one module). It edits wine-src in place, writes the two patches and
leaves the tree as the patches would have left it. Patches 0006..0008 that are
applied already are taken off first.

0006  Every export of a DLL in dlls.txt whose name starts with "wine_" or
      "__wine_" becomes "<ordinal> <type> ... -noname name", i.e. it stays in
      the export table at the ordinal it had, but without its name. The entry
      that has ordinal 1 gets that number written out too: winebuild takes the
      lowest explicit ordinal as the table's base, and the base must stay 1.
      The few places that looked such a function up by name (the unix-call
      dispatcher in libs/winecrt0, the syscall dispatcher in win32u, the IME window
      procedure in user32, and the Wine detection of the conformance suites in
      include/wine/test.h) look it up by the same ordinal instead.
0008  Names that only Wine's own plumbing uses and that a program can see anyway
      (thread names, a pipe name, environment variable names, a registry resource
      type, the builtin-module signature, two CRT message strings) lose the word
      "wine"; each is renamed in every place that has to agree.
0007  Windows 10 22H2 version resources: FileVersion/ProductVersion
      10.0.19045.3803, ProductName, CompanyName, LegalCopyright for all modules
      (include/ntverp.h.in, include/common.ver), FileDescription per module from
      version_strings.txt (each module's Makefile.in, or its own version.rc).
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "wine-src")
PATCHES = os.path.join(HERE, "patches")

PRODUCT_NAME = '"Microsoft\u00ae Windows\u00ae Operating System"'
PRODUCT_VERSION = "10,0,19045,3803"
COPYRIGHT = '"\u00a9 Microsoft Corporation. All rights reserved."'


def modules():
    """(dir, file) for every dlls.txt line whose source dir exists."""
    out = []
    for line in open(os.path.join(HERE, "dlls.txt")):
        line = line.split("#")[0]
        line = re.sub(r"\s+", "", line)
        if not line:
            continue
        spec, _, fname = line.partition("=")
        d = spec if "/" in spec else "dlls/" + spec
        if not fname:
            base = os.path.basename(d)
            fname = base + (".exe" if d.startswith("programs/") else ".dll")
        if os.path.isdir(os.path.join(SRC, d)):
            out.append((d, fname))
    return out


def sh(*cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def read(path):
    with open(os.path.join(SRC, path), newline="", encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(os.path.join(SRC, path), "w", newline="", encoding="utf-8") as f:
        f.write(text)


# ---------------------------------------------------------------- 0006: specs
WINE_NAME = re.compile(r"^(__)?wine_")


def def_ordinals(winebuild, spec_path, dll):
    """name -> ordinal as winebuild assigns them for x86_64, and the entry at ordinal 1."""
    d = sh(winebuild, "--target=x86_64-windows", "--def", "-E", os.path.join(SRC, spec_path), "-F", dll)
    ords = {}
    for m in re.finditer(r"^\s+(\S+?)(?:=\S+)?\s+@(\d+)", d, re.M):
        ords[m.group(1)] = int(m.group(2))
    return ords


ORDS = {}   # (module, export name) -> ordinal, for the call sites patched afterwards


def do_specs(winebuild):
    total = 0
    for d, fname in modules():
        spec = "%s/%s.spec" % (d, os.path.splitext(fname)[0])
        if not os.path.exists(os.path.join(SRC, spec)):
            continue
        text = read(spec)
        if not re.search(r"^@\s.*\b(__)?wine_", text, re.M):
            continue
        ords = def_ordinals(winebuild, spec, fname)
        lines = text.split("\n")
        first = min(ords.values())
        # ordinals taken by the entries that are explicit already, x86_64 view
        changed = []
        extra = 0
        for i, l in enumerate(lines):
            m = re.match(r"^@(\s+)(\S+)((?:\s+-\S+)*)\s+((?:__)?wine_\w+)(.*)$", l)
            if not m or not WINE_NAME.match(m.group(4)):
                continue
            name = m.group(4)
            if name not in ords:
                # an entry for another architecture (-arch=arm64ec ...): same number as
                # its x86_64 sibling, found by dropping the arch suffix
                # (the number must differ: both exist in an arm64ec build)
                if re.sub(r"_arm64ec$", "", name) not in ords:
                    sys.exit("no ordinal for %s in %s" % (name, spec))
                extra += 1
                ordn = max(ords.values()) + extra
            else:
                ordn = ords[name]
            lines[i] = "%d%s%s -noname %s%s" % (ordn, m.group(1), m.group(2) + m.group(3), name, m.group(5))
            changed.append(name)
        if not changed:
            continue
        # pin the base
        for i, l in enumerate(lines):
            m = re.match(r"^@(\s+)(\S+.*)$", l)
            if not m:
                continue
            # find the entry that is ordinal `first` in the def: the first '@' entry whose name has it
            nm = re.match(r"^(?:\S+)(?:\s+-\S+)*\s+(\S+?)(?:\(|\s|$)", m.group(2))
            if nm and ords.get(nm.group(1)) == first and first == 1:
                lines[i] = "1" + m.group(1) + m.group(2)
                break
        else:
            if first != 1:
                sys.exit("%s: base %d, cannot pin" % (spec, first))
        write(spec, "\n".join(lines))
        for nme in changed:
            ORDS[(os.path.splitext(fname)[0], nme)] = ords.get(nme)
        total += len(changed)
        print("%s: %d exports by ordinal only" % (spec, len(changed)))
    print("total %d" % total)


def replace_once(path, old, new, count=1):
    t = read(path)
    if t.count(old) != count:
        sys.exit("%s: expected %d x %r, found %d" % (path, count, old, t.count(old)))
    write(path, t.replace(old, new))


def do_lookups():
    """Name lookups of the functions made nameless above become ordinal lookups."""
    unixcall = ORDS[("ntdll", "__wine_unix_call_dispatcher")]
    syscall = ORDS[("ntdll", "__wine_syscall_dispatcher")]
    imewnd = ORDS[("imm32", "__wine_ime_wnd_proc")]
    servercall = ORDS[("ntdll", "wine_server_call")]
    replace_once("libs/winecrt0/unix_lib.c",
                 'static inline void *get_dispatcher( const char *name )\n{',
                 'static inline void *find_ntdll_ordinal( HMODULE module, ULONG ordinal )\n'
                 '{\n    void *proc;\n\n    return LdrGetProcedureAddress( module, NULL, ordinal, &proc ) ? NULL : proc;\n}\n\n'
                 'static inline void *get_dispatcher( const char *name )\n{')
    replace_once("libs/winecrt0/unix_lib.c",
                 'dispatcher = RtlFindExportedRoutineByName( module, "__wine_unix_call_dispatcher" );',
                 'dispatcher = find_ntdll_ordinal( module, %d );' % unixcall)
    replace_once("dlls/win32u/main.c",
                 'dispatcher_ptr = RtlFindExportedRoutineByName( ntdll, "__wine_syscall_dispatcher" );',
                 'LdrGetProcedureAddress( ntdll, NULL, %d, (void **)&dispatcher_ptr );' % syscall)
    # the conformance suites (not shipped) ask ntdll whether it is Wine's to decide about todo_wine
    replace_once("include/wine/test.h",
                 'return (GetProcAddress( module, "wine_server_call" ) != NULL);',
                 'return (GetProcAddress( module, (LPCSTR)%d ) != NULL);' % servercall)
    replace_once("dlls/user32/misc.c",
                 'GetProcAddress(imm32, "__wine_ime_wnd_proc");',
                 'GetProcAddress(imm32, (LPCSTR)%d);' % imewnd)


# ------------------------------------------------------------ 0008: names
RENAMES = [
    # (file, old, new, expected occurrences)
    # the builtin-module signature in the DOS header: written by winebuild, read by the loader
    ("tools/winebuild/spec32.c", 'builtin_signature[32] = "Wine builtin DLL"', 'builtin_signature[32] = "System builtin DLL"', 1),
    ("dlls/ntdll/loader.c", 'builtin_signature[] = "Wine builtin DLL"', 'builtin_signature[] = "System builtin DLL"', 1),
    # environment variables the unix side would set; the layer never sets them
    ("dlls/ntdll/locale.c", 'L"WINEUSERLOCALE"', 'L"NTSYS_USERLOCALE"', 1),
    ("dlls/ntdll/locale.c", ", 14, bufferW", ", 16, bufferW", 1),   # the length in bytes of the name above
    ("dlls/ntdll/loader.c", 'L"WINEBUILDDIR"', 'L"NTSYS_BUILDDIR"', 1),
    ("dlls/ntdll/loader.c", 'L"WINEDLLDIR%u"', 'L"NTSYS_DLLDIR%u"', 1),
    ("dlls/ntdll/loader.c", 'L"WINEBOOTSTRAPMODE"', 'L"NTSYS_BOOTSTRAPMODE"', 1),
    ("dlls/ntdll/loader.c", 'L"WINESYSTEMDLLPATH"', 'L"NTSYS_SYSTEMDLLPATH"', 1),
    ("dlls/kernelbase/debug.c", 'L"WINEDEBUG="', 'L"NTSYS_DBG="', 1),
    ("dlls/kernelbase/sync.c", 'L"WINEUSERNAME"', 'L"NTSYS_USERNAME"', 1),
    ("dlls/kernelbase/locale.c", 'L"WINEUSERLOCALE"', 'L"NTSYS_USERLOCALE"', 1),
    ("dlls/kernelbase/locale.c", 'L"WINEUNIXCP"', 'L"NTSYS_UNIXCP"', 1),
    ("dlls/advapi32/advapi.c", '"WINEUSERNAME"', '"NTSYS_USERNAME"', 2),   # the A and the W function
    ("programs/services/services.c", 'L"WINEBOOTSTRAPMODE"', 'L"NTSYS_BOOTSTRAPMODE"', 2),
    ("programs/wineboot/wineboot.c", 'L"WINEBUILDDIR"', 'L"NTSYS_BUILDDIR"', 1),
    # thread names (SetThreadDescription) and pipe names
    ("dlls/ntdll/threadpool.c", 'L"wine_threadpool_', 'L"threadpool_', 5),
    ("dlls/kernelbase/console.c", 'pipe\\\\wine_pty_signal_pipe', 'pipe\\\\pty_signal_pipe', 1),
    # exports looked up in a 16-bit module that does not exist here
    ("dlls/kernelbase/file.c", '"__wine_vxd_open"', '"vxd_open"', 1),
    ("dlls/kernel32/file.c", '"__wine_vxd_get_proc"', '"vxd_get_proc"', 1),
    # the C runtime's abort dialog
    ("dlls/msvcrt/exit.c", 'or Cancel to start the Wine debugger.', 'or Cancel to start the debugger.', 1),
    ("dlls/msvcrt/exit.c", 'L"Wine C++ Runtime Library"', 'L"Microsoft Visual C++ Runtime Library"', 1),
]


def do_names():
    for path, old, new, n in RENAMES:
        replace_once(path, old, new, n)
    # resource type of the registry scripts: widl writes it, the .rc files of the modules
    # name it, setupapi and winedump read it
    replace_once("libs/winecrt0/register.c", "{'W','I','N','E','_','R','E','G','I','S','T','R','Y',0}", "{'R','E','G','I','S','T','R','Y',0}")
    for path in ("tools/widl/register.c", "dlls/setupapi/fakedll.c", "tools/winedump/pe.c"):
        t = read(path)
        assert "WINE_REGISTRY" in t, path
        write(path, t.replace("WINE_REGISTRY", "REGISTRY"))
    for d, _ in modules():
        full = os.path.join(SRC, d)
        for f in sorted(os.listdir(full)):
            if f.endswith(".rc") and "WINE_REGISTRY" in read(d + "/" + f):
                write(d + "/" + f, read(d + "/" + f).replace("WINE_REGISTRY", "REGISTRY"))


# ------------------------------------------------------------ 0007: versions
def load_descriptions():
    d = {}
    for line in open(os.path.join(HERE, "version_strings.txt"), encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        name, status, desc = [x.strip() for x in line.split("|", 2)]
        d[name] = desc
    return d


def do_versions():
    # one set of values for every module
    p = "include/ntverp.h.in"
    t = read(p)
    t = t.replace(
        '#ifndef VER_PRODUCTNAME_STR\n#define VER_PRODUCTNAME_STR "@PACKAGE_STRING@"\n#endif\n',
        "#undef VER_PRODUCTNAME_STR\n#define VER_PRODUCTNAME_STR %s\n" % PRODUCT_NAME)
    t = re.sub(r"#ifndef VER_PRODUCTVERSION\n#define VER_PRODUCTVERSION [^\n]*\n#endif\n",
               lambda m: "#undef VER_PRODUCTVERSION\n#define VER_PRODUCTVERSION %s\n" % PRODUCT_VERSION, t)
    assert "10,0,19045,3803" in t and "Operating System" in t
    write(p, t)

    p = "include/common.ver"
    t = read(p)
    t = re.sub(r"#ifndef VER_LEGALCOPYRIGHT_STR\n#define VER_LEGALCOPYRIGHT_STR [^\n]*\n#endif\n",
               lambda m: "#undef VER_LEGALCOPYRIGHT_STR\n#define VER_LEGALCOPYRIGHT_STR %s\n" % COPYRIGHT, t)
    t = t.replace("#ifndef VER_FILEVERSION\n#define VER_FILEVERSION VER_PRODUCTVERSION\n#endif\n",
                  "#undef VER_FILEVERSION\n#define VER_FILEVERSION VER_PRODUCTVERSION\n")
    t = t.replace("#ifndef VER_FILEVERSION_STR\n#define VER_FILEVERSION_STR __WINE_VER_VERSTRING(VER_FILEVERSION)\n#endif\n",
                  "#undef VER_FILEVERSION_STR\n#define VER_FILEVERSION_STR __WINE_VER_VERSTRING(VER_FILEVERSION)\n")
    assert "Microsoft Corporation. All rights" in t and "#undef VER_FILEVERSION\n" in t
    write(p, t)

    desc = load_descriptions()
    for d, fname in modules():
        if fname not in desc:
            sys.exit("version_strings.txt has no line for %s" % fname)
        value = '"%s"' % desc[fname]
        mk = d + "/Makefile.in"
        rc = None
        for f in sorted(os.listdir(os.path.join(SRC, d))):
            if f.endswith(".rc") and "#define VER_FILEDESCRIPTION_STR" in read(d + "/" + f):
                rc = d + "/" + f
        if rc:
            t = re.sub(r'#define VER_FILEDESCRIPTION_STR "[^"]*"', lambda m: "#define VER_FILEDESCRIPTION_STR " + value, read(rc))
            write(rc, t)
            continue
        t = read(mk)
        # drop what Wine had: the description is ours, the version comes from ntverp.h
        t = re.sub(r"^VER_(FILEDESCRIPTION_STR|PRODUCTVERSION)\s*=.*\n", "", t, flags=re.M)
        t = re.sub(r"^(MODULE\s*=.*\n)", lambda m: m.group(1) + "VER_FILEDESCRIPTION_STR = " + value + "\n", t, count=1, flags=re.M)
        assert "VER_FILEDESCRIPTION_STR" in t, mk
        write(mk, t)
        print("%s: %s" % (mk, value))


HEADERS = {
    "0006-export-wine-names-by-ordinal.patch": """Export Wine-internal functions by ordinal only.

Every export whose name starts with wine_ or __wine_ in the .spec files of the
DLLs in dlls.txt gets "-noname": it stays in the export table at the ordinal it
had (the numbers are written out, so they no longer depend on the order of the
file), but no name for it exists in the table or in the string pool. Modules
that import these functions bind by ordinal through the import libraries
winebuild generates from the same spec. The entry that has ordinal 1 is
numbered explicitly as well: winebuild takes the lowest explicit ordinal as the
base of the export table and it has to stay 1.
Generated by wine-pe/gen-nowine.py.

""",
    "0008-neutral-internal-names.patch": """Names that only Wine's plumbing uses lose the word "wine".

Thread names, a pipe name, the environment variables the unix side would set
(the layer sets none of them), the resource type of the registry scripts, the
builtin-module signature in the DOS header (winebuild writes it, ntdll's loader
reads it) and the two strings of the C runtime's abort dialog. Each name is
changed in every place that has to agree; nothing else changes.
Generated by wine-pe/gen-nowine.py.

""",
    "0007-windows-version-resources.patch": """Version resources of the file Windows 10 22H2 x64 ships under the same name.

include/ntverp.h.in and include/common.ver stop honouring per-module values
for the version, product name and copyright: FileVersion and ProductVersion
10.0.19045.3803, ProductName "Microsoft(R) Windows(R) Operating System",
CompanyName "Microsoft Corporation", LegalCopyright "(C) Microsoft
Corporation. All rights reserved.". Each module of dlls.txt gets its
FileDescription from wine-pe/version_strings.txt (Makefile.in, or the version.rc
of kernel32); modules that had no version resource get one.
Generated by wine-pe/gen-nowine.py.

""",
}


def make_patch(name, paths=None):
    diff = sh("git", "-C", SRC, "diff", "--no-color", "--no-ext-diff")
    with open(os.path.join(PATCHES, name), "w", encoding="utf-8") as f:
        f.write(HEADERS[name] + diff)
    sh("git", "-C", SRC, "add", "-A")


def main():
    winebuild = os.path.join(HERE, "build/tools/winebuild/winebuild")
    if len(sys.argv) > 2 and sys.argv[1] == "--winebuild":
        winebuild = sys.argv[2]
    for name in ("0008-neutral-internal-names.patch", "0007-windows-version-resources.patch",
                 "0006-export-wine-names-by-ordinal.patch"):
        pf = os.path.join(PATCHES, name)
        if os.path.exists(pf) and subprocess.run(["git", "-C", SRC, "apply", "--check", "--reverse", pf],
                                                 capture_output=True).returncode == 0:
            sh("git", "-C", SRC, "apply", "--reverse", pf)
    sh("git", "-C", SRC, "add", "-A")   # baseline: the tree as patches 0001..0005 leave it
    do_specs(winebuild)
    do_lookups()
    make_patch("0006-export-wine-names-by-ordinal.patch")
    do_versions()
    make_patch("0007-windows-version-resources.patch")
    do_names()
    make_patch("0008-neutral-internal-names.patch")
    sh("git", "-C", SRC, "reset", "-q")
    print("wrote patches 0006, 0007 and 0008; wine-src now has them applied")


if __name__ == "__main__":
    main()
