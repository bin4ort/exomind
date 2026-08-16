#include <stdio.h>
int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
int div_safe(int a, int b) {
    if (b == 0) return -1;
    return a / b;
}
const char *calc_name(void) { return "calc v1"; }
int main(void) { printf("%d\n", add(2, 3)); return 0; }
