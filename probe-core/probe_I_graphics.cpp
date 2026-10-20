// probe_I_graphics.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_I_graphics(Reporter& r) {
    r.run("stub", "register_I_graphics", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_I_graphics)";
    });
}
}  // namespace probe
