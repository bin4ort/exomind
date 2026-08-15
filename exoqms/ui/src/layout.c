/* layout.c — simplified layout model.
 *
 * Static flow (boxes stack vertically), explicit positioning
 * (absolute/relative/fixed offsets), and flex-row/flex-column in
 * simplified form. This is a static model of the CSS box model, not a
 * renderer: every approximated dimension is marked with an EST_* bit
 * and geometry findings say "(approx)" when any involved dimension was
 * estimated. When a dimension cannot be determined at all (unknown
 * units, unresolved var(), background images) the box is marked -1 and
 * the geometry checks emit nothing for it (honest: no finding beats a
 * false positive).
 */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { M_BLOCK, M_INLINE, M_FLEXROW };

typedef struct ln { int ok, pct, aut, unk; double px; } ln_t;

static ln_t parse_len(const char *v, double fontsz)
{
    ln_t r = {0};
    const char *p = v;
    double num = 0;
    if (!v || !*v) {
        r.unk = 1;
        return r;
    }
    while (*p && ascii_space((unsigned char)*p))
        p++;
    if (ci_eq(p, "auto")) {
        r.aut = 1;
        return r;
    }
    if (strcmp(p, "thin") == 0) { r.ok = 1; r.px = 1; return r; }
    if (strcmp(p, "medium") == 0) { r.ok = 1; r.px = 3; return r; }
    if (strcmp(p, "thick") == 0) { r.ok = 1; r.px = 5; return r; }
    if (!parse_num(p, &num)) {
        r.unk = 1;
        return r;
    }
    while (ascii_digit((unsigned char)*p) || *p == '.' || *p == '-')
        p++;
    if (*p == 0) {
        r.ok = 1;           /* unitless: treated as px (CSS only for 0) */
        r.px = num;
        return r;
    }
    if (strncmp(p, "px", 2) == 0) {
        r.ok = 1;
        r.px = num;
        return r;
    }
    if (strncmp(p, "pt", 2) == 0) {
        r.ok = 1;
        r.px = num * 1.3333;
        return r;
    }
    if (strncmp(p, "em", 2) == 0) {
        r.ok = 1;
        r.px = num * fontsz;
        return r;
    }
    if (strncmp(p, "rem", 3) == 0) {
        r.ok = 1;
        r.px = num * DEFAULT_FONTSZ;
        return r;
    }
    if (*p == '%') {
        r.pct = 1;
        r.px = num;
        r.ok = 1;
        return r;
    }
    r.unk = 1;
    return r;
}

static double fontsize_kw(const char *v, double fallback)
{
    if (ci_eq(v, "xx-small")) return 9;
    if (ci_eq(v, "x-small")) return 10;
    if (ci_eq(v, "small")) return 13;
    if (ci_eq(v, "medium")) return 16;
    if (ci_eq(v, "large")) return 18;
    if (ci_eq(v, "x-large")) return 24;
    if (ci_eq(v, "xx-large")) return 32;
    if (ci_eq(v, "smaller")) return fallback * 0.83;
    if (ci_eq(v, "larger")) return fallback * 1.2;
    return 0;
}

static int tag_is(const char *tag, const char *name)
{
    return tag && strcmp(tag, name) == 0;
}

static int is_inline_tag(const char *tag)
{
    static const char *const t[] = {
        "a", "span", "label", "em", "strong", "code", "b", "i", "u",
        "small", "mark", "time", "q", "abbr", "sub", "sup", "cite",
        "kbd", "s", "del", "ins", NULL
    };
    int i;
    for (i = 0; t[i]; i++)
        if (tag_is(tag, t[i]))
            return 1;
    return 0;
}

static int is_inline_block_tag(const char *tag)
{
    return tag_is(tag, "button") || tag_is(tag, "input") ||
           tag_is(tag, "select") || tag_is(tag, "textarea") ||
           tag_is(tag, "img") || tag_is(tag, "svg");
}

static int is_hidden_ctx_tag(const char *tag)
{
    return tag_is(tag, "head") || tag_is(tag, "script") ||
           tag_is(tag, "style") || tag_is(tag, "template") ||
           tag_is(tag, "noscript") || tag_is(tag, "title");
}

/* ---------------- style computation ---------------- */

static void parse_radius(const char *v, double *rad, uint8_t *set,
                         uint8_t *pct)
{
    char *copy = xstrdup(v);
    char *slash = strchr(copy, '/');
    char *t;
    int vals[4];
    int pctv[4] = {0, 0, 0, 0};
    int n = 0;
    if (slash)
        *slash = 0;
    t = strtok(copy, " \t\n\r\f");
    while (t && n < 4) {
        ln_t l = parse_len(t, DEFAULT_FONTSZ);
        if (l.unk || l.aut)
            vals[n] = -1;
        else if (l.pct) {
            vals[n] = (int)l.px;
            pctv[n] = 1;
        } else {
            vals[n] = (int)l.px;
        }
        n++;
        t = strtok(NULL, " \t\n\r\f");
    }
    free(copy);
    if (n == 0)
        return;
    if (n == 1) {
        set[0] = set[1] = set[2] = set[3] = 1;
        rad[0] = rad[1] = rad[2] = rad[3] = vals[0];
        pct[0] = pct[1] = pct[2] = pct[3] = (uint8_t)pctv[0];
    } else if (n == 2) {
        set[0] = set[2] = 1; rad[0] = rad[2] = vals[0]; pct[0] = pct[2] = (uint8_t)pctv[0];
        set[1] = set[3] = 1; rad[1] = rad[3] = vals[1]; pct[1] = pct[3] = (uint8_t)pctv[1];
    } else if (n == 3) {
        set[0] = 1; rad[0] = vals[0]; pct[0] = (uint8_t)pctv[0];
        set[1] = set[3] = 1; rad[1] = rad[3] = vals[1]; pct[1] = pct[3] = (uint8_t)pctv[1];
        set[2] = 1; rad[2] = vals[2]; pct[2] = (uint8_t)pctv[2];
    } else {
        int i;
        for (i = 0; i < 4; i++) {
            set[i] = 1;
            rad[i] = vals[i];
            pct[i] = (uint8_t)pctv[i];
        }
    }
}

static void compute_node(layout_t *L, node_t *n, const char *inh_color,
                         double inh_fontsz, int inh_vis, int hidden_ctx,
                         double inh_opacity)
{
    comp_t *c;
    cstyle_t *st;
    int matched = 0;
    int i;
    const char *v;
    const char *inline_style;

    if (!n->tag) {
        /* text node: no own comp */
        size_t i;
        for (i = 0; i < n->nkids; i++)
            compute_node(L, n->kids[i], inh_color, inh_fontsz, inh_vis,
                         hidden_ctx, inh_opacity);
        return;
    }
    n->cidx = (int)L->ncomp;
    L->ncomp++;
    L->st = realloc(L->st, L->ncomp * sizeof *L->st);
    L->comp = realloc(L->comp, L->ncomp * sizeof *L->comp);
    if (!L->st || !L->comp)
        abort();
    memset(&L->st[n->cidx], 0, sizeof *L->st);
    memset(&L->comp[n->cidx], 0, sizeof *L->comp);
    c = &L->comp[n->cidx];
    st = &L->st[n->cidx];

    css_apply(L->css, n, st, &matched);
    inline_style = node_attr(n, "style");
    if (inline_style)
        css_apply_inline(L->css, inline_style, st);
    c->matched_css = matched > 0;

    /* hidden context + visibility + display + opacity */
    if (is_hidden_ctx_tag(n->tag) || hidden_ctx || node_attr(n, "hidden"))
        c->hidden = 1;
    if (st->set[P_DISP] && !st->unres[P_DISP]) {
        v = st->v[P_DISP];
        if (ci_eq(v, "none")) {
            c->disp = DP_NONE;
            c->hidden = 1;
        } else if (ci_eq(v, "inline")) {
            c->disp = DP_INLINE;
        } else if (ci_eq(v, "inline-block")) {
            c->disp = DP_INLINE_BLOCK;
        } else if (ci_eq(v, "flex") || ci_eq(v, "inline-flex")) {
            c->disp = DP_FLEX;
        } else if (ci_eq(v, "grid") || ci_eq(v, "inline-grid")) {
            c->disp = DP_GRID;
        } else if (ci_eq(v, "block")) {
            c->disp = DP_BLOCK;
        } else {
            c->disp = DP_BLOCK;   /* unknown: approximate as block */
        }
    } else if (is_inline_tag(n->tag)) {
        c->disp = DP_INLINE;
    } else if (is_inline_block_tag(n->tag)) {
        c->disp = DP_INLINE_BLOCK;
    } else {
        c->disp = DP_BLOCK;
    }
    if (st->set[P_VIS] && !st->unres[P_VIS]) {
        if (ci_eq(st->v[P_VIS], "hidden") || ci_eq(st->v[P_VIS], "collapse"))
            inh_vis = 1;
        else if (ci_eq(st->v[P_VIS], "visible"))
            inh_vis = 0;
    }
    if (inh_vis)
        c->hidden = 1;
    c->opacity = 1.0;
    if (st->set[P_OPAC] && !st->unres[P_OPAC]) {
        double o = 0;
        if (parse_num(st->v[P_OPAC], &o)) {
            c->opacity = o;
            if (o <= 0)
                c->hidden = 1;
        }
    }
    c->eff_opaque = inh_opacity * c->opacity >= 1.0;

    /* font-size (inherits) */
    c->fontsz = inh_fontsz;
    if (st->set[P_FONTSZ] && !st->unres[P_FONTSZ]) {
        ln_t l = parse_len(st->v[P_FONTSZ], inh_fontsz);
        double kw = fontsize_kw(st->v[P_FONTSZ], inh_fontsz);
        if (kw > 0)
            c->fontsz = kw;
        else if (l.ok && l.pct)
            c->fontsz = inh_fontsz * l.px / 100.0;
        else if (l.ok)
            c->fontsz = l.px;
    }
    /* line-height */
    c->lh = c->fontsz * DEFAULT_LH;
    if (st->set[P_LH] && !st->unres[P_LH]) {
        double num = 0;
        ln_t l = parse_len(st->v[P_LH], c->fontsz);
        if (l.ok && l.pct)
            c->lh = c->fontsz * l.px / 100.0;
        else if (l.ok)
            c->lh = l.px;
        else if (parse_num(st->v[P_LH], &num))   /* unitless number */
            c->lh = c->fontsz * num;
    }

    /* colors */
    if (st->set[P_COLOR] && !st->unres[P_COLOR]) {
        color_t col = color_parse(st->v[P_COLOR]);
        if (col.valid && col.a == 255) {
            c->color = col;
            c->color_set = 1;
            inh_color = st->v[P_COLOR];
        }
    }
    if (st->set[P_BG] && !st->unres[P_BG]) {
        color_t col = color_parse(st->v[P_BG]);
        if (col.valid && col.a > 0) {
            c->bg = col;
            c->bg_set = 1;
            c->bg_var = st->fromvar[P_BG];
        }
    }
    if (st->set[P_BGIMG] && !st->unres[P_BGIMG])
        c->bg_img = 1;

    /* position */
    c->pos = 0;
    if (st->set[P_POS] && !st->unres[P_POS]) {
        if (ci_eq(st->v[P_POS], "relative"))
            c->pos = 1;
        else if (ci_eq(st->v[P_POS], "absolute"))
            c->pos = 2;
        else if (ci_eq(st->v[P_POS], "fixed"))
            c->pos = 3;
    }
    for (i = 0; i < 4; i++) {
        static const int keys[4] = { P_TOP, P_RIGHT, P_BOTTOM, P_LEFT };
        static const int mkeys[4] = { P_MT, P_MR, P_MB, P_ML };
        static const int pkeys[4] = { P_PT, P_PR, P_PB, P_PL };
        ln_t l;
        if (st->set[keys[i]] && !st->unres[keys[i]]) {
            l = parse_len(st->v[keys[i]], c->fontsz);
            if (l.ok && !l.pct) {
                c->off[i] = l.px;
                c->offs[i] = 1;
            }
        }
        if (st->set[mkeys[i]] && !st->unres[mkeys[i]]) {
            l = parse_len(st->v[mkeys[i]], c->fontsz);
            if (l.ok && !l.pct) {
                c->m[i] = l.px;
                c->ms[i] = 1;
            }
        }
        if (st->set[pkeys[i]] && !st->unres[pkeys[i]]) {
            l = parse_len(st->v[pkeys[i]], c->fontsz);
            if (l.ok && !l.pct) {
                c->p[i] = l.px;
                c->ps[i] = 1;
            }
        }
    }
    if (st->set[P_RAD] && !st->unres[P_RAD])
        parse_radius(st->v[P_RAD], c->rad, c->rs, c->radpct);
    /* flex containers default to row direction */
    c->flxrow = c->disp == DP_FLEX;
    if (st->set[P_FLXDIR] && !st->unres[P_FLXDIR])
        c->flxrow = ci_eq(st->v[P_FLXDIR], "row") ||
                    ci_eq(st->v[P_FLXDIR], "row-reverse");

    /* borders */
    if (st->set[P_BTW] && !st->unres[P_BTW]) {
        ln_t l = parse_len(st->v[P_BTW], c->fontsz);
        if (l.ok && !l.pct && l.px >= 1)
            c->bordered = 1;
    }
    if (st->set[P_BRW] && !st->unres[P_BRW]) {
        ln_t l = parse_len(st->v[P_BRW], c->fontsz);
        if (l.ok && !l.pct && l.px >= 1)
            c->bordered = 1;
    }
    if (st->set[P_BBW] && !st->unres[P_BBW]) {
        ln_t l = parse_len(st->v[P_BBW], c->fontsz);
        if (l.ok && !l.pct && l.px >= 1)
            c->bordered = 1;
    }
    if (st->set[P_BLW] && !st->unres[P_BLW]) {
        ln_t l = parse_len(st->v[P_BLW], c->fontsz);
        if (l.ok && !l.pct && l.px >= 1)
            c->bordered = 1;
    }
    if (st->set[P_BW] && !st->unres[P_BW]) {
        ln_t l = parse_len(st->v[P_BW], c->fontsz);
        if (l.ok && !l.pct && l.px >= 1)
            c->bordered = 1;
    }

    /* body UA margin */
    if (tag_is(n->tag, "body") && !c->ms[0] && !c->ms[1] && !c->ms[2] && !c->ms[3]) {
        c->m[0] = c->m[1] = c->m[2] = c->m[3] = 8;
        c->ms[0] = c->ms[1] = c->ms[2] = c->ms[3] = 1;
    }

    {
        size_t i;
        int nc = (int)hidden_ctx || c->hidden;
        double child_op = inh_opacity * c->opacity;
        for (i = 0; i < n->nkids; i++)
            compute_node(L, n->kids[i], inh_color, c->fontsz, inh_vis, nc,
                         child_op);
    }
}

/* ---------------- intrinsic sizing ---------------- */

static double text_width(const char *t, double fontsz)
{
    double w = 0;
    size_t i = 0;
    while (t[i]) {
        size_t j = i;
        uint32_t cp = utf8_next(t, &j);
        if (cp >= 0x80)
            w += 0.8 * fontsz;   /* wide glyph, approx */
        else
            w += 0.5 * fontsz;
        i = j;
    }
    return w;
}

/* ---------------- layout ---------------- */

static double effective_width(layout_t *L, node_t *n, double avail)
{
    comp_t *c = &L->comp[n->cidx];
    cstyle_t *st = &L->st[n->cidx];
    ln_t l;
    double w = -1;
    if (st->set[P_W] && !st->unres[P_W]) {
        l = parse_len(st->v[P_W], c->fontsz);
        if (l.ok && !l.pct && !l.aut)
            w = l.px;
        else if (l.ok && l.pct)
            w = avail * l.px / 100.0;
    } else if (st->set[P_W] && st->unres[P_W]) {
        return -1;
    }
    if (w >= 0) {
        if (st->set[P_MINW] && !st->unres[P_MINW]) {
            l = parse_len(st->v[P_MINW], c->fontsz);
            if (l.ok && !l.pct && w < l.px)
                w = l.px;
        }
        if (st->set[P_MAXW] && !st->unres[P_MAXW]) {
            l = parse_len(st->v[P_MAXW], c->fontsz);
            if (l.ok && !l.pct && w > l.px)
                w = l.px;
        }
    }
    return w;
}

static double intrinsic_width(layout_t *L, node_t *n)
{
    comp_t *c = &L->comp[n->cidx];
    size_t i;
    double w = 0;
    buf_t txt = {0};
    node_own_text(n, &txt);
    if (txt.len)
        w += text_width(txt.p, c->fontsz);
    buf_free(&txt);
    for (i = 0; i < n->nkids; i++) {
        node_t *k = n->kids[i];
        if (k->tag && k->cidx >= 0 && !L->comp[k->cidx].hidden &&
            L->comp[k->cidx].disp == DP_INLINE)
            w += intrinsic_width(L, k);
    }
    return w + 8;   /* approx glyph padding for inline text */
}

static void lay_text(layout_t *L, node_t *n, double cx, double cw,
                     double *lx, double *ly, double *lh)
{
    double fontsz = DEFAULT_FONTSZ;
    double w, h;
    if (n->parent && n->parent->cidx >= 0)
        fontsz = L->comp[n->parent->cidx].fontsz;
    w = text_width(n->text, fontsz);
    h = fontsz * DEFAULT_LH;
    if (*lx > cx && *lx + w > cx + cw) {
        *lx = cx;
        *ly += *lh;
        *lh = 0;
    }
    *lx += w;
    if (h > *lh)
        *lh = h;
}

static void lay_elem(layout_t *L, node_t *n, double cx, double cy, double cw,
                     int mode, double *ynext, double *lx, double *ly,
                     double *lh);

static void lay_abs(layout_t *L, node_t *n)
{
    comp_t *c = &L->comp[n->cidx];
    cstyle_t *st = &L->st[n->cidx];
    const node_t *anc = n->parent;
    double cbx = 0, cby = 0, cbw = L->vw, cbh = L->vh;
    double w, h, x, y;
    unsigned est = 0;
    if (c->pos == 3) {
        cbx = 0;
        cby = 0;
        cbw = L->vw;
        cbh = L->vh;
    } else {
        while (anc) {
            if (anc->tag && anc->cidx >= 0 &&
                L->comp[anc->cidx].pos != 0) {
                comp_t *cb = &L->comp[anc->cidx];
                cbx = cb->x + cb->p[3];
                cby = cb->y + cb->p[0];
                cbw = cb->w - cb->p[3] - cb->p[1];
                cbh = cb->h - cb->p[0] - cb->p[2];
                break;
            }
            anc = anc->parent;
        }
    }
    w = effective_width(L, n, cbw);
    if (w < 0) {
        w = intrinsic_width(L, n);
        est |= EST_W;
    }
    if (st->set[P_H] && !st->unres[P_H]) {
        ln_t l = parse_len(st->v[P_H], c->fontsz);
        if (l.ok && !l.pct)
            h = l.px;
        else
            h = -1;
    } else {
        h = -1;
    }
    if (c->offs[0]) {
        y = cby + c->off[0];
    } else if (c->offs[2]) {
        y = cby + cbh - h - c->off[2];
    } else {
        y = cby;
        est |= EST_Y;
    }
    if (c->offs[3]) {
        x = cbx + c->off[3];
    } else if (c->offs[1]) {
        x = cbx + cbw - w - c->off[1];
    } else {
        x = cbx;
        est |= EST_X;
    }
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->est = est;
    /* lay children */
    {
        double yy = y + c->p[0];
        double l2x = x + c->p[3], l2y = y + c->p[0], l2h = 0;
        size_t i;
        for (i = 0; i < n->nkids; i++) {
            node_t *k = n->kids[i];
            int kmode;
            if (!k->tag) {
                lay_text(L, k, x + c->p[3], w - c->p[3] - c->p[1],
                         &l2x, &l2y, &l2h);
                continue;
            }
            if (k->cidx < 0 || L->comp[k->cidx].hidden)
                continue;
            if (L->comp[k->cidx].pos >= 2) {
                lay_abs(L, k);
                continue;
            }
            if (c->disp == DP_FLEX && c->flxrow)
                kmode = M_FLEXROW;
            else if (L->comp[k->cidx].disp == DP_INLINE ||
                     L->comp[k->cidx].disp == DP_INLINE_BLOCK)
                kmode = M_INLINE;
            else
                kmode = M_BLOCK;
            lay_elem(L, k, x + c->p[3], y + c->p[0], w - c->p[3] - c->p[1],
                     kmode, &yy, &l2x, &l2y, &l2h);
        }
        if (h < 0) {
            double content = yy - (y + c->p[0]);
            if (l2y + l2h - (y + c->p[0]) > content)
                content = l2y + l2h - (y + c->p[0]);
            h = content + c->p[0] + c->p[2];
            c->est |= EST_H;
        }
        c->h = h;
        if (c->offs[2] && c->h >= 0 && !c->offs[0]) {
            y = cby + cbh - c->h - c->off[2];
            c->y = y;
        }
    }
}

static void lay_elem(layout_t *L, node_t *n, double cx, double cy, double cw,
                     int mode, double *ynext, double *lx, double *ly,
                     double *lh)
{
    comp_t *c = &L->comp[n->cidx];
    cstyle_t *st = &L->st[n->cidx];
    double w, h, x, y;
    unsigned est = 0;
    double mt, mr, mb, ml, pt, pr, pb, pl;
    size_t i;
    double yy, l2x, l2y, l2h = 0;
    double inner_w;

    (void)cy;
    mt = c->ms[0] ? c->m[0] : 0;
    mr = c->ms[1] ? c->m[1] : 0;
    mb = c->ms[2] ? c->m[2] : 0;
    ml = c->ms[3] ? c->m[3] : 0;
    pt = c->ps[0] ? c->p[0] : 0;
    pr = c->ps[1] ? c->p[1] : 0;
    pb = c->ps[2] ? c->p[2] : 0;
    pl = c->ps[3] ? c->p[3] : 0;

    /* width */
    w = effective_width(L, n, cw - ml - mr);
    if (w < 0) {
        if (mode == M_BLOCK) {
            w = cw - ml - mr;   /* auto block width fills parent */
            if (w < 0)
                w = 0;
        } else {
            w = intrinsic_width(L, n);
            est |= EST_W;
        }
    }

    /* placement */
    if (mode == M_BLOCK) {
        x = cx + ml;
        y = *ynext + mt;
    } else if (mode == M_FLEXROW) {
        x = *lx + ml;
        y = *ly + mt;
    } else {
        if (*lx > cx && *lx + w > cx + cw && w < cw) {
            *lx = cx;
            *ly += *lh;
            *lh = 0;
        }
        x = *lx + ml;
        y = *ly + mt;
    }

    /* children */
    yy = y + pt;
    l2x = x + pl;
    l2y = y + pt;
    inner_w = w - pl - pr;
    if (inner_w < 0)
        inner_w = 0;
    for (i = 0; i < n->nkids; i++) {
        node_t *k = n->kids[i];
        int kmode;
        if (!k->tag) {
            lay_text(L, k, x + pl, inner_w, &l2x, &l2y, &l2h);
            continue;
        }
        if (k->cidx < 0 || L->comp[k->cidx].hidden)
            continue;
        if (L->comp[k->cidx].pos >= 2) {
            lay_abs(L, k);
            if (L->comp[k->cidx].est)
                est |= EST_H;
            continue;
        }
        if (c->disp == DP_FLEX && c->flxrow)
            kmode = M_FLEXROW;
        else if (L->comp[k->cidx].disp == DP_INLINE ||
                 L->comp[k->cidx].disp == DP_INLINE_BLOCK)
            kmode = M_INLINE;
        else
            kmode = M_BLOCK;
        lay_elem(L, k, x + pl, y + pt, inner_w, kmode, &yy, &l2x, &l2y, &l2h);
    }

    /* height */
    if (st->set[P_H] && !st->unres[P_H]) {
        ln_t l = parse_len(st->v[P_H], c->fontsz);
        if (l.ok && !l.pct) {
            h = l.px;
        } else if (l.ok && l.pct) {
            h = -1;   /* % height vs auto parent: cannot determine */
        } else {
            h = -1;
        }
    } else {
        h = -1;
    }
    if (h < 0) {
        double content = yy - (y + pt);
        if (l2y + l2h - (y + pt) > content)
            content = l2y + l2h - (y + pt);
        h = content + pt + pb;
        est |= EST_H;   /* content-derived height: approximate */
    }
    if (st->set[P_MINH] && !st->unres[P_MINH]) {
        ln_t l = parse_len(st->v[P_MINH], c->fontsz);
        if (l.ok && !l.pct && h < l.px) {
            h = l.px;
            est |= EST_H;
        }
    }
    if (st->set[P_MAXH] && !st->unres[P_MAXH]) {
        ln_t l = parse_len(st->v[P_MAXH], c->fontsz);
        if (l.ok && !l.pct && h > l.px) {
            h = l.px;
            est |= EST_H;
        }
    }

    /* relative offset */
    if (c->pos == 1) {
        if (c->offs[3]) {
            x += c->off[3];
            est |= EST_X;
        } else if (c->offs[1]) {
            x = cx + cw - w - c->off[1];
            est |= EST_X;
        }
        if (c->offs[0]) {
            y += c->off[0];
        } else if (c->offs[2]) {
            y -= c->off[2];   /* bottom offset: shift up (approx) */
            est |= EST_Y;
        }
    }

    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->est = est;

    /* advance cursors */
    if (mode == M_BLOCK) {
        *ynext = y + h + mb;
        *lx = cx + ml;
        *ly = *ynext;
        *lh = 0;
    } else if (mode == M_FLEXROW) {
        *lx = x + w + mr;
        if (*ly + h > *lh)
            *lh = *ly + h;
    } else {
        *lx = x + w + mr;
        if (h + mt + mb > *lh)
            *lh = h + mt + mb;
    }
}

int layout_build(layout_t *L, node_t *root, css_t *css)
{
    size_t i;
    L->root = root;
    L->css = css;
    L->vw = VIEW_W;
    L->vh = VIEW_H;
    compute_node(L, root, "#000000", DEFAULT_FONTSZ, 0, 0, 1.0);
    /* root synthetic node */
    {
        comp_t *rc = &L->comp[0];
        rc->x = 0;
        rc->y = 0;
        rc->w = L->vw;
        rc->h = L->vh;
    }
    /* lay the document tree under the root */
    {
        double ynext = 0, lx = 0, ly = 0, lh = 0;
        for (i = 0; i < root->nkids; i++) {
            node_t *k = root->kids[i];
            if (k->cidx < 0 || L->comp[k->cidx].hidden)
                continue;
            if (k->tag && L->comp[k->cidx].pos >= 2) {
                lay_abs(L, k);
                continue;
            }
            if (k->tag && (L->comp[k->cidx].disp == DP_INLINE ||
                           L->comp[k->cidx].disp == DP_INLINE_BLOCK)) {
                lay_elem(L, k, 0, 0, L->vw, M_INLINE, &ynext, &lx, &ly, &lh);
            } else {
                lay_elem(L, k, 0, 0, L->vw, M_BLOCK, &ynext, &lx, &ly, &lh);
            }
        }
    }
    /* page background: body, then html, else white */
    {
        const node_t *b = NULL, *h = NULL;
        for (i = 0; i < root->nkids; i++) {
            node_t *k = root->kids[i];
            if (k->tag && strcmp(k->tag, "html") == 0)
                h = k;
            if (k->tag && strcmp(k->tag, "body") == 0)
                b = k;
        }
        if (h && !b) {
            for (i = 0; i < h->nkids; i++)
                if (h->kids[i]->tag && strcmp(h->kids[i]->tag, "body") == 0)
                    b = h->kids[i];
        }
        if (b && b->cidx >= 0 && L->comp[b->cidx].bg_set) {
            L->page_bg = L->comp[b->cidx].bg;
            L->page_bg_set = 1;
        } else if (h && h->cidx >= 0 && L->comp[h->cidx].bg_set) {
            L->page_bg = L->comp[h->cidx].bg;
            L->page_bg_set = 1;
        }
        if (!L->page_bg_set) {
            L->page_bg.r = L->page_bg.g = L->page_bg.b = 255;
            L->page_bg.a = 255;
            L->page_bg.valid = 1;
            L->page_bg_set = 1;
        }
    }
    return 0;
}

/* ---------------- effective colors ---------------- */

static color_t eff_bg(layout_t *L, const node_t *n, int *set)
{
    color_t acc;
    const node_t *anc = n;
    acc.valid = 1;
    acc.r = acc.g = acc.b = 0;
    acc.a = 0;
    *set = 1;
    while (anc) {
        if (anc->tag && anc->cidx >= 0) {
            comp_t *c = &L->comp[anc->cidx];
            if (c->bg_img) {
                *set = 0;
                return acc;
            }
            if (c->bg_set) {
                if (acc.a == 0)
                    acc = c->bg;
                else if (acc.a < 255)
                    acc = color_composite(acc, c->bg);
            }
        }
        anc = anc->parent;
    }
    if (acc.a == 0)
        acc = L->page_bg;
    else if (acc.a < 255)
        acc = color_composite(acc, L->page_bg);
    return acc;
}

static color_t eff_color(layout_t *L, const node_t *n)
{
    const node_t *anc = n;
    while (anc) {
        if (anc->tag && anc->cidx >= 0 && L->comp[anc->cidx].color_set)
            return L->comp[anc->cidx].color;
        anc = anc->parent;
    }
    {
        color_t c;
        c.r = c.g = c.b = 0;
        c.a = 255;
        c.valid = 1;
        return c;
    }
}

color_t node_eff_bg(layout_t *L, const node_t *n, int *set)
{
    return eff_bg(L, n, set);
}

color_t node_eff_color(layout_t *L, const node_t *n)
{
    return eff_color(L, n);
}
