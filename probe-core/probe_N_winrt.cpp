// probe_N_winrt.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Haiku
#include "probe.h"
namespace probe {
void register_N_winrt(Reporter& r) {
    r.run("stub", "register_N_winrt", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_N_winrt)";
    });
}
}  // namespace probe
