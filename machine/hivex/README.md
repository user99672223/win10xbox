# machine/hivex: registry hive tooling (hivex is NOT used)

`docs/DECISIONS.md` D13 planned to read REGF hives through the hivex library
(`third_party/hivex`, LGPL-2.1). That was evaluated and **rejected**; the
registry module instead ships its own REGF reader/writer:

| | |
|---|---|
| `core/registry/regf.{h,cpp}` | REGF reader (`regf::Hive`, read-only, bounds-checked) and whole-hive writer (`regf::WriteHive`) |
| `core/registry/registry.cpp` | virtual `\Registry` tree + write overlay on top of `regf::Hive` |
| `machine/hivex/mkfixture.cpp` | generator for the synthetic test hives in `tests/fixtures/registry/machine` |

## Why not hivex

* The submodule has no `hivex.h`: it is generated from `generator/generator.ml`
  by OCaml at build time, and there is no OCaml in our toolchains (llvm-mingw
  on Linux, MSVC on CI).
* `lib/*.c` hard-depends on POSIX/GNU pieces that MSVC lacks: `iconv`
  (UTF-16LE <-> UTF-8 in `handle.c`/`utf16.c`), `pthread` rwlocks, `<unistd.h>`,
  `byteswap.h`, gnulib `full-read`/`full-write`/`c-ctype`, `open(2)` flags, and
  a Win32 `mmap` shim (`mmap.c`) that stores a mapping handle in the hive handle.
  Porting it means shipping an iconv replacement, a pthread shim, a hand-written
  `hivex.h` and a config header, and keeping all of that building under both
  `cl /W4 /permissive-` and clang-mingw, with no MSVC available locally to verify.
* What the layer needs is small: read nk/vk/lf/lh/li/ri/db cells, never write
  a supplied hive (writes go to an in-memory overlay), plus a writer for
  `NtSaveKey` and for generating fixtures. That is roughly 600 lines, fully
  under our control, bounds-checked against hostile hives, and has no
  third-party licence obligations.

Nothing in `regf.cpp` is copied from hivex. The REGF layout is documented
public knowledge (Maxim Suhanov's "Windows registry file format specification",
Peter Norris' "The Internal Structure of the Windows Registry"); hivex's
`lib/hivex-internal.h` struct comments were consulted as one cross-check of
field offsets, nothing more. The writer's output was validated with an
independent parser (python `regipy`) while developing it.

The `third_party/hivex` submodule remains pinned (see `third_party/PINS.md`)
but is not built or linked. If a future need arises (e.g. hivex's tolerant
repair of damaged hives), it can still be added behind
`config::registry_backend = RegistryBackend::Hivex`; its LGPL-2.1 terms would
then apply to that build target only, as for the other `third_party/` entries.

## Fixture hives

`tests/fixtures/registry/machine/` is a miniature machine folder:

```
Windows\System32\config\SOFTWARE   Microsoft\Windows NT\CurrentVersion (22H2 values), Fixture\* test keys
Windows\System32\config\SYSTEM     Select, ControlSet001\...
Users\User\NTUSER.DAT              Environment, Software\Fixture
```

They are **synthetic**: invented content written by `regf::WriteHive`, never
Microsoft files. The generator is deterministic (fixed timestamps), so
regenerating yields byte-identical files:

```
cmake --build <build-dir> --target regen-registry-fixtures
# or directly: w10x_mkfixture <output machine dir>
```

`tests/unit/test_registry.cpp` hashes these files before and after its overlay
tests to prove supplied hives are never modified.
