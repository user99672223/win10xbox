// probe_L_processes.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Opus
#include "probe.h"
namespace probe {
void register_L_processes(Reporter& r) {
    r.run("stub", "register_L_processes", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_L_processes)";
    });
}
}  // namespace probe
