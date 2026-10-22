/* Layout of the mingw-w64 ddk headers (Microsoft's documented structures) for the same list. */
#include <stddef.h>
#include <ntddk.h>
#include "layout_table.h"
#define S(t) {"sizeof(" #t ")", sizeof(t)},
#define O(t, f) {"offsetof(" #t "." #f ")", offsetof(t, f)},
#ifdef __MINGW32__
#define OB(t, f, byte) O(t, f)
#else /* the WDK: `f` is a bit field of `byte` */
#define OB(t, f, byte) {"offsetof(" #t "." #f ")", offsetof(t, byte)},
#endif
const layout_entry ddk_layout[] = {
#include "layout_list.inc"
    {0, 0}};
