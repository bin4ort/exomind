/* css.c — CSS subset parser + cascade.
 *
 * Selector subset: element, .class, #id, `*`, descendant combinator and
 * `>` child combinator, comma-separated rule lists. Pseudo-classes
 * (:hover, :focus, ...) are stripped from simple selectors; `:root` is
 * treated as `html`. Rules using `[attr]`, `+`, `~` selectors are
 * skipped and counted (honest: unsupported). @media/@import/@keyframes/
 * @font-face/@supports blocks are skipped and counted. Declarations:
 * the property subset in exoqms.h. Custom properties (`--x: v`) are
 * collected and resolved via var(); unresolved vars are marked unknown
 * and never produce findings (honest: cannot determine).
 */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- token helpers ---------------- */

static char *strip_comments(const char *src, size_t len, size_t *outlen)
{
    buf_t b = {0};
    size_t i = 0;
    while (i < len) {
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
                i++;
            i += 2;
        } else {
            buf_append(&b, &src[i], 1);
            i++;
        }
    }
    *outlen = b.len;
    return b.p ? b.p : xstrdup("");
}

static size_t skip_balanced(const char *s, size_t i, size_t len)
{
    int depth = 0;
    while (i < len) {
        if (s[i] == '{')
            depth++;
        else if (s[i] == '}') {
            depth--;
            if (depth <= 0)
                return i + 1;
        }
        i++;
    }
    return len;
}

static int token_has(const char *s, char c)
{
    return strchr(s, c) != NULL;
}

/* ---------------- selectors ---------------- */

static void simple_free(simple_t *s)
{
    size_t i;
    free(s->tag);
    free(s->id);
    for (i = 0; i < s->ncls; i++)
        free(s->cls[i]);
    free(s->cls);
}

static void selector_free(selector_t *s)
{
    size_t i;
    for (i = 0; i < s->nparts; i++)
        simple_free(&s->parts[i]);
    free(s->parts);
}

static int parse_simple(const char *tok, simple_t *out)
{
    const char *p = tok;
    memset(out, 0, sizeof *out);
    if (*p == '#') {
        size_t n = 0;
        p++;
        while (p[n] && p[n] != '.' && p[n] != '#')
            n++;
        if (n == 0)
            return -1;
        out->id = xstrndup(p, n);
        p += n;
    } else if (*p == '.') {
        size_t n = 0;
        p++;
        while (p[n] && p[n] != '.' && p[n] != '#')
            n++;
        if (n == 0)
            return -1;
        out->cls = xcalloc(sizeof *out->cls);
        out->cls[0] = xstrndup(p, n);
        out->ncls = 1;
        p += n;
    } else {
        size_t n = 0;
        while (p[n] && p[n] != '.' && p[n] != '#')
            n++;
        if (n == 0)
            return -1;
        if (n == 1 && *p == '*')
            out->tag = xstrdup("*");
        else
            out->tag = xstrndup(p, n);
        p += n;
    }
    while (*p) {
        if (*p == '.') {
            size_t n = 0;
            const char *q = p + 1;
            while (q[n] && q[n] != '.' && q[n] != '#')
                n++;
            if (n == 0)
                return -1;
            out->cls = realloc(out->cls, (out->ncls + 1) * sizeof *out->cls);
            if (!out->cls)
                abort();
            out->cls[out->ncls++] = xstrndup(q, n);
            p = q + n;
        } else if (*p == '#') {
            size_t n = 0;
            const char *q = p + 1;
            while (q[n] && q[n] != '.' && q[n] != '#')
                n++;
            if (n == 0)
                return -1;
            free(out->id);
            out->id = xstrndup(q, n);
            p = q + n;
        } else {
            return -1;
        }
    }
    return 0;
}

/* parse one selector like "div.card > a.link:hover"; 0 on success,
 * -1 on unsupported syntax ([, +, ~). Pseudo-classes are stripped. */
static int parse_selector(const char *s, selector_t *out)
{
    char *copy = xstrdup(s);
    char *tok;
    int pending_child = 0;
    size_t i;
    int spec = 0;
    memset(out, 0, sizeof *out);
    if (token_has(copy, '[') || token_has(copy, '+') || token_has(copy, '~')) {
        free(copy);
        return -1;
    }
    if (ci_eq(copy, ":root")) {
        free(copy);
        copy = xstrdup("html");
    }
    tok = strtok(copy, " \t\n\r\f");
    while (tok) {
        char *colon = strchr(tok, ':');
        char *t;
        if (colon)
            *colon = 0;
        t = tok;
        while (*t && ascii_space((unsigned char)*t))
            t++;
        if (*t == '>') {
            pending_child = 1;
            t++;
            while (*t && ascii_space((unsigned char)*t))
                t++;
            if (!*t) {
                tok = strtok(NULL, " \t\n\r\f");
                continue;
            }
        }
        {
            char *endgt = strchr(t, '>');
            if (endgt)
                *endgt = 0;
            if (*t) {
                out->parts = realloc(out->parts,
                                     (out->nparts + 1) * sizeof *out->parts);
                if (!out->parts)
                    abort();
                if (parse_simple(t, &out->parts[out->nparts]) != 0) {
                    simple_free(&out->parts[out->nparts]);
                    out->parts = realloc(out->parts,
                                         out->nparts * sizeof *out->parts);
                    free(copy);
                    return -1;
                }
                out->parts[out->nparts].child = pending_child;
                pending_child = 0;
                out->nparts++;
            }
            if (endgt)
                pending_child = 1;
        }
        tok = strtok(NULL, " \t\n\r\f");
    }
    free(copy);
    if (out->nparts == 0) {
        free(out->parts);
        out->parts = NULL;
        return -1;
    }
    for (i = 0; i < out->nparts; i++) {
        simple_t *sp = &out->parts[i];
        if (sp->id)
            spec += 100;
        spec += (int)sp->ncls * 10;
        if (sp->tag && strcmp(sp->tag, "*") != 0)
            spec += 1;
    }
    out->spec = spec;
    return 0;
}

static int simple_match(const simple_t *s, const node_t *n)
{
    size_t i;
    const char *id;
    if (!n->tag || !n->tag[0])
        return 0;
    if (s->tag && strcmp(s->tag, "*") != 0 && strcmp(s->tag, n->tag) != 0)
        return 0;
    if (s->id) {
        id = node_attr(n, "id");
        if (!id || strcmp(id, s->id) != 0)
            return 0;
    }
    for (i = 0; i < s->ncls; i++)
        if (!node_has_class(n, s->cls[i]))
            return 0;
    return 1;
}

static int selector_match(const selector_t *sel, const node_t *n)
{
    size_t i;
    const node_t *cur = n;
    if (sel->nparts == 0)
        return 0;
    if (!simple_match(&sel->parts[sel->nparts - 1], cur))
        return 0;
    for (i = sel->nparts - 1; i-- > 0;) {
        const node_t *par = cur->parent;
        if (sel->parts[i + 1].child) {
            if (!par || !simple_match(&sel->parts[i], par))
                return 0;
            cur = par;
        } else {
            while (par && !simple_match(&sel->parts[i], par))
                par = par->parent;
            if (!par)
                return 0;
            cur = par;
        }
    }
    return 1;
}

/* ---------------- declarations ---------------- */

static void decl_free(decl_t *d)
{
    free(d->prop);
    free(d->value);
}

/* split declarations on top-level ';' (paren/quote aware) and store into r
 * (or into the var table when the prop is a custom property). */
static void parse_declarations(const char *s, size_t len, rule_t *r, css_t *c)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        size_t depth = 0;
        char q = 0;
        char *prop;
        char *value;
        while (j < len && s[j] != ':' && s[j] != ';' && s[j] != '}')
            j++;
        if (j >= len || s[j] != ':') {
            while (i < len && s[i] != ';')
                i++;
            i++;
            continue;
        }
        prop = xstrndup(s + i, j - i);
        str_trim(prop);
        lc_ascii(prop);
        i = j + 1;
        j = i;
        while (j < len) {
            char c = s[j];
            if (q) {
                if (c == q && s[j - 1] != '\\')
                    q = 0;
            } else if (c == '"' || c == '\'') {
                q = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                if (depth > 0)
                    depth--;
            } else if ((c == ';' || c == '}') && depth == 0) {
                break;
            }
            j++;
        }
        value = xstrndup(s + i, j - i);
        str_trim(value);
        {
            char *bang = strstr(value, "!important");
            if (bang) {
                while (bang > value && ascii_space((unsigned char)bang[-1]))
                    bang--;
                *bang = 0;
                str_trim(value);
            }
        }
        if (prop[0] == '-' && prop[1] == '-') {
            int found = 0;
            size_t k;
            for (k = 0; k < c->nvars; k++) {
                if (strcmp(c->vars[k].name, prop) == 0) {
                    free(c->vars[k].value);
                    c->vars[k].value = xstrdup(value);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                c->vars = realloc(c->vars, (c->nvars + 1) * sizeof *c->vars);
                if (!c->vars)
                    abort();
                c->vars[c->nvars].name = xstrdup(prop);
                c->vars[c->nvars].value = xstrdup(value);
                c->nvars++;
            }
        } else if (prop[0]) {
            r->decls = realloc(r->decls, (r->ndecls + 1) * sizeof *r->decls);
            if (!r->decls)
                abort();
            r->decls[r->ndecls].prop = prop;
            r->decls[r->ndecls].value = value;
            r->ndecls++;
        } else {
            free(prop);
            free(value);
        }
        i = j;
        if (i < len && (s[i] == ';' || s[i] == '}'))
            i++;
        if (j < len && s[j] == '}')
            break;
    }
}

/* ---------------- entry point ---------------- */

css_t css_parse(const char *src, size_t len)
{
    css_t c = {0};
    size_t n = 0;
    char *s = strip_comments(src, len, &n);
    size_t i = 0;
    size_t ruleorder = 0;

    while (i < n) {
        size_t j;
        while (i < n && (ascii_space((unsigned char)s[i]) || s[i] == ';'))
            i++;
        if (i >= n)
            break;
        if (s[i] == '}') {
            i++;
            continue;
        }
        if (s[i] == '@') {
            j = i;
            while (j < n && s[j] != '{' && s[j] != ';')
                j++;
            if (j < n && s[j] == '{')
                i = skip_balanced(s, j, n);
            else
                i = j < n ? j + 1 : n;
            c.skipped_at++;
            continue;
        }
        j = i;
        while (j < n && s[j] != '{')
            j++;
        if (j >= n)
            break;
        {
            char *seltext = xstrndup(s + i, j - i);
            rule_t *r = NULL;
            char *p;
            int unsupported = 0;
            int have_sel = 0;
            str_trim(seltext);
            if (seltext[0]) {
                c.rules = realloc(c.rules, (c.nrules + 1) * sizeof *c.rules);
                if (!c.rules)
                    abort();
                memset(&c.rules[c.nrules], 0, sizeof *c.rules);
                r = &c.rules[c.nrules];
                r->order = ruleorder++;
            }
            p = seltext;
            while (p) {
                char *comma = strchr(p, ',');
                char *one;
                selector_t sel;
                if (comma)
                    *comma = 0;
                one = p;
                str_trim(one);
                if (one[0]) {
                    if (!token_has(one, '[') && !token_has(one, '+') &&
                        !token_has(one, '~')) {
                        if (parse_selector(one, &sel) == 0) {
                            r->sels = realloc(r->sels,
                                              (r->nsels + 1) * sizeof *r->sels);
                            if (!r->sels)
                                abort();
                            r->sels[r->nsels++] = sel;
                            have_sel = 1;
                        } else {
                            unsupported = 1;
                        }
                    } else {
                        unsupported = 1;
                    }
                }
                p = comma ? comma + 1 : NULL;
            }
            {
                size_t dstart = j + 1;
                size_t dend = dstart;
                int depth = 0;
                char q = 0;
                while (dend < n) {
                    char c = s[dend];
                    if (q) {
                        if (c == q && s[dend - 1] != '\\')
                            q = 0;
                    } else if (c == '"' || c == '\'') {
                        q = c;
                    } else if (c == '(') {
                        depth++;
                    } else if (c == ')') {
                        if (depth > 0)
                            depth--;
                    } else if (c == '}' && depth == 0) {
                        break;
                    }
                    dend++;
                }
                if (r) {
                    if (unsupported || !have_sel) {
                        size_t k;
                        for (k = 0; k < r->nsels; k++)
                            selector_free(&r->sels[k]);
                        free(r->sels);
                        r->sels = NULL;
                        r->nsels = 0;
                        c.skipped_rules++;
                    }
                    parse_declarations(s + dstart, dend - dstart, r, &c);
                    if (r->nsels == 0) {
                        size_t k;
                        for (k = 0; k < r->ndecls; k++)
                            decl_free(&r->decls[k]);
                        free(r->decls);
                        r->decls = NULL;
                        r->ndecls = 0;
                        ruleorder--;
                    } else {
                        c.nrules++;
                    }
                }
                i = dend < n ? dend + 1 : n;
            }
            free(seltext);
        }
    }
    free(s);
    return c;
}

void css_free(css_t *c)
{
    size_t i, k;
    for (i = 0; i < c->nrules; i++) {
        for (k = 0; k < c->rules[i].nsels; k++)
            selector_free(&c->rules[i].sels[k]);
        free(c->rules[i].sels);
        for (k = 0; k < c->rules[i].ndecls; k++)
            decl_free(&c->rules[i].decls[k]);
        free(c->rules[i].decls);
    }
    free(c->rules);
    for (i = 0; i < c->nvars; i++) {
        free(c->vars[i].name);
        free(c->vars[i].value);
    }
    free(c->vars);
    memset(c, 0, sizeof *c);
}

static const char *var_lookup(css_t *c, const char *name)
{
    size_t i;
    for (i = 0; i < c->nvars; i++)
        if (strcmp(c->vars[i].name, name) == 0)
            return c->vars[i].value;
    return NULL;
}

const char *css_var_resolve(css_t *c, const char *name, char *out, size_t outsz)
{
    const char *v = var_lookup(c, name);
    if (v) {
        snprintf(out, outsz, "%s", v);
        return out;
    }
    return NULL;
}

/* resolve var() references; returns 1 if some var was missing (the caller
 * must treat the result as unknown and emit no finding for it). */
static int resolve_vars(css_t *c, const char *value, buf_t *out, int depth)
{
    size_t i = 0;
    int unresolved = 0;
    if (depth > 8) {
        buf_puts(out, value);
        return 1;
    }
    while (value[i]) {
        if (value[i] == 'v' && strncmp(value + i, "var(", 4) == 0) {
            size_t j = i + 4;
            size_t k = j;
            while (value[k] && value[k] != ')')
                k++;
            if (value[k] == ')') {
                char *name = xstrndup(value + j, k - j);
                char *fallback = strchr(name, ',');
                buf_t tb = {0};
                if (fallback)
                    *fallback = 0;
                str_trim(name);
                {
                    const char *rv = var_lookup(c, name);
                    if (rv) {
                        buf_puts(&tb, rv);
                    } else if (fallback) {
                        buf_puts(&tb, fallback + 1);
                        str_trim(tb.p);
                    } else {
                        unresolved = 1;
                        buf_puts(&tb, "");
                    }
                    if (tb.len)
                        unresolved |= resolve_vars(c, tb.p, out, depth + 1);
                }
                buf_free(&tb);
                free(name);
                i = k + 1;
                continue;
            }
        }
        buf_append(out, &value[i], 1);
        i++;
    }
    return unresolved;
}

void css_palette(css_t *c, vec_t *out)
{
    size_t i;
    for (i = 0; i < c->nvars; i++) {
        if (strstr(c->vars[i].name, "color")) {
            buf_t b = {0};
            int un = resolve_vars(c, c->vars[i].value, &b, 0);
            str_trim(b.p);
            if (!un && b.p && b.p[0]) {
                color_t col = color_parse(b.p);
                if (col.valid) {
                    char hex[16];
                    color_hex(&col, hex);
                    vec_push(out, xstrdup(hex));
                }
            }
            buf_free(&b);
        }
    }
}

int css_has_palette(css_t *c)
{
    size_t i;
    for (i = 0; i < c->nvars; i++)
        if (strstr(c->vars[i].name, "color"))
            return 1;
    return 0;
}

/* ---------------- property keys and cascade ---------------- */

static int prop_key(const char *p)
{
    static const struct { const char *n; int k; } tab[] = {
        { "color", P_COLOR },
        { "background-color", P_BG },
        { "background-image", P_BGIMG },
        { "border-radius", P_RAD },
        { "width", P_W }, { "height", P_H },
        { "min-width", P_MINW }, { "max-width", P_MAXW },
        { "min-height", P_MINH }, { "max-height", P_MAXH },
        { "margin-top", P_MT }, { "margin-right", P_MR },
        { "margin-bottom", P_MB }, { "margin-left", P_ML },
        { "padding-top", P_PT }, { "padding-right", P_PR },
        { "padding-bottom", P_PB }, { "padding-left", P_PL },
        { "position", P_POS }, { "top", P_TOP }, { "right", P_RIGHT },
        { "bottom", P_BOTTOM }, { "left", P_LEFT },
        { "display", P_DISP }, { "float", P_FLOAT }, { "box-sizing", P_BOX },
        { "font-size", P_FONTSZ }, { "line-height", P_LH },
        { "opacity", P_OPAC }, { "visibility", P_VIS },
        { "flex-direction", P_FLXDIR }, { "justify-content", P_JUSTIFY },
        { "align-items", P_ALIGNI }, { "align-self", P_ALIGNS },
        { "flex-wrap", P_FLXWRAP }, { "flex-basis", P_FLXBASIS },
        { "flex-grow", P_FLXGROW }, { "flex-shrink", P_FLXSHRINK },
        { "text-align", P_TEXTALIGN },
        { "border-width", P_BW }, { "border-color", P_BWC },
        { "border-top-width", P_BTW }, { "border-right-width", P_BRW },
        { "border-bottom-width", P_BBW }, { "border-left-width", P_BLW },
        { "border-top-color", P_BTC }, { "border-right-color", P_BRC },
        { "border-bottom-color", P_BBC }, { "border-left-color", P_BLC },
    };
    size_t i;
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(p, tab[i].n) == 0)
            return tab[i].k;
    return -1;
}

typedef struct best {
    int spec;
    size_t order;
    const char *value;      /* pointer into css-owned memory or own */
    char *own;              /* owned copy if any */
    int has;
    int fromvar;
} best_t;

static void best_put(best_t *b, int spec, size_t order, const char *value,
                     int fromvar)
{
    if (!b->has || spec > b->spec || (spec == b->spec && order >= b->order)) {
        free(b->own);
        b->own = NULL;
        b->spec = spec;
        b->order = order;
        b->value = value;
        b->fromvar = fromvar;
        b->has = 1;
    }
}

static void best_put_own(best_t *b, int spec, size_t order, const char *value,
                         int fromvar)
{
    if (!b->has || spec > b->spec || (spec == b->spec && order >= b->order)) {
        free(b->own);
        b->own = xstrdup(value);
        b->spec = spec;
        b->order = order;
        b->value = b->own;
        b->fromvar = fromvar;
        b->has = 1;
    }
}

/* split a value into up to 4 whitespace words; returns word count.
 * caller frees words[0..min(4,nw)-1]. */
static int split_words(const char *v, char *words[4])
{
    char *copy = xstrdup(v);
    char *t;
    int n = 0;
    t = strtok(copy, " \t\n\r\f");
    while (t) {
        if (n < 4)
            words[n] = xstrdup(t);
        n++;
        t = strtok(NULL, " \t\n\r\f");
    }
    free(copy);
    return n;
}

static int is_border_style(const char *t)
{
    return strcmp(t, "solid") == 0 || strcmp(t, "dashed") == 0 ||
           strcmp(t, "dotted") == 0 || strcmp(t, "none") == 0 ||
           strcmp(t, "double") == 0 || strcmp(t, "groove") == 0 ||
           strcmp(t, "ridge") == 0 || strcmp(t, "inset") == 0 ||
           strcmp(t, "outset") == 0 || strcmp(t, "hidden") == 0;
}

static int is_fontsize_kw(const char *t)
{
    return strcmp(t, "small") == 0 || strcmp(t, "medium") == 0 ||
           strcmp(t, "large") == 0 || strcmp(t, "x-small") == 0 ||
           strcmp(t, "x-large") == 0 || strcmp(t, "xx-small") == 0 ||
           strcmp(t, "xx-large") == 0 || strcmp(t, "smaller") == 0 ||
           strcmp(t, "larger") == 0;
}

static double fontsize_kw_px(const char *t)
{
    if (ci_eq(t, "xx-small")) return 9;
    if (ci_eq(t, "x-small")) return 10;
    if (ci_eq(t, "small")) return 13;
    if (ci_eq(t, "large")) return 18;
    if (ci_eq(t, "x-large")) return 24;
    if (ci_eq(t, "xx-large")) return 32;
    return 16;
}

/* resolve one declaration into the best table (cascade step). */
static void resolve_decl(css_t *c, const char *prop, const char *value,
                         int spec, size_t order, best_t *best)
{
    int i;
    (void)c;
    if (strcmp(prop, "background") == 0) {
        char *words[4];
        int nw;
        const char *col = NULL;
        int fromvar = 0;
        int img = 0;
        nw = split_words(value, words);
        for (i = 0; i < nw && i < 4; i++) {
            char *t = words[i];
            if (strncmp(t, "url(", 4) == 0 || strstr(t, "gradient") ||
                strncmp(t, "image-set", 9) == 0 || strcmp(t, "none") == 0) {
                img = 1;
            } else if (strstr(t, "var(")) {
                col = t;
                fromvar = 1;
            } else if (color_parse(t).valid) {
                col = t;
            }
        }
        if (col) {
            best_put_own(&best[P_BG], spec, order, col, fromvar);
        }
        if (img) {
            best_put(&best[P_BGIMG], spec, order, "image", 0);
        }
        for (i = 0; i < nw && i < 4; i++)
            free(words[i]);
        return;
    }
    if (strcmp(prop, "border") == 0 || strcmp(prop, "border-top") == 0 ||
        strcmp(prop, "border-right") == 0 || strcmp(prop, "border-bottom") == 0 ||
        strcmp(prop, "border-left") == 0) {
        int wkey, ckey;
        char *words[4];
        int nw;
        int wi;
        const char *wval = NULL, *cval = NULL;
        if (strcmp(prop, "border") == 0) { wkey = P_BW; ckey = P_BWC; }
        else if (strcmp(prop, "border-top") == 0) { wkey = P_BTW; ckey = P_BTC; }
        else if (strcmp(prop, "border-right") == 0) { wkey = P_BRW; ckey = P_BRC; }
        else if (strcmp(prop, "border-bottom") == 0) { wkey = P_BBW; ckey = P_BBC; }
        else { wkey = P_BLW; ckey = P_BLC; }
        nw = split_words(value, words);
        for (wi = 0; wi < nw && wi < 4; wi++) {
            char *t = words[wi];
            if (is_border_style(t))
                ;
            else if (strcmp(t, "thin") == 0 || strcmp(t, "medium") == 0 ||
                     strcmp(t, "thick") == 0 || ascii_digit((unsigned char)t[0]) ||
                     t[0] == '.') {
                wval = t;
            } else if (strstr(t, "var(")) {
                /* resolve now to classify: a var may be a color or a width */
                buf_t rb = {0};
                int un = resolve_vars(c, t, &rb, 0);
                if (!un) {
                    str_trim(rb.p);
                    if (rb.p && color_parse(rb.p).valid)
                        cval = t;
                    else
                        wval = t;
                }
                buf_free(&rb);
            } else if (color_parse(t).valid) {
                cval = t;
            }
        }
        if (wval)
            best_put_own(&best[wkey], spec, order, wval,
                         strstr(wval, "var(") != NULL);
        if (cval)
            best_put_own(&best[ckey], spec, order, cval,
                         strstr(cval, "var(") != NULL);
        for (i = 0; i < nw && i < 4; i++)
            free(words[i]);
        return;
    }
    if (strcmp(prop, "margin") == 0 || strcmp(prop, "padding") == 0) {
        static const int map[4] = { P_MT, P_MR, P_MB, P_ML };
        static const int pmap[4] = { P_PT, P_PR, P_PB, P_PL };
        const int *kmap = strcmp(prop, "margin") == 0 ? map : pmap;
        char *words[4];
        int nw = split_words(value, words);
        if (nw == 1) {
            for (i = 0; i < 4; i++)
                best_put_own(&best[kmap[i]], spec, order, words[0], 0);
        } else if (nw == 2) {
            for (i = 0; i < 2; i++) {
                best_put_own(&best[kmap[i]], spec, order, words[0], 0);
                best_put_own(&best[kmap[i + 2]], spec, order, words[1], 0);
            }
        } else if (nw == 3) {
            best_put_own(&best[kmap[0]], spec, order, words[0], 0);
            best_put_own(&best[kmap[1]], spec, order, words[1], 0);
            best_put_own(&best[kmap[2]], spec, order, words[2], 0);
            best_put_own(&best[kmap[3]], spec, order, words[1], 0);
        } else if (nw == 4) {
            for (i = 0; i < 4; i++)
                best_put_own(&best[kmap[i]], spec, order, words[i], 0);
        }
        for (i = 0; i < nw && i < 4; i++)
            free(words[i]);
        return;
    }
    if (strcmp(prop, "font") == 0) {
        char *words[8];
        int nw = 0;
        char *copy = xstrdup(value);
        char *t;
        const char *sz = NULL, *lh = NULL;
        t = strtok(copy, " \t\n\r\f");
        while (t && nw < 8) {
            words[nw++] = xstrdup(t);
            t = strtok(NULL, " \t\n\r\f");
        }
        free(copy);
        for (i = 0; i < nw; i++) {
            char *slash = strchr(words[i], '/');
            if (ascii_digit((unsigned char)words[i][0]) || words[i][0] == '.' ||
                is_fontsize_kw(words[i])) {
                if (slash) {
                    *slash = 0;
                    sz = words[i];
                    lh = slash + 1;
                } else if (!sz) {
                    sz = words[i];
                }
            }
        }
        if (sz) {
            buf_t b = {0};
            if (is_fontsize_kw(sz)) {
                buf_printf(&b, "%.0fpx", fontsize_kw_px(sz));
            } else {
                buf_puts(&b, sz);
            }
            best_put_own(&best[P_FONTSZ], spec, order, b.p,
                         strstr(sz, "var(") != NULL);
            buf_free(&b);
        }
        if (lh)
            best_put_own(&best[P_LH], spec, order, lh, 0);
        for (i = 0; i < nw; i++)
            free(words[i]);
        return;
    }
    if (strcmp(prop, "flex") == 0) {
        char *words[4];
        int nw = split_words(value, words);
        if (nw == 1) {
            best_put_own(&best[P_FLXGROW], spec, order, words[0], 0);
        } else if (nw >= 2) {
            best_put_own(&best[P_FLXGROW], spec, order, words[0], 0);
            best_put_own(&best[P_FLXSHRINK], spec, order, words[1], 0);
            if (nw >= 3)
                best_put_own(&best[P_FLXBASIS], spec, order, words[2], 0);
        }
        for (i = 0; i < nw && i < 4; i++)
            free(words[i]);
        return;
    }
    {
        int key = prop_key(prop);
        if (key >= 0) {
            best_put(&best[key], spec, order, value,
                     strstr(value, "var(") != NULL);
        }
    }
}

static void best_to_style(css_t *c, best_t *best, cstyle_t *st)
{
    size_t d;
    for (d = 0; d < P_NPROP; d++) {
        if (best[d].has) {
            buf_t out = {0};
            int un = resolve_vars(c, best[d].value, &out, 0);
            free(st->v[d]);
            st->v[d] = out.p ? out.p : xstrdup("");
            st->set[d] = 1;
            st->unres[d] = (uint8_t)un;
            st->fromvar[d] = (uint8_t)best[d].fromvar;
        }
        free(best[d].own);
    }
}

void css_apply(css_t *c, const node_t *n, cstyle_t *st, int *matched)
{
    best_t best[P_NPROP];
    size_t r, d;
    memset(best, 0, sizeof best);
    for (r = 0; r < c->nrules; r++) {
        size_t s;
        int spec = 0;
        int m = 0;
        for (s = 0; s < c->rules[r].nsels; s++) {
            if (selector_match(&c->rules[r].sels[s], n)) {
                m = 1;
                if (c->rules[r].sels[s].spec > spec)
                    spec = c->rules[r].sels[s].spec;
            }
        }
        if (!m)
            continue;
        if (matched)
            (*matched)++;
        for (d = 0; d < c->rules[r].ndecls; d++)
            resolve_decl(c, c->rules[r].decls[d].prop,
                         c->rules[r].decls[d].value, spec,
                         c->rules[r].order, best);
    }
    best_to_style(c, best, st);
}

void css_apply_inline(css_t *c, const char *style, cstyle_t *st)
{
    rule_t tmp;
    best_t best[P_NPROP];
    size_t d;
    memset(&tmp, 0, sizeof tmp);
    memset(best, 0, sizeof best);
    parse_declarations(style, strlen(style), &tmp, c);
    for (d = 0; d < tmp.ndecls; d++)
        resolve_decl(c, tmp.decls[d].prop, tmp.decls[d].value, 100000, 0, best);
    for (d = 0; d < tmp.ndecls; d++)
        decl_free(&tmp.decls[d]);
    free(tmp.decls);
    best_to_style(c, best, st);
}
