/* Layout of drivers/include/ntk/ntk.h, built standalone (no windows.h). */
#include <stddef.h>
#include "ntk/ntk.h"
#include "layout_table.h"
#define S(t) {"sizeof(" #t ")", sizeof(t)},
#define O(t, f) {"offsetof(" #t "." #f ")", offsetof(t, f)},
#define OB(t, f, byte) O(t, f)
#define SA(t, a) S(t)
const layout_entry ntk_layout[] = {
#include "layout_list.inc"
    {0, 0}};
