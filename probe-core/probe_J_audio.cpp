// probe_J_audio.cpp - STUB. Overwritten by the owning module. Registers a single SKIPPED
// record so the tree links and a partial run shows the module as pending.
// Sonnet
#include "probe.h"
namespace probe {
void register_J_audio(Reporter& r) {
    r.run("stub", "register_J_audio", [](Record& rec) {
        rec.status = Status::SKIPPED;
        rec.detail = "module not yet implemented (register_J_audio)";
    });
}
}  // namespace probe
