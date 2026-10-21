# Pinned sources

Submodules (see `.gitmodules`, all shallow): MemoryModule, Detours, minhook,
zydis (+zycore), phnt, windows-syscalls (j00ru), hivex,
Windows-Driver-Frameworks (KMDF), speakeasy (API reference only, not built),
pafish, al-khaser (fidelity tests).

Pinned-tag fetches (too large for submodules; scripts clone `--depth 1` at the
tag and the tag is the only thing that may change here):

| Project | Repo | Pin | Fetched by |
|---------|------|-----|------------|
| Wine | https://github.com/wine-mirror/wine | `wine-11.9` | `wine-pe/fetch-wine.sh` |
| ReactOS | https://github.com/reactos/reactos | `master` @ `a97c8a2cdcf6` (commit pin; release tags are far older than the code we port) | `drivers/fetch-reactos.sh` |

Update a pin by changing the value in the script and this table in the same
commit.
