#include <stddef.h>
#include <ntddk.h>
#include "kmdf_min.h"
#include "layout_table.h"
#define S(t) {"sizeof(" #t ")", sizeof(t)},
#define O(t, f) {"offsetof(" #t "." #f ")", offsetof(t, f)},
const layout_entry wdf_real_layout[] = {
#include "layout_wdf_list.inc"
    {0, 0}};
