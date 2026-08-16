/* exosched schedule parser: plain text
 *   "in 90s \"msg\""                 one-shot relative
 *   "at <epoch> \"msg\""             one-shot absolute
 *   "every 10m \"msg\""              recurring (s|m|h|d)
 *   "every 10m \"msg\" until <ep>"   recurring with a last-fire epoch
 */
#include "exosched.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DURATION 315360000LL /* 10 years, mirrors exomind's ttl cap */

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

/* parses "<n><unit>" and returns the duration in seconds */
static int parse_duration(const char **pp, const char *end, int64_t *secs,
                          char *err, size_t errsz)
{
    const char *p = *pp;
    while (p < end && isdigit((unsigned char)*p))
        p++;
    if (p == *pp) {
        snprintf(err, errsz, "bad schedule: expected a number");
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
    if (n <= 0 || (long long)(n * mult) > MAX_DURATION) {
        snprintf(err, errsz, "bad schedule: duration out of range");
        return -1;
    }
    *pp = p;
    *secs = (int64_t)n * mult;
    return 0;
}

/* parses "in <n><unit> ..." into fire_epoch */
static int parse_in(const char **pp, const char *end, int64_t *fire_epoch,
                    char *err, size_t errsz)
{
    int64_t secs = 0;
    if (parse_duration(pp, end, &secs, err, errsz) != 0)
        return -1;
    *fire_epoch = now_epoch() + secs;
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
    if (epoch < now_epoch()) {
        snprintf(err, errsz, "bad schedule: 'at' is in the past");
        return -1;
    }
    *pp = p;
    *fire_epoch = epoch;
    return 0;
}

/* parses "every <n><unit> ..." into repeat_s; the first fire is now+repeat */
static int parse_every(const char **pp, const char *end, int64_t *repeat_s,
                       int64_t *fire_epoch, char *err, size_t errsz)
{
    int64_t secs = 0;
    if (parse_duration(pp, end, &secs, err, errsz) != 0)
        return -1;
    *repeat_s = secs;
    *fire_epoch = now_epoch() + secs;
    return 0;
}

/* parses "until <epoch>" (optional suffix of the every form) */
static int parse_until(const char **pp, const char *end, int64_t *until,
                       char *err, size_t errsz)
{
    skip_ws(pp, end);
    if (end - *pp >= 5 && strncmp(*pp, "until", 5) == 0) {
        *pp += 5;
        skip_ws(pp, end);
        const char *p = *pp;
        while (p < end && isdigit((unsigned char)*p))
            p++;
        if (p == *pp) {
            snprintf(err, errsz, "bad schedule: expected an epoch after 'until'");
            return -1;
        }
        long long epoch = strtoll(*pp, NULL, 10);
        if (epoch <= 0 || epoch > 4102444800LL) {
            snprintf(err, errsz, "bad schedule: until epoch out of range");
            return -1;
        }
        *pp = p;
        *until = epoch;
    }
    return 0;
}

/* consumes a trailing `receipt=1` token if present; returns 0 and sets
 * *out when the rest of the body is empty or only that token */
static int trailing_receipt(const char *p, const char *end, int *out)
{
    skip_ws(&p, end);
    if (p >= end) {
        *out = 0;
        return 1;
    }
    if (end - p == 9 && strncmp(p, "receipt=1", 9) == 0) {
        *out = 1;
        return 1;
    }
    return 0;
}

int parse_schedule(const char *body, size_t len, int64_t *fire_epoch,
                   int64_t *repeat_s, int64_t *until_epoch,
                   int *receipt, char **msg, char *err, size_t errsz)
{
    const char *p = body;
    const char *end = body + len;
    *repeat_s = 0;
    *until_epoch = 0;
    *receipt = 0;
    skip_ws(&p, end);

    if (end - p >= 5 && strncmp(p, "every", 5) == 0 &&
        (p + 5 >= end || p[5] == ' ' || p[5] == '\t')) {
        p += 5;
        skip_ws(&p, end);
        if (parse_every(&p, end, repeat_s, fire_epoch, err, errsz) != 0)
            return -1;
    } else if (end - p >= 2 && p[0] == 'i' && p[1] == 'n') {
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
                 "bad schedule: expected 'in <n><s|m|h|d> \"msg\"', "
                 "'at <epoch> \"msg\"' or 'every <n><s|m|h|d> \"msg\"'");
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
        *msg = b.p ? b.p : xstrdup("");
        skip_ws(&p, end);
        if (p < end) {
            if (*repeat_s > 0 && strncmp(p, "until", 5) == 0 &&
                (p + 5 >= end || p[5] == ' ' || p[5] == '\t')) {
                if (parse_until(&p, end, until_epoch, err, errsz) != 0) {
                    free(*msg);
                    *msg = NULL;
                    return -1;
                }
                if (!trailing_receipt(p, end, receipt)) {
                    free(*msg);
                    *msg = NULL;
                    snprintf(err, errsz,
                             "bad schedule: trailing garbage after 'until'");
                    return -1;
                }
            } else if (trailing_receipt(p, end, receipt)) {
                /* trailing receipt=1 token accepted */
            } else {
                free(*msg);
                *msg = NULL;
                snprintf(err, errsz, "bad schedule: trailing garbage after quote");
                return -1;
            }
        }
    } else {
        if (*repeat_s > 0 && (end - p >= 5 && strncmp(p, "until", 5) == 0)) {
            snprintf(err, errsz,
                     "bad schedule: 'until' requires a quoted message");
            return -1;
        }
        if (p >= end) {
            snprintf(err, errsz, "bad schedule: missing message");
            return -1;
        }
        size_t rest = (size_t)(end - p);
        if (rest > MAX_MSG) {
            snprintf(err, errsz, "bad schedule: message too long");
            return -1;
        }
        const char *tp = p;
        while (tp < end && isspace((unsigned char)*tp))
            tp++;
        if (end - tp == 9 && strncmp(tp, "receipt=1", 9) == 0)
            *receipt = 1;
        char *m = xstrndup(p, rest);
        trim_tail(m);
        if (!m[0]) {
            free(m);
            snprintf(err, errsz, "bad schedule: missing message");
            return -1;
        }
        *msg = m;
    }

    if (*repeat_s > 0 && *until_epoch > 0 && *until_epoch < *fire_epoch) {
        free(*msg);
        *msg = NULL;
        snprintf(err, errsz, "bad schedule: 'until' is before the first fire");
        return -1;
    }
    return 0;
}
