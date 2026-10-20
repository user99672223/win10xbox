// probe_B_ntdll_stubs.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Opus
#include "probe.h"
namespace probe {
void register_B_ntdll_stubs(Reporter& r) {
    r.run("stub", "register_B_ntdll_stubs", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_B_ntdll_stubs)";
    });
}
}  // namespace probe
