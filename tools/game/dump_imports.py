#!/usr/bin/env python3
"""dump_imports.py: write the import tables of every x64 PE in a directory as plain text (python3, standard library only).

    python3 tools/game/dump_imports.py <input dir or file>... <output dir>

Walks every input directory recursively (files named *.exe *.dll *.ocx *.ax *.cpl *.drv *.sys), parses the PE32+ (x64)
import directory and the delay-load import directory, and writes one `<file name>_imports.txt` per PE into <output dir>
(`RocketLeague.exe` -> `RocketLeague.exe_imports.txt`; a second file of the same name in another folder is written as
`<relative path with / replaced by __>_imports.txt`). PE32 (x86) and non-PE files are named on stderr and skipped.
Nothing but names and one size is written: no hashes, no section data, no strings other than the import names.

File format (UTF-8 text, LF; consumed by tests/guest/closure.c):

    # win10xbox imports v1
    module <file name> <size in bytes>
      <imported dll, lower case> <symbol>        one import per line, sorted, deduped
      <imported dll, lower case> #<ordinal>      an import by ordinal
    ...

Lines starting with '#' are comments. A symbol never contains a space. `module` appears once per file. Delay-load
imports are listed with the ordinary ones: for the closure a symbol is needed either way.
"""
import os
import struct
import sys

PE_EXTS = (".exe", ".dll", ".ocx", ".ax", ".cpl", ".drv", ".sys")


class NotPE(Exception):
    pass


def parse_sections(data, nsec, off):
    secs = []
    for i in range(nsec):
        name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, off + 40 * i)
        secs.append((va, max(vsize, rsize), rptr, rsize))
    return secs


def make_rva2off(data, secs):
    def rva2off(rva):
        for va, size, rptr, rsize in secs:
            if va <= rva < va + size:
                o = rptr + (rva - va)
                if o >= len(data):
                    raise NotPE("RVA outside file")
                return o
        raise NotPE("RVA 0x%x in no section" % rva)
    return rva2off


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1")


def read_thunks(data, rva2off, rva):
    """Yield symbol strings from an import name table (8-byte PE32+ thunks) until the zero terminator."""
    o = rva2off(rva)
    while True:
        (t,) = struct.unpack_from("<Q", data, o)
        o += 8
        if t == 0:
            return
        if t >> 63:
            yield "#%d" % (t & 0xFFFF)
        else:
            yield cstr(data, rva2off(t & 0x7FFFFFFF) + 2)   # skip the 2-byte hint


def parse_imports(data):
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise NotPE("not MZ")
    (lfanew,) = struct.unpack_from("<I", data, 0x3C)
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        raise NotPE("no PE signature")
    machine, nsec, _t, _p, _n, optsize, _c = struct.unpack_from("<HHIIIHH", data, lfanew + 4)
    opt = lfanew + 24
    (magic,) = struct.unpack_from("<H", data, opt)
    if magic != 0x20B:
        raise NotPE("PE32 (not x64), machine 0x%x" % machine)
    (imagebase,) = struct.unpack_from("<Q", data, opt + 24)
    (ndd,) = struct.unpack_from("<I", data, opt + 108)
    dd = opt + 112
    secs = parse_sections(data, nsec, opt + optsize)
    rva2off = make_rva2off(data, secs)
    out = set()

    def dirent(i):
        if i >= ndd:
            return 0, 0
        return struct.unpack_from("<II", data, dd + 8 * i)

    rva, size = dirent(1)                        # IMAGE_DIRECTORY_ENTRY_IMPORT
    if rva:
        o = rva2off(rva)
        while True:
            oft, _ts, _fc, name, ft = struct.unpack_from("<IIIII", data, o)
            o += 20
            if name == 0 and ft == 0:
                break
            dll = cstr(data, rva2off(name)).lower()
            for s in read_thunks(data, rva2off, oft or ft):
                out.add((dll, s))
    rva, size = dirent(13)                       # IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
    if rva:
        o = rva2off(rva)
        while True:
            attrs, name, _mod, _iat, int_, _b, _u, _ts = struct.unpack_from("<IIIIIIII", data, o)
            o += 32
            if name == 0:
                break
            if not attrs & 1:                    # pre-VC7 descriptors hold virtual addresses, not RVAs
                name -= imagebase
                int_ -= imagebase
            dll = cstr(data, rva2off(name)).lower()
            for s in read_thunks(data, rva2off, int_):
                out.add((dll, s))
    return sorted(out)


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    *inputs, outdir = argv[1:]
    os.makedirs(outdir, exist_ok=True)
    files = []
    for inp in inputs:
        if os.path.isdir(inp):
            for root, _dirs, names in os.walk(inp):
                for n in sorted(names):
                    if n.lower().endswith(PE_EXTS):
                        full = os.path.join(root, n)
                        files.append((full, os.path.relpath(full, inp)))
        else:
            files.append((inp, os.path.basename(inp)))
    used = set()
    nmod = nimp = 0
    for full, rel in sorted(files):
        base = os.path.basename(full)
        try:
            with open(full, "rb") as f:
                data = f.read()
            imports = parse_imports(data)
        except (NotPE, struct.error, ValueError, OSError) as e:
            sys.stderr.write("dump_imports: skip %s: %s\n" % (rel, e))
            continue
        outname = base + "_imports.txt"
        if outname.lower() in used:
            outname = rel.replace(os.sep, "__").replace("/", "__") + "_imports.txt"
        used.add(outname.lower())
        with open(os.path.join(outdir, outname), "w", encoding="utf-8", newline="\n") as f:
            f.write("# win10xbox imports v1\nmodule %s %d\n" % (base.replace(" ", "_"), len(data)))
            for dll, sym in imports:
                f.write("  %s %s\n" % (dll, sym))
        nmod += 1
        nimp += len(imports)
    print("dump_imports: %d modules, %d imports -> %s" % (nmod, nimp, outdir))
    return 0 if nmod else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
