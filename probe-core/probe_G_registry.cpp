// probe_G_registry.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_G_registry(Reporter& r) {
    r.run("stub", "register_G_registry", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_G_registry)";
    });
}
}  // namespace probe
