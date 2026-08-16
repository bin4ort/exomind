/* exocrawl: private metasearch. SearXNG JSON API (multi-instance,
 * rotated) with a DuckDuckGo HTML fallback. No accounts, no cookies. */
#include "exocrawl.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

static const char *SEARXNG_INSTANCES[] = {
    "https://searx.be",
    "https://search.brave4u.com",
    "https://searx.tiekoetter.com",
    "https://opnxng.com",
    "https://baresearch.org",
    "https://paulgo.io",
    NULL};

#define SEARXNG_INSTANCES_COUNT 6

static size_t g_inst_idx;

/* strip <...> tags from a snippet */
static void strip_html(char *s)
{
    char *w = s;
    int in_tag = 0;
    for (char *r = s; *r; r++) {
        if (*r == '<') {
            in_tag = 1;
            continue;
        }
        if (*r == '>') {
            in_tag = 0;
            continue;
        }
        if (!in_tag)
            *w++ = *r;
    }
    *w = 0;
}

/* crude JSON string extraction: find "field" and copy its string value */
static int json_str_field(const char *j, size_t n, const char *field,
                          char *out, size_t cap)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", field);
    const char *p = j;
    const char *end = j + n;
    while ((p = strstr(p, needle)) && p < end) {
        p += strlen(needle);
        while (p < end && (isspace((unsigned char)*p) || *p == ':'))
            p++;
        if (p < end && *p == '"') {
            p++;
            size_t w = 0;
            while (p < end && *p != '"' && w + 1 < cap) {
                if (*p == '\\' && p + 1 < end) {
                    p++;
                    if (*p == 'n')
                        out[w++] = ' ';
                    else if (*p == 't')
                        out[w++] = ' ';
                    else if (*p == '\\' || *p == '"')
                        out[w++] = *p;
                    p++;
                } else {
                    out[w++] = *p++;
                }
            }
            out[w] = 0;
            return w > 0 ? 1 : 0;
        }
        p += strlen(needle);
    }
    return 0;
}

/* count "results" objects by scanning for {"url": ...} within the results
 * array: pragmatic approach - find `"results": [` then walk objects. */
static void searxng_parse(const char *body, size_t n, result_t **out,
                          size_t *n_out, int limit)
{
    const char *res = strstr(body, "\"results\"");
    if (!res)
        return;
    const char *arr = strchr(res, '[');
    if (!arr)
        return;
    const char *p = arr;
    const char *end = body + n;
    result_t *r = NULL;
    size_t nr = 0;
    while (p < end && nr < (size_t)limit) {
        const char *obj = strchr(p, '{');
        if (!obj || obj >= end)
            break;
        const char *close = strchr(obj, '}');
        if (!close || close >= end)
            break;
        size_t olen = (size_t)(close - obj + 1);
        result_t rr;
        memset(&rr, 0, sizeof rr);
        char tmp[4096];
        if (json_str_field(obj, olen, "url", tmp, sizeof tmp) && tmp[0]) {
            rr.url = strdup(tmp);
            char t2[2048], s2[2048];
            if (json_str_field(obj, olen, "title", t2, sizeof t2))
                rr.title = strdup(t2);
            if (json_str_field(obj, olen, "content", s2, sizeof s2)) {
                strip_html(s2);
                rr.snippet = strdup(s2);
            }
            r = realloc(r, (nr + 1) * sizeof(result_t));
            if (!r)
                break;
            r[nr++] = rr;
        }
        p = close + 1;
    }
    *out = r;
    *n_out = nr;
}

/* DuckDuckGo HTML: parse .result elements */
static void ddg_parse(const char *body, size_t n, result_t **out,
                      size_t *n_out, int limit)
{
    const char *p = body;
    const char *end = body + n;
    result_t *r = NULL;
    size_t nr = 0;
    while (p < end && nr < (size_t)limit) {
        const char *res = strstr(p, "result__a");
        if (!res || res >= end)
            break;
        /* url in href */
        const char *h = res;
        while (h < end && (h - res) < 512 && strncmp(h, "href=", 5) != 0)
            h++;
        char url[4096] = "";
        if (h < end && (h - res) < 512) {
            h += 5;
            if (*h == '"' || *h == '\'')
                h++;
            const char *u = h;
            while (h < end && *h != '"' && *h != '\'')
                h++;
            size_t ul = (size_t)(h - u);
            if (ul > 0 && ul < sizeof url) {
                memcpy(url, u, ul);
                url[ul] = 0;
            }
            /* strip ddg redirect prefix */
            const char *uddg = strstr(url, "uddg=");
            if (uddg) {
                char dec[2048];
                const char *v = uddg + 5;
                size_t w = 0;
                for (; *v && *v != '&' && w + 1 < sizeof dec; v++) {
                    if (*v == '%' && v[1] && v[2]) {
                        char hb[3] = {v[1], v[2], 0};
                        dec[w++] = (char)strtol(hb, NULL, 16);
                        v += 2;
                    } else {
                        dec[w++] = *v;
                    }
                }
                dec[w] = 0;
                snprintf(url, sizeof url, "%s", dec);
            }
        }
        /* title: text after the anchor open */
        const char *gt = strchr(res, '>');
        const char *tend = gt ? strstr(gt + 1, "</a>") : NULL;
        char title[2048] = "";
        if (gt && tend && tend < end) {
            size_t tl = (size_t)(tend - gt - 1);
            if (tl < sizeof title) {
                memcpy(title, gt + 1, tl);
                title[tl] = 0;
            }
        }
        /* snippet: next result__snippet */
        const char *sn = tend ? strstr(tend, "result__snippet") : NULL;
        char snip[2048] = "";
        if (sn && sn < end) {
            const char *sgt = strchr(sn, '>');
            const char *stend = sgt ? strstr(sgt + 1, "</a>") : NULL;
            if (!stend)
                stend = sgt ? strstr(sgt + 1, "</div>") : NULL;
            if (sgt && stend && stend < end) {
                size_t sl = (size_t)(stend - sgt - 1);
                if (sl < sizeof snip) {
                    memcpy(snip, sgt + 1, sl);
                    snip[sl] = 0;
                }
            }
        }
        /* filter sponsored/ads: DDG HTML includes paid results */
        if (strstr(url, "ad_domain") || strstr(url, "aclick") ||
            strstr(url, "y.js?ad"))
            p = (sn && sn < end) ? sn + 1 : (tend ? tend + 1 : res + 1);
        if (url[0]) {
            result_t rr;
            memset(&rr, 0, sizeof rr);
            rr.url = strdup(url);
            strip_html(title);
            strip_html(snip);
            char dec[2048];
            html_entity_decode(title, strlen(title), dec, sizeof dec);
            rr.title = strdup(dec);
            html_entity_decode(snip, strlen(snip), dec, sizeof dec);
            rr.snippet = strdup(dec);
            r = realloc(r, (nr + 1) * sizeof(result_t));
            if (!r)
                break;
            r[nr++] = rr;
        }
        p = (sn && sn < end) ? sn + 1 : (tend ? tend + 1 : res + 1);
    }
    *out = r;
    *n_out = nr;
}

static int searxng_search(net_t *n, const char *q, int limit, result_t **out,
                          size_t *n_out, char *err, size_t errsz)
{
    char *enc = url_encode(q);
    if (!enc)
        return -1;
    size_t start = __sync_fetch_and_add(&g_inst_idx, 1);
    for (size_t k = 0; k < 4; k++) {
        const char *inst =
            SEARXNG_INSTANCES[(start + k) % SEARXNG_INSTANCES_COUNT];
        char url[8192];
        snprintf(url, sizeof url,
                 "%s/search?q=%s&format=json&language=en", inst, enc);
        resp_t r;
        if (net_fetch(n, url, &r, err, errsz) == 0 && r.status == 200 &&
            r.body && r.blen > 0) {
            searxng_parse(r.body, r.blen, out, n_out, limit);
            resp_free(&r);
            if (*n_out > 0) {
                free(enc);
                return 0;
            }
            /* empty: try next instance */
            continue;
        }
        resp_free(&r);
    }
    free(enc);
    snprintf(err, errsz, "no searxng instance returned results");
    return -1;
}

static int ddg_search(net_t *n, const char *q, int limit, result_t **out,
                      size_t *n_out, char *err, size_t errsz)
{
    char *enc = url_encode(q);
    if (!enc)
        return -1;
    char url[8192];
    snprintf(url, sizeof url, "https://html.duckduckgo.com/html/?q=%s", enc);
    resp_t r;
    if (net_fetch(n, url, &r, err, errsz) != 0 || r.status != 200 ||
        !r.body) {
        free(enc);
        return -1;
    }
    ddg_parse(r.body, r.blen, out, n_out, limit);
    resp_free(&r);
    free(enc);
    return *n_out > 0 ? 0 : -1;
}

int search_run(net_t *n, const char *q, const char *engines, int limit,
               result_t **out, size_t *n_out, char *err, size_t errsz)
{
    *out = NULL;
    *n_out = 0;
    result_t *r = NULL;
    size_t nr = 0;
    int want_searxng = !engines || strstr(engines, "searxng") != NULL ||
                       strstr(engines, "all") != NULL;
    int want_ddg = engines && strstr(engines, "ddg") != NULL;
    if (!engines)
        want_ddg = 1;
    if (want_searxng) {
        size_t m = 0;
        result_t *sr = NULL;
        if (searxng_search(n, q, limit, &sr, &m, err, errsz) == 0 && m > 0) {
            r = sr;
            nr = m;
        }
    }
    if (want_ddg && nr < (size_t)limit) {
        size_t m = 0;
        result_t *dr = NULL;
        if (ddg_search(n, q, limit - (int)nr, &dr, &m, err, errsz) == 0 &&
            m > 0) {
            for (size_t i = 0; i < m && nr < (size_t)limit; i++) {
                r = realloc(r, (nr + 1) * sizeof(result_t));
                if (!r)
                    break;
                r[nr++] = dr[i];
            }
            free(dr);
        }
    }
    if (nr == 0) {
        snprintf(err, errsz, "no results from any engine");
        return -1;
    }
    *out = r;
    *n_out = nr;
    return 0;
}

void results_free(result_t *r, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(r[i].title);
        free(r[i].url);
        free(r[i].snippet);
    }
    free(r);
}
