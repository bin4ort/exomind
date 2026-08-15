/* checks.c — the seven audit checks:
 *   emoji-icon    emoji in interactive/icon contexts instead of SVG
 *   overlap       intersecting interactive element boxes (siblings, not nested)
 *   misalign      siblings that should share an edge but don't
 *   corner-mismatch  rounded corner meets square neighbor on a shared edge
 *   background    no affordance, bg == page bg, off-palette hardcoded colors
 *   sdk-default   interactive element with zero CSS rules targeting it
 *   contrast      text vs background below WCAG AA 4.5:1 (3:1 large text)
 *
 * Honesty: when geometry or color cannot be determined, the check emits
 * nothing for that element. Estimated geometry is marked "(approx)" in
 * the finding reason.
 */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOL 1.0        /* edge coincidence tolerance, px */
#define MIN_OVERLAP 2.0

finding_t *finding_new(int major, const char *check, const char *sel,
                       const char *reason, int line, const char *file)
{
    finding_t *f = xcalloc(sizeof *f);
    f->major = major;
    f->check = check;
    f->sel = xstrdup(sel);
    f->reason = xstrdup(reason);
    f->line = line;
    f->file = xstrdup(file);
    return f;
}

void findings_free(vec_t *out)
{
    size_t i;
    for (i = 0; i < out->len; i++) {
        finding_t *f = out->it[i];
        free(f->sel);
        free(f->reason);
        free((char *)f->file);
        free(f);
    }
    free(out->it);
    memset(out, 0, sizeof *out);
}

static int tag_is(const node_t *n, const char *name)
{
    return n->tag && strcmp(n->tag, name) == 0;
}

static int name_has_icon(const char *s)
{
    return s && (strstr(s, "icon") || strstr(s, "ico") || strstr(s, "btn"));
}

static int is_interactive(const node_t *n)
{
    static const char *const tags[] = {
        "button", "a", "input", "select", "textarea", "label",
        "summary", "option", NULL
    };
    int i;
    for (i = 0; tags[i]; i++)
        if (tag_is(n, tags[i]))
            return 1;
    return 0;
}

static int is_sdk_tag(const node_t *n)
{
    static const char *const tags[] = {
        "button", "a", "input", "select", "textarea", NULL
    };
    int i;
    for (i = 0; tags[i]; i++)
        if (tag_is(n, tags[i]))
            return 1;
    return 0;
}

static int is_ancestor(const node_t *a, const node_t *b)
{
    const node_t *p = b->parent;
    while (p) {
        if (p == a)
            return 1;
        p = p->parent;
    }
    return 0;
}

static int share_class(const node_t *a, const node_t *b)
{
    const char *ca = node_attr(a, "class");
    char *copy;
    char *t;
    int found = 0;
    if (!ca)
        return 0;
    copy = xstrdup(ca);
    t = strtok(copy, " \t\n\r\f");
    while (t) {
        if (node_has_class(b, t)) {
            found = 1;
            break;
        }
        t = strtok(NULL, " \t\n\r\f");
    }
    free(copy);
    return found;
}

static int emoji_allowed(const uint32_t *allow, size_t nallow, uint32_t cp)
{
    size_t i;
    for (i = 0; i < nallow; i++)
        if (allow[i] == cp)
            return 1;
    return 0;
}

static int text_has_visible(const char *t)
{
    while (*t) {
        if (!ascii_space((unsigned char)*t))
            return 1;
        t++;
    }
    return 0;
}

static double corner_radius(const comp_t *c, int idx)
{
    double r;
    if (!c->rs[idx])
        return 0;
    r = c->rad[idx];
    if (c->radpct[idx] && c->w > 0 && c->h > 0) {
        double m = c->w < c->h ? c->w : c->h;
        r = m * r / 100.0;
    }
    return r;
}

/* ---------------- check 1: emoji-icon ---------------- */

static void emoji_check_element(layout_t *L, node_t *n, const char *file,
                                const uint32_t *allowset, size_t nallow,
                                vec_t *out)
{
    comp_t *c = &L->comp[n->cidx];
    const char *cls = node_attr(n, "class");
    const char *id = node_attr(n, "id");
    int ctx;
    buf_t text = {0};
    size_t i = 0;
    char first_emoji[8] = {0};
    size_t fe_len = 0;
    if (c->hidden)
        return;
    if (tag_is(n, "textarea") || tag_is(n, "input") || tag_is(n, "select"))
        return;
    ctx = is_interactive(n) || name_has_icon(cls) || name_has_icon(id);
    if (!ctx)
        return;
    node_own_text(n, &text);
    if (!text.len) {
        buf_free(&text);
        return;
    }
    while (i < text.len) {
        size_t j = i;
        uint32_t cp = utf8_next(text.p, &j);
        if (utf8_is_emoji(cp) && !emoji_allowed(allowset, nallow, cp)) {
            if (!fe_len)
                fe_len = utf8_write(cp, first_emoji);
            break;
        }
        i = j;
    }
    buf_free(&text);
    if (fe_len) {
        char sel[512];
        char reason[512];
        node_selector(n, sel, sizeof sel);
        snprintf(reason, sizeof reason,
                 "emoji %s in visible UI text where an icon belongs "
                 "(use an SVG <use> or <img> icon instead)", first_emoji);
        vec_push(out, finding_new(1, "emoji-icon", sel, reason, n->line, file));
    }
}

/* ---------------- check 2: overlap ---------------- */

static void check_overlap(layout_t *L, node_t *root, const char *file,
                          vec_t *out)
{
    node_t *iv[8192];
    size_t niv = 0;
    size_t i, j;
    node_t **stack;
    size_t sp = 0;
    node_t *p;
    stack = xcalloc((L->ncomp + 1) * sizeof *stack);
    stack[sp++] = root;
    while (sp > 0) {
        p = stack[--sp];
        if (p->tag && p->cidx >= 0) {
            comp_t *c = &L->comp[p->cidx];
            if (!c->hidden && c->w >= 2 && c->h >= 2 &&
                is_interactive(p) && niv < 8192)
                iv[niv++] = p;
        }
        for (i = 0; i < p->nkids; i++)
            if (sp < L->ncomp + 1)
                stack[sp++] = p->kids[i];
    }
    free(stack);
    for (i = 0; i < niv; i++) {
        for (j = i + 1; j < niv; j++) {
            comp_t *a = &L->comp[iv[i]->cidx];
            comp_t *b = &L->comp[iv[j]->cidx];
            double ix, iy;
            char sel[512];
            char osel[512];
            char reason[600];
            if (is_ancestor(iv[i], iv[j]) || is_ancestor(iv[j], iv[i]))
                continue;
            ix = (a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w) -
                 (a->x > b->x ? a->x : b->x);
            iy = (a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h) -
                 (a->y > b->y ? a->y : b->y);
            if (ix <= MIN_OVERLAP || iy <= MIN_OVERLAP)
                continue;
            node_selector(iv[i], sel, sizeof sel);
            node_selector(iv[j], osel, sizeof osel);
            snprintf(reason, sizeof reason,
                     "overlaps %s (intersection %.0fx%.0fpx%s)",
                     osel, ix, iy,
                     (a->est | b->est) ? ", approx geometry" : "");
            vec_push(out, finding_new(1, "overlap", sel, reason,
                                      iv[i]->line, file));
        }
    }
}

/* ---------------- check 3: misalign ---------------- */

static void check_misalign(layout_t *L, node_t *n, const char *file,
                           vec_t *out)
{
    size_t i, j;
    comp_t *pc = &L->comp[n->cidx];
    int row = pc->disp == DP_FLEX && pc->flxrow;
    if (n->nkids < 2)
        return;
    for (i = 0; i < n->nkids; i++) {
        node_t *a = n->kids[i];
        for (j = i + 1; j < n->nkids; j++) {
            node_t *b = n->kids[j];
            comp_t *ca, *cb;
            char sel[512];
            char reason[512];
            double d;
            if (!a->tag || !b->tag || a->cidx < 0 || b->cidx < 0)
                continue;
            ca = &L->comp[a->cidx];
            cb = &L->comp[b->cidx];
            if (ca->hidden || cb->hidden || ca->w < 2 || cb->w < 2 ||
                ca->h < 2 || cb->h < 2)
                continue;
            if (strcmp(a->tag, b->tag) != 0 || !share_class(a, b))
                continue;
            if (ca->w != cb->w && (ca->w > cb->w + TOL || ca->w < cb->w - TOL))
                continue;
            if (ca->h != cb->h && (ca->h > cb->h + TOL || ca->h < cb->h - TOL))
                continue;
            if (row) {
                d = ca->y > cb->y ? ca->y - cb->y : cb->y - ca->y;
                if (d <= 2)
                    continue;
                {
                    char sa[512];
                    node_selector(a, sa, sizeof sa);
                    node_selector(b, sel, sizeof sel);
                    snprintf(reason, sizeof reason,
                             "row sibling %.180s misaligned vertically (top "
                             "edges differ by %.0fpx%s)",
                             sa, d,
                             (ca->est | cb->est) ? ", approx geometry" : "");
                }
            } else {
                d = ca->x > cb->x ? ca->x - cb->x : cb->x - ca->x;
                if (d <= 2)
                    continue;
                {
                    char sa[512];
                    node_selector(a, sa, sizeof sa);
                    node_selector(b, sel, sizeof sel);
                    snprintf(reason, sizeof reason,
                             "sibling %.180s misaligned horizontally (left "
                             "edges differ by %.0fpx%s)",
                             sa, d,
                             (ca->est | cb->est) ? ", approx geometry" : "");
                }
            }
            vec_push(out, finding_new(0, "misalign", sel, reason,
                                      b->line, file));
        }
    }
}

/* ---------------- check 4: corner-mismatch ---------------- */

static void check_corners(layout_t *L, node_t *n, const char *file,
                          vec_t *out)
{
    size_t i, j;
    for (i = 0; i < n->nkids; i++) {
        node_t *a = n->kids[i];
        for (j = i + 1; j < n->nkids; j++) {
            node_t *b = n->kids[j];
            comp_t *ca, *cb;
            double ra_bl, ra_br, rb_tl, rb_tr;
            double ra_tr, ra_br2, rb_tl2, rb_bl;
            char sel[512];
            char reason[1152];
            double ox;
            if (!a->tag || !b->tag || a->cidx < 0 || b->cidx < 0)
                continue;
            ca = &L->comp[a->cidx];
            cb = &L->comp[b->cidx];
            if (ca->hidden || cb->hidden || ca->w < 2 || ca->h < 2 ||
                cb->w < 2 || cb->h < 2)
                continue;
            /* vertical adjacency: a above b */
            ox = (ca->x + ca->w < cb->x + cb->w ? ca->x + ca->w : cb->x + cb->w) -
                 (ca->x > cb->x ? ca->x : cb->x);
            if (ox >= 4 && (ca->y + ca->h) > cb->y - TOL &&
                (ca->y + ca->h) < cb->y + TOL) {
                char sa[512], sb2[512];
                ra_bl = corner_radius(ca, 2);
                ra_br = corner_radius(ca, 3);
                rb_tl = corner_radius(cb, 0);
                rb_tr = corner_radius(cb, 1);
                if ((ra_bl >= 2 && rb_tl < 2) || (ra_br >= 2 && rb_tr < 2) ||
                    (rb_tl >= 2 && ra_bl < 2) || (rb_tr >= 2 && ra_br < 2)) {
                    node_selector(a, sa, sizeof sa);
                    node_selector(b, sb2, sizeof sb2);
                    node_selector(b, sel, sizeof sel);
                    snprintf(reason, sizeof reason,
                             "shared edge with %.180s: rounded corners "
                             "(%.180s bottom %.0fpx, %.180s top %.0fpx) do "
                             "not connect to a straight line",
                             sa, sa, ra_bl > ra_br ? ra_bl : ra_br,
                             sb2, rb_tl > rb_tr ? rb_tl : rb_tr);
                    vec_push(out, finding_new(0, "corner-mismatch", sel,
                                              reason, b->line, file));
                }
            }
            /* horizontal adjacency: a left of b */
            {
                double oy = (ca->y + ca->h < cb->y + cb->h
                                 ? ca->y + ca->h : cb->y + cb->h) -
                            (ca->y > cb->y ? ca->y : cb->y);
                if (oy >= 4 && (ca->x + ca->w) > cb->x - TOL &&
                    (ca->x + ca->w) < cb->x + TOL) {
                    char sa[512], sb2[512];
                    ra_tr = corner_radius(ca, 1);
                    ra_br2 = corner_radius(ca, 2);
                    rb_tl2 = corner_radius(cb, 0);
                    rb_bl = corner_radius(cb, 3);
                    if ((ra_tr >= 2 && rb_tl2 < 2) ||
                        (ra_br2 >= 2 && rb_bl < 2) ||
                        (rb_tl2 >= 2 && ra_tr < 2) ||
                        (rb_bl >= 2 && ra_br2 < 2)) {
                        node_selector(a, sa, sizeof sa);
                        node_selector(b, sb2, sizeof sb2);
                        node_selector(b, sel, sizeof sel);
                        snprintf(reason, sizeof reason,
                                 "shared edge with %.180s: rounded corner "
                                 "does not connect to a straight line",
                                 sa);
                        vec_push(out, finding_new(0, "corner-mismatch", sel,
                                                  reason, b->line, file));
                    }
                }
            }
        }
    }
}

/* ---------------- check 5: background ---------------- */

static void check_background(layout_t *L, node_t *n, const char *file,
                             const vec_t *palette, int have_palette,
                             vec_t *out)
{
    comp_t *c = &L->comp[n->cidx];
    char sel[512];
    char reason[512];
    char bghex[16];
    char pagehex[16];
    int eff_set;
    color_t eff;
    if (c->hidden || !is_interactive(n))
        return;
    eff = node_eff_bg(L, n, &eff_set);
    if (!eff_set)
        return;
    color_hex(&eff, bghex);
    color_hex(&L->page_bg, pagehex);
    if (!c->bordered) {
        if (!c->bg_set) {
            if (tag_is(n, "a")) {
                color_t col = node_eff_color(L, n);
                char colhex[16];
                color_hex(&col, colhex);
                if (color_eq(&col, &eff)) {
                    node_selector(n, sel, sizeof sel);
                    snprintf(reason, sizeof reason,
                             "link text color %s equals its background %s: "
                             "invisible link, no affordance",
                             colhex, bghex);
                    vec_push(out, finding_new(1, "background", sel, reason,
                                              n->line, file));
                }
            } else if (tag_is(n, "button") || tag_is(n, "input") ||
                       tag_is(n, "select") || tag_is(n, "textarea") ||
                       tag_is(n, "summary")) {
                node_selector(n, sel, sizeof sel);
                snprintf(reason, sizeof reason,
                         "no background (transparent) on page background %s: "
                         "interactive element without affordance", bghex);
                vec_push(out, finding_new(1, "background", sel, reason,
                                          n->line, file));
            }
        } else if (color_eq(&c->bg, &L->page_bg)) {
            if (tag_is(n, "button") || tag_is(n, "input") ||
                tag_is(n, "select") || tag_is(n, "textarea") ||
                tag_is(n, "summary")) {
                node_selector(n, sel, sizeof sel);
                snprintf(reason, sizeof reason,
                         "background %s equals the page background %s: no "
                         "visual affordance", bghex, pagehex);
                vec_push(out, finding_new(1, "background", sel, reason,
                                          n->line, file));
            }
        }
    }
    if (have_palette && c->bg_set && !c->bg_var) {
        size_t i;
        int in = 0;
        if (color_eq(&c->bg, &L->page_bg))
            in = 1;
        for (i = 0; !in && i < palette->len; i++) {
            char hex[16];
            color_t pcol = color_parse(palette->it[i]);
            color_hex(&pcol, hex);
            if (color_eq(&pcol, &c->bg))
                in = 1;
        }
        if (!in) {
            node_selector(n, sel, sizeof sel);
            snprintf(reason, sizeof reason,
                     "background %s is neither in the theme palette nor the "
                     "page background %s", bghex, pagehex);
            vec_push(out, finding_new(1, "background", sel, reason,
                                      n->line, file));
        }
    }
}

/* ---------------- check 6: sdk-default ---------------- */

static void check_sdk(layout_t *L, node_t *n, const char *file, vec_t *out)
{
    comp_t *c = &L->comp[n->cidx];
    char sel[512];
    char reason[512];
    if (c->hidden || !is_sdk_tag(n))
        return;
    if (!c->matched_css) {
        node_selector(n, sel, sizeof sel);
        snprintf(reason, sizeof reason,
                 "no CSS rule targets this <%s> element anywhere in the "
                 "stylesheet: SDK default widget, custom design avoided",
                 n->tag);
        vec_push(out, finding_new(1, "sdk-default", sel, reason,
                                  n->line, file));
    }
}

/* ---------------- check 7: contrast ---------------- */

static void check_contrast(layout_t *L, node_t *n, const char *file,
                           vec_t *out)
{
    comp_t *c = &L->comp[n->cidx];
    buf_t text = {0};
    int eff_set;
    color_t bg, fg;
    double ratio, thresh;
    char sel[512];
    char reason[512];
    char bghex[16], fghex[16];
    if (c->hidden)
        return;
    if (!c->eff_opaque)
        return;
    node_own_text(n, &text);
    if (!text_has_visible(text.p ? text.p : "")) {
        buf_free(&text);
        return;
    }
    buf_free(&text);
    bg = node_eff_bg(L, n, &eff_set);
    if (!eff_set)
        return;
    fg = node_eff_color(L, n);
    ratio = color_ratio(&fg, &bg);
    thresh = c->fontsz >= 24 ? 3.0 : 4.5;
    if (ratio < thresh) {
        node_selector(n, sel, sizeof sel);
        color_hex(&fg, fghex);
        color_hex(&bg, bghex);
        snprintf(reason, sizeof reason,
                 "contrast ratio %.2f:1 (%s text on %s background) below "
                 "%.1f:1 WCAG AA%s",
                 ratio, fghex, bghex, thresh,
                 thresh == 3.0 ? " (large text)" : "");
        vec_push(out, finding_new(1, "contrast", sel, reason, n->line, file));
    }
}

/* ---------------- runner ---------------- */

void checks_run(layout_t *L, const char *file, const char *emoji_allow,
                int no_emoji, vec_t *out)
{
    uint32_t allowset[64];
    size_t nallow = 0;
    vec_t palette = {0};
    int have_palette = 0;
    node_t **stack;
    size_t sp = 0;
    node_t *p;
    size_t i;

    if (emoji_allow) {
        size_t j = 0;
        while (emoji_allow[j] && nallow < 64) {
            size_t k = j;
            uint32_t cp = utf8_next(emoji_allow, &k);
            if (cp != 0xFFFD)
                allowset[nallow++] = cp;
            j = k;
        }
    }
    if (css_has_palette(L->css)) {
        css_palette(L->css, &palette);
        have_palette = 1;
    }
    stack = xcalloc((L->ncomp + 1) * sizeof *stack);
    stack[sp++] = L->root;
    while (sp > 0) {
        p = stack[--sp];
        if (p->tag && p->cidx >= 0) {
            if (!no_emoji)
                emoji_check_element(L, p, file, allowset, nallow, out);
            check_background(L, p, file, &palette, have_palette, out);
            check_sdk(L, p, file, out);
            check_contrast(L, p, file, out);
            if (p->nkids >= 2) {
                check_misalign(L, p, file, out);
                check_corners(L, p, file, out);
            }
        }
        for (i = 0; i < p->nkids; i++)
            if (sp < L->ncomp + 1)
                stack[sp++] = p->kids[i];
    }
    check_overlap(L, L->root, file, out);
    for (i = 0; i < palette.len; i++)
        free(palette.it[i]);
    free(palette.it);
    free(stack);
}
