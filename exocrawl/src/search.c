/* exocrawl: independent private metasearch.
 *
 * No third-party aggregator: five search engines are fetched directly
 * through our own HTTP transport (curl binary) and parsed by our own
 * adapters — DuckDuckGo HTML, Mojeek HTML, Marginalia HTML, Bing HTML,
 * and Wikipedia opensearch JSON. Sponsored results are filtered per
 * engine. Requests are stateless: no accounts, no cookies, UA rotation,
 * per-host pacing.
 */
#include "exocrawl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void html_unescape(char *s)
{
    char dec[2048];
    html_entity_decode(s, strlen(s), dec, sizeof dec);
    snprintf(s, 2048, "%s", dec);
}

/* ---------------- helpers ---------------- */

typedef struct {
    result_t *r;
    size_t nr;
    size_t cap;
    int limit;
} resbuf_t;

static void rb_add(resbuf_t *rb, const char *title, const char *url,
                   const char *snip)
{
    if (!title || !url || !url[0] || rb->nr >= (size_t)rb->limit)
        return;
    /* sponsored filter: known ad url shapes */
    if (strstr(url, "ad_domain") || strstr(url, "aclick") ||
        strstr(url, "y.js?ad") || strstr(url, "sponsored") ||
        strstr(url, "advert"))
        return;
    if (rb->nr >= rb->cap) {
        rb->cap = rb->cap ? rb->cap * 2 : 16;
        result_t *nw = realloc(rb->r, rb->cap * sizeof(result_t));
        if (!nw)
            return;
        rb->r = nw;
    }
    result_t *e = &rb->r[rb->nr++];
    memset(e, 0, sizeof *e);
    char t[1024], s[2048];
    snprintf(t, sizeof t, "%s", title);
    snprintf(s, sizeof s, "%s", snip ? snip : "");
    strip_html(t);
    strip_html(s);
    html_unescape(t);
    html_unescape(s);
    e->title = strdup(t);
    e->url = strdup(url);
    e->snippet = strdup(s);
}

static void rb_free(resbuf_t *rb)
{
    results_free(rb->r, rb->nr);
    rb->r = NULL;
    rb->nr = 0;
    rb->cap = 0;
}

static void rb_merge(resbuf_t *dst, resbuf_t *src)
{
    for (size_t i = 0; i < src->nr && dst->nr < (size_t)dst->limit; i++) {
        dst->r = realloc(dst->r, (dst->nr + 1) * sizeof(result_t));
        if (!dst->r)
            break;
        dst->r[dst->nr++] = src->r[i];
    }
    free(src->r);
    src->r = NULL;
    src->nr = 0;
}

/* find an attribute value in a tag string (href, class, ...) */
static int tag_attr(const char *tag, size_t n, const char *name, char *out,
                    size_t cap)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < n; i++) {
        if (tag[i] == name[0] &&
            (i == 0 || isspace((unsigned char)tag[i - 1])) &&
            strncmp(tag + i, name, nl) == 0 && tag[i + nl] == '=') {
            size_t j = i + nl + 1;
            char q = 0;
            if (tag[j] == '"' || tag[j] == '\'') {
                q = tag[j];
                j++;
            }
            size_t w = 0;
            while (j < n && (q ? tag[j] != q : (tag[j] != ' ' && tag[j] != '>'))) {
                if (w + 1 < cap)
                    out[w++] = tag[j];
                j++;
            }
            out[w] = 0;
            return 1;
        }
    }
    return 0;
}

/* skip an HTML comment */

/* ---------------- engine: DuckDuckGo HTML ---------------- */

static void ddg_parse(const char *body, size_t n, resbuf_t *rb)
{
    const char *p = body;
    const char *end = body + n;
    while (p < end && rb->nr < (size_t)rb->limit) {
        const char *res = strstr(p, "result__a");
        if (!res || res >= end)
            break;
        /* tag containing href */
        const char *gt = strchr(res, '>');
        const char *lt = res - 1;
        while (lt > body && *lt != '<')
            lt--;
        char href[4096] = "";
        char tag[8192];
        size_t tl = (size_t)(gt - lt + 1);
        if (tl < sizeof tag) {
            memcpy(tag, lt, tl);
            tag[tl] = 0;
            tag_attr(tag, tl, "href", href, sizeof href);
        }
        /* decode the ddg redirect and strip it */
        const char *uddg = strstr(href, "uddg=");
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
            snprintf(href, sizeof href, "%s", dec);
        }
        char title[2048] = "";
        if (gt && gt + 1 < end) {
            const char *tend = strstr(gt + 1, "</a>");
            if (tend && tend < end && (size_t)(tend - gt - 1) < sizeof title) {
                memcpy(title, gt + 1, (size_t)(tend - gt - 1));
                title[tend - gt - 1] = 0;
            }
        }
        const char *sn = gt ? strstr(gt, "result__snippet") : NULL;
        char snip[2048] = "";
        if (sn && sn < end) {
            const char *sgt = strchr(sn, '>');
            const char *stend = sgt ? strstr(sgt + 1, "</a>") : NULL;
            if (!stend)
                stend = sgt ? strstr(sgt + 1, "</div>") : NULL;
            if (sgt && stend && stend < end &&
                (size_t)(stend - sgt - 1) < sizeof snip) {
                memcpy(snip, sgt + 1, (size_t)(stend - sgt - 1));
                snip[stend - sgt - 1] = 0;
            }
        }
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "ddg: title=[%s] href=[%s] snip=[%s]\n", title, href, snip);
        rb_add(rb, title, href, snip);
        p = (sn && sn < end) ? sn + 1 : (gt ? gt + 1 : res + 1);
    }
}

/* ---------------- engine: Mojeek ---------------- */

static void mojeek_parse(const char *body, size_t n, resbuf_t *rb)
{
    const char *p = body;
    const char *end = body + n;
    while (p < end && rb->nr < (size_t)rb->limit) {
        const char *li = strstr(p, "class=\"result\"");
        if (!li || li >= end)
            break;
        const char *lt = li - 1;
        while (lt > body && *lt != '<')
            lt--;
        /* within this <li>, find the title anchor and snippet */
        const char *seg = li;
        const char *se = strstr(seg, "</li>");
        if (!se || se >= end)
            se = end;
        const char *a = strstr(seg, "class=\"title\"");
        char url[4096] = "", title[2048] = "";
        if (a && a < se) {
            const char *ag = strchr(a, '>');
            const char *al = a - 1;
            while (al > seg && *al != '<')
                al--;
            char tag[4096];
            size_t atl = (size_t)(ag - al + 1);
            if (atl < sizeof tag) {
                memcpy(tag, al, atl);
                tag[atl] = 0;
                tag_attr(tag, atl, "href", url, sizeof url);
            }
            if (ag && ag + 1 < se) {
                const char *tend = strstr(ag + 1, "</a>");
                if (tend && tend < se && (size_t)(tend - ag - 1) < sizeof title) {
                    memcpy(title, ag + 1, (size_t)(tend - ag - 1));
                    title[tend - ag - 1] = 0;
                }
            }
        }
        const char *sn = strstr(seg, "class=\"s\"");
        char snip[2048] = "";
        if (sn && sn < se) {
            const char *sgt = strchr(sn, '>');
            const char *stend = sgt ? strstr(sgt + 1, "</p>") : NULL;
            if (sgt && stend && stend < se &&
                (size_t)(stend - sgt - 1) < sizeof snip) {
                memcpy(snip, sgt + 1, (size_t)(stend - sgt - 1));
                snip[stend - sgt - 1] = 0;
            }
        }
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "marg: [%s] [%s]\n", title, url);
        rb_add(rb, title, url, snip);
        p = se < end ? se + 5 : end;
    }
}

/* ---------------- engine: Marginalia ---------------- */

static void marginalia_parse(const char *body, size_t n, resbuf_t *rb)
{
    const char *p = body;
    const char *end = body + n;
    while (p < end && rb->nr < (size_t)rb->limit) {
        const char *li = strstr(p, "results-item");
        if (!li || li >= end)
            break;
        const char *se = strstr(li, "</li>");
        if (!se || se >= end)
            se = end;
        /* first anchor inside the item */
        const char *a = strchr(li, '<');
        char url[4096] = "", title[2048] = "";
        while (a && a < se) {
            const char *ag = strchr(a, '>');
            if (!ag || ag >= se)
                break;
            char tag[4096];
            size_t tl = (size_t)(ag - a + 1);
            if (tl < sizeof tag) {
                memcpy(tag, a, tl);
                tag[tl] = 0;
                if (strncmp(tag, "<a ", 3) == 0 ||
                    strncmp(tag, "<a>", 3) == 0) {
                    tag_attr(tag, tl, "href", url, sizeof url);
                    const char *tend = strstr(ag + 1, "</a>");
                    if (tend && tend < se &&
                        (size_t)(tend - ag - 1) < sizeof title) {
                        memcpy(title, ag + 1, (size_t)(tend - ag - 1));
                        title[tend - ag - 1] = 0;
                    }
                    break;
                }
            }
            a = strchr(ag + 1, '<');
        }
        const char *sn = strstr(li, "description");
        char snip[2048] = "";
        if (sn && sn < se) {
            const char *sgt = strchr(sn, '>');
            const char *stend = sgt ? strstr(sgt + 1, "</p>") : NULL;
            if (!stend)
                stend = sgt ? strstr(sgt + 1, "</div>") : NULL;
            if (sgt && stend && stend < se &&
                (size_t)(stend - sgt - 1) < sizeof snip) {
                memcpy(snip, sgt + 1, (size_t)(stend - sgt - 1));
                snip[stend - sgt - 1] = 0;
            }
        }
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "marg: [%s] [%s]\n", title, url);
        rb_add(rb, title, url, snip);
        p = se < end ? se + 5 : end;
    }
}

/* ---------------- engine: Bing ---------------- */

static void bing_parse(const char *body, size_t n, resbuf_t *rb)
{
    const char *p = body;
    const char *end = body + n;
    while (p < end && rb->nr < (size_t)rb->limit) {
        const char *algo = strstr(p, "b_algo");
        if (!algo || algo >= end)
            break;
        const char *se = strstr(algo, "</li>");
        if (!se || se >= end)
            se = end;
        const char *h2 = strstr(algo, "<h2");
        char url[4096] = "", title[2048] = "";
        if (h2 && h2 < se) {
            const char *a = strchr(h2, '<');
            while (a && a < se) {
                const char *ag = strchr(a, '>');
                if (!ag || ag >= se)
                    break;
                char tag[4096];
                size_t tl = (size_t)(ag - a + 1);
                if (tl < sizeof tag) {
                    memcpy(tag, a, tl);
                    tag[tl] = 0;
                    if (strncmp(tag, "<a ", 3) == 0) {
                        tag_attr(tag, tl, "href", url, sizeof url);
                        const char *tend = strstr(ag + 1, "</a>");
                        if (tend && tend < se &&
                            (size_t)(tend - ag - 1) < sizeof title) {
                            memcpy(title, ag + 1, (size_t)(tend - ag - 1));
                            title[tend - ag - 1] = 0;
                        }
                        break;
                    }
                }
                a = strchr(ag + 1, '<');
            }
        }
        const char *sn = strstr(algo, "<p");
        char snip[2048] = "";
        if (sn && sn < se) {
            const char *sgt = strchr(sn, '>');
            const char *stend = sgt ? strstr(sgt + 1, "</p>") : NULL;
            if (sgt && stend && stend < se &&
                (size_t)(stend - sgt - 1) < sizeof snip) {
                memcpy(snip, sgt + 1, (size_t)(stend - sgt - 1));
                snip[stend - sgt - 1] = 0;
            }
        }
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "marg: [%s] [%s]\n", title, url);
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "bing: [%s] [%s]\n", title, url);
        rb_add(rb, title, url, snip);
        p = se < end ? se + 5 : end;
    }
}

/* ---------------- engine: Wikipedia opensearch ---------------- */

static void wiki_parse(const char *body, size_t n, resbuf_t *rb)
{
    /* [query, [titles...], [descriptions...], [urls...]] */
    const char *arr[4] = {NULL, NULL, NULL, NULL};
    const char *p = body;
    const char *end = body + n;
    for (int k = 0; k < 4 && p < end; k++) {
        p = strchr(p, '[');
        if (!p)
            break;
        arr[k] = p;
        p++;
    }
    if (getenv("EXOCRAWL_DEBUG"))
        fprintf(stderr, "wiki arr: %d %d %d %d\n",
                arr[0] != NULL, arr[1] != NULL, arr[2] != NULL,
                arr[3] != NULL);
    if (!arr[1] || !arr[3])
        return;
    const char *t = arr[1];
    const char *u = arr[3];
    const char *d = arr[2] ? arr[2] : arr[1];
    for (int i = 0; i < 10 && rb->nr < (size_t)rb->limit; i++) {
        const char *tq = strchr(t, '"');
        if (!tq)
            break;
        const char *tqe = strchr(tq + 1, '"');
        if (!tqe)
            break;
        char title[2048];
        size_t tl = (size_t)(tqe - tq - 1);
        if (tl >= sizeof title)
            tl = sizeof title - 1;
        memcpy(title, tq + 1, tl);
        title[tl] = 0;
        t = tqe + 1;
        const char *uq = strchr(u, '"');
        if (!uq)
            break;
        const char *uqe = strchr(uq + 1, '"');
        if (!uqe)
            break;
        char url[4096];
        size_t ul = (size_t)(uqe - uq - 1);
        if (ul >= sizeof url)
            ul = sizeof url - 1;
        memcpy(url, uq + 1, ul);
        url[ul] = 0;
        u = uqe + 1;
        char snip[2048] = "";
        const char *dq = strchr(d, '"');
        if (dq) {
            const char *dqe = strchr(dq + 1, '"');
            if (dqe && (size_t)(dqe - dq - 1) < sizeof snip) {
                memcpy(snip, dq + 1, (size_t)(dqe - dq - 1));
                snip[dqe - dq - 1] = 0;
                d = dqe + 1;
            }
        }
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "wiki: [%s] [%s]\n", title, url);
        rb_add(rb, title, url, snip);
    }
}

/* ---------------- dispatch ---------------- */

typedef struct {
    const char *name;
    void (*parse)(const char *, size_t, resbuf_t *);
    const char *url_fmt;
    int pace_ms;
} engine_t;

static const engine_t ENGINES[] = {
    {"ddg", ddg_parse, "https://html.duckduckgo.com/html/?q=%s", 800},
    {"mojeek", mojeek_parse, "https://www.mojeek.com/search?q=%s", 400},
    {"marginalia", marginalia_parse,
     "https://search.marginalia.nu/search?query=%s", 700},
    {"bing", bing_parse, "https://www.bing.com/search?q=%s", 900},
    {"wikipedia", wiki_parse,
     "https://en.wikipedia.org/w/api.php?action=opensearch&format=json&limit=10&search=%s",
     300},
    {NULL, NULL, NULL, 0}};

int search_run(net_t *n, const char *q, const char *engines, int limit,
               result_t **out, size_t *n_out, char *err, size_t errsz)
{
    *out = NULL;
    *n_out = 0;
    char *enc = url_encode(q);
    if (!enc)
        return -1;
    resbuf_t all = {0, 0, 0, limit};
    int any = 0;
    for (int e = 0; ENGINES[e].name; e++) {
        const char *want = ENGINES[e].name;
        if (engines && *engines && !strstr(engines, want) &&
            !strstr(engines, "all"))
            continue;
        char url[8192];
        snprintf(url, sizeof url, ENGINES[e].url_fmt, enc);
        if (n->engine_base && *n->engine_base) {
            /* replace the scheme://host with the override base */
            const char *path = strstr(url, "://");
            const char *slash = path ? strchr(path + 3, '/') : NULL;
            if (path && slash) {
                char newurl[8192];
                snprintf(newurl, sizeof newurl, "%s/%s%s", n->engine_base,
                         ENGINES[e].name, slash);
                snprintf(url, sizeof url, "%s", newurl);
            }
        }
        pace_wait(ENGINES[e].name, ENGINES[e].pace_ms);
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "fetch %s -> %s\n", ENGINES[e].name, url);
        resp_t r;
        char eerr[256];
        if (net_fetch(n, url, &r, eerr, sizeof eerr) != 0 || r.status != 200 ||
            !r.body) {
            resp_free(&r);
            continue;
        }
        resbuf_t rb = {0, 0, 0, limit};
        ENGINES[e].parse(r.body, r.blen, &rb);
        if (getenv("EXOCRAWL_DEBUG"))
            fprintf(stderr, "engine %s: status %d, %zu results, url %s\n",
                    ENGINES[e].name, r.status, rb.nr, url);
        resp_free(&r);
        if (rb.nr > 0) {
            any = 1;
            rb_merge(&all, &rb);
        } else {
            rb_free(&rb);
        }
        if (all.nr >= (size_t)limit)
            break;
    }
    free(enc);
    if (!any) {
        snprintf(err, errsz, "no results from any engine");
        return -1;
    }
    *out = all.r;
    *n_out = all.nr;
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
