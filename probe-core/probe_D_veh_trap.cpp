// probe_D_veh_trap.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Opus
#include "probe.h"
namespace probe {
void register_D_veh_trap(Reporter& r) {
    r.run("stub", "register_D_veh_trap", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_D_veh_trap)";
    });
}
}  // namespace probe
