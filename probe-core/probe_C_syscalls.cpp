// probe_C_syscalls.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Haiku
#include "probe.h"
namespace probe {
void register_C_syscalls(Reporter& r) {
    r.run("stub", "register_C_syscalls", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_C_syscalls)";
    });
}
}  // namespace probe
