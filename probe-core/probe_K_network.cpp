// probe_K_network.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_K_network(Reporter& r) {
    r.run("stub", "register_K_network", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_K_network)";
    });
}
}  // namespace probe
