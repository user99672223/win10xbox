# Sources of the driver host library (win10xbox_drivers). Included from
# drivers/CMakeLists.txt; kept separate so the file list can be reviewed on its own.
set(WIN10XBOX_DRIVERS_SOURCES
  # user-mode ntoskrnl: the API surface a driver imports
  ntoskrnl/exports.cpp
  ntoskrnl/ke.cpp
  ntoskrnl/worker.cpp
  ntoskrnl/pool_ex.cpp
  ntoskrnl/io.cpp
  ntoskrnl/ob_ps_mm.cpp
  ntoskrnl/rtl_crt.cpp
  ntoskrnl/registry.cpp
  # driver host: namespace, object registry, loader, IRP engine, dispatcher hooks
  host/objects.cpp
  host/namespace.cpp
  host/driver_loader.cpp
  host/irp_top.cpp
  host/hooks.cpp
  host/host.cpp
  # privileged-instruction trap-and-emulate (VEH: cr8/msr/port I/O/cli/sti)
  host/privileged.cpp
  # minimal WDF (KMDF) function-table shim
  kmdf/wdf_shim.cpp
)
