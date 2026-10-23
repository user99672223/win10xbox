#include <stdio.h>
int main(int argc, char **argv) {
    printf("hello from tcc %d\n", 6 * 7);
    printf("argc=%d sizeof(void*)=%d\n", argc, (int)sizeof(void *));
    return 42;
}
