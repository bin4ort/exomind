/* exosched schedule parser: plain text "in 90s \"msg\"" / "at <epoch> \"msg\"". */
#include "exosched.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void skip_ws(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' ||
                       **p == '\n'))
        (*p)++;
}

static void trim_tail(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                     s[n - 1] == '\n'))
        s[--n] = 0;
}

/* parses "in <n><unit> ..." into fire_epoch */
static int parse_in(const char **pp, const char *end, int64_t *fire_epoch,
                    char *err, size_t errsz)
{
    const char *p = *pp;
    while (p < end && isdigit((unsigned char)*p))
        p++;
    if (p == *pp) {
        snprintf(err, errsz, "bad schedule: expected a number after 'in'");
        return -1;
    }
    long long n = strtoll(*pp, NULL, 10);
    const char *un = p;
    if (un >= end) {
        snprintf(err, errsz, "bad schedule: missing unit (s/m/h/d)");
        return -1;
    }
    long mult;
    switch (*un) {
    case 's': mult = 1; break;
    case 'm': mult = 60; break;
    case 'h': mult = 3600; break;
    case 'd': mult = 86400; break;
    default:
        snprintf(err, errsz, "bad schedule: unknown unit '%c' (use s/m/h/d)",
                 *un);
        return -1;
    }
    p = un + 1;
    if (n <= 0 || n > 315360000) {
        snprintf(err, errsz, "bad schedule: duration out of range");
        return -1;
    }
    *pp = p;
    *fire_epoch = now_epoch() + n * mult;
    return 0;
}

/* parses "at <epoch> ..." into fire_epoch */
static int parse_at(const char **pp, const char *end, int64_t *fire_epoch,
                    char *err, size_t errsz)
{
    const char *p = *pp;
    while (p < end && (isdigit((unsigned char)*p) ||
                       (p == *pp && *p == '-')))
        p++;
    if (p == *pp) {
        snprintf(err, errsz, "bad schedule: expected an epoch number after 'at'");
        return -1;
    }
    long long epoch = strtoll(*pp, NULL, 10);
    if (epoch < 0 || epoch > 4102444800LL) {
        snprintf(err, errsz, "bad schedule: epoch out of range");
        return -1;
    }
    *pp = p;
    *fire_epoch = epoch;
    return 0;
}

int parse_schedule(const char *body, size_t len, int64_t *fire_epoch,
                   char **msg, char *err, size_t errsz)
{
    const char *p = body;
    const char *end = body + len;
    skip_ws(&p, end);

    if (end - p >= 2 && p[0] == 'i' && p[1] == 'n') {
        p += 2;
        skip_ws(&p, end);
        if (parse_in(&p, end, fire_epoch, err, errsz) != 0)
            return -1;
    } else if (end - p >= 2 && p[0] == 'a' && p[1] == 't') {
        p += 2;
        skip_ws(&p, end);
        if (parse_at(&p, end, fire_epoch, err, errsz) != 0)
            return -1;
    } else {
        snprintf(err, errsz,
                 "bad schedule: expected 'in <n><s|m|h|d> \"msg\"' or "
                 "'at <epoch> \"msg\"'");
        return -1;
    }

    skip_ws(&p, end);
    if (p < end && *p == '"') {
        p++;
        buf_t b = {0};
        int closed = 0;
        while (p < end) {
            char c = *p;
            if (c == '"') {
                p++;
                closed = 1;
                break;
            }
            if (c == '\\' && p + 1 < end) {
                char e = p[1];
                if (e == '"' || e == '\\') {
                    buf_put(&b, &e, 1);
                    p += 2;
                    continue;
                }
                /* unknown escape: keep both chars literally */
                buf_put(&b, &c, 1);
                buf_put(&b, &e, 1);
                p += 2;
                continue;
            }
            buf_put(&b, &c, 1);
            p++;
            if (b.len > MAX_MSG) {
                buf_free(&b);
                snprintf(err, errsz, "bad schedule: message too long");
                return -1;
            }
        }
        if (!closed) {
            buf_free(&b);
            snprintf(err, errsz, "bad schedule: unterminated quote");
            return -1;
        }
        skip_ws(&p, end);
        if (p < end) {
            buf_free(&b);
            snprintf(err, errsz, "bad schedule: trailing garbage after quote");
            return -1;
        }
        *msg = b.p ? b.p : xstrdup("");
    } else {
        if (p >= end) {
            snprintf(err, errsz, "bad schedule: missing message");
            return -1;
        }
        size_t rest = (size_t)(end - p);
        if (rest > MAX_MSG) {
            snprintf(err, errsz, "bad schedule: message too long");
            return -1;
        }
        char *m = xstrndup(p, rest);
        trim_tail(m);
        if (!m[0]) {
            free(m);
            snprintf(err, errsz, "bad schedule: missing message");
            return -1;
        }
        *msg = m;
    }
    return 0;
}
