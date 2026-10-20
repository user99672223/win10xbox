// probe_H_token.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_H_token(Reporter& r) {
    r.run("stub", "register_H_token", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_H_token)";
    });
}
}  // namespace probe
