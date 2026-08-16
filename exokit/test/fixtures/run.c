#include <stdio.h>
#include <string.h>
static int add(int a, int b) { return a + b; }
static int div_safe(int a, int b) { return a / b; }
static int sub(int a, int b) { return a - b; }
static const char *calc_name(void) { return "calc v1"; }
int main(void)
{
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *nl = strchr(tab + 1, '\n');
        if (nl) *nl = 0;
        const char *fn = line, *args = tab + 1;
        int a = 0, b = 0;
        if (!strcmp(fn, "add")) {
            sscanf(args, "%d %d", &a, &b);
            printf("%d\n", add(a, b));
        } else if (!strcmp(fn, "div_safe")) {
            sscanf(args, "%d %d", &a, &b);
            if (b == 0)
                printf("error: division by zero\n");
            else
                printf("%d\n", div_safe(a, b));
        } else if (!strcmp(fn, "sub")) {
            sscanf(args, "%d %d", &a, &b);
            printf("%d\n", sub(a, b));
        } else if (!strcmp(fn, "calc_name")) {
            printf("%s\n", calc_name());
        } else {
            printf("error: no such fn %s\n", fn);
        }
        fflush(stdout);
    }
    return 0;
}
