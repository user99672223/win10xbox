// core_run.cpp - the dispatch table. Runs every module in category order.
// Adding a module = implement its register_* (declared in probe.h) in a .cpp
// and make sure it is listed here. Each call is independent; a module that
// throws or faults at registration time is itself guarded so the rest still run.
#include "probe.h"

namespace probe {

void run_all(Reporter& r) {
    using Fn = void (*)(Reporter&);
    struct Entry { const char* label; Fn fn; };
    const Entry table[] = {
        {"A/dynamic-code",   &register_A_dynamic_code},
        {"B/dll-inventory",  &register_B_dll_inventory},
        {"B/ntdll-stubs",    &register_B_ntdll_stubs},
        {"C/syscalls",       &register_C_syscalls},
        {"D/veh-trap",       &register_D_veh_trap},
        {"E/cpu-identity",   &register_E_cpu_identity},
        {"F/filesystem",     &register_F_filesystem},
        {"G/registry",       &register_G_registry},
        {"H/token",          &register_H_token},
        {"I/graphics",       &register_I_graphics},
        {"J/audio",          &register_J_audio},
        {"K/network",        &register_K_network},
        {"L/processes",      &register_L_processes},
        {"M/system",         &register_M_system},
        {"N/winrt",          &register_N_winrt},
    };
    for (const auto& e : table) {
        // Guard registration itself: a module that faults while enumerating
        // must not take down the modules after it.
        uint32_t code = 0;
        Fn fn = e.fn;
        Reporter* rp = &r;
        bool ok = guarded([fn, rp] { fn(*rp); }, &code);
        if (!ok) {
            Record rec;
            rec.category = "core";
            rec.name = std::string("module-crash:") + e.label;
            rec.status = Status::CRASH;
            rec.errorcode = code;
            rec.detail = "module faulted during registration: " + error_text(code);
            r.emit(rec);
        }
    }
}

}  // namespace probe
