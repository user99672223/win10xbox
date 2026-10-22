/* Layout of the mingw-w64 ddk headers (Microsoft's documented structures) for the same list. */
#include <stddef.h>
#include <ntddk.h>
#include "layout_table.h"
#define S(t) {"sizeof(" #t ")", sizeof(t)},
#define O(t, f) {"offsetof(" #t "." #f ")", offsetof(t, f)},
#ifdef __MINGW32__
#define OB(t, f, byte) O(t, f)
#define SA(t, a) {"sizeof(" #t ")", (sizeof(t) + (a) - 1) / (a) * (a)},   /* mingw-w64 omits DECLSPEC_ALIGN */
#else /* the WDK: `f` is a bit field of `byte`; the declared alignment is in sizeof already */
#define OB(t, f, byte) {"offsetof(" #t "." #f ")", offsetof(t, byte)},
#define SA(t, a) S(t)
#endif
const layout_entry ddk_layout[] = {
#include "layout_list.inc"
    {0, 0}};
