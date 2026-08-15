/*
 * exoqms-code fixture: the same operations done right.
 * Every error-returning result is guarded with an if-not/else path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fopen guarded with if-not + else; failure propagates */
int read_config(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1; /* error path points here */
    }
    size_t got = fread(buf, 1, n, f);
    if (got == 0 && ferror(f)) {
        (void)fclose(f); /* deliberate disclaimer: already failing */
        return -2;
    }
    if (fclose(f) != 0)
        return -3;
    return (int)got;
}

/* open + close results checked */
int ensure_file(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    if (close(fd) != 0)
        return -2;
    return 0;
}

/* i initialized at declaration */
int sum_until(int n)
{
    int i = 0;
    int total = 0;
    while (i < n) {
        total += i;
        i++;
    }
    return total;
}

/* write result checked, failure handled, (void) disclaimer where deliberate */
int write_all(int fd, const char *data)
{
    size_t len = strlen(data);
    ssize_t w = write(fd, data, len);
    if (w != (ssize_t)len)
        return -1;
    (void)fsync(fd); /* deliberate: fire-and-forget flush */
    return 0;
}

/* malloc checked before deref */
int alloc_init(int *out)
{
    int *arr = malloc(16 * sizeof(int));
    if (!arr)
        return -1;
    arr[0] = 42;
    *out = arr[0];
    free(arr);
    return 0;
}
