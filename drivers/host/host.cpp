// Driver host lifecycle and the small public accessors of drivers.h.
#include "../ntoskrnl/internal.h"
#include "win10xbox/dispatch.h"

namespace win10xbox::drivers {
using namespace detail;

namespace {
bool g_initialized = false;
SRWLOCK g_init_lock = SRWLOCK_INIT;
bool g_hooks_requested = false;
}  // namespace

bool IsInitialized() { return g_initialized; }

NTSTATUS Initialize(const Options* opts) {
    AcquireSRWLockExclusive(&g_init_lock);
    if (g_initialized) { ReleaseSRWLockExclusive(&g_init_lock); return STATUS_SUCCESS; }
    Options o = opts ? *opts : Options{};
    if (!IrqlInit() || !PoolInit()) {
        ReleaseSRWLockExclusive(&g_init_lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    SetStrictIrql(o.strict_irql);
    InitializeListHead(&g_ps_loaded_module_list);
    ObPsMmInit();
    if (!WorkerStart()) {
        ReleaseSRWLockExclusive(&g_init_lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_initialized = true;
    ReleaseSRWLockExclusive(&g_init_lock);
    g_hooks_requested = o.install_dispatcher_hooks;
    if (o.install_dispatcher_hooks) {
        NTSTATUS st = InstallDispatcherHooks();
        if (!NT_SUCCESS(st)) WARN(driver, "dispatcher hooks not installed: %08lx", static_cast<unsigned long>(st));
    }
    TRACE(driver, "driver host initialised (%zu ntoskrnl exports)", ExportCount());
    return STATUS_SUCCESS;
}

void Shutdown() {
    if (!g_initialized) return;
    SendShutdownIrps();
    CloseAllHandles();
    DriversUnloadAll();
    RemoveDispatcherHooks();
    WorkerStop();
    ObPsMmShutdown();
    NsClear();
    ObjClearAll();
    IrqlShutdown();
    PoolShutdown();
    g_initialized = false;
    TRACE(driver, "driver host shut down");
}

std::uint8_t GetVirtualIrql() { return CurrentIrql(); }
void SetVirtualIrql(std::uint8_t irql) { SwapIrql(irql); }
bool InjectInterrupt(std::uint32_t vector) { return InterruptInject(vector); }

}  // namespace win10xbox::drivers
