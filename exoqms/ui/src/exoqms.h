/* exoqms-ui — the UI quality auditor for the exomind stack.
 * A zero-dependency C11 static analyzer: reads HTML + CSS and detects
 * seven classes of UI defects (emoji icons, overlapping elements,
 * misalignment, corner-geometry mismatch, background problems,
 * unstyled SDK-default widgets, low text contrast).
 *
 * Honest scope: this is a static analyzer with a simplified layout
 * model, not a pixel-perfect renderer. The supported HTML/CSS subsets
 * and the layout approximations are documented in README.md.
 */
#ifndef EXOQMS_H
#define EXOQMS_H

#include <stddef.h>
#include <stdint.h>

#define EXOQMS_VERSION "0.1.0"

#define VIEW_W 1024.0          /* assumed viewport width, px */
#define VIEW_H 768.0           /* assumed viewport height, px */
#define DEFAULT_FONTSZ 16.0    /* px */
#define DEFAULT_LH 1.4         /* normal line-height, approx */

/* ---------- util ---------- */
typedef struct buf { char *p; size_t len, cap; } buf_t;
void *xmalloc(size_t n);
void *xcalloc(size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void buf_append(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);

typedef struct vec { void **it; size_t len, cap; } vec_t;
void vec_push(vec_t *v, void *p);

void str_trim(char *s);                  /* strip ASCII whitespace both ends */
int ci_eq(const char *a, const char *b); /* ASCII case-insensitive eq */
int hexval(int c);
int ascii_space(int c);
int ascii_digit(int c);
void lc_ascii(char *s);
int parse_num(const char *s, double *out);  /* leading [-]NN[.NN], trailing junk ok */
char *json_escape(const char *s, size_t n);

uint32_t utf8_next(const char *s, size_t *i);  /* decode; advances *i, U+FFFD on error */
int utf8_is_emoji(uint32_t cp);
size_t utf8_write(uint32_t cp, char out[4]);

char *file_read(const char *path, size_t *len, char *err, size_t errsz);
int dir_walk_html(const char *dir, vec_t *out);  /* recursive *.html list */

/* property keys for computed style values */
enum {
    P_COLOR, P_BG, P_BGIMG, P_RAD, P_W, P_H, P_MINW, P_MAXW, P_MINH, P_MAXH,
    P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL,
    P_POS, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT,
    P_DISP, P_FLOAT, P_BOX, P_FONTSZ, P_LH, P_OPAC, P_VIS,
    P_FLXDIR, P_JUSTIFY, P_ALIGNI, P_ALIGNS, P_FLXWRAP,
    P_FLXBASIS, P_FLXGROW, P_FLXSHRINK, P_TEXTALIGN,
    P_BTW, P_BRW, P_BBW, P_BLW, P_BW, P_BWC, P_BTC, P_BRC, P_BBC, P_BLC,
    P_NPROP
};

typedef struct cstyle {
    char *v[P_NPROP];
    uint8_t set[P_NPROP];
    uint8_t unres[P_NPROP];   /* value contains an unresolved var()/calc()/unit */
    uint8_t fromvar[P_NPROP]; /* value came from a var() reference */
} cstyle_t;

/* ---------- html ---------- */
typedef struct attr { char *name; char *value; } attr_t;
typedef struct node {
    char *tag;              /* lowercase tag name; NULL for text nodes */
    char *text;             /* decoded text content (text nodes only) */
    int line;               /* 1-based source line of the open tag */
    attr_t *attrs;
    size_t nattrs;
    struct node **kids;
    size_t nkids;
    struct node *parent;
    int cidx;               /* index into the layout comp array, -1 = none */
    int in_hidden_ctx;      /* under head/script/style/template/... */
} node_t;

node_t *html_parse(const char *src, size_t len, int *errline);
void html_free(node_t *n);
const char *node_attr(const node_t *n, const char *name);
int node_has_class(const node_t *n, const char *cls);
void node_selector(const node_t *n, char *out, size_t outsz);
int node_own_text(const node_t *n, buf_t *out);  /* direct text children, decoded */

/* ---------- css ---------- */
typedef struct decl { char *prop; char *value; } decl_t;
typedef struct simple {
    char *tag;              /* NULL = none present, "*" = universal */
    char *id;               /* NULL = none */
    char **cls;
    size_t ncls;
    int child;              /* must be direct child of the previous part */
} simple_t;
typedef struct selector {
    simple_t *parts;
    size_t nparts;
    int spec;
} selector_t;
typedef struct rule {
    selector_t *sels;
    size_t nsels;
    decl_t *decls;
    size_t ndecls;
    size_t order;
} rule_t;
typedef struct var { char *name; char *value; } var_t;
typedef struct css {
    rule_t *rules;
    size_t nrules;
    var_t *vars;
    size_t nvars;
    int skipped_at;         /* @media/@import/@keyframes/... skipped */
    int skipped_rules;      /* rules with unsupported selectors skipped */
} css_t;

css_t css_parse(const char *src, size_t len);
void css_free(css_t *c);
const char *css_var_resolve(css_t *c, const char *name, char *out, size_t outsz);
void css_palette(css_t *c, vec_t *out);  /* resolves --color* vars to hex strings */
int css_has_palette(css_t *c);
void css_apply(css_t *c, const node_t *n, cstyle_t *st, int *matched);
void css_apply_inline(css_t *c, const char *style, cstyle_t *st);

/* ---------- color ---------- */
typedef struct color { int r, g, b, a; int valid; } color_t;  /* 0..255 */
color_t color_parse(const char *s);
double color_lum(const color_t *c);             /* relative luminance 0..1 */
double color_ratio(const color_t *a, const color_t *b);  /* WCAG ratio */
void color_hex(const color_t *c, char out[16]); /* #rrggbb */
int color_eq(const color_t *a, const color_t *b);
color_t color_composite(color_t fg, color_t bg); /* alpha blend over bg */

/* ---------- layout ---------- */
typedef struct comp {
    int hidden;             /* not rendered (display:none, hidden, opacity 0, hidden ctx) */
    double x, y, w, h;      /* box in px; -1 on any dimension that cannot be determined */
    uint8_t est;            /* EST_* bits: approximate dimension */
    double m[4], p[4];      /* margins / padding: T R B L */
    uint8_t ms[4], ps[4];
    double rad[4];          /* border-radius per corner: TL TR BR BL */
    uint8_t rs[4], radpct[4];
    int pos;                /* 0 static, 1 relative, 2 absolute, 3 fixed */
    double off[4];          /* top right bottom left */
    uint8_t offs[4];
    int disp;               /* 0 block, 1 inline, 2 inline-block, 3 none, 4 flex, 5 grid */
    int flxrow;             /* flex-direction: row */
    double fontsz;
    double lh;              /* line height, px */
    double opacity;
    int bg_set;             /* own background-color declared (non-transparent) */
    int color_set;          /* own color declared */
    int bg_img;             /* background-image / gradient present: bg unknown */
    int bg_var;             /* background came from a var() reference */
    color_t bg, color;      /* own declared colors (valid only if *_set) */
    int bordered;           /* any border side >= 1px */
    int matched_css;        /* >= 1 CSS rule matched this element */
    color_t bg_eff, col_eff;/* resolved effective colors (after inheritance/alpha) */
    int bg_eff_set, col_eff_set;
    int eff_opaque;         /* effective opacity is exactly 1 */
} comp_t;

enum {
    EST_X = 1, EST_Y = 2, EST_W = 4, EST_H = 8
};

typedef struct layout {
    node_t *root;
    css_t *css;
    cstyle_t *st;           /* per node (indexed by cidx) */
    comp_t *comp;
    size_t ncomp;
    double vw, vh;
    color_t page_bg;        /* body/html background or white default */
    int page_bg_set;
} layout_t;

int layout_build(layout_t *L, node_t *root, css_t *css);
color_t node_eff_bg(layout_t *L, const node_t *n, int *set);
color_t node_eff_color(layout_t *L, const node_t *n);

enum { DP_BLOCK, DP_INLINE, DP_INLINE_BLOCK, DP_NONE, DP_FLEX, DP_GRID };

/* ---------- checks ---------- */
typedef struct finding {
    int major;
    const char *check;      /* static id string */
    char *sel;              /* element selector path */
    char *reason;
    int line;
    const char *file;
} finding_t;

void checks_run(layout_t *L, const char *file, const char *emoji_allow,
                int no_emoji, vec_t *out);
finding_t *finding_new(int major, const char *check, const char *sel,
                       const char *reason, int line, const char *file);
void findings_free(vec_t *out);

#endif /* EXOQMS_H */
