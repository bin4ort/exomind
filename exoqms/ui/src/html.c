/* html.c — HTML subset parser.
 *
 * Supported: tags, attributes (class/id/style/src/href/...), text nodes,
 * nested structure, self-closing tags (`/>` and void elements), comments,
 * doctype, entity decoding of the common named + numeric entities.
 * Limitations (honest): no HTML5 spec error recovery, no implied end-tags
 * beyond auto-closing on parent close, `<p>`-style optional end tags are
 * NOT implied, unclosed tags stay open until the document ends, NUL bytes
 * are skipped. Good enough for the audit subset: finding real defect
 * patterns, not validating markup.
 */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr", NULL
};

static int is_void_tag(const char *tag)
{
    int i;
    for (i = 0; void_tags[i]; i++)
        if (strcmp(tag, void_tags[i]) == 0)
            return 1;
    return 0;
}

static int tagchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

static void decode_entity(const char *s, size_t *i, size_t len, buf_t *out)
{
    size_t j = *i;
    size_t k;
    static const struct { const char *name; const char *val; } named[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" },
        { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
    };
    if (s[j] != '&')
        return;
    k = j + 1;
    if (k < len && s[k] == '#') {
        long cp = 0;
        k++;
        if (k < len && (s[k] == 'x' || s[k] == 'X')) { k++; }
        while (k < len) {
            int h = hexval((unsigned char)s[k]);
            if (h < 0)
                break;
            cp = cp * 16 + h;
            k++;
        }
        if (k < len && s[k] == ';') {
            char u[4];
            size_t n = utf8_write((uint32_t)cp, u);
            buf_append(out, u, n);
            *i = k + 1;
            return;
        }
        *i = j + 1;
        buf_append(out, "&", 1);
        return;
    }
    {
        size_t m = 0;
        while (k + m < len && m < 8 && s[k + m] != ';')
            m++;
        if (k + m < len && s[k + m] == ';') {
            char *name = xstrndup(s + k, m);
            size_t x;
            for (x = 0; x < sizeof named / sizeof named[0]; x++) {
                if (strcmp(name, named[x].name) == 0) {
                    buf_puts(out, named[x].val);
                    *i = k + m + 1;
                    free(name);
                    return;
                }
            }
            free(name);
        }
    }
    *i = j + 1;
    buf_append(out, "&", 1);
}

static int read_tagname(const char *s, size_t len, size_t *i, char *out, size_t outsz)
{
    size_t j = *i;
    size_t n = 0;
    while (j < len && n + 1 < outsz && tagchar((unsigned char)s[j])) {
        out[n++] = (char)(s[j] >= 'A' && s[j] <= 'Z' ? s[j] + 32 : s[j]);
        j++;
    }
    if (n == 0)
        return 0;
    out[n] = 0;
    *i = j;
    return 1;
}

static void skip_ws(const char *s, size_t len, size_t *i, int *line)
{
    while (*i < len) {
        char c = s[*i];
        if (c == '\n')
            (*line)++;
        if (!ascii_space((unsigned char)c))
            break;
        (*i)++;
    }
}

static char *read_attr_value(const char *s, size_t len, size_t *i, int *line)
{
    buf_t b = {0};
    size_t j = *i;
    char q = 0;
    if (j < len && (s[j] == '"' || s[j] == '\'')) {
        q = s[j];
        j++;
    }
    while (j < len) {
        char c = s[j];
        if (c == '\n')
            (*line)++;
        if (q) {
            if (c == q) {
                j++;
                break;
            }
        } else {
            if (ascii_space((unsigned char)c) || c == '>')
                break;
        }
        if (c == '&') {
            decode_entity(s, &j, len, &b);
            continue;
        }
        buf_append(&b, &c, 1);
        j++;
    }
    *i = j;
    return b.p ? b.p : xstrdup("");
}

static void add_attr(node_t *n, const char *name, const char *value)
{
    size_t i;
    for (i = 0; i < n->nattrs; i++) {
        if (strcmp(n->attrs[i].name, name) == 0) {
            free(n->attrs[i].value);
            n->attrs[i].value = xstrdup(value);
            return;
        }
    }
    n->attrs = realloc(n->attrs, (n->nattrs + 1) * sizeof *n->attrs);
    if (!n->attrs)
        abort();
    n->attrs[n->nattrs].name = xstrdup(name);
    n->attrs[n->nattrs].value = xstrdup(value);
    n->nattrs++;
}

static void add_kid(node_t *parent, node_t *kid)
{
    parent->kids = realloc(parent->kids, (parent->nkids + 1) * sizeof *parent->kids);
    if (!parent->kids)
        abort();
    parent->kids[parent->nkids++] = kid;
    kid->parent = parent;
}

static node_t *new_node(const char *tag, int line)
{
    node_t *n = xcalloc(sizeof *n);
    n->cidx = -1;
    n->line = line;
    if (tag)
        n->tag = xstrdup(tag);
    return n;
}

static void add_text_node(node_t *parent, const char *s, size_t len, int line)
{
    if (len == 0)
        return;
    if (parent->nkids > 0 && parent->kids[parent->nkids - 1]->text) {
        node_t *last = parent->kids[parent->nkids - 1];
        buf_t b = {0};
        buf_append(&b, last->text, strlen(last->text));
        buf_append(&b, s, len);
        free(last->text);
        last->text = b.p;
        return;
    }
    {
        node_t *t = new_node(NULL, line);
        t->text = xstrndup(s, len);
        add_kid(parent, t);
    }
}

node_t *html_parse(const char *src, size_t len, int *errline)
{
    node_t *root = new_node("", 1);
    node_t *stack[1024];
    size_t sp = 0;
    size_t i = 0;
    int line = 1;

    stack[sp++] = root;
    while (i < len) {
        char c = src[i];
        if (c == '\0') {
            i++;
            continue;
        }
        if (c != '<') {
            size_t j = i;
            size_t tstart = i;
            int tline = line;
            while (j < len && src[j] != '<') {
                if (src[j] == '\0') {
                    j++;
                    continue;
                }
                if (src[j] == '\n')
                    line++;
                j++;
            }
            if (j > tstart)
                add_text_node(stack[sp - 1], src + tstart, j - tstart, tline);
            i = j;
            continue;
        }
        if (i + 3 < len && strncmp(src + i, "<!--", 4) == 0) {
            size_t j = i + 4;
            while (j + 2 < len && strncmp(src + j, "-->", 3) != 0) {
                if (src[j] == '\n')
                    line++;
                j++;
            }
            i = j + 3;
            if (i > len)
                i = len;
            continue;
        }
        if (i + 1 < len && src[i + 1] == '!') {
            size_t j = i + 2;
            while (j < len && src[j] != '>') {
                if (src[j] == '\n')
                    line++;
                j++;
            }
            i = j < len ? j + 1 : len;
            continue;
        }
        if (i + 1 < len && src[i + 1] == '?') {
            size_t j = i + 2;
            while (j < len && src[j] != '>') {
                if (src[j] == '\n')
                    line++;
                j++;
            }
            i = j < len ? j + 1 : len;
            continue;
        }
        if (i + 1 < len && src[i + 1] == '/') {
            char tname[128];
            size_t j = i + 2;
            if (!read_tagname(src, len, &j, tname, sizeof tname)) {
                i = j + 1;
                continue;
            }
            while (j < len && src[j] != '>') {
                if (src[j] == '\n')
                    line++;
                j++;
            }
            i = j < len ? j + 1 : len;
            if (sp > 1) {
                size_t k = sp - 1;
                while (k > 0 && strcmp(stack[k]->tag ? stack[k]->tag : "", tname) != 0)
                    k--;
                if (k > 0)
                    sp = k;   /* pop the matched element (and unclosed above it) */
            }
            continue;
        }
        /* open tag */
        {
            char tname[128];
            size_t j = i + 1;
            int selfclose = 0;
            int tline = line;
            node_t *n;
            if (!read_tagname(src, len, &j, tname, sizeof tname)) {
                i++;
                continue;
            }
            n = new_node(tname, tline);
            for (;;) {
                char aname[256];
                size_t k = 0;
                skip_ws(src, len, &j, &line);
                if (j >= len)
                    break;
                if (src[j] == '>') {
                    j++;
                    break;
                }
                if (src[j] == '/' && j + 1 < len && src[j + 1] == '>') {
                    selfclose = 1;
                    j += 2;
                    break;
                }
                while (j < len && k + 1 < sizeof aname &&
                       !ascii_space((unsigned char)src[j]) && src[j] != '=' &&
                       src[j] != '>' && src[j] != '/') {
                    aname[k++] = src[j];
                    j++;
                }
                if (k == 0)
                    break;
                aname[k] = 0;
                skip_ws(src, len, &j, &line);
                if (j < len && src[j] == '=') {
                    char *val;
                    j++;
                    skip_ws(src, len, &j, &line);
                    val = read_attr_value(src, len, &j, &line);
                    add_attr(n, aname, val);
                    free(val);
                } else {
                    add_attr(n, aname, "");
                }
            }
            add_kid(stack[sp - 1], n);
            if (!selfclose && !is_void_tag(tname) && sp < 1024)
                stack[sp++] = n;
            i = j;
            continue;
        }
    }
    if (errline)
        *errline = line;
    return root;
}

void html_free(node_t *n)
{
    size_t i;
    if (!n)
        return;
    for (i = 0; i < n->nattrs; i++) {
        free(n->attrs[i].name);
        free(n->attrs[i].value);
    }
    free(n->attrs);
    for (i = 0; i < n->nkids; i++)
        html_free(n->kids[i]);
    free(n->kids);
    free(n->tag);
    free(n->text);
    free(n);
}

const char *node_attr(const node_t *n, const char *name)
{
    size_t i;
    for (i = 0; i < n->nattrs; i++)
        if (strcmp(n->attrs[i].name, name) == 0)
            return n->attrs[i].value;
    return NULL;
}

int node_has_class(const node_t *n, const char *cls)
{
    const char *v = node_attr(n, "class");
    size_t l = strlen(cls);
    if (!v)
        return 0;
    while (*v) {
        while (*v && ascii_space((unsigned char)*v))
            v++;
        if (strncmp(v, cls, l) == 0 && (v[l] == 0 || ascii_space((unsigned char)v[l])))
            return 1;
        while (*v && !ascii_space((unsigned char)*v))
            v++;
    }
    return 0;
}

void node_selector(const node_t *n, char *out, size_t outsz)
{
    buf_t b = {0};
    const node_t *p = n;
    const node_t *chain[512];
    size_t nc = 0;
    size_t i;
    while (p && nc < 512) {
        chain[nc++] = p;
        p = p->parent;
    }
    for (i = nc; i-- > 0;) {
        const node_t *cur = chain[i];
        const char *id;
        if (!cur->tag || !cur->tag[0])
            continue;
        if (b.len)
            buf_puts(&b, " > ");
        buf_printf(&b, "%s", cur->tag);
        id = node_attr(cur, "id");
        if (id && id[0])
            buf_printf(&b, "#%s", id);
        {
            const char *cls = node_attr(cur, "class");
            if (cls) {
                const char *t = cls;
                while (*t) {
                    size_t start;
                    while (*t && ascii_space((unsigned char)*t))
                        t++;
                    start = (size_t)(t - cls);
                    while (*t && !ascii_space((unsigned char)*t))
                        t++;
                    if (t - cls > (ptrdiff_t)start)
                        buf_printf(&b, ".%.*s", (int)(t - cls - start), cls + start);
                }
            }
        }
    }
    if (b.len == 0)
        buf_puts(&b, "(none)");
    snprintf(out, outsz, "%s", b.p);
    buf_free(&b);
}

int node_own_text(const node_t *n, buf_t *out)
{
    size_t i;
    int any = 0;
    for (i = 0; i < n->nkids; i++) {
        if (n->kids[i]->text) {
            buf_puts(out, n->kids[i]->text);
            any = 1;
        }
    }
    return any;
}
