# include <cstdio>
#include <cstring>
class Reader {
public:
    int load(const char *p) {
        FILE *f = fopen(p, "r");
        if (!f) {
            return -1;
        }
        char buf[256];
        size_t got = fread(buf, 1, sizeof buf, f);
        if (got == 0 && ferror(f)) {
            (void)fclose(f);
            return -2;
        }
        if (fclose(f) != 0)
            return -3;
        return (int)got;
    }
};
