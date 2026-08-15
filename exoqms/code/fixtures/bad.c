/*
 * exoqms-code fixture: deliberate error-handling violations.
 * Each violation names the check it must trigger.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* missing-error-path: fopen result used without if-not branch */
int read_config(const char *path)
{
    FILE *f = fopen(path, "r");      /* line 15 */
    char buf[256];
    size_t got = fread(buf, 1, sizeof buf, f);  /* f used unguarded */
    fclose(f);
    return (int)got;
}

/* unchecked-return: open() result dropped */
void ensure_file(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0644); /* assignment: fine */
    close(fd);                                      /* close result dropped */
}

/* uninitialized-use: i read before assignment */
int sum_until(int n)
{
    int i;               /* declared, no initializer */
    int total = 0;
    while (i < n) {      /* i read before assignment */
        total += i;
        i++;
    }
    return total;
}

/* swallowed-error: failure checked with an empty branch */
int write_all(int fd, const char *data)
{
    ssize_t w = write(fd, data, strlen(data));
    if (w != (ssize_t)strlen(data)) {
        /* failure eaten: no return, no else */
    }
    if (fsync(fd) != 0) {
        /* failure eaten: empty branch after a direct call */
    }
    return 0;
}

/* unchecked-deref-alloc: malloc result dereferenced unguarded */
int alloc_init(int *out)
{
    int *arr = malloc(16 * sizeof(int));   /* may fail */
    arr[0] = 42;                            /* unguarded deref */
    *out = arr[0];
    free(arr);
    return 0;
}
