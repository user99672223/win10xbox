// Backend selection (docs/DECISIONS.md D8). Headless today; the D3D12 /
// CoreWindow backend plugs in here once Probe I has results.
#include "winexbox/driver.h"
#include "internal.h"

namespace winexbox {

std::unique_ptr<IWinexboxDriver> SelectBackend(const BackendConfig& cfg) {
    using win10xbox::config::DisplayBackend;
    switch (cfg.backend) {
    case DisplayBackend::Headless:
        break;
    case DisplayBackend::D3D12:
        // D8: the real backend is written against IWinexboxDriver after Probe I.
        FIXME(winex, "DisplayBackend::D3D12 not implemented yet, using the headless backend\n");
        break;
    }
    return CreateHeadlessDriver(cfg);
}

std::unique_ptr<IWinexboxDriver> SelectBackend() {
    const auto& c = win10xbox::config::Current();
    BackendConfig bc;
    bc.backend = c.display_backend;
    return SelectBackend(bc);
}

}  // namespace winexbox
