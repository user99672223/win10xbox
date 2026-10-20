// probe_A_dynamic_code.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Opus
#include "probe.h"
namespace probe {
void register_A_dynamic_code(Reporter& r) {
    r.run("stub", "register_A_dynamic_code", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_A_dynamic_code)";
    });
}
}  // namespace probe
