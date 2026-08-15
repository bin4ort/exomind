/* svgparse.c — the supported SVG subset parser.
 *
 * Elements: svg g path circle ellipse rect line polygon polyline.
 * Attributes: id, d, cx/cy/r, rx/ry, x/y, x1/y1/x2/y2, width/height,
 * points, transform (translate/scale/rotate applied; matrix/skew
 * skipped with a stderr note), viewBox, data-shape.
 * Path commands: M/m L/l H/h V/v C/c S/s Q/q T/t A/a Z/z. Curves are
 * sampled: C/S -> 8 points, Q/T -> 4, A -> 8. Paths are treated as
 * filled (open subpaths are closed implicitly, like SVG fill); lines
 * and polylines are stroke-only (zero area). <defs> and <use> subtrees
 * are ignored. Garbage or missing geometry data -> element skipped
 * with no finding.
 */
#include "svg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUT 16777216u   /* 16 MiB, like the rest of the stack */
#define MAX_ELEMS 20000
#define MAX_PTS   200000
#define MAX_ATTRS 64

typedef struct mtx { double a, b, c, d, e, f; } mtx_t; /* x'=a x + c y + e, y'=b x + d y + f */

static const mtx_t IDENT = {1, 0, 0, 1, 0, 0};

typedef struct pctx {
    const char *p, *end;
    const char *path;
    svgdoc_t *d;
    int line;
} pctx_t;

/* ---------- small scanning helpers ---------- */

static void bump_line(pctx_t *pc, const char *from, const char *to)
{
    const char *q;
    for (q = from; q < to; q++)
        if (*q == '\n')
            pc->line++;
}

static void skip_spaces(pctx_t *pc)
{
    const char *p = pc->p;
    while (p < pc->end && ascii_space((unsigned char)*p))
        p++;
    bump_line(pc, pc->p, p);
    pc->p = p;
}

static int peek(pctx_t *pc, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(pc->end - pc->p) >= n && strncmp(pc->p, s, n) == 0;
}

static void consume(pctx_t *pc, const char *s)
{
    size_t n = strlen(s);
    bump_line(pc, pc->p, pc->p + n);
    pc->p += n;
}

/* skip until the given closing substring; returns 0 if not found */
static int skip_until(pctx_t *pc, const char *close)
{
    const char *p = pc->p, *end = pc->end;
    size_t n = strlen(close);
    while (p + n <= end) {
        if (strncmp(p, close, n) == 0) {
            bump_line(pc, pc->p, p);
            pc->p = p;
            return 1;
        }
        p++;
    }
    bump_line(pc, pc->p, end);
    pc->p = end;
    return 0;
}

/* ---------- number parsing (robust; never advances on failure) ---------- */

static const char *num_skip_sep(const char *p, const char *end)
{
    while (p < end && (ascii_space((unsigned char)*p) || *p == ','))
        p++;
    return p;
}

/* parse [-]digits[.digits][e[+-]digits]; on success returns end pointer
 * and sets *out; on failure returns NULL */
static const char *parse_num(const char *p, const char *end, double *out)
{
    const char *q = p;
    int sign = 1;
    double v = 0;
    int digits = 0;
    if (q < end && (*q == '-' || *q == '+')) {
        if (*q == '-')
            sign = -1;
        q++;
    }
    while (q < end && ascii_digit((unsigned char)*q)) {
        v = v * 10 + (*q - '0');
        q++;
        digits++;
    }
    if (q < end && *q == '.') {
        double f = 0.1;
        q++;
        while (q < end && ascii_digit((unsigned char)*q)) {
            v += (*q - '0') * f;
            f *= 0.1;
            q++;
            digits++;
        }
    }
    if (!digits)
        return NULL;
    v *= sign;
    if (q < end && (*q == 'e' || *q == 'E')) {
        const char *r = q + 1;
        int esign = 1, ed = 0;
        double ev = 0;
        if (r < end && (*r == '-' || *r == '+')) {
            if (*r == '-')
                esign = -1;
            r++;
        }
        while (r < end && ascii_digit((unsigned char)*r)) {
            ev = ev * 10 + (*r - '0');
            r++;
            ed++;
        }
        if (ed)
            v *= pow(10.0, esign * ev);
    }
    if (!isfinite(v))
        return NULL;
    *out = v;
    return q;
}

static int parse_nums(const char *s, double *out, int maxn)
{
    const char *p = s, *end = s + strlen(s);
    int n = 0;
    while (n < maxn) {
        p = num_skip_sep(p, end);
        if (p >= end)
            break;
        p = parse_num(p, end, &out[n]);
        if (!p)
            break;
        n++;
    }
    return n;
}

/* ---------- matrix math ---------- */

static mtx_t mtx_compose(const mtx_t *a, const mtx_t *b) /* apply b, then a */
{
    mtx_t r;
    r.a = a->a * b->a + a->c * b->b;
    r.b = a->b * b->a + a->d * b->b;
    r.c = a->a * b->c + a->c * b->d;
    r.d = a->b * b->c + a->d * b->d;
    r.e = a->a * b->e + a->c * b->f + a->e;
    r.f = a->b * b->e + a->d * b->f + a->f;
    return r;
}

static pt_t mtx_apply(const mtx_t *m, pt_t p)
{
    pt_t r;
    r.x = m->a * p.x + m->c * p.y + m->e;
    r.y = m->b * p.x + m->d * p.y + m->f;
    return r;
}

static double mtx_det(const mtx_t *m)
{
    return m->a * m->d - m->b * m->c;
}

static mtx_t mtx_translate(double tx, double ty)
{
    mtx_t m = IDENT;
    m.e = tx;
    m.f = ty;
    return m;
}

static mtx_t mtx_scale(double sx, double sy)
{
    mtx_t m = IDENT;
    m.a = sx;
    m.d = sy;
    return m;
}

static mtx_t mtx_rotate_deg(double deg)
{
    double r = deg * (3.14159265358979323846 / 180.0), c = cos(r), s = sin(r);
    mtx_t m = IDENT;
    m.a = c;
    m.b = s;
    m.c = -s;
    m.d = c;
    return m;
}

/* parse a transform attribute: translate/scale/rotate applied, other
 * functions skipped with a note; returns the composed matrix */
static mtx_t parse_transform(pctx_t *pc, const char *val, int line)
{
    const char *p = val, *end = val + strlen(val);
    mtx_t m = IDENT;
    while (1) {
        char fn[32];
        size_t fl = 0;
        double args[8];
        int n;
        while (p < end && ascii_space((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        while (p < end && !ascii_space((unsigned char)*p) && *p != '(' && fl < 31)
            fn[fl++] = *p++;
        fn[fl] = 0;
        if (p >= end || *p != '(')
            break;
        p++;
        {
            const char *close = p;
            int depth = 1;
            while (close < end && depth > 0) {
                if (*close == '(')
                    depth++;
                else if (*close == ')')
                    depth--;
                close++;
            }
            if (depth != 0)
                break;
            n = parse_nums(p, args, 8);
            p = close;
        }
        if (ci_eq(fn, "translate") && n >= 1) {
            mtx_t t = mtx_translate(args[0], n >= 2 ? args[1] : 0);
            m = mtx_compose(&t, &m);
        } else if (ci_eq(fn, "scale") && n >= 1) {
            mtx_t t = mtx_scale(args[0], n >= 2 ? args[1] : args[0]);
            m = mtx_compose(&t, &m);
        } else if (ci_eq(fn, "rotate") && n >= 1) {
            mtx_t r = mtx_rotate_deg(args[0]);
            if (n >= 3) {
                mtx_t t1 = mtx_translate(args[1], args[2]);
                mtx_t t2 = mtx_translate(-args[1], -args[2]);
                m = mtx_compose(&t1, &m);
                m = mtx_compose(&r, &m);
                m = mtx_compose(&t2, &m);
            } else {
                m = mtx_compose(&r, &m);
            }
        } else {
            pc->d->ntfm_skipped++;
            fprintf(stderr, "note: %s: line %d: unsupported transform function %s skipped\n",
                    pc->path, line, fn);
        }
    }
    return m;
}

/* ---------- element construction ---------- */

static elem_t *elem_new(const char *tag, int line)
{
    elem_t *e = xcalloc(sizeof *e);
    e->tag = tag;
    e->line = line;
    return e;
}

static void elem_pt(elem_t *e, pt_t p, int seg) /* append point; segment from previous if seg */
{
    if (e->npts >= MAX_PTS)
        return;
    if (seg && e->npts > 0) {
        pt_t q = e->pts[e->npts - 1];
        if (q.x != p.x || q.y != p.y) {
            if (e->nsegs < MAX_PTS) {
                e->segs[e->nsegs].a = q;
                e->segs[e->nsegs].b = p;
                e->nsegs++;
            }
        }
    }
    if (e->npts == e->cpts) {
        e->cpts = e->cpts ? e->cpts * 2 : 32;
        e->pts = xrealloc_(e->pts, e->cpts * sizeof(pt_t));
        e->segs = xrealloc_(e->segs, e->cpts * sizeof(seg_t));
    }
    e->pts[e->npts++] = p;
}

static void elem_ring(elem_t *e, pt_t *ring, size_t n, int close)
{
    size_t i;
    for (i = 0; i < n; i++)
        elem_pt(e, ring[i], i > 0);
    if (close && n > 1 && (e->pts[n - 1].x != ring[0].x || e->pts[n - 1].y != ring[0].y)) {
        if (e->nsegs < MAX_PTS) {
            e->segs[e->nsegs].a = ring[n - 1];
            e->segs[e->nsegs].b = ring[0];
            e->nsegs++;
        }
    }
}

static void elem_finish(elem_t *e, const mtx_t *m)
{
    size_t i;
    double det = fabs(mtx_det(m));
    if (e->npts) {
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (i = 0; i < e->npts; i++) {
            pt_t p = mtx_apply(m, e->pts[i]);
            e->pts[i] = p;
            if (p.x < x0) x0 = p.x;
            if (p.y < y0) y0 = p.y;
            if (p.x > x1) x1 = p.x;
            if (p.y > y1) y1 = p.y;
        }
        for (i = 0; i < e->nsegs; i++) {
            e->segs[i].a = mtx_apply(m, e->segs[i].a);
            e->segs[i].b = mtx_apply(m, e->segs[i].b);
        }
        e->x0 = x0;
        e->y0 = y0;
        e->x1 = x1;
        e->y1 = y1;
        e->has_pts = 1;
    }
    e->area *= det;
}

static void elem_free(elem_t *e)
{
    free(e->id);
    free(e->pts);
    free(e->segs);
    free(e);
}

/* ---------- path data ---------- */

typedef struct pathst {
    elem_t *e;
    pt_t cur, start;
    double acc;
    int nsub;
    int last_cubic, last_quad;
    pt_t last_ctrl;
} pathst_t;

static void subpath_close(pathst_t *st)
{
    if (st->nsub >= 3)
        st->e->area += fabs(st->acc) * 0.5;
    st->acc = 0;
    st->nsub = 0;
}

static void p_move(pathst_t *st, pt_t p)
{
    subpath_close(st);
    st->start = p;
    st->cur = p;
    st->nsub = 1;
    elem_pt(st->e, p, 0);
}

static void p_line(pathst_t *st, pt_t p)
{
    if (st->nsub == 0)
        p_move(st, p);
    else {
        st->acc += st->cur.x * p.y - p.x * st->cur.y;
        elem_pt(st->e, p, 1);
        st->cur = p;
        st->nsub++;
    }
}

static void p_curve(pathst_t *st, pt_t *pts, int n)
{
    int k;
    for (k = 1; k <= n; k++) {
        pt_t p;
        double t = (double)k / n;
        if (n == 8) { /* cubic: pts[0]=P0 pts[1]=C1 pts[2]=C2 pts[3]=P3 */
            double u = 1 - t;
            p.x = u * u * u * pts[0].x + 3 * u * u * t * pts[1].x +
                  3 * u * t * t * pts[2].x + t * t * t * pts[3].x;
            p.y = u * u * u * pts[0].y + 3 * u * u * t * pts[1].y +
                  3 * u * t * t * pts[2].y + t * t * t * pts[3].y;
        } else { /* quad: pts[0]=P0 pts[1]=C pts[2]=P1 */
            double u = 1 - t;
            p.x = u * u * pts[0].x + 2 * u * t * pts[1].x + t * t * pts[2].x;
            p.y = u * u * pts[0].y + 2 * u * t * pts[1].y + t * t * pts[2].y;
        }
        p_line(st, p);
    }
}

static void p_arc(pathst_t *st, double rx, double ry, double rot,
                  int large, int sweep, pt_t p1)
{
    pt_t p0 = st->cur, sample;
    int k;
    double phi = rot * (3.14159265358979323846 / 180.0);
    double f1 = cos(phi), f2 = sin(phi);
    double dx2 = (p0.x - p1.x) / 2, dy2 = (p0.y - p1.y) / 2;
    double x1p = f1 * dx2 + f2 * dy2;
    double y1p = -f2 * dx2 + f1 * dy2;
    double lam, cx, cy, th, dth;
    if (p0.x == p1.x && p0.y == p1.y)
        return; /* degenerate arc: omitted per SVG spec */
    if (fabs(rx) < 1e-9 || fabs(ry) < 1e-9) { /* straight line, 8 samples */
        for (k = 1; k <= 8; k++) {
            double t = (double)k / 8;
            sample.x = p0.x + (p1.x - p0.x) * t;
            sample.y = p0.y + (p1.y - p0.y) * t;
            p_line(st, sample);
        }
        return;
    }
    rx = fabs(rx);
    ry = fabs(ry);
    lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) {
        rx *= sqrt(lam);
        ry *= sqrt(lam);
    }
    {
        double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
        double coef = den > 0 ? sqrt(num > 0 ? num : 0) / den : 0;
        int sign = (large == sweep) ? -1 : 1;
        cx = sign * coef * (rx * y1p / ry);
        cy = sign * coef * (-ry * x1p / rx);
    }
    th = atan2((y1p - cy) / ry, (x1p - cx) / rx);
    dth = atan2((-y1p - cy) / ry, (-x1p - cx) / rx) - th;
    if (sweep == 0 && dth > 0)
        dth -= 2 * 3.14159265358979323846;
    if (sweep == 1 && dth < 0)
        dth += 2 * 3.14159265358979323846;
    for (k = 1; k <= 8; k++) {
        double t = (double)k / 8;
        double ang = th + dth * t;
        double x = rx * cos(ang), y = ry * sin(ang);
        sample.x = f1 * x - f2 * y + (p0.x + p1.x) / 2;
        sample.y = f2 * x + f1 * y + (p0.y + p1.y) / 2;
        if (k == 8)
            sample = p1;
        p_line(st, sample);
    }
}

static void path_parse(const char *d, elem_t *e)
{
    const char *p = d, *end = d + strlen(d);
    pathst_t st;
    pt_t prev_ctrl = {0, 0};
    int prev_cubic = 0, prev_quad = 0;
    char last = 0;
    int have = 0;
    memset(&st, 0, sizeof st);
    st.e = e;
    while (p < end) {
        double n[7];
        int cnt = 0;
        const char *q;
        char c = *p;
        if (ascii_space((unsigned char)c) || c == ',') {
            p++;
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            p++;
            if (c == 'Z' || c == 'z') {
                if (st.nsub > 0) {
                    if (st.cur.x != st.start.x || st.cur.y != st.start.y) {
                        st.acc += st.cur.x * st.start.y - st.start.x * st.cur.y;
                        if (e->npts > 0 && e->nsegs < MAX_PTS) {
                            e->segs[e->nsegs].a = st.cur;
                            e->segs[e->nsegs].b = st.start;
                            e->nsegs++;
                        }
                    }
                    subpath_close(&st);
                }
                last = c;
                continue;
            }
            last = c;
            have = 1;
            continue;
        }
        if (!have)
            break; /* garbage before any command */
        /* read args for the last command (implicit repetition) */
        {
            double a[7];
            int i = 0;
            const char *r = p;
            while (i < 7) {
                const char *at = num_skip_sep(r, end);
                if (at >= end)
                    break;
                r = parse_num(at, end, &a[i]);
                if (!r) {
                    r = at; /* keep the position: end of arg list */
                    break;
                }
                i++;
            }
            if (i == 0)
                break; /* garbage number stream */
            q = r;
            cnt = i;
            memcpy(n, a, sizeof a);
            p = q;
        }
        switch (last) {
        case 'M': case 'm': case 'L': case 'l':
        case 'H': case 'h': case 'V': case 'v': {
            pt_t t;
            int k = 0;
            for (k = 0; k + 1 <= cnt || (k < cnt && (last == 'H' || last == 'h' ||
                  last == 'V' || last == 'v')); k += (last == 'H' || last == 'h' ||
                  last == 'V' || last == 'v') ? 1 : 2) {
                if (last == 'H' || last == 'h' || last == 'V' || last == 'v') {
                    if (k >= cnt)
                        break;
                    t = st.cur;
                    if (last == 'H')
                        t.x = n[k];
                    else if (last == 'h')
                        t.x = st.cur.x + n[k];
                    else if (last == 'V')
                        t.y = n[k];
                    else
                        t.y = st.cur.y + n[k];
                } else {
                    if (k + 1 >= cnt)
                        break;
                    t.x = n[k];
                    t.y = n[k + 1];
                    if (last == 'm')
                        t.x += st.cur.x, t.y += st.cur.y;
                    else if (last == 'l')
                        t.x += st.cur.x, t.y += st.cur.y;
                }
                if (last == 'M' || last == 'm') {
                    p_move(&st, t);
                    last = (last == 'M') ? 'L' : 'l';
                    prev_cubic = prev_quad = 0;
                } else {
                    p_line(&st, t);
                    prev_cubic = prev_quad = 0;
                }
            }
            break;
        }
        case 'C': case 'c': case 'S': case 's': {
            int k = 0;
            int need = (last == 'C' || last == 'c') ? 6 : 4;
            while (k + need - 1 < cnt) {
                pt_t pts[4];
                pt_t c1, c2;
                pts[0] = st.cur;
                if (last == 'C' || last == 'c') {
                    c1.x = n[k]; c1.y = n[k + 1];
                    c2.x = n[k + 2]; c2.y = n[k + 3];
                } else {
                    c1 = prev_cubic ? (pt_t){2 * st.cur.x - prev_ctrl.x,
                                             2 * st.cur.y - prev_ctrl.y} : st.cur;
                    c2.x = n[k]; c2.y = n[k + 1];
                }
                if (last == 'c' || last == 's') {
                    c1.x += st.cur.x; c1.y += st.cur.y;
                    c2.x += st.cur.x; c2.y += st.cur.y;
                }
                pts[1] = c1;
                pts[2] = c2;
                pts[3].x = n[k + (last == 'C' || last == 'c' ? 4 : 2)];
                pts[3].y = n[k + (last == 'C' || last == 'c' ? 5 : 3)];
                if (last == 'c' || last == 's') {
                    pts[3].x += st.cur.x;
                    pts[3].y += st.cur.y;
                }
                p_curve(&st, pts, 8);
                prev_ctrl = pts[2];
                prev_cubic = 1;
                prev_quad = 0;
                k += (last == 'C' || last == 'c') ? 6 : 4;
            }
            break;
        }
        case 'Q': case 'q': case 'T': case 't': {
            int k = 0;
            int need = (last == 'Q' || last == 'q') ? 4 : 2;
            while (k + need - 1 < cnt) {
                pt_t pts[3];
                pts[0] = st.cur;
                if (last == 'Q' || last == 'q') {
                    pts[1].x = n[k]; pts[1].y = n[k + 1];
                    pts[2].x = n[k + 2]; pts[2].y = n[k + 3];
                    if (last == 'q') {
                        pts[1].x += st.cur.x; pts[1].y += st.cur.y;
                        pts[2].x += st.cur.x; pts[2].y += st.cur.y;
                    }
                    k += 4;
                } else {
                    pts[1] = prev_quad ? (pt_t){2 * st.cur.x - prev_ctrl.x,
                                                2 * st.cur.y - prev_ctrl.y} : st.cur;
                    pts[2].x = n[k]; pts[2].y = n[k + 1];
                    if (last == 't') {
                        pts[2].x += st.cur.x; pts[2].y += st.cur.y;
                    }
                    k += 2;
                }
                p_curve(&st, pts, 4);
                prev_ctrl = pts[1];
                prev_quad = 1;
                prev_cubic = 0;
            }
            break;
        }
        case 'A': case 'a': {
            int k = 0;
            while (k + 6 < cnt) {
                pt_t t;
                t.x = n[k + 5]; t.y = n[k + 6];
                if (last == 'a') {
                    t.x += st.cur.x; t.y += st.cur.y;
                }
                p_arc(&st, n[k], n[k + 1], n[k + 2], (int)n[k + 3],
                      (int)n[k + 4], t);
                k += 7;
            }
            break;
        }
        default:
            break; /* M repetition handled as L; others: ignore */
        }
    }
    /* implicit close (SVG fill semantics): finalize the last subpath */
    if (st.nsub > 0) {
        if (st.cur.x != st.start.x || st.cur.y != st.start.y) {
            st.acc += st.cur.x * st.start.y - st.start.x * st.cur.y;
            if (e->npts > 0 && e->nsegs < MAX_PTS) {
                e->segs[e->nsegs].a = st.cur;
                e->segs[e->nsegs].b = st.start;
                e->nsegs++;
            }
        }
        subpath_close(&st);
    }
}

/* ---------- element builders ---------- */

typedef struct attr { const char *name; const char *value; } attr_t;

static const char *attr_get(const attr_t *as, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(as[i].name, name) == 0)
            return as[i].value;
    return NULL;
}

static void add_elem(pctx_t *pc, elem_t *e, const attr_t *as, size_t na,
                     const mtx_t *m)
{
    const char *id = attr_get(as, na, "id");
    if (id)
        e->id = xstrdup(id);
    elem_finish(e, m);
    if (!e->has_pts) {
        elem_free(e);
        return;
    }
    if (pc->d->nel < MAX_ELEMS)
        pc->d->el[pc->d->nel++] = e;
    else
        elem_free(e);
}

static void build_circle(pctx_t *pc, const attr_t *as, size_t na, const mtx_t *m)
{
    double cx = 0, cy = 0, r = 0;
    const char *v;
    elem_t *e;
    pt_t ring[16];
    int k, n = 0;
    v = attr_get(as, na, "cx");
    if (v) parse_nums(v, &cx, 1);
    v = attr_get(as, na, "cy");
    if (v) parse_nums(v, &cy, 1);
    v = attr_get(as, na, "r");
    if (!v || parse_nums(v, &r, 1) < 1 || r <= 0)
        return; /* no geometry: skip with no finding */
    e = elem_new("circle", pc->line);
    e->closed = 1;
    e->area = 3.14159265358979323846 * r * r;
    for (k = 0; k < 16; k++) { /* 16 perimeter samples, corners included */
        double ang = 2 * 3.14159265358979323846 * k / 16;
        ring[n].x = cx + r * cos(ang);
        ring[n].y = cy + r * sin(ang);
        n++;
    }
    elem_ring(e, ring, n, 1);
    add_elem(pc, e, as, na, m);
}

static void build_ellipse(pctx_t *pc, const attr_t *as, size_t na, const mtx_t *m)
{
    double cx = 0, cy = 0, rx = 0, ry = 0;
    const char *v;
    elem_t *e;
    pt_t ring[16];
    int k, n = 0;
    v = attr_get(as, na, "cx");
    if (v) parse_nums(v, &cx, 1);
    v = attr_get(as, na, "cy");
    if (v) parse_nums(v, &cy, 1);
    v = attr_get(as, na, "rx");
    if (v) parse_nums(v, &rx, 1);
    v = attr_get(as, na, "ry");
    if (v) parse_nums(v, &ry, 1);
    if (rx <= 0 || ry <= 0)
        return;
    e = elem_new("ellipse", pc->line);
    e->closed = 1;
    e->area = 3.14159265358979323846 * rx * ry;
    for (k = 0; k < 16; k++) {
        double ang = 2 * 3.14159265358979323846 * k / 16;
        ring[n].x = cx + rx * cos(ang);
        ring[n].y = cy + ry * sin(ang);
        n++;
    }
    elem_ring(e, ring, n, 1);
    add_elem(pc, e, as, na, m);
}

static void build_rect(pctx_t *pc, const attr_t *as, size_t na, const mtx_t *m)
{
    double x = 0, y = 0, w = 0, h = 0;
    const char *v;
    elem_t *e;
    pt_t ring[4];
    v = attr_get(as, na, "x");
    if (v) parse_nums(v, &x, 1);
    v = attr_get(as, na, "y");
    if (v) parse_nums(v, &y, 1);
    v = attr_get(as, na, "width");
    if (v) parse_nums(v, &w, 1);
    v = attr_get(as, na, "height");
    if (v) parse_nums(v, &h, 1);
    if (w == 0 || h == 0)
        return;
    w = fabs(w);
    h = fabs(h);
    e = elem_new("rect", pc->line);
    e->closed = 1;
    e->area = w * h;
    ring[0].x = x; ring[0].y = y;
    ring[1].x = x + w; ring[1].y = y;
    ring[2].x = x + w; ring[2].y = y + h;
    ring[3].x = x; ring[3].y = y + h;
    elem_ring(e, ring, 4, 1);
    add_elem(pc, e, as, na, m);
}

static void build_line(pctx_t *pc, const attr_t *as, size_t na, const mtx_t *m)
{
    double n[4];
    const char *v;
    elem_t *e;
    pt_t a, b;
    memset(n, 0, sizeof n);
    v = attr_get(as, na, "x1"); if (v) parse_nums(v, &n[0], 1);
    v = attr_get(as, na, "y1"); if (v) parse_nums(v, &n[1], 1);
    v = attr_get(as, na, "x2"); if (v) parse_nums(v, &n[2], 1);
    v = attr_get(as, na, "y2"); if (v) parse_nums(v, &n[3], 1);
    e = elem_new("line", pc->line);
    a.x = n[0]; a.y = n[1];
    b.x = n[2]; b.y = n[3];
    if (a.x == b.x && a.y == b.y) {
        elem_free(e);
        return;
    }
    elem_pt(e, a, 0);
    elem_pt(e, b, 1);
    add_elem(pc, e, as, na, m);
}

static void build_points(pctx_t *pc, const attr_t *as, size_t na, const mtx_t *m,
                         const char *tag, int closed)
{
    const char *v = attr_get(as, na, "points");
    double n[512];
    int cnt, k;
    elem_t *e;
    if (!v)
        return;
    cnt = parse_nums(v, n, 512);
    if (cnt < 2)
        return;
    e = elem_new(tag, pc->line);
    e->closed = closed;
    for (k = 0; k + 1 < cnt; k += 2)
        elem_pt(e, (pt_t){n[k], n[k + 1]}, k > 0);
    if (closed && cnt >= 4) {
        pt_t first = {n[0], n[1]}, lastp = e->pts[e->npts - 1];
        if (first.x != lastp.x || first.y != lastp.y)
            elem_pt(e, first, 1);
        if (e->npts >= 3)
            e->area = fabs(shoelace_(e->pts, e->npts));
    }
    add_elem(pc, e, as, na, m);
}

/* ---------- attribute parsing ---------- */

static void parse_attrs(pctx_t *pc, attr_t *as, size_t *na, int *self_close)
{
    *na = 0;
    *self_close = 0;
    while (pc->p < pc->end) {
        char name[64];
        size_t nl = 0;
        skip_spaces(pc);
        if (pc->p >= pc->end)
            return;
        if (*pc->p == '>') {
            pc->p++;
            pc->line++;
            return;
        }
        if (*pc->p == '/' && pc->p + 1 < pc->end && pc->p[1] == '>') {
            pc->p += 2;
            pc->line++;
            *self_close = 1;
            return;
        }
        while (pc->p < pc->end && *pc->p != '=' && !ascii_space((unsigned char)*pc->p) &&
               *pc->p != '>' && *pc->p != '/' && nl < 63)
            name[nl++] = *pc->p++;
        name[nl] = 0;
        while (pc->p < pc->end && ascii_space((unsigned char)*pc->p))
            pc->p++;
        if (pc->p < pc->end && *pc->p == '=') {
            char value[4096];
            size_t vl = 0;
            pc->p++;
            while (pc->p < pc->end && ascii_space((unsigned char)*pc->p))
                pc->p++;
            if (pc->p < pc->end && (*pc->p == '"' || *pc->p == '\'')) {
                char q = *pc->p++;
                while (pc->p < pc->end && *pc->p != q && vl < 4095) {
                    if (*pc->p == '\n')
                        pc->line++;
                    value[vl++] = *pc->p++;
                }
                if (pc->p < pc->end)
                    pc->p++;
            } else {
                while (pc->p < pc->end && !ascii_space((unsigned char)*pc->p) &&
                       *pc->p != '>' && vl < 4095) {
                    if (*pc->p == '\n')
                        pc->line++;
                    value[vl++] = *pc->p++;
                }
            }
            value[vl] = 0;
            if (nl > 0 && *na < MAX_ATTRS) {
                as[*na].name = xstrdup(name);
                as[*na].value = xstrdup(value);
                (*na)++;
            }
        } else if (nl > 0 && *na < MAX_ATTRS) {
            as[*na].name = xstrdup(name);
            as[*na].value = xstrdup("");
            (*na)++;
        }
    }
}

static void free_attrs(attr_t *as, size_t na)
{
    size_t i;
    for (i = 0; i < na; i++) {
        free((char *)as[i].name);
        free((char *)as[i].value);
    }
}

/* ---------- document walk ---------- */

static void parse_children(pctx_t *pc, const mtx_t *parent_m, int in_defs)
{
    while (1) {
        char tag[64];
        size_t tl = 0;
        attr_t attrs[MAX_ATTRS];
        size_t na;
        int self_close, tag_line;
        mtx_t m;
        const char *tfm;
        skip_spaces(pc);
        if (pc->p >= pc->end)
            return;
        if (*pc->p != '<') { /* raw text: skip to next tag */
            const char *q = memchr(pc->p, '<', (size_t)(pc->end - pc->p));
            bump_line(pc, pc->p, q ? q : pc->end);
            pc->p = q ? q : pc->end;
            continue;
        }
        if (peek(pc, "</")) {
            const char *q = memchr(pc->p, '>', (size_t)(pc->end - pc->p));
            bump_line(pc, pc->p, q ? q + 1 : pc->end);
            pc->p = q ? q + 1 : pc->end;
            return;
        }
        if (peek(pc, "<!--")) {
            consume(pc, "<!--");
            skip_until(pc, "-->");
            if (peek(pc, "-->"))
                consume(pc, "-->");
            continue;
        }
        if (peek(pc, "<?")) {
            consume(pc, "<?");
            skip_until(pc, "?>");
            if (peek(pc, "?>"))
                consume(pc, "?>");
            continue;
        }
        if (peek(pc, "<!")) { /* doctype */
            consume(pc, "<!");
            if (!skip_until(pc, ">"))
                return;
            consume(pc, ">");
            continue;
        }
        pc->p++; /* '<' */
        tag_line = pc->line;
        while (pc->p < pc->end && !ascii_space((unsigned char)*pc->p) &&
               *pc->p != '>' && *pc->p != '/' && tl < 63) {
            if (*pc->p == '\n')
                pc->line++;
            tag[tl++] = *pc->p++;
        }
        tag[tl] = 0;
        parse_attrs(pc, attrs, &na, &self_close);
        m = *parent_m;
        tfm = attr_get(attrs, na, "transform");
        if (tfm) {
            mtx_t tmp = parse_transform(pc, tfm, tag_line);
            m = mtx_compose(&tmp, &m);
        }
        if (!in_defs && (ci_eq(tag, "svg") || ci_eq(tag, "g") ||
                         ci_eq(tag, "path") || ci_eq(tag, "circle") ||
                         ci_eq(tag, "ellipse") || ci_eq(tag, "rect") ||
                         ci_eq(tag, "line") || ci_eq(tag, "polygon") ||
                         ci_eq(tag, "polyline"))) {
            if (ci_eq(tag, "svg")) {
                const char *vb = attr_get(attrs, na, "viewBox");
                const char *ds = attr_get(attrs, na, "data-shape");
                if (vb && !pc->d->has_vb && parse_nums(vb, pc->d->vb, 4) == 4 &&
                    pc->d->vb[2] > 0 && pc->d->vb[3] > 0)
                    pc->d->has_vb = 1;
                if (ds && !pc->d->shape_hint)
                    pc->d->shape_hint = xstrdup(ds);
                pc->d->has_svg = 1;
            } else if (ci_eq(tag, "path")) {
                const char *d = attr_get(attrs, na, "d");
                if (d) {
                    elem_t *e = elem_new("path", tag_line);
                    e->closed = 1;
                    path_parse(d, e);
                    add_elem(pc, e, attrs, na, &m);
                }
            } else if (ci_eq(tag, "circle")) {
                build_circle(pc, attrs, na, &m);
            } else if (ci_eq(tag, "ellipse")) {
                build_ellipse(pc, attrs, na, &m);
            } else if (ci_eq(tag, "rect")) {
                build_rect(pc, attrs, na, &m);
            } else if (ci_eq(tag, "line")) {
                build_line(pc, attrs, na, &m);
            } else if (ci_eq(tag, "polygon")) {
                build_points(pc, attrs, na, &m, "polygon", 1);
            } else { /* polyline */
                build_points(pc, attrs, na, &m, "polyline", 0);
            }
        }
        free_attrs(attrs, na);
        if (!self_close) {
            int skip = in_defs || ci_eq(tag, "defs") || ci_eq(tag, "use");
            parse_children(pc, &m, skip);
        }
    }
}

svgdoc_t *svg_parse(const char *src, size_t len, const char *path)
{
    pctx_t pc;
    svgdoc_t *d = xcalloc(sizeof *d);
    d->el = xmalloc(MAX_ELEMS * sizeof(elem_t *));
    if (len > MAX_INPUT)
        fprintf(stderr, "note: %s: input truncated at %u bytes\n", path,
                (unsigned)MAX_INPUT);
    pc.p = src;
    pc.end = src + (len < MAX_INPUT ? len : MAX_INPUT);
    pc.path = path ? path : "svg";
    pc.d = d;
    pc.line = 1;
    parse_children(&pc, &IDENT, 0);
    return d;
}

void svg_free(svgdoc_t *d)
{
    size_t i;
    for (i = 0; i < d->nel; i++)
        elem_free(d->el[i]);
    free(d->el);
    free(d->shape_hint);
    free(d);
}
