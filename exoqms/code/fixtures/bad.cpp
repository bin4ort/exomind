# include <cstdio>
#include <cstring>
class Reader {
public:
    int load(const char *p) {
        FILE *f = fopen(p, "r");
        char buf[256];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        return (int)got;
    }
    char *dup(const char *s) {
        char *d = strdup(s);
        d[0] = toupper(d[0]);
        return d;
    }
};
