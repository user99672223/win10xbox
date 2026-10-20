// probe_F_filesystem.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_F_filesystem(Reporter& r) {
    r.run("stub", "register_F_filesystem", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_F_filesystem)";
    });
}
}  // namespace probe
