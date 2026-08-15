/* geom.c — small 2D geometry helpers: shoelace area, monotone chain
 * convex hull (Andrew). All exact doubles, no dependencies.
 */
#include "svg.h"

#include <stdlib.h>
#include <string.h>

double shoelace_(const pt_t *pts, size_t n)
{
    double s = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        const pt_t *a = &pts[i];
        const pt_t *b = &pts[(i + 1) % n];
        s += a->x * b->y - b->x * a->y;
    }
    return s * 0.5;
}

static int cmp_pt(const void *pa, const void *pb)
{
    const pt_t *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    return 0;
}

/* cross product of (b-a) x (c-b); > 0 = left turn */
static double cross(const pt_t *a, const pt_t *b, const pt_t *c)
{
    return (b->x - a->x) * (c->y - b->y) - (b->y - a->y) * (c->x - b->x);
}

/* monotone chain (Andrew); out must hold n points; returns hull size */
int convex_hull(const pt_t *pts, size_t n, pt_t *out)
{
    pt_t *p;
    size_t i, k = 0, lower;
    long long j;
    if (n <= 2) {
        for (i = 0; i < n; i++)
            out[i] = pts[i];
        return (int)n;
    }
    p = xmalloc(n * sizeof(pt_t));
    memcpy(p, pts, n * sizeof(pt_t));
    qsort(p, n, sizeof(pt_t), cmp_pt);
    /* lower hull */
    for (i = 0; i < n; i++) {
        while (k >= 2 && cross(&out[k - 2], &out[k - 1], &p[i]) <= 0)
            k--;
        out[k++] = p[i];
    }
    /* upper hull */
    lower = k + 1;
    for (j = (long long)n - 2; j >= 0; j--) {
        while (k >= lower && cross(&out[k - 2], &out[k - 1], &p[j]) <= 0)
            k--;
        out[k++] = p[j];
    }
    k--; /* duplicate of the first point */
    free(p);
    return (int)k;
}
