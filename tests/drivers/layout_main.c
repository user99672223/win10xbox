/* Compares ntk.h layouts with the ddk headers; prints every mismatch. Exit 0 when identical. */
#include <stdio.h>
#include <string.h>
#include "layout_table.h"

int main(void) {
    int bad = 0, n = 0;
    for (; ntk_layout[n].name && ddk_layout[n].name; ++n) {
        if (strcmp(ntk_layout[n].name, ddk_layout[n].name) != 0) {
            printf("FAIL table mismatch at %d: %s vs %s\n", n, ntk_layout[n].name, ddk_layout[n].name);
            return 2;
        }
        if (ntk_layout[n].value != ddk_layout[n].value) {
            printf("FAIL %s: ntk %llu (0x%llx), ddk %llu (0x%llx)\n", ntk_layout[n].name, ntk_layout[n].value,
                   ntk_layout[n].value, ddk_layout[n].value, ddk_layout[n].value);
            ++bad;
        }
    }
    if (ntk_layout[n].name != ddk_layout[n].name && (ntk_layout[n].name || ddk_layout[n].name)) {
        printf("FAIL tables differ in length\n");
        return 2;
    }
    printf("%d layout facts compared, %d mismatches\n", n, bad);
    return bad ? 1 : 0;
}
