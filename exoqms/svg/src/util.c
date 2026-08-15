/* util.c — small memory/string/file helpers (same conventions as exoqms-ui). */
#include "svg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "error: out of memory\n");
        exit(2);
    }
    return p;
}

void *xcalloc(size_t n)
{
    void *p = calloc(n ? n : 1, 1);
    if (!p) {
        fprintf(stderr, "error: out of memory\n");
        exit(2);
    }
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

void buf_append(buf_t *b, const void *d, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n + 1)
            nc *= 2;
        b->p = xrealloc_(b->p, nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
}

void *xrealloc_(void *p, size_t n)
{
    p = realloc(p, n);
    if (!p) {
        fprintf(stderr, "error: out of memory\n");
        exit(2);
    }
    return p;
}

void buf_puts(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

void vec_push(vec_t *v, void *p)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->it = xrealloc_(v->it, v->cap * sizeof(void *));
    }
    v->it[v->len++] = p;
}

int ci_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int ascii_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

int ascii_digit(int c)
{
    return c >= '0' && c <= '9';
}

void lc_ascii(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s += 32;
}

char *json_escape(const char *s, size_t n)
{
    buf_t b = {0};
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_puts(&b, "\\\""); break;
        case '\\': buf_puts(&b, "\\\\"); break;
        case '\n': buf_puts(&b, "\\n"); break;
        case '\r': buf_puts(&b, "\\r"); break;
        case '\t': buf_puts(&b, "\\t"); break;
        default:
            if (c < 0x20)
                buf_append(&b, "\uFFFD", strlen("\uFFFD"));
            else
                buf_append(&b, &s[i], 1);
        }
    }
    return b.p ? b.p : xstrdup("");
}

/* read a whole file; strips NUL bytes (SVG is text) */
char *file_read(const char *path, size_t *len, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *data;
    size_t i, n, w = 0;
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "%s: seek failed", path);
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        snprintf(err, errsz, "%s: seek failed", path);
        fclose(f);
        return NULL;
    }
    data = xmalloc((size_t)sz + 1);
    n = fread(data, 1, (size_t)sz, f);
    fclose(f);
    for (i = 0; i < n; i++)
        if (data[i] != 0)
            data[w++] = data[i];
    data[w] = 0;
    *len = w;
    return data;
}

/* recursive *.svg walk; out entries are strdup'd paths, caller frees */
int dir_walk_svg(const char *dir, vec_t *out)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char path[4096];
        struct stat st;
        size_t pl = strlen(de->d_name);
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            dir_walk_svg(path, out);
        } else if (S_ISREG(st.st_mode) && pl >= 4 &&
                   strcmp(de->d_name + pl - 4, ".svg") == 0) {
            vec_push(out, xstrdup(path));
        }
    }
    closedir(d);
    return 0;
}
