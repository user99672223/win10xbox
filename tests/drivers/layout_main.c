/* Compares ntk.h / wdf_types.h layouts with Microsoft's headers (mingw-w64 ddk, KMDF 1.33); prints every mismatch. */
#include <stdio.h>
#include <string.h>
#include "layout_table.h"

static int compare(const char* what, const layout_entry* a, const layout_entry* b) {
    int bad = 0, n = 0;
    for (; a[n].name && b[n].name; ++n) {
        if (strcmp(a[n].name, b[n].name) != 0) {
            printf("FAIL %s: table mismatch at %d: %s vs %s\n", what, n, a[n].name, b[n].name);
            return 1000;
        }
        if (a[n].value != b[n].value) {
            printf("FAIL %s %s: ours %llu (0x%llx), Microsoft's %llu (0x%llx)\n", what, a[n].name, a[n].value, a[n].value,
                   b[n].value, b[n].value);
            ++bad;
        }
    }
    if (a[n].name || b[n].name) {
        printf("FAIL %s: tables differ in length\n", what);
        return 1000;
    }
    printf("%s: %d layout facts compared, %d mismatches\n", what, n, bad);
    return bad;
}

int main(void) {
    int bad = compare("ntk.h vs ddk", ntk_layout, ddk_layout);
#ifdef HAVE_WDF
    bad += compare("wdf_types.h vs KMDF 1.33", wdf_shim_layout, wdf_real_layout);
#endif
    return bad ? 1 : 0;
}
