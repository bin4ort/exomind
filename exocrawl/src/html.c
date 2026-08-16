/* exocrawl: HTML -> token-efficient plain text extraction.
 *
 * Boilerplate (nav, footer, ads, cookie banners, modals, share/social
 * widgets, comments) is dropped by tag and by class/id heuristics.
 * Reading content is emitted in document order: headings become `# `,
 * list items `- `, pre/code stay verbatim. Links and images are
 * collected as tab-separated lines ("anchor<TAB>url" / "alt<TAB>src").
 */
#include "exocrawl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

static const char *SKIP_TAGS[] = {
    "script", "style", "noscript", "template", "svg", "head", "iframe",
    "form", "nav", "footer", "aside", "select", "option", "button",
    "canvas", "video", "audio", "dialog", NULL};

static const char *BLOCK_TAGS[] = {
    "p", "div", "section", "article", "main", "h1", "h2", "h3", "h4",
    "h5", "h6", "li", "pre", "blockquote", "br", "tr", "table", "ul",
    "ol", "header", "hr", "figure", NULL};

static const char *HEADING_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6", NULL};
static const char *INLINE_TAGS[] = {
    "b", "i", "em", "strong", "span", "a", "u", "s", "del", "ins", "small",
    "sub", "sup", "mark", "code", "abbr", "cite", "q", "label", "button",
    "font", "kbd", "samp", "var", NULL
};

static const char *BOILER_ATTR[] = {
    "nav", "menu", "sidebar", "footer", "header", "ads", "advert", "sponsor",
    "cookie", "banner", "popup", "modal", "overlay", "comment", "share",
    "social", "related", "recommend", "subscribe", "newsletter", "promo",
    "breadcrumb", "pagination", "toolbar", "widget", NULL};

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static int ci_eq(const char *a, size_t n, const char *b)
{
    size_t bn = strlen(b);
    if (n != bn)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int in_list(const char *tag, size_t n, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (ci_eq(tag, n, list[i]))
            return 1;
    return 0;
}

static int attr_has_boiler(const char *attr)
{
    if (!attr)
        return 0;
    for (int i = 0; BOILER_ATTR[i]; i++) {
        if (strstr(attr, BOILER_ATTR[i]))
            return 1;
    }
    return 0;
}

/* parse an attribute value (quoted or unquoted) from cursor */

/* collect every attribute of the tag in one pass */
typedef struct {
    char class[512];
    char id[512];
    char href[2048];
    char src[2048];
    char alt[512];
} attrs_t;

static void tag_attrs(const char *p, const char *end, attrs_t *a)
{
    memset(a, 0, sizeof *a);
    const char *q = p + 1;
    if (*q == '/')
        q++;
    while (q < end && (isalnum((unsigned char)*q) || *q == '-'))
        q++;
    while (q < end && *q != '>') {
        const char *ns = q;
        while (q < end && *q != '=' && *q != ' ' && *q != '>')
            q++;
        size_t nn = (size_t)(q - ns);
        if (q < end && *q == '=') {
            q++;
            char quote = 0;
            if (*q == '"' || *q == '\'') {
                quote = *q;
                q++;
            }
            const char *val = q;
            while (q < end && (quote ? *q != quote : (*q != ' ' && *q != '>')))
                q++;
            size_t vn = (size_t)(q - val);
            if (quote && q < end)
                q++;
            char *dst = NULL;
            size_t cap = 0;
            if (ci_eq(ns, nn, "class")) {
                dst = a->class;
                cap = sizeof a->class;
            } else if (ci_eq(ns, nn, "id")) {
                dst = a->id;
                cap = sizeof a->id;
            } else if (ci_eq(ns, nn, "href")) {
                dst = a->href;
                cap = sizeof a->href;
            } else if (ci_eq(ns, nn, "src")) {
                dst = a->src;
                cap = sizeof a->src;
            } else if (ci_eq(ns, nn, "alt")) {
                dst = a->alt;
                cap = sizeof a->alt;
            }
            if (dst) {
                size_t take = vn < cap - 1 ? vn : cap - 1;
                memcpy(dst, val, take);
                dst[take] = 0;
            }
        } else {
            while (q < end && *q != ' ' && *q != '>')
                q++;
        }
        while (q < end && isspace((unsigned char)*q))
            q++;
    }
}

/* resolve a relative URL against base (crude but effective) */
static void resolve_url(const char *base, const char *href, char *out,
                        size_t cap)
{
    if (!href || !href[0]) {
        out[0] = 0;
        return;
    }
    if (strstr(href, "://")) {
        snprintf(out, cap, "%s", href);
        return;
    }
    if (href[0] == '/') {
        const char *p = strstr(base, "://");
        if (!p) {
            snprintf(out, cap, "%s", href);
            return;
        }
        const char *hostend = strchr(p + 3, '/');
        size_t n = hostend ? (size_t)(hostend - base) : strlen(base);
        if (n >= cap)
            n = cap - 1;
        memcpy(out, base, n);
        out[n] = 0;
        snprintf(out + n, cap - n, "%s", href);
        return;
    }
    /* relative: strip base's last path segment */
    const char *q = strchr(base, '?');
    size_t bn = q ? (size_t)(q - base) : strlen(base);
    const char *slash = NULL;
    for (size_t i = 0; i < bn; i++)
        if (base[i] == '/')
            slash = base + i;
    if (!slash) {
        snprintf(out, cap, "%s/%s", base, href);
        return;
    }
    size_t n = (size_t)(slash - base) + 1;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
    snprintf(out + n, cap - n, "%s", href);
}

/* token-efficient text sink with line-level dedupe */
typedef struct {
    buf_t b;
    size_t max;
    char last_line[512];
    size_t nlines;
    int in_pre;
    int pending_heading;
    int pending_li;
} sink_t;

static void sink_flush(sink_t *s)
{
    /* collapse repeated blank lines */
    size_t len = s->b.len;
    while (len > 0 && (s->b.p[len - 1] == '\n' || s->b.p[len - 1] == ' ' ||
                       s->b.p[len - 1] == '\t'))
        len--;
    if (s->b.len != len)
        s->b.len = len;
    if (s->b.p)
        s->b.p[s->b.len] = 0;
}

static void sink_text(sink_t *s, const char *t, size_t n)
{
    if (s->b.len >= s->max)
        return;
    for (size_t i = 0; i < n && s->b.len < s->max; i++) {
        char c = t[i];
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        s->b.p[s->b.len++] = c;
    }
}

static void sink_newline(sink_t *s)
{
    if (s->b.len && s->b.p[s->b.len - 1] != '\n' && s->b.len < s->max)
        s->b.p[s->b.len++] = '\n';
    if (s->b.p)
        s->b.p[s->b.len] = 0;
}



/* the extractor: recursive descent over tags */
typedef struct {
    sink_t sink;
    page_t *page;
    const char *base;
    int depth;
    int in_skip;   /* inside a skipped subtree */
    int skip_depth;
    char cur_link[4096];  /* href of an open <a>, "" when none */
    buf_t cur_link_text;
} ctx_t;

static void walk(ctx_t *c, const char *p, const char *end);

/* ensure a space between words split by an inline tag (<b>bold</b>and)
 * unless the sink is at a line boundary or in preformatted text */
static void inline_separate(ctx_t *c)
{
    if (c->sink.in_pre)
        return;
    if (c->sink.b.len &&
        c->sink.b.p[c->sink.b.len - 1] != ' ' &&
        c->sink.b.p[c->sink.b.len - 1] != '\n' &&
        c->sink.b.len < c->sink.max) {
        c->sink.b.p[c->sink.b.len++] = ' ';
        if (c->sink.b.p)
            c->sink.b.p[c->sink.b.len] = 0;
    }
}

static void walk_text(ctx_t *c, const char *t, size_t n)
{
    if (c->in_skip)
        return;
    char dec[1024];
    size_t dn = html_entity_decode(t, n, dec, sizeof dec);
    if (c->cur_link[0] && c->cur_link_text.len + dn + 1 < 4096) {
        if (c->cur_link_text.len > 0) {
            buf_putc(&c->cur_link_text, ' ');
        } else if (c->sink.b.len &&
                   c->sink.b.p[c->sink.b.len - 1] != ' ' &&
                   c->sink.b.len < c->sink.max) {
            /* the text run was trimmed: restore the word separator */
            c->sink.b.p[c->sink.b.len++] = ' ';
            if (c->sink.b.p)
                c->sink.b.p[c->sink.b.len] = 0;
        }
        buf_puts(&c->cur_link_text, dec);
    }
    /* link text is reading text too: emit to the sink as well */
    if (c->sink.in_pre)
        sink_text(&c->sink, dec, dn);
    else
        sink_text(&c->sink, dec, dn);
}

/* walk_tag updates parser state only; the walk() loop owns traversal. */
static void walk_tag(ctx_t *c, const char *p, const char *end)
{
    const char *q = p + 1;
    int closing = 0;
    if (*q == '/') {
        closing = 1;
        q++;
    }
    const char *name = q;
    while (q < end && (isalnum((unsigned char)*q) || *q == '-'))
        q++;
    size_t nlen = (size_t)(q - name);
    if (nlen == 0)
        return;
    if (nlen >= 16)
        nlen = 16;
    char tag[32];
    memcpy(tag, name, nlen);
    tag[nlen] = 0;

    if (closing) {
        if (in_list(tag, nlen, INLINE_TAGS))
            inline_separate(c);
        if (c->in_skip && c->skip_depth > 0)
            c->skip_depth--;
        if (c->skip_depth == 0 && c->in_skip)
            c->in_skip = 0;
        if (ci_eq(tag, nlen, "pre") || ci_eq(tag, nlen, "code"))
            c->sink.in_pre = 0;
        if (ci_eq(tag, nlen, "a") && c->cur_link[0]) {
            char line[8192];
            snprintf(line, sizeof line, "%s\t%s",
                     c->cur_link_text.p ? c->cur_link_text.p : "",
                     c->cur_link);
            if (c->page->nlinks < 200 && c->page->links)
                c->page->links[c->page->nlinks++] = strdup(line);
            c->cur_link[0] = 0;
            c->cur_link_text.len = 0;
        }
        return;
    }

    attrs_t a;
    tag_attrs(p, end, &a);

    int skip_tag = in_list(tag, nlen, SKIP_TAGS);
    int skip_attr = !skip_tag &&
                    (attr_has_boiler(a.class) || attr_has_boiler(a.id));
    if (skip_tag || skip_attr) {
        if (!c->in_skip)
            c->skip_depth = 0;
        c->in_skip = 1;
        c->skip_depth++;
        return;
    }

    /* links and images */
    if (ci_eq(tag, nlen, "a")) {
        if (a.href[0] && a.href[0] != '#' && !strstr(a.href, "javascript:") &&
            !strstr(a.href, "mailto:")) {
            char res[4096];
            resolve_url(c->base, a.href, res, sizeof res);
            snprintf(c->cur_link, sizeof c->cur_link, "%s", res);
            if (!c->cur_link_text.p)
                buf_init(&c->cur_link_text, 512);
            c->cur_link_text.len = 0;
        }
    }
    if (ci_eq(tag, nlen, "img") && a.src[0]) {
        char res[4096];
        resolve_url(c->base, a.src, res, sizeof res);
        char line[4608];
        snprintf(line, sizeof line, "%s\t%s", a.alt, res);
        if (c->page->nimages < 100 && c->page->images)
            c->page->images[c->page->nimages++] = strdup(line);
    }

    if (ci_eq(tag, nlen, "title")) {
        /* capture title text within this element */
        const char *t = q;
        while (t < end && *t != '>')
            t++;
        if (t < end)
            t++;
        const char *tend = t;
        while (tend + 7 < end && strncasecmp(tend, "</title", 7) != 0)
            tend++;
        size_t tl = (size_t)(tend - t);
        char dec[1024];
        html_entity_decode(t, tl, dec, sizeof dec);
        free(c->page->title);
        c->page->title = strdup(dec);
        return;
    }

    if (ci_eq(tag, nlen, "pre") || ci_eq(tag, nlen, "code")) {
        c->sink.in_pre = 1;
    }
    if (in_list(tag, nlen, INLINE_TAGS))
        inline_separate(c);
    if (in_list(tag, nlen, HEADING_TAGS)) {
        c->sink.pending_heading = (int)tag[1] - '0';
    } else if (ci_eq(tag, nlen, "li")) {
        c->sink.pending_li = 1;
    } else if (in_list(tag, nlen, BLOCK_TAGS) && !c->sink.in_pre) {
        sink_newline(&c->sink);
    }
}

static void walk(ctx_t *c, const char *p, const char *end)
{
    while (p < end && c->sink.b.len < c->sink.max) {
        if (*p == '<') {
            /* comment or declaration: skip */
            if (p + 3 < end && strncmp(p, "<!--", 4) == 0) {
                const char *e = strstr(p + 4, "-->");
                if (!e)
                    break;
                p = e + 3;
                continue;
            }
            if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
                const char *e = strchr(p, '>');
                if (!e)
                    break;
                p = e + 1;
                continue;
            }
            walk_tag(c, p, end);
            /* find this tag's end to continue after it */
            const char *e = strchr(p, '>');
            if (!e)
                break;
            p = e + 1;
            continue;
        }
        /* text run until next '<' */
        const char *t = p;
        while (p < end && *p != '<')
            p++;
        size_t n = (size_t)(p - t);
        if (c->in_skip)
            continue;
        /* flush pending heading/list-item prefixes */
        if (c->sink.pending_heading) {
            int lvl = c->sink.pending_heading;
            c->sink.pending_heading = 0;
            size_t s0 = 0;
            while (s0 < n && isspace((unsigned char)t[s0]))
                s0++;
            size_t e0 = n;
            while (e0 > s0 && isspace((unsigned char)t[e0 - 1]))
                e0--;
            if (e0 > s0) {
                sink_newline(&c->sink);
                for (int i = 0; i < lvl && c->sink.b.len + 2 < c->sink.max; i++)
                    buf_putc(&c->sink.b, '#');
                buf_putc(&c->sink.b, ' ');
                walk_text(c, t + s0, e0 - s0);
                sink_newline(&c->sink);
                p = t + e0;
                continue;
            }
        }
        if (c->sink.pending_li) {
            c->sink.pending_li = 0;
            size_t s0 = 0;
            while (s0 < n && isspace((unsigned char)t[s0]))
                s0++;
            size_t e0 = n;
            while (e0 > s0 && isspace((unsigned char)t[e0 - 1]))
                e0--;
            if (e0 > s0) {
                sink_newline(&c->sink);
                if (c->sink.b.len + 3 < c->sink.max) {
                    c->sink.b.p[c->sink.b.len++] = '-';
                    c->sink.b.p[c->sink.b.len++] = ' ';
                }
                walk_text(c, t + s0, e0 - s0);
                sink_newline(&c->sink);
                p = t + e0;
                continue;
            }
        }
        size_t s = 0;
        while (s < n && isspace((unsigned char)t[s]))
            s++;
        size_t e = n;
        while (e > s && isspace((unsigned char)t[e - 1]))
            e--;
        if (e > s)
            walk_text(c, t + s, e - s);
    }
}

size_t page_extract(const char *html, size_t hlen, const char *base,
                    page_t *p, size_t max)
{
    memset(p, 0, sizeof *p);
    p->links = calloc(200, sizeof(char *));
    p->images = calloc(100, sizeof(char *));
    if (!p->links || !p->images)
        return 0;

    buf_t b;
    buf_init(&b, max ? max : 8192);
    ctx_t c;
    memset(&c, 0, sizeof c);
    c.page = p;
    c.base = base ? base : "";
    c.sink.max = max ? max : 8192;
    c.sink.b = b;
    c.sink.b.p = malloc(c.sink.max + 1);
    if (!c.sink.b.p)
        return 0;
    c.sink.b.cap = c.sink.max + 1;
    if (!c.cur_link_text.p)
        buf_init(&c.cur_link_text, 512);
    walk(&c, html, html + hlen);
    sink_flush(&c.sink);
    buf_free(&c.cur_link_text);
    p->text = c.sink.b.p;
    return c.sink.b.len;
}

void page_free(page_t *p)
{
    free(p->title);
    free(p->text);
    for (size_t i = 0; i < p->nlinks; i++)
        free(p->links[i]);
    free(p->links);
    for (size_t i = 0; i < p->nimages; i++)
        free(p->images[i]);
    free(p->images);
    memset(p, 0, sizeof *p);
}
