// probe_B_dll_inventory.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Haiku
#include "probe.h"
namespace probe {
void register_B_dll_inventory(Reporter& r) {
    r.run("stub", "register_B_dll_inventory", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_B_dll_inventory)";
    });
}
}  // namespace probe
