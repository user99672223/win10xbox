/* rdrand: 16 RDRAND attempts (8 x r32, 8 x r64) through the layer. The console's CPU has no RDRAND; the layer advertises it
 * (D18) and emulates the #UD. With WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1 the loader rewrites every rdrand/rdseed site to a
 * ud2, so the emulation path also runs on a CPU that has the instruction. Every attempt must set CF (success), and the
 * values must not all be equal. Prints `rdrand: ok n=16`, exit 42. */
#include <windows.h>
#include <immintrin.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define RDRND_TARGET __attribute__((target("rdrnd")))
#else
#define RDRND_TARGET
#endif

RDRND_TARGET static int draw32(unsigned* v) { return _rdrand32_step(v); }
RDRND_TARGET static int draw64(unsigned long long* v) { return _rdrand64_step(v); }

int main(void) {
    unsigned long long vals[16];
    int i, distinct = 0, j;
    for (i = 0; i < 16; i++) {
        int cf;
        if (i < 8) {
            unsigned v = 0;
            cf = draw32(&v);
            vals[i] = v;
        } else {
            unsigned long long v = 0;
            cf = draw64(&v);
            vals[i] = v;
        }
        if (!cf) {
            printf("rdrand: FAIL attempt %d: CF clear\n", i);
            return 1;
        }
    }
    for (i = 1; i < 16; i++) {
        for (j = 0; j < i; j++) if (vals[i] == vals[j]) break;
        if (j == i) distinct++;
    }
    if (distinct == 0) {
        printf("rdrand: FAIL all 16 values equal (%llx)\n", vals[0]);
        return 1;
    }
    printf("rdrand: ok n=16\n");
    return 42;
}
