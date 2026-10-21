# Loader (`core/loader/`)

The loader is the "unix side" of image mapping (E3 in `DECISIONS.md`): it maps
x64 PE32+ files into the host process, hands out exports, rewrites the guest's
syscall thunks to enter the dispatcher, and patches the instructions that
would otherwise expose the real machine (`cpuid`, `rdtsc`, ...). Everything is
pure Win32/NT; memory operations go straight to the host (`VirtualAlloc`,
`VirtualProtect`, host `RtlAddFunctionTable`, ...) and never through the
dispatcher.

| File | Content |
|------|---------|
| `mapper.cpp` | `MapImageFromMemory/File`, `FinalizeProtections`, `UnmapImage`, `FindExport[ByOrdinal]`. Derived from MemoryModule (MPL-2.0, attribution in the file header). |
| `stubs.cpp` | `RewriteSyscallStubs` and the thunk recognisers. |
| `scanner.cpp` | `ScanAndPatch[Range]`, `EnsureTrapHandlerInstalled`, `ReleaseScanSites`: Zydis scan, inline thunks, int3 + VEH fallback. |
| `internal.{h,cpp}` | host ntdll lookup, `ScopedWritable`, "protected range" registry. |

## What is *not* ours (and why)

Wine's `dlls/ntdll/loader.c` (the guest ntdll, E1) does all of these; the
loader deliberately does not:

* **import resolution, API sets, forwarders, delay-load, bound imports** -
  they depend on Wine's module list, `dll_overrides`, activation contexts and
  the apiset schema. Re-implementing them here would give two sources of truth.
  `FindExport` therefore *returns the forwarder string* (`"NTDLL.RtlFoo"`,
  `"NTDLL.#123"`) and a null pointer; Wine resolves it.
* **TLS** (static TLS slots, `AddressOfIndex`, callbacks) and **DllMain /
  entry point ordering** (loader lock, `LdrpRunInitializeRoutines`). The
  mapper only guarantees that every absolute address in the TLS directory and
  callback array is relocated (tested), and exposes `entry`.
* **SxS/activation contexts, KnownDlls, file search**.

The mapper's contract with Wine is "behave like `NtMapViewOfSection` of an
image section": sections at their RVAs, relocated, with the image's final page
protections, `ImageBase` in the mapped header updated to the real base
(as `LdrRelocateImage` does).

## Mapper

Flow: validate -> reserve `SizeOfImage` (committed `PAGE_READWRITE`) at
`preferred` or the header `ImageBase` -> copy headers -> copy sections in
ascending-RVA order -> relocate -> update `ImageBase` -> register `.pdata`
(`RegisterUnwind`) -> apply final protections (unless `ReserveOnly`).

Status codes: `STATUS_SUCCESS`; `STATUS_IMAGE_NOT_AT_BASE` (informational,
success, image was relocated); `STATUS_INVALID_IMAGE_WIN_32` (PE32);
`STATUS_INVALID_IMAGE_FORMAT` (everything else malformed, other machines,
unsupported relocation types); `STATUS_CONFLICTING_ADDRESSES` (cannot be
placed and must not be moved); `STATUS_NO_MEMORY`; `STATUS_INVALID_PARAMETER`.
The constants are `kStatus*` in `loader.h`. `kDefaultMapFlags` =
`RegisterUnwind`.

### Corner cases handled (all covered by `tests/unit/test_loader.cpp`)

| Case | Behaviour |
|------|-----------|
| PE32 (`Magic` 0x10B) | `STATUS_INVALID_IMAGE_WIN_32`. D6: no WoW64. |
| Machine != AMD64 (ARM64, I386 PE32+...) | `STATUS_INVALID_IMAGE_FORMAT`. |
| Truncated file, bad `MZ`/`PE` signatures, `e_lfanew` out of file, section table past EOF, >96 sections, optional header smaller than the fixed part | `STATUS_INVALID_IMAGE_FORMAT`. All header fields are copied with `memcpy`, never dereferenced in place. |
| `NumberOfRvaAndSizes` > 16 or larger than the optional header holds | Clamped; missing data directories read as {0,0}. |
| `SectionAlignment` not a power of two, or < page size and != `FileAlignment` | `STATUS_INVALID_IMAGE_FORMAT` (same rule as the NT memory manager). |
| `SizeOfImage` 0 / > ~1.75 GiB, section outside `SizeOfImage`, raw data past EOF | `STATUS_INVALID_IMAGE_FORMAT`. |
| Zero sections | Headers only; `SizeOfImage` still reserved; headers `PAGE_READONLY`, pages beyond them `PAGE_NOACCESS`. |
| `SizeOfHeaders` larger than the first section RVA | Headers are copied first, sections second, so the section wins. The overlapped page gets the **union** of header (R) and section permissions. |
| Section table not covered by `SizeOfHeaders` | The table is still copied (and counts as header) because `FinalizeProtections`/the scanner re-parse it from the mapped image. |
| Unsorted section table | Sections are copied in ascending RVA order. |
| Overlapping sections | Higher RVA overwrites the overlap; each page's protection is the union of every section touching it (so an `RX` + `RW` overlap page is `RWX`). Pages no section covers are `PAGE_NOACCESS`. |
| `VirtualSize` > `SizeOfRawData` | Tail is zero (fresh `VirtualAlloc` memory). |
| `VirtualSize` < `SizeOfRawData` | Only `VirtualSize` bytes are copied (rest of the page stays zero). |
| `VirtualSize` == 0 | Raw size is used (NT behaviour). |
| `SectionAlignment` == `FileAlignment` < 4 KiB (flat images) | Accepted; page protections are the per-page union. |
| `ImageBase` busy | Falls back to any address and relocates; returns `STATUS_IMAGE_NOT_AT_BASE`, `relocated = true`, `image_base_preferred` keeps the header value. |
| `preferred` given | Tried first (must be 64 KiB aligned, else treated as busy). `preferred != ImageBase` means relocation even if free. |
| `ImageBase` == 0 | System picks the address; image is always relocated. With `NoRelocate`: `STATUS_CONFLICTING_ADDRESSES`. |
| `IMAGE_FILE_RELOCS_STRIPPED` | Treated exactly like `NoRelocate`: it only maps at its own `ImageBase`, otherwise `STATUS_CONFLICTING_ADDRESSES` (memory freed). |
| `NoRelocate` | Fails with `STATUS_CONFLICTING_ADDRESSES` if the base would differ from the header `ImageBase`, including a deliberately different `preferred`. |
| No reloc directory but not stripped | Maps anywhere; there is simply nothing to fix up (resource-only DLLs). |
| Relocation types | `ABSOLUTE` (padding), `HIGHLOW`, `DIR64`. Anything else (`HIGH`, `LOW`, `HIGHADJ`, MIPS/ARM/...) -> `STATUS_INVALID_IMAGE_FORMAT`. |
| Relocations spanning pages | A `DIR64`/`HIGHLOW` at page offset 0xFFC-0xFFF crosses into the next page; the target is bounds-checked against the image, not the page. |
| Odd-sized reloc blocks | `SizeOfBlock` 8+2n+1: the n complete entries are applied, the next block starts exactly `SizeOfBlock` bytes later (possibly unaligned, as ntdll does). Blocks that overrun the directory contribute only the entries inside it. |
| `SizeOfBlock` == 0 | Terminates the table (linker padding); 1..7 is malformed. |
| Reloc target outside the image | `STATUS_INVALID_IMAGE_FORMAT`. |
| Entry point RVA >= `SizeOfImage` | `STATUS_INVALID_IMAGE_FORMAT`; RVA 0 -> `entry == nullptr`. |
| `.pdata` | Registered with the **host** `RtlAddFunctionTable` (E2: exceptions are dispatched by the host's `KiUserExceptionDispatcher`) when the directory is 4-aligned and inside the image; removed again by `UnmapImage`. |

### Protections

Headers are `PAGE_READONLY`. Section flags map R/W/X -> `NOACCESS`,
`READONLY`, `READWRITE`, `EXECUTE`, `EXECUTE_READ`, `EXECUTE_READWRITE`
(W implies R; no flags means `NOACCESS`; copy-on-write is not modelled).
`ReserveOnly` leaves the whole image `PAGE_READWRITE` so the caller can patch
before `FinalizeProtections`; `ScanAndPatch` works in both states (it reads
the executable sections from the section table, not from the current page
protection).

### Exports

`FindExport`/`FindExportByOrdinal` work on any mapped *or host-loaded* image
(they only read the in-memory headers) and bounds-check every table against
`SizeOfImage`.

* Name lookup is a binary search; **a miss falls back to a linear scan**, so
  linkers that emit an unsorted name table still work.
* Ordinal base != 1 is honoured (`ordinal - Base` indexes the function table);
  ordinals below the base or past the table return null.
* An RVA inside the export directory is a **forwarder**: null is returned and
  the string (`"NTDLL.RtlFoo"`, `"NTDLL.#123"`) is written to `*forwarder`.
* A zero RVA (hole in a sparse ordinal range) and an RVA >= `SizeOfImage`
  return null with an empty forwarder.
* Name/ordinal entries pointing outside the image, or unterminated names, are
  skipped.

## Syscall stub rewriting

Recognised shapes (bytes in hex; `id` is the 32-bit syscall number):

```
Windows 10 x64 (24 bytes)                       Wine 11 PE ntdll (32 bytes)
00 4C 8B D1               mov r10,rcx           00 4C 8B D1
03 B8 id id id id         mov eax,id            03 B8 id id id id
08 F6 04 25 08 03 FE 7F 01 test [7FFE0308],1    08 (same 8 bytes)
10 75 03                  jne +3                10 75 03
12 0F 05                  syscall               12 0F 05
14 C3                     ret                   14 C3
15 CD 2E                  int 2Eh               15 EB 01        jmp +1
17 C3                     ret                   17 C3
                                                18 FF 14 25 00 10 FE 7F   call [7FFE1000]
                                                1F C3
```

The Wine shape is taken from `include/wine/asm.h` (`__ASM_SYSCALL_FUNC`,
`__x86_64__` branch) and was checked against the built
`wine-pe/out/builtin/ntdll.dll` (258 Nt*/Zw* thunks, all matched).

Rewrite: bytes `[8,22)` of the thunk become

```
FF 25 00 00 00 00        jmp qword ptr [rip+0]
<imm64 target>
```

`mov r10,rcx ; mov eax,id` at `[0,8)` is untouched, so the dispatcher entry
sees `eax = id`, `r10 = arg0` and the original return address on the stack.
Worst-case write is 14 bytes at +8, inside the 24-byte minimum thunk.

* Candidates are exported names `Nt[A-Z]*` / `Zw[A-Z]*` (so `NtdllDefWindowProc_A`
  is not a candidate). `Zw*` aliases share the `Nt*` address: the thunk is
  rewritten once and reported once, under the `Nt*` name. Forwarded exports
  and thunks of any other shape (CET variants, the short Windows 7 form,
  `NtCurrentTeb`/`NtGetTickCount`-style inline implementations) are reported
  in `unmatched` by name and left alone.
* Rewritten thunks are recognised on a second call (idempotent; the target is
  simply replaced) and registered as *protected ranges* so that the scanner
  never decodes the absolute address (see below).
* Not atomic: do it before any guest thread runs the image.
* Page protection is lifted with `ScopedWritable` (RWX when the page is
  executable, falling back to RW if the host forbids RWX - D11) and restored.

## Scanner

`ScanAndPatch(img)` scans every `IMAGE_SCN_MEM_EXECUTE` section;
`ScanAndPatchRange` scans one raw range (JIT output, a host module section).
Sites: `cpuid` (0F A2), `rdtsc` (0F 31), `rdtscp` (0F 01 F9), `syscall`
(0F 05), `sysenter` (0F 34), `int 2Eh` (CD 2E) and, with
`patch_kuser_reads`, any non-control-transfer instruction with an absolute
memory operand (no base, no index, not FS/GS) whose displacement is in
`[0x7FFE0000, 0x7FFE1000)`. Option mapping: `patch_cpuid` -> cpuid;
`patch_rdtsc` -> rdtsc and rdtscp; `patch_syscall` -> syscall, sysenter, int 2E.

### How a section is swept

Zydis linear sweep, instruction by instruction, with these safeguards:

* decoding **resynchronises at function starts** (`.pdata` begin/end RVAs,
  entry point, named exports) so data embedded in `.text` cannot desynchronise
  the rest of a function;
* **protected ranges** are skipped: rewritten syscall thunks and previously
  patched sites. This makes `ScanAndPatch` idempotent (a second call reports
  zero sites) and keeps the 8-byte target address inside a rewritten thunk
  from being decoded as code. Un-rewritten syscall thunks are recognised by
  shape and skipped too, so the order of `RewriteSyscallStubs` and
  `ScanAndPatch` does not matter;
* a second pass collects **branch targets**: every rel8/rel32 jump/call/loop
  target in the executable sections, function starts, and hints from
  initialised data (aligned dwords that are RVAs into code - MSVC/LLVM jump
  tables; dwords that are offsets from their own address - GCC jump tables;
  aligned qwords that are absolute code VAs - relocated pointers).

### Strategy (a): inline jump to a per-site thunk

The site plus the following *whole* instructions, until at least 5 bytes are
covered, are replaced by `E9 rel32` (+ `CC` filler). The displaced
instructions are relocated into the thunk. The thunk lives in a ~64 KiB-granular
arena allocated within +/-2 GiB of the image (found with `VirtualQuery`,
written while RW, then sealed `PAGE_EXECUTE_READ` **before** any site is
patched; a new arena per call, never reused, so sealed code is never made
writable again).

It is refused (falling back to strategy b) when:

* any byte of the displaced area other than the first is a branch target
  (second pass) or the start of another site;
* a displaced instruction is a `call`, `jmp`, `ret`, `iret`, interrupt, `xbegin`,
  `loop*`, `jrcxz`/`jecxz` or any relative branch other than a plain `jcc`;
* a rip-relative displacement or a `jcc` target cannot be re-encoded in 32 bits
  from the thunk address;
* a displaced instruction cannot be decoded or crosses the end of the section;
* the image is so large that the arena cannot be within reach, or no arena
  could be allocated.

Relocation is byte-exact (no re-encoding through an assembler): the raw bytes
are copied; the 32-bit disp of a rip-relative operand and the rel32 of a
`jcc` are adjusted; a rel8 `jcc` is rewritten as `0F 8x rel32`.

Thunk layout (all emitted as raw bytes at runtime - MSVC x64 has no inline asm):

```
pushfq ; push rbp ; mov rbp,rsp ; sub rsp,0x1C0 ; and rsp,-16
mov [rsp+0x20+8*slot], r      ; every GPR except rsp/rbp: slots follow ScanRegs
rbp, rsp (= rbp+16), rflags (= [rbp+8]) and rip (= the site address) filled in
movups [rsp+0xB0+16*i], xmm0..15
cld ; mov ecx,kind ; lea rdx,[rsp+0x20] ; mov r8,ctx ; mov rax,handler ; call rax
test al,al ; lea rax,[L_original] ; lea rcx,[L_skip] ; cmovnz rax,rcx
mov [rbp+8],rax               ; becomes the `ret` target
mov [rbp],<ScanRegs.rbp>
movups xmm0..15 <- frame ; mov r,[rsp+..] for all GPRs (rax last)
push qword [rsp+rflags] ; popfq ; mov rsp,rbp ; pop rbp ; ret   -> rsp is the caller's again
L_original:  <the original instruction>
L_skip:      <relocated displaced instructions>
             jmp qword ptr [rip+0] ; dq site+region_length
```

* The frame is 16-byte aligned with the 0x20 shadow space the handler expects;
  the red zone does not exist on Windows x64, so using stack below `rsp` is
  safe.
* The handler may change any GPR and `rflags` in `ScanRegs`; the thunk loads
  them all back, **including when it returns false**. `ScanRegs.rsp` and
  `.rip` are informational: changes are ignored (both strategies).
* Return true = "emulated": the original instruction is skipped. Return false
  = the thunk executes the original instruction (from the thunk, not from its
  address - fine for the instruction kinds scanned) with the possibly modified
  registers.
* `KuserRead`: the handler must decode the instruction at `regs->rip` itself
  and update the destination; the thunk only provides the register file.

### Strategy (b): `int3` + vectored exception handler

Used whenever (a) is refused (typically `cpuid; ret`, a site right before a
branch target, ...). The first byte of the site is overwritten with `CC`, the
site is recorded in a global table, and an out-of-line copy "original
instruction + `jmp [rip+0]` back" is prepared. A first-chance VEH (installed
once through the **host** `RtlAddVectoredExceptionHandler`, first position)
reacts to `EXCEPTION_BREAKPOINT`:

* looks the site up by `ExceptionAddress` (falling back to `Rip` and `Rip-1`,
  because hosts differ on where `Rip` points after an `int3`);
* unknown breakpoints (debugger, `DebugBreak`) -> `EXCEPTION_CONTINUE_SEARCH`;
* fills `ScanRegs` from `CONTEXT`, calls the handler, writes the registers
  back, and sets `Rip = site + length` (handled) or `Rip = out-of-line copy`
  (not handled), then `EXCEPTION_CONTINUE_EXECUTION`.

It costs a real exception per execution (microseconds) and needs the host to
deliver `EXCEPTION_BREAKPOINT` to VEHs (probe D).

### Results and bookkeeping

`ScanResult.patched` = inline, `trapped` = int3, `skipped` = could not be
patched either way (no memory for a stub, protection change refused, or a
KUSER access inside a control-transfer instruction). `sites` lists patched and
trapped sites only, in address order. `ReleaseScanSites(start,size)` (called
by `UnmapImage`; call it yourself for ranges passed to `ScanAndPatchRange`)
drops trap-table entries and frees the thunk arenas.

## Known gaps and limits

1. **Unwinding through thunks.** Thunk and stub code has no unwind info. An
   exception raised *by a displaced instruction* (e.g. a faulting load that was
   moved into the thunk) or by the handler itself cannot be unwound through the
   thunk frame by the host `RtlVirtualUnwind`/SEH walk (the faulting `Rip` is
   not in any registered function table). Strategy (b) is not affected for
   faults in the original code, only for the handler. Possible fix: register a
   dynamic function table whose entries chain to the original function's
   unwind info.
2. **Not thread-safe against running code.** Patching rewrites up to 14 (stubs)
   or `region_length` (sites) bytes non-atomically. Patch before guest threads
   execute the code; `ScopedWritable` prefers RWX so concurrent readers don't
   fault, which does not make the byte write atomic.
3. **Branch-target discovery is heuristic.** Targets reached only through
   computed jumps whose tables the data hints do not recognise (tables in
   `.text`, hand-built pointers, `jmp rax` into the middle of the displaced
   bytes) cannot be seen. Such code is wrong after an inline patch. Mitigation: function starts and the data hints above are
   treated as targets; a future per-site policy (force trap) would cover known-hostile code.
4. **Linear-sweep false positives.** Data in code sections without a covering
   function start can decode to a bogus `0F A2`/`0F 31`. `.pdata` resync
   reduces but cannot eliminate this; leaf functions without `.pdata` rely on
   the sweep staying in sync.
5. **A handler cannot be replaced** by a second scan: already patched sites are
   skipped. Unmap/remap or keep a dispatching handler.
6. **`ScanRegs` is GPR + flags only.** AVX/x87 state is saved/restored around
   the handler call but not exposed; `rdtscp`'s `IA32_TSC_AUX` is the
   handler's business (`rcx`).
7. **Section alignment < 4 KiB** images are accepted only when
   `SectionAlignment == FileAlignment`; per-page protections are the union of
   overlapping sections (tiny packer-style PEs may therefore get RWX pages).
8. **No SEC_IMAGE sharing** (D2): every mapping is a private copy.
9. **PE32/WoW64, ARM64X/CHPE, delay-load, resources, load config (CFG, SEH
   table checks)** are not interpreted. `LoadConfig` CFG bitmaps are not set up.
10. Thunk arena memory for `ScanAndPatchRange` stays allocated until
    `ReleaseScanSites` (or process exit).
11. Compiled under MSVC `/W4 /permissive-` and clang-mingw, but the thunk
    emitter assumes the Windows x64 calling convention and an `x86_64` host.

## Notes for the dispatcher

* `RewriteSyscallStubs(img, SyscallEntryAddress(), ...)`: the entry sees the
  stack exactly as after `call NtXxx` (return address at `[rsp]`), `eax` = id,
  `r10` = first argument, `rdx/r8/r9` = args 2-4. Only the mov pair of the
  thunk survives; nothing else in the thunk executes.
* Unmatched names from Wine 11.9 ntdll: `NtGetTickCount` (not a thunk).
* The scanner never patches syscall thunks (they are skipped), so
  `patch_syscall` only affects `syscall`/`int 2E`/`sysenter` embedded in other
  code (statically linked syscall wrappers, protectors).
* Handlers run on the guest thread with the guest stack; keep them small and
  non-blocking, and do not call back into patched code from a handler.
* Tests: `tests/unit/test_loader.cpp` (also maps Wine's built `ntdll.dll` and
  every PE builtin when `wine-pe/out/builtin` exists), guest programs in
  `tests/guest/`.
