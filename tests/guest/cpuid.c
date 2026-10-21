/* guest_cpuid: prints CPUID leaf 1 ECX and the 12-character vendor string.
 * Used to observe what the loader's cpuid patching makes the guest see. */
#include <intrin.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int r[4];
    char vendor[13];

    __cpuid(r, 0);
    memcpy(vendor + 0, &r[1], 4);   /* ebx */
    memcpy(vendor + 4, &r[3], 4);   /* edx */
    memcpy(vendor + 8, &r[2], 4);   /* ecx */
    vendor[12] = 0;

    __cpuid(r, 1);
    printf("cpuid.1.ecx=%08x\n", (unsigned)r[2]);
    printf("vendor=%s\n", vendor);
    fflush(stdout);
    return 0;
}
