/* Subset of <wdf.h> for the KMDF test driver, in an order clang accepts.
 * The WDK's wdf.h includes wdfdevice.h before wdfrequest.h, and wdfdevice.h defines an inline function that takes
 * WDF_REQUEST_TYPE by value: MSVC accepts the forward-declared enum, clang does not. Including wdfrequest.h first
 * defines the enum early; everything else is the unmodified Microsoft header set (third_party/Windows-Driver-Frameworks).
 */
#ifndef KMDF_MIN_H
#define KMDF_MIN_H
#ifndef _Dispatch_type_
#include <driverspecs.h>
#endif
#define WdfFunctions WdfFunctions_01033
typedef VOID (*WDFFUNC)(VOID);
extern const WDFFUNC *WdfFunctions;
#include "wdftypes.h"
#include "wdfglobals.h"
#include "wdffuncenum.h"
#include "wdfstatus.h"
#include "wdfassert.h"
#include "wdfverifier.h"
#include "wdfpool.h"
#include "wdfobject.h"
#include "wdfsync.h"
#include "wdfcore.h"
#include "wdfdriver.h"
#include "wdfmemory.h"
#include "wdffileobject.h"
#include "wdfrequest.h"
#include "wdfdevice.h"
#include "wdfio.h"
#include "wdfcontrol.h"
#endif
