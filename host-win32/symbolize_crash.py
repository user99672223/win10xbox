#!/usr/bin/env python3
"""symbolize_crash.py - look the addresses of a host-win32 crash record up in the linker map.

Used by .github/workflows/host-win32-smoke.yml (best effort, output only: it never decides a verdict) and usable
by hand on the downloaded artifact. Python 3 standard library only; ASCII output.

  symbolize_crash.py --map win10xbox.map [--log smoke-console.log] [--exe win10xbox.exe] [--dump crash.dmp] crash.txt
  symbolize_crash.py --map win10xbox.map --rva 0x236c            # one lookup, RVA relative to the load base
  symbolize_crash.py --map win10xbox.map --addr 0x7ff6a1b4236c --base 0x7ff6a1b20000

How an address becomes a function:
  RVA         = address - load base of win10xbox.exe      (the base is printed by the host on its first stderr lines:
                                                          "host-win32: host-image base=0x... size=0x...")
  map address = preferred load address + RVA              ("Preferred load address is ..." in the map header)
  function    = the last symbol of the map at or below that address (Publics by Value and Static symbols).
The map is written by the very link that wrote win10xbox.exe: its "Timestamp is" equals the PE timestamp that the
host prints as timestamp=0x... in the host-image line (this script compares them and says so).

What it reads from crash.txt (crash.cpp's records): the faulting address, the bytes at RIP, the raw stack words, and
the host module table (the host process's own modules, saved before the guest took PEB->Ldr over) and the guest
module list of the record, so a stack word inside a DLL or a guest module is attributed as well.
With --dump it also scans the faulting thread's whole saved stack (crash.dmp, a minidump) for words that point into
the host image. With --exe those words are checked for being return addresses (the bytes before them are a call).
Stack words are raw: x64 code has no frame chain, so stale values show up; "call site" is the strong evidence.
"""
import argparse
import bisect
import mmap
import os
import re
import struct
import sys

EXC_NAMES = {
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION", 0xC00000FD: "EXCEPTION_STACK_OVERFLOW",
    0xC000001D: "EXCEPTION_ILLEGAL_INSTRUCTION", 0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN", 0x80000100: "EXCEPTION_WINE_STUB",
}


def hx(v, width=0):
    return "0x%0*x" % (width, v)


def basename(path):
    return re.split(r"[\\/]", path)[-1]


# ---------------------------------------------------------------------------------------------------------------
# The linker map (MSVC layout; lld-link writes the same layout minus the 'f' flag column)
# ---------------------------------------------------------------------------------------------------------------

SYM_LINE = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{8,16})(?:\s+(.*?))?\s*$")
SEC_LINE = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})H\s+(\S+)\s+(\S+)\s*$")


class MapFile:
    def __init__(self, path):
        self.path = path
        self.timestamp = None
        self.preferred = None
        self.code_sections = set()
        self.addrs = []
        self.syms = []   # sorted by address: (addr, name, flags, lib:object, section)
        self._parse()

    def _parse(self):
        raw = []
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                m = re.search(r"Timestamp is ([0-9A-Fa-f]+)", line)
                if m and self.timestamp is None:
                    self.timestamp = int(m.group(1), 16)
                m = re.search(r"Preferred load address is ([0-9A-Fa-f]+)", line)
                if m and self.preferred is None:
                    self.preferred = int(m.group(1), 16)
                m = SEC_LINE.match(line)
                if m:
                    if m.group(5).upper() == "CODE":
                        self.code_sections.add(int(m.group(1), 16))
                    continue
                m = SYM_LINE.match(line)
                if not m:
                    continue
                sec = int(m.group(1), 16)
                addr = int(m.group(4), 16)
                if sec == 0 or addr == 0:
                    continue   # absolute / linker-defined
                tail = (m.group(5) or "").split()
                flags = ""
                while tail and tail[0] in ("f", "i"):
                    flags += tail.pop(0)
                raw.append((addr, m.group(3), flags, " ".join(tail), sec))
        raw.sort(key=lambda s: s[0])
        self.syms = raw
        self.addrs = [s[0] for s in raw]

    def lookup(self, map_addr):
        """The nearest symbol at or below map_addr: (names, delta, lib:object, in_code) or None.

        in_code is False when that symbol is in a data section, i.e. the address is not code."""
        i = bisect.bisect_right(self.addrs, map_addr) - 1
        if i < 0:
            return None
        addr = self.syms[i][0]
        same = []
        j = i
        while j >= 0 and self.syms[j][0] == addr:
            same.append(self.syms[j])
            j -= 1
        same.reverse()
        same.sort(key=lambda s: 0 if "f" in s[2] else 1)   # a function flag first, when the map has them
        in_code = (not self.code_sections) or same[0][4] in self.code_sections
        return [s[1] for s in same[:2]], map_addr - addr, same[0][3], in_code


# ---------------------------------------------------------------------------------------------------------------
# The executable (optional): section table, to check that a stack word is a return address
# ---------------------------------------------------------------------------------------------------------------

class Pe:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not a PE file")
        nt = struct.unpack_from("<I", d, 0x3C)[0]
        if d[nt:nt + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        nsec, = struct.unpack_from("<H", d, nt + 6)
        self.stamp, = struct.unpack_from("<I", d, nt + 8)
        optsz, = struct.unpack_from("<H", d, nt + 20)
        opt = nt + 24
        self.size_of_image, = struct.unpack_from("<I", d, opt + 56)
        self.sections = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            vsize, va, rawsz, rawptr = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((va, vsize, rawptr, rawsz))

    def read(self, rva, n):
        for va, vsize, rawptr, rawsz in self.sections:
            if va <= rva and rva + n <= va + min(vsize, rawsz):
                off = rawptr + (rva - va)
                return self.data[off:off + n]
        return None

    def is_call_site(self, rva):
        """True when the instruction that ends at `rva` is a call: E8 rel32, or FF /2 in any ModRM form."""
        b = self.read(rva - 8, 8)
        if b is None:
            return False
        if b[3] == 0xE8:                                     # call rel32: 5 bytes
            return True
        for length in range(2, 8):                           # FF /2 [SIB] [disp], `length` bytes up to the end
            s = 8 - length
            if b[s] != 0xFF:
                continue
            modrm = b[s + 1]
            if (modrm >> 3) & 7 != 2:
                continue
            mod, rm = modrm >> 6, modrm & 7
            need = 2
            sib_base5 = False
            if mod != 3 and rm == 4:
                need += 1                                    # SIB byte
                sib_base5 = s + 2 < 8 and (b[s + 2] & 7) == 5
            if mod == 1:
                need += 1                                    # disp8
            elif mod == 2 or (mod == 0 and (rm == 5 or sib_base5)):
                need += 4                                    # disp32 / rip-relative
            if need == length:
                return True
        return False


# ---------------------------------------------------------------------------------------------------------------
# crash.txt and the run's log
# ---------------------------------------------------------------------------------------------------------------

HOST_IMAGE = re.compile(r"host-win32: host-image base=0x([0-9a-f]+) size=0x([0-9a-f]+) preferred=0x([0-9a-f]+) "
                        r"timestamp=0x([0-9a-f]+)(?: path=(.*))?")
TABLE_ROW = re.compile(r"^  (0x[0-9a-f]{16})  size 0x([0-9a-f]{8})(?:  stamp 0x([0-9a-f]{8}))?  (.*)$")


class Image:
    def __init__(self, base, size, preferred=None, stamp=None, path=""):
        self.base, self.size, self.preferred, self.stamp, self.path = base, size, preferred, stamp, path

    def contains(self, a):
        return self.base <= a < self.base + self.size


def read_host_image(log_path):
    if not log_path or not os.path.isfile(log_path):
        return None
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = HOST_IMAGE.search(line)
            if m:
                return Image(int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16),
                             (m.group(5) or "").strip())
    return None


class Record:
    def __init__(self, number):
        self.number = number
        self.kind = ""
        self.code = None
        self.code_name = ""
        self.at = None
        self.av = None            # (kind word, address)
        self.rip = None
        self.regs = {}
        self.rip_bytes = None     # bytes starting at RIP
        self.stack = []           # (offset, value)
        self.host_table = []      # (base, size, stamp, path)
        self.guest_table = []


def parse_crash(path):
    recs = []
    cur = None
    section = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            m = re.match(r"^crash record #(\d+)", line)
            if m:
                cur = Record(int(m.group(1)))
                recs.append(cur)
                section = None
                continue
            if cur is None:
                continue
            if line.startswith("kind: "):
                cur.kind = line[6:].split(" - ")[0].split(",")[0].strip()
            elif line.startswith("host modules"):
                section = "host"
            elif line.startswith("modules in PEB->Ldr") or line.startswith("modules in the loader lists"):
                section = "guest"
            elif line.startswith("exception: "):
                section = None
                m = re.match(r"exception: 0x([0-9a-f]{8})\s*(\S*)", line)
                if m:
                    cur.code = int(m.group(1), 16)
                    cur.code_name = m.group(2) or EXC_NAMES.get(cur.code, "")
            elif line.startswith("  at 0x"):
                cur.at = int(line.split()[1], 16)
            elif line.startswith("  access violation: "):
                m = re.match(r"  access violation: (\S+(?: \(DEP\))?) (0x[0-9a-f]{16})", line)
                if m:
                    cur.av = (m.group(1), int(m.group(2), 16))
            elif line.startswith("  rip=0x"):
                m = re.match(r"  rip=(0x[0-9a-f]{16})", line)
                if m:
                    cur.rip = int(m.group(1), 16)
            elif re.match(r"^  r[a-z0-9]{2} ?=0x", line):
                for name, val in re.findall(r"(r[a-z0-9]{2,3}) ?=(0x[0-9a-f]{16})", line):
                    cur.regs[name] = int(val, 16)
            elif line.startswith("bytes at rip"):
                tail = line.split(":", 1)[1] if ":" in line else ""
                if "|" in tail:
                    tail = tail.split("|", 1)[1]
                cur.rip_bytes = bytes(int(t, 16) for t in tail.split() if re.fullmatch(r"[0-9a-f]{2}", t))
            elif line.startswith("stack at rsp"):
                section = "stack"
            else:
                m = re.match(r"^  \[rsp\+0x([0-9a-f]+)\] (0x[0-9a-f]{16})", line)
                if m and section == "stack":
                    cur.stack.append((int(m.group(1), 16), int(m.group(2), 16)))
                    continue
                m = TABLE_ROW.match(line)
                if m and section in ("host", "guest"):
                    row = (int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16) if m.group(3) else None,
                           m.group(4))
                    (cur.host_table if section == "host" else cur.guest_table).append(row)
                elif not line.startswith("  ") and section in ("host", "guest", "stack"):
                    section = None
    return recs


# ---------------------------------------------------------------------------------------------------------------
# The minidump (optional): the faulting thread's saved stack
# ---------------------------------------------------------------------------------------------------------------

def minidump_stack(path, max_words=4096):
    """(thread id, rip, rsp, [(offset from rsp, word)]) of the exception thread of a minidump; raises on trouble."""
    with open(path, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            sig, _ver, nstreams, dir_rva = struct.unpack_from("<IIII", m, 0)
            if sig != 0x504D444D:
                raise ValueError("not a minidump")
            streams = {}
            for i in range(nstreams):
                typ, size, rva = struct.unpack_from("<III", m, dir_rva + 12 * i)
                streams.setdefault(typ, (size, rva))
            if 6 not in streams:
                raise ValueError("the dump has no exception stream")
            ex_rva = streams[6][1]
            tid, = struct.unpack_from("<I", m, ex_rva)
            _ctx_size, ctx_rva = struct.unpack_from("<II", m, ex_rva + 160)
            rsp, = struct.unpack_from("<Q", m, ctx_rva + 0x98)
            rip, = struct.unpack_from("<Q", m, ctx_rva + 0xF8)
            ranges = []   # (start, size, file offset)
            if 3 in streams:
                n, = struct.unpack_from("<I", m, streams[3][1])
                for i in range(n):
                    start, size, rva = struct.unpack_from("<QII", m, streams[3][1] + 4 + 48 * i + 24)
                    ranges.append((start, size, rva))
            if 5 in streams:
                n, = struct.unpack_from("<I", m, streams[5][1])
                for i in range(n):
                    start, size, rva = struct.unpack_from("<QII", m, streams[5][1] + 4 + 16 * i)
                    ranges.append((start, size, rva))
            if 9 in streams:
                n, base_rva = struct.unpack_from("<QQ", m, streams[9][1])
                off = base_rva
                for i in range(n):
                    start, size = struct.unpack_from("<QQ", m, streams[9][1] + 16 + 16 * i)
                    ranges.append((start, size, off))
                    off += size
            for start, size, off in ranges:
                if start <= rsp < start + size:
                    nbytes = min(size - (rsp - start), max_words * 8)
                    nbytes -= nbytes % 8
                    data = m[off + (rsp - start): off + (rsp - start) + nbytes]
                    words = struct.unpack("<%dQ" % (len(data) // 8), data)
                    return tid, rip, rsp, [(8 * i, w) for i, w in enumerate(words)]
            raise ValueError("no saved memory range contains the exception RSP %s" % hx(rsp))
        finally:
            m.close()


# ---------------------------------------------------------------------------------------------------------------
# Attribution and output
# ---------------------------------------------------------------------------------------------------------------

class Symbolizer:
    def __init__(self, mp, image, pe):
        self.mp, self.image, self.pe = mp, image, pe

    def preferred(self):
        if self.mp.preferred is not None:
            return self.mp.preferred
        return (self.image.preferred or 0) if self.image else 0

    def host_text(self, addr, ret_addr):
        """'win10xbox.exe+0xRVA => symbol+0xN [obj]' for an address inside the host image, else None.

        With ret_addr (a stack word) only code qualifies: data pointers and addresses without a symbol give None."""
        if self.image is None or not self.image.contains(addr):
            return None
        rva = addr - self.image.base
        name = basename(self.image.path) if self.image.path else "win10xbox.exe"
        hit = self.mp.lookup(self.preferred() + rva)
        if ret_addr and (hit is None or not hit[3]):
            return None
        if hit is None:
            what = "no symbol at or below this address in the map"
        else:
            names, delta, obj, in_code = hit
            if not in_code:
                what = "a DATA address, not code (nearest symbol %s+%s)" % ("/".join(names), hx(delta))
            else:
                what = "%s+%s%s" % ("/".join(names), hx(delta), (" [%s]" % obj) if obj else "")
                if delta > 0x10000:
                    what += "   (nearest symbol is %s bytes below: probably not inside a mapped function)" % hx(delta)
        call = ""
        if ret_addr and self.pe is not None and hit is not None and hit[3] and self.pe.is_call_site(rva):
            call = "   <- call site"
        return "host image %s+%s => %s%s" % (name, hx(rva), what, call)

    def other_text(self, addr, rec):
        for tag, table in (("host module", rec.host_table), ("guest module", rec.guest_table)):
            for base, size, _stamp, path in table:
                if base <= addr < base + size:
                    return "%s %s+%s" % (tag, basename(path), hx(addr - base))
        return None

    def describe(self, addr, rec, ret_addr=False):
        """(text, is_host_image) for an address, or (None, False) when no table knows it."""
        text = self.host_text(addr, ret_addr)
        if text:
            return text, True
        if ret_addr and self.image is not None and self.image.contains(addr):
            return None, False   # a data pointer into the host image: not a return address
        text = self.other_text(addr, rec)
        return (text, False) if text else (None, False)


def consistency(mp, image, pe, out):
    out("map: %s: %d symbols, preferred load address %s, Timestamp is %s" % (
        mp.path, len(mp.syms), hx(mp.preferred or 0), hx(mp.timestamp, 8) if mp.timestamp is not None else "?"))
    stamps = {"map": mp.timestamp}
    if pe is not None:
        stamps["exe"] = pe.stamp
        out("exe: PE TimeDateStamp %s, SizeOfImage %s" % (hx(pe.stamp, 8), hx(pe.size_of_image, 8)))
    if image is not None:
        stamps["run"] = image.stamp
        out("run: host image base %s size %s preferred %s timestamp %s" % (
            hx(image.base, 16), hx(image.size, 8), hx(image.preferred or 0, 16),
            hx(image.stamp, 8) if image.stamp is not None else "?"))
        if pe is not None and pe.size_of_image != image.size:
            out("WARNING: SizeOfImage of the exe (%s) differs from the one the run printed (%s)" % (
                hx(pe.size_of_image), hx(image.size)))
    known = {k: v for k, v in stamps.items() if v is not None}
    if len(set(known.values())) <= 1:
        out("timestamps (%s): all equal -> the map belongs to the binary that ran" % ", ".join(sorted(known)))
    else:
        out("WARNING: TIMESTAMPS DIFFER (%s): this map may not belong to the binary that ran; do not trust the "
            "names below" % ", ".join("%s=%s" % (k, hx(v, 8)) for k, v in sorted(known.items())))


def single_lookup(args, mp, image, pe):
    preferred = mp.preferred or 0
    if args.rva is not None:
        rva = args.rva
    else:
        if image is None:
            print("--addr needs --base (or --log with the host-image line)")
            return 2
        rva = args.addr - image.base
    hit = mp.lookup(preferred + rva)
    if hit is None:
        print("RVA %s (map address %s): no symbol" % (hx(rva), hx(preferred + rva)))
        return 0
    names, delta, obj, in_code = hit
    call = "  <- call site" if (pe is not None and in_code and pe.is_call_site(rva)) else ""
    print("RVA %s (map address %s) => %s+%s%s%s%s" % (
        hx(rva), hx(preferred + rva), "/".join(names), hx(delta), (" [%s]" % obj) if obj else "",
        "" if in_code else "  (a data symbol: not code)", call))
    return 0


def run(args):
    def out(s=""):
        print(s)

    mp = MapFile(args.map)
    if not mp.syms:
        out("symbolize_crash: no symbols could be read from %s" % args.map)
        return 0
    pe = None
    if args.exe and os.path.isfile(args.exe):
        try:
            pe = Pe(args.exe)
        except Exception as e:   # noqa: BLE001 - best effort
            out("symbolize_crash: cannot read %s as a PE file: %s" % (args.exe, e))
    if args.base is not None:
        image = Image(args.base, args.size or (pe.size_of_image if pe else 0x10000000), mp.preferred)
    else:
        image = read_host_image(args.log)

    if args.rva is not None or args.addr is not None:
        return single_lookup(args, mp, image, pe)

    consistency(mp, image, pe, out)
    if image is None:
        out("symbolize_crash: no host-image line in the log (and no --base): taking the host image from each "
            "record's host module table instead")

    sy = Symbolizer(mp, image, pe)
    headline = None
    callers = []
    first_rec = None
    for crash_path in args.crash:
        if not os.path.isfile(crash_path):
            continue
        for rec in parse_crash(crash_path):
            if sy.image is None and rec.host_table:   # the first row of the host table is the executable
                b, s, st, p = rec.host_table[0]
                sy.image = Image(b, s, None, st, p)
            first_rec = first_rec or rec
            out()
            out("== %s record #%d: %s %s ==" % (basename(crash_path), rec.number, rec.kind,
                                              ("(" + rec.code_name + ")") if rec.code_name else ""))
            if rec.av:
                out("access violation: %s %s" % (rec.av[0], hx(rec.av[1], 16)))
            fault = rec.at if rec.at is not None else rec.rip
            this_headline = None
            if fault is not None:
                text, _host = sy.describe(fault, rec)
                out("fault address %s: %s" % (hx(fault, 16), text or "in NO known module (see the <unattributed; ...> "
                                              "text in crash.txt)"))
                if text:
                    this_headline = "%s%s at %s" % (rec.code_name or hx(rec.code or 0, 8),
                                                    (" %s %s" % (rec.av[0], hx(rec.av[1]))) if rec.av else "", text)
            if rec.rip_bytes:
                note = "   = mov eax,[rdx+rax], the instruction of the console crash report" \
                    if rec.rip_bytes[:3] == b"\x8b\x04\x02" else ""
                out("bytes at RIP: %s%s" % (" ".join("%02x" % b for b in rec.rip_bytes[:8]), note))
                if this_headline:
                    this_headline += " | RIP bytes " + " ".join("%02x" % b for b in rec.rip_bytes[:4])
            if "rax" in rec.regs and "rdx" in rec.regs:
                out("rax=%s rdx=%s" % (hx(rec.regs["rax"], 16), hx(rec.regs["rdx"], 16)))
            chain = []
            skipped = 0
            for off, val in rec.stack:
                text, is_host = sy.describe(val, rec, ret_addr=True)
                if text:
                    chain.append((off, val, text, is_host))
                elif sy.image is not None and sy.image.contains(val):
                    skipped += 1
            if chain:
                out("stack words that point into a known module, lowest address first (raw words, not a verified "
                    "backtrace):")
                for off, val, text, _h in chain:
                    out("  [rsp+%s] %s  %s" % (hx(off, 3), hx(val, 16), text))
                if skipped:
                    out("  (%d more stack words point into the host image's data or the image base: not code, left out)" % skipped)
            else:
                out("no stack word of this record points into a known module")
            if headline is None and this_headline:
                headline = this_headline
                host_chain = [t for _o, _v, t, h in chain if h]
                callers = ([t for t in host_chain if "<- call site" in t] or host_chain)[:4]

    if args.dump and first_rec is not None and os.path.isfile(args.dump):
        try:
            tid, rip, rsp, words = minidump_stack(args.dump)
            out()
            out("== %s: the faulting thread 0x%x, RSP %s, %d words saved; the ones that point into host-image CODE ==" % (
                basename(args.dump), tid, hx(rsp, 16), len(words)))
            shown = 0
            for off, w in words:
                text = sy.host_text(w, True)
                if not text:
                    continue
                out("  [rsp+%s] %s  %s" % (hx(off, 3), hx(w, 16), text))
                shown += 1
                if shown >= 80:
                    out("  ... (the first 80 shown)")
                    break
            if shown == 0:
                out("  none")
        except Exception as e:   # noqa: BLE001 - best effort
            out("== %s could not be scanned: %s ==" % (args.dump, e))

    if args.summary and headline:
        try:
            with open(args.summary, "a", encoding="utf-8") as f:
                f.write("\n#### crash: %s\n\n```text\n%s\n" % (args.label or "host", headline))
                for c in callers:
                    f.write("  stack word: %s\n" % c)
                f.write("```\n")
        except OSError:
            pass
    return 0


def num(s):
    return int(s, 0)


def main(argv):
    try:   # a Windows console codepage may not encode every character of a path in the map; never fail on output
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--map", required=True)
    ap.add_argument("--log")
    ap.add_argument("--exe")
    ap.add_argument("--dump")
    ap.add_argument("--summary")
    ap.add_argument("--label")
    ap.add_argument("--base", type=num)
    ap.add_argument("--size", type=num)
    ap.add_argument("--rva", type=num)
    ap.add_argument("--addr", type=num)
    ap.add_argument("crash", nargs="*")
    args = ap.parse_args(argv)
    try:
        return run(args)
    except Exception as e:   # noqa: BLE001 - this tool must never fail a job
        print("symbolize_crash: failed: %s: %s" % (type(e).__name__, e))
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
