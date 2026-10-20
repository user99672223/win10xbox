// probe_E_cpu_identity.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Opus
#include "probe.h"
namespace probe {
void register_E_cpu_identity(Reporter& r) {
    r.run("stub", "register_E_cpu_identity", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_E_cpu_identity)";
    });
}
}  // namespace probe
