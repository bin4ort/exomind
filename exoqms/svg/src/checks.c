/* checks.c — the tree rule-set for generated SVG assets.
 *
 * The real-world motivation: an AI-made tree SVG that isn't as simple
 * as a tapered stem and a round crown is almost certainly anything but
 * an actual tree. These checks catch that with geometry, not taste.
 *
 *   stem-taper      trunk width at 10%/90% of trunk height (cross-section
 *                   of the trunk elements): ratio top/bottom in [0.15,0.9]
 *   stem-missing    no elements in the lower (trunk) region at all
 *   crown-roundness crown bbox aspect in [0.75,1.5] AND convexity >= 0.8
 *                   (union area vs monotone-chain hull over sampled crown
 *                   points); a single element filling >= 85% of its own
 *                   bbox is a box-shaped crown (a square has convexity
 *                   1.0, so convexity alone cannot flag it)
 *   proportions     trunk_h/crown_h in [0.15,0.6] AND crown_w/total_h
 *                   in [0.4,1.6] (minor)
 *   symmetry        crown area left/right balance >= 0.6 about the trunk
 *                   axis (minor); skipped when the stem is missing
 *   empty-shape     total painted area < 0.5% of the total bbox (major);
 *                   degenerate input: crown/trunk shape checks are then
 *                   skipped (geometry is meaningless), stem-missing is
 *                   still reported
 *   fragmented      more than 8 disconnected stroke groups (minor)
 *   out-of-bounds   element fully outside the svg viewBox (minor)
 *
 * Region split: crown = elements whose bbox center lies in the top 65%
 * of the total bbox height; trunk = the rest. If the crown bbox reaches
 * below the top 70% of the total height, trunk elements that still sit
 * inside the crown's horizontal span and do not extend below the crown
 * bottom are reclassified as crown (the 65% line cut through the canopy).
 */
#include "svg.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static finding_t *finding_new(int major, const char *check, const char *file,
                              const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    finding_t *f;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    f = xcalloc(sizeof *f);
    f->major = major;
    f->check = check;
    f->reason = xstrdup(buf);
    f->file = xstrdup(file);
    return f;
}

static double clamp01(double v)
{
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

/* horizontal extent of all trunk segments crossing the horizontal line y */
static double trunk_width(const elem_t **tr, size_t ntr, double y)
{
    double mn = 1e300, mx = -1e300;
    int hit = 0;
    size_t i, j;
    for (i = 0; i < ntr; i++) {
        const elem_t *e = tr[i];
        for (j = 0; j < e->nsegs; j++) {
            const pt_t *a = &e->segs[j].a, *b = &e->segs[j].b;
            double t, x;
            if (a->y == b->y)
                continue;
            if ((a->y - y) * (b->y - y) > 0)
                continue;
            t = (y - a->y) / (b->y - a->y);
            x = a->x + (b->x - a->x) * t;
            if (x < mn) mn = x;
            if (x > mx) mx = x;
            hit = 1;
        }
    }
    return hit ? mx - mn : -1;
}

static void union_find_init(int *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = i;
}

static int uf_find(int *p, int x)
{
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static void uf_union(int *p, int a, int b)
{
    a = uf_find(p, a);
    b = uf_find(p, b);
    if (a != b)
        p[a] = b;
}

static const char *elem_desc(const elem_t *e, char *buf, size_t sz)
{
    snprintf(buf, sz, "%s%s%s", e->tag, e->id ? "#" : "", e->id ? e->id : "");
    return buf;
}

static void tree_audit(svgdoc_t *d, const char *file, vec_t *out)
{
    size_t i, j;
    int has = 0;
    double tx0 = 1e300, ty0 = 1e300, tx1 = -1e300, ty1 = -1e300;
    double total_area = 0;
    int *is_crown;
    size_t n_crown = 0, n_trunk = 0;
    elem_t **trunk;
    double cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    double split, tw, th, bbox_area, top70;
    int degenerate;
    char desc[128];

    for (i = 0; i < d->nel; i++) {
        elem_t *e = d->el[i];
        if (!e->has_pts)
            continue;
        has = 1;
        if (e->x0 < tx0) tx0 = e->x0;
        if (e->y0 < ty0) ty0 = e->y0;
        if (e->x1 > tx1) tx1 = e->x1;
        if (e->y1 > ty1) ty1 = e->y1;
        total_area += e->area;
    }
    if (!has) {
        vec_push(out, finding_new(1, "empty-shape", file,
                                  "no geometry: zero elements with sampled points"));
        return;
    }
    tw = tx1 - tx0;
    th = ty1 - ty0;
    bbox_area = tw * th;
    degenerate = bbox_area <= 0 || total_area < 0.005 * bbox_area;

    /* ---------- region split: crown = top 65% of total bbox ---------- */
    split = ty0 + 0.65 * th;
    is_crown = xcalloc((d->nel ? d->nel : 1) * sizeof(int));
    for (i = 0; i < d->nel; i++) {
        elem_t *e = d->el[i];
        if (!e->has_pts)
            continue;
        is_crown[i] = (e->y0 + e->y1) * 0.5 <= split;
    }
    for (i = 0; i < d->nel; i++) {
        elem_t *e = d->el[i];
        if (!e->has_pts)
            continue;
        if (is_crown[i]) {
            if (n_crown == 0) { cx0 = e->x0; cy0 = e->y0; cx1 = e->x1; cy1 = e->y1; }
            else {
                if (e->x0 < cx0) cx0 = e->x0;
                if (e->y0 < cy0) cy0 = e->y0;
                if (e->x1 > cx1) cx1 = e->x1;
                if (e->y1 > cy1) cy1 = e->y1;
            }
            n_crown++;
        } else {
            if (n_trunk == 0) { bx0 = e->x0; by0 = e->y0; bx1 = e->x1; by1 = e->y1; }
            else {
                if (e->x0 < bx0) bx0 = e->x0;
                if (e->y0 < by0) by0 = e->y0;
                if (e->x1 > bx1) bx1 = e->x1;
                if (e->y1 > by1) by1 = e->y1;
            }
            n_trunk++;
        }
    }
    /* adjustment: canopy spanning the split keeps its trunk material only
     * when it extends below the crown bottom or sits outside its span */
    top70 = ty0 + 0.7 * th;
    if (n_crown > 0 && cy1 > top70) {
        for (i = 0; i < d->nel; i++) {
            elem_t *e = d->el[i];
            if (!e->has_pts || is_crown[i])
                continue;
            if (e->y1 <= cy1 + 1e-9 && e->x1 > cx0 && e->x0 < cx1) {
                is_crown[i] = 1;
                n_crown++;
                n_trunk--;
                if (e->x0 < cx0) cx0 = e->x0;
                if (e->y0 < cy0) cy0 = e->y0;
                if (e->x1 > cx1) cx1 = e->x1;
                if (e->y1 > cy1) cy1 = e->y1;
            }
        }
    }

    /* ---------- strange-geometry: empty-shape (major) ---------- */
    if (degenerate)
        vec_push(out, finding_new(1, "empty-shape", file,
                                  "total painted area %.0f px² vs bbox %.0f px² (%.2f%% < 0.5%%): degenerate, no usable shape geometry",
                                  total_area, bbox_area, bbox_area > 0 ? 100.0 * total_area / bbox_area : 0));

    /* ---------- stem ---------- */
    trunk = xmalloc((n_trunk + 1) * sizeof(elem_t *));
    {
        size_t k = 0;
        for (i = 0; i < d->nel; i++)
            if (d->el[i]->has_pts && !is_crown[i])
                trunk[k++] = d->el[i];
    }
    if (n_trunk == 0) {
        vec_push(out, finding_new(1, "stem-missing", file,
                                  "no elements below the crown region (split at 65%% of total height): trunk absent"));
    } else if (!degenerate) {
        double wtop, wbot;
        if (by1 - by0 >= 1.0) {
            wtop = trunk_width((const elem_t **)trunk, n_trunk, by0 + 0.10 * (by1 - by0));
            wbot = trunk_width((const elem_t **)trunk, n_trunk, by0 + 0.90 * (by1 - by0));
            if (wtop >= 0 && wbot > 0) {
                double ratio = wtop / wbot;
                if (ratio > 0.9)
                    vec_push(out, finding_new(1, "stem-taper", file,
                              "trunk is parallel-sided: width ratio top/bottom = %.2f (want ≤ 0.9; widths at 10%%/90%% of trunk height)",
                              ratio));
                else if (ratio < 0.15)
                    vec_push(out, finding_new(1, "stem-taper", file,
                              "trunk is a spike: width ratio top/bottom = %.2f (want ≥ 0.15; widths at 10%%/90%% of trunk height)",
                              ratio));
            }
        }
    }

    /* ---------- crown ---------- */
    if (!degenerate && n_crown > 0) {
        size_t npts = 0;
        double cw = cx1 - cx0, ch = cy1 - cy0;
        double aspect = ch > 0 ? cw / ch : -1;
        double union_area = 0;
        for (i = 0; i < d->nel; i++)
            if (d->el[i]->has_pts && is_crown[i]) {
                union_area += d->el[i]->area;
                npts += d->el[i]->npts;
            }
        if (npts >= 3) {
            pt_t *all = xmalloc(npts * sizeof(pt_t));
            pt_t *hull = xmalloc(npts * 2 * sizeof(pt_t)); /* hull can push up to 2n-1 */
            size_t k = 0;
            double convexity = -1, hull_area = -1;
            int box = 0;
            double fill = 0;
            char why[256] = "";
            for (i = 0; i < d->nel; i++) {
                elem_t *e = d->el[i];
                if (e->has_pts && is_crown[i])
                    for (j = 0; j < e->npts; j++)
                        all[k++] = e->pts[j];
            }
            hull_area = fabs(shoelace_(hull, convex_hull(all, npts, hull)));
            if (hull_area > 0) {
                convexity = union_area / hull_area;
                if (convexity > 1)
                    convexity = 1;
            }
            if (n_crown == 1) {
                elem_t *e = NULL;
                for (i = 0; i < d->nel; i++)
                    if (d->el[i]->has_pts && is_crown[i])
                        e = d->el[i];
                if (e && e->area > 0 && (e->x1 - e->x0) * (e->y1 - e->y0) > 0) {
                    fill = e->area / ((e->x1 - e->x0) * (e->y1 - e->y0));
                    if (fill >= 0.85)
                        box = 1;
                }
            }
            if (aspect >= 0 && (aspect < 0.75 || aspect > 1.5))
                snprintf(why, sizeof why, "crown bbox aspect %.2f out of [0.75,1.5]", aspect);
            if (convexity >= 0 && convexity < 0.8)
                snprintf(why, sizeof why, "crown convexity %.2f below 0.8 (union %.0f px² vs hull %.0f px²)", convexity, union_area, hull_area);
            if (box)
                snprintf(why, sizeof why, "box-shaped crown: single element fills %.0f%% of its own bbox (a square is convex; convexity cannot flag it)", 100 * fill);
            if (why[0])
                vec_push(out, finding_new(1, "crown-roundness", file,
                                          "%s (aspect %.2f, convexity %s, union area %.0f px²)",
                                          why, aspect, convexity >= 0 ? (convexity >= 0.8 ? ">= 0.8" : "< 0.8") : "n/a", union_area));
            free(all);
            free(hull);
        }
        /* ---------- proportions (minor) ---------- */
        if (n_trunk > 0 && ch > 0) {
            double trunk_h = by1 - by0;
            double r1 = trunk_h / ch;
            double r2 = cw / th;
            if (r1 < 0.15 || r1 > 0.6 || r2 < 0.4 || r2 > 1.6)
                vec_push(out, finding_new(0, "proportions", file,
                                          "trunk/crown height ratio %.2f (want 0.15-0.6), crown width/total height %.2f (want 0.4-1.6)",
                                          r1, r2));
        }
        /* ---------- symmetry (minor; skip when stem missing) ---------- */
        if (n_trunk > 0) {
            double axis = (bx0 + bx1) * 0.5;
            double left = 0, right = 0;
            for (i = 0; i < d->nel; i++) {
                elem_t *e = d->el[i];
                double w;
                if (!e->has_pts || !is_crown[i])
                    continue;
                w = e->x1 - e->x0;
                if (w <= 0)
                    continue;
                left += e->area * clamp01((axis - e->x0) / w);
                right += e->area * clamp01((e->x1 - axis) / w);
            }
            if (left + right > 0) {
                double hi = left > right ? left : right;
                double lo = left < right ? left : right;
                double ratio = lo / hi;
                if (ratio < 0.6)
                    vec_push(out, finding_new(0, "symmetry", file,
                                              "crown leans %.0f/%.0f (left/right area about trunk axis x=%.1f, balance ratio %.2f, want ≥ 0.6)",
                                              left, right, axis, ratio));
            }
        }
    }

    /* ---------- strange-geometry: fragmented (minor) ---------- */
    {
        size_t npath = 0, total = 0;
        elem_t **paths = xmalloc((d->nel + 1) * sizeof(elem_t *));
        int *uf;
        size_t comps = 0;
        double tol;
        for (i = 0; i < d->nel; i++) {
            const elem_t *e = d->el[i];
            if (e->has_pts && (ci_eq(e->tag, "path") || ci_eq(e->tag, "polygon") ||
                               ci_eq(e->tag, "polyline") || ci_eq(e->tag, "line"))) {
                paths[npath++] = d->el[i];
                total += e->npts;
            }
        }
        if (npath > 1 && total <= 4000) {
            uf = xmalloc(npath * sizeof(int));
            union_find_init(uf, (int)npath);
            tol = 0.02 * hypot(tw, th);
            for (i = 0; i < npath && tol > 0; i++) {
                for (j = i + 1; j < npath; j++) {
                    size_t a, b;
                    int linked = 0;
                    for (a = 0; a < paths[i]->npts && !linked; a++) {
                        for (b = 0; b < paths[j]->npts; b++) {
                            double dx = paths[i]->pts[a].x - paths[j]->pts[b].x;
                            double dy = paths[i]->pts[a].y - paths[j]->pts[b].y;
                            if (dx * dx + dy * dy <= tol * tol) {
                                uf_union(uf, (int)i, (int)j);
                                linked = 1;
                                break;
                            }
                        }
                    }
                }
            }
            for (i = 0; i < npath; i++)
                if (uf_find(uf, (int)i) == (int)i)
                    comps++;
            if (comps > 8)
                vec_push(out, finding_new(0, "fragmented", file,
                                          "%zu disconnected stroke groups (isolated paths/lines; want ≤ 8)",
                                          comps));
            free(uf);
        }
        free(paths);
    }

    /* ---------- strange-geometry: out-of-bounds (minor) ---------- */
    if (d->has_vb) {
        double vx0 = d->vb[0], vy0 = d->vb[1];
        double vx1 = d->vb[0] + d->vb[2], vy1 = d->vb[1] + d->vb[3];
        for (i = 0; i < d->nel; i++) {
            elem_t *e = d->el[i];
            if (!e->has_pts)
                continue;
            if (e->x1 < vx0 || e->x0 > vx1 || e->y1 < vy0 || e->y0 > vy1)
                vec_push(out, finding_new(0, "out-of-bounds", file,
                                          "%s bbox [%.0f,%.0f,%.0f,%.0f] lies entirely outside viewBox [%.0f,%.0f,%.0f,%.0f]",
                                          elem_desc(e, desc, sizeof desc), e->x0, e->y0,
                                          e->x1 - e->x0, e->y1 - e->y0, vx0, vy0, vx1, vy1));
        }
    }

    free(is_crown);
    free(trunk);
}

static const char *detect_shape(const svgdoc_t *d, const char *path)
{
    const char *base;
    char buf[256];
    if (d->shape_hint)
        return ci_eq(d->shape_hint, "tree") ? "tree" : NULL;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(buf, sizeof buf, "%s", base);
    lc_ascii(buf);
    return strstr(buf, "tree") ? "tree" : NULL;
}

void audit_run(svgdoc_t *d, const char *path, int shape_mode, audit_result_t *res)
{
    const char *shape;
    size_t i;
    memset(res, 0, sizeof *res);
    shape = shape_mode == SHAPE_TREE ? "tree" : detect_shape(d, path);
    res->shape = xstrdup(shape ? shape : "unknown");
    if (!shape) {
        res->kind = RES_SKIP;
        return;
    }
    res->kind = RES_FIND;
    tree_audit(d, path, &res->findings);
    for (i = 0; i < res->findings.len; i++)
        ((finding_t *)res->findings.it[i])->shape = xstrdup(shape);
}

void audit_result_free(audit_result_t *r)
{
    free(r->findings.it);
    free(r->shape);
    memset(r, 0, sizeof *r);
}

void findings_free(vec_t *out)
{
    size_t i;
    for (i = 0; i < out->len; i++) {
        finding_t *f = out->it[i];
        free(f->reason);
        free(f->file);
        free(f->shape);
        free(f);
    }
    free(out->it);
    memset(out, 0, sizeof *out);
}
