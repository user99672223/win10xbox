// probe_M_system.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_M_system(Reporter& r) {
    r.run("stub", "register_M_system", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_M_system)";
    });
}
}  // namespace probe
