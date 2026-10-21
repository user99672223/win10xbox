/* guest_hello: smallest possible real Windows program. Exit code 42. */
#include <stdio.h>

int main(void) {
    printf("hello from guest\n");
    fflush(stdout);
    return 42;
}
