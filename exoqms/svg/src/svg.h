/* svg.h — exoqms-svg: the asset-logic QMS auditor for generated SVG.
 * A zero-dependency C11 static analyzer: reads the supported SVG subset
 * and checks the LOGIC of shapes (geometry, not taste). First shape kind:
 * trees. Shape kinds are pluggable rule-sets behind checks_run_tree().
 */
#ifndef EXOQMS_SVG_H
#define EXOQMS_SVG_H

#include <stddef.h>

#define SVG_VERSION "0.1.0"

/* ---------- util ---------- */
typedef struct buf { char *p; size_t len, cap; } buf_t;
void *xmalloc(size_t n);
void *xcalloc(size_t n);
void *xrealloc_(void *p, size_t n);
char *xstrdup(const char *s);
void buf_append(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_free(buf_t *b);

typedef struct vec { void **it; size_t len, cap; } vec_t;
void vec_push(vec_t *v, void *p);

int ci_eq(const char *a, const char *b);          /* ASCII case-insensitive eq */
int ascii_space(int c);
int ascii_digit(int c);
void lc_ascii(char *s);
char *json_escape(const char *s, size_t n);
char *file_read(const char *path, size_t *len, char *err, size_t errsz);
int dir_walk_svg(const char *dir, vec_t *out);    /* recursive *.svg list */

/* ---------- geometry ---------- */
typedef struct pt { double x, y; } pt_t;
typedef struct seg { pt_t a, b; } seg_t;

typedef struct elem {
    const char *tag;        /* static tag name */
    char *id;               /* id attribute or NULL */
    int line;               /* 1-based source line of the open tag */
    pt_t *pts;              /* sampled boundary points, world coords */
    size_t npts, cpts;
    seg_t *segs;            /* boundary segments, world coords */
    size_t nsegs, csegs;
    int closed;             /* closed shape: area is meaningful */
    double area;            /* painted area, world units */
    double x0, y0, x1, y1;  /* bbox over pts */
    int has_pts;
} elem_t;

/* ---------- svg document ---------- */
typedef struct svgdoc {
    elem_t **el;
    size_t nel, cel;
    double vb[4];           /* viewBox x y w h */
    int has_vb;
    char *shape_hint;       /* data-shape attribute of the root <svg> */
    size_t ntfm_skipped;    /* unsupported transform functions ignored */
    int has_svg;            /* an <svg> root element was seen */
} svgdoc_t;

svgdoc_t *svg_parse(const char *src, size_t len, const char *path);
void svg_free(svgdoc_t *d);

/* ---------- geometry helpers ---------- */
double shoelace_(const pt_t *pts, size_t n);      /* signed, then caller abs */
int convex_hull(const pt_t *pts, size_t n, pt_t *out);  /* returns hull size */

/* ---------- checks ---------- */
typedef struct finding {
    int major;
    const char *check;      /* static id string */
    char *reason;
    char *file;             /* owned */
    char *shape;            /* owned: shape id this finding came from */
} finding_t;

enum { SHAPE_TREE, SHAPE_AUTO };
enum { RES_FIND, RES_SKIP };                    /* audit result kinds */

typedef struct audit_result {
    int kind;               /* RES_FIND (tree checks ran) or RES_SKIP */
    char *shape;            /* detected/used shape id, e.g. "tree" */
    vec_t findings;         /* finding items are owned by the caller once
                             * moved out; audit_result_free frees only the
                             * array and res->shape */
} audit_result_t;

/* audit one parsed svg: runs the tree rule-set when shape resolves to
 * tree (explicit, or auto via data-shape/filename), else kind=RES_SKIP */
void audit_run(svgdoc_t *d, const char *path, int shape_mode,
               audit_result_t *res);
void audit_result_free(audit_result_t *r);
void findings_free(vec_t *out);

#endif /* EXOQMS_SVG_H */
