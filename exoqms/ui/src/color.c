/* color.c — color parsing (hex/rgb/rgba/hsl/named/transparent), relative
 * luminance and WCAG contrast ratio.
 */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int named_color(const char *s, color_t *c)
{
    static const struct { const char *name; int r, g, b; } named[] = {
        { "black", 0, 0, 0 }, { "silver", 192, 192, 192 },
        { "gray", 128, 128, 128 }, { "grey", 128, 128, 128 },
        { "white", 255, 255, 255 }, { "maroon", 128, 0, 0 },
        { "red", 255, 0, 0 }, { "purple", 128, 0, 128 },
        { "fuchsia", 255, 0, 255 }, { "magenta", 255, 0, 255 },
        { "green", 0, 128, 0 }, { "lime", 0, 255, 0 },
        { "olive", 128, 128, 0 }, { "yellow", 255, 255, 0 },
        { "navy", 0, 0, 128 }, { "blue", 0, 0, 255 },
        { "teal", 0, 128, 128 }, { "aqua", 0, 255, 255 },
        { "cyan", 0, 255, 255 }, { "orange", 255, 165, 0 },
        { "pink", 255, 192, 203 }, { "brown", 165, 42, 42 },
        { "gold", 255, 215, 0 }, { "indigo", 75, 0, 130 },
        { "ivory", 255, 255, 240 }, { "khaki", 240, 230, 140 },
        { "lavender", 230, 230, 250 }, { "salmon", 250, 128, 114 },
        { "tan", 210, 180, 140 }, { "tomato", 255, 99, 71 },
        { "violet", 238, 130, 238 }, { "wheat", 245, 222, 179 },
        { "coral", 255, 127, 80 }, { "crimson", 220, 20, 60 },
        { "darkgray", 169, 169, 169 }, { "darkgrey", 169, 169, 169 },
        { "darkslategray", 47, 79, 79 }, { "lightgray", 211, 211, 211 },
        { "lightgrey", 211, 211, 211 }, { "whitesmoke", 245, 245, 245 },
    };
    size_t i;
    for (i = 0; i < sizeof named / sizeof named[0]; i++) {
        if (ci_eq(s, named[i].name)) {
            c->r = named[i].r;
            c->g = named[i].g;
            c->b = named[i].b;
            c->a = 255;
            c->valid = 1;
            return 1;
        }
    }
    return 0;
}

static int parse_hex(const char *s, color_t *c)
{
    size_t n = strlen(s);
    int i;
    if (s[0] != '#')
        return 0;
    if (n == 4) {           /* #rgb */
        for (i = 0; i < 3; i++) {
            int h = hexval((unsigned char)s[1 + i]);
            if (h < 0)
                return 0;
            c->r = h * 17; c->g = h * 17; c->b = h * 17;
        }
        c->a = 255;
        c->valid = 1;
        return 1;
    }
    if (n == 5) {           /* #rgba */
        for (i = 0; i < 4; i++) {
            int h = hexval((unsigned char)s[1 + i]);
            if (h < 0)
                return 0;
            if (i == 0) c->r = h * 17;
            else if (i == 1) c->g = h * 17;
            else if (i == 2) c->b = h * 17;
            else c->a = h * 17;
        }
        c->valid = 1;
        return 1;
    }
    if (n == 7) {           /* #rrggbb */
        for (i = 0; i < 3; i++) {
            int h1 = hexval((unsigned char)s[1 + i * 2]);
            int h2 = hexval((unsigned char)s[2 + i * 2]);
            if (h1 < 0 || h2 < 0)
                return 0;
            if (i == 0) c->r = h1 * 16 + h2;
            else if (i == 1) c->g = h1 * 16 + h2;
            else c->b = h1 * 16 + h2;
        }
        c->a = 255;
        c->valid = 1;
        return 1;
    }
    if (n == 9) {           /* #rrggbbaa */
        for (i = 0; i < 4; i++) {
            int h1 = hexval((unsigned char)s[1 + i * 2]);
            int h2 = hexval((unsigned char)s[2 + i * 2]);
            if (h1 < 0 || h2 < 0)
                return 0;
            if (i == 0) c->r = h1 * 16 + h2;
            else if (i == 1) c->g = h1 * 16 + h2;
            else if (i == 2) c->b = h1 * 16 + h2;
            else c->a = h1 * 16 + h2;
        }
        c->valid = 1;
        return 1;
    }
    return 0;
}

static int parse_rgb_fn(const char *s, color_t *c)
{
    const char *p = s;
    int comps[4] = {0};
    int ncomp = 0;
    if (ci_eq(p, "rgba") && p[4] == '(')
        p += 5;
    else if (ci_eq(p, "rgb") && p[3] == '(')
        p += 4;
    else
        return 0;
    while (*p && *p != ')') {
        double v = 0;
        if (ascii_digit((unsigned char)*p) || *p == '.' ||
            (*p == '-' && ascii_digit((unsigned char)p[1]))) {
            int neg = 0;
            if (*p == '-') { neg = 1; p++; }
            while (ascii_digit((unsigned char)*p)) {
                v = v * 10 + (*p - '0');
                p++;
            }
            if (*p == '.') {
                double f = 0.1;
                p++;
                while (ascii_digit((unsigned char)*p)) {
                    v += (*p - '0') * f;
                    f /= 10;
                    p++;
                }
            }
            if (neg)
                v = -v;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '%') {
                p++;
                v = v * 255.0 / 100.0;
            }
            if (ncomp < 4)
                comps[ncomp] = (int)(v + 0.5);
            ncomp++;
        } else if (*p == ',' || *p == '/' || *p == ' ' || *p == '\t') {
            p++;
        } else {
            return 0;
        }
    }
    if (ncomp < 3)
        return 0;
    c->r = comps[0];
    c->g = comps[1];
    c->b = comps[2];
    c->a = ncomp >= 4 ? comps[3] : 255;
    if (c->r > 255) c->r = 255;
    if (c->g > 255) c->g = 255;
    if (c->b > 255) c->b = 255;
    if (c->a > 255) c->a = 255;
    c->valid = 1;
    return 1;
}

static int parse_hsl_fn(const char *s, color_t *c)
{
    /* approximate conversion; hsl() in real pages is rare in audit context */
    double h, s1, l;
    double r, g, b, t1, t2, hh;
    const char *p;
    if (ci_eq(s, "hsl") && s[3] == '(')
        p = s + 4;
    else
        return 0;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;
    if (!parse_num(p, &h))
        return 0;
    while (*p && *p != ',' && *p != 'd')
        p++;
    if (*p == 'd')
        p += 2;                       /* skip "deg" */
    while (*p && (*p == ',' || *p == ' ' || *p == '\t'))
        p++;
    if (!parse_num(p, &s1))
        return 0;
    while (*p && *p != ',' && *p != '%')
        p++;
    if (*p == '%')
        p++;
    while (*p && (*p == ',' || *p == ' ' || *p == '\t'))
        p++;
    if (!parse_num(p, &l))
        return 0;
    s1 = s1 > 1 ? s1 / 100.0 : s1;
    l = l > 1 ? l / 100.0 : l;
    if (s1 <= 0) {
        r = g = b = l * 255.0;
    } else {
        t2 = l < 0.5 ? l * (1 + s1) : l + s1 - l * s1;
        t1 = 2 * l - t2;
        hh = h / 360.0;
        {
            double hue[3];
            int i;
            hue[0] = hh + 1.0 / 3.0;
            hue[1] = hh;
            hue[2] = hh - 1.0 / 3.0;
            for (i = 0; i < 3; i++) {
                double v;
                if (hue[i] < 0) hue[i] += 1;
                if (hue[i] > 1) hue[i] -= 1;
                if (6 * hue[i] < 1) v = t1 + (t2 - t1) * 6 * hue[i];
                else if (2 * hue[i] < 1) v = t2;
                else if (3 * hue[i] < 2) v = t1 + (t2 - t1) * (2.0 / 3.0 - hue[i]) * 6;
                else v = t1;
                if (i == 0) r = v * 255.0;
                else if (i == 1) g = v * 255.0;
                else b = v * 255.0;
            }
        }
    }
    c->r = (int)(r + 0.5);
    c->g = (int)(g + 0.5);
    c->b = (int)(b + 0.5);
    c->a = 255;
    c->valid = 1;
    return 1;
}

color_t color_parse(const char *s)
{
    color_t c = {0, 0, 0, 255, 0};
    char buf[64];
    if (!s)
        return c;
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    str_trim(buf);
    if (ci_eq(buf, "transparent")) {
        c.a = 0;
        c.valid = 1;
        return c;
    }
    if (parse_hex(buf, &c))
        return c;
    if (parse_rgb_fn(buf, &c))
        return c;
    if (parse_hsl_fn(buf, &c))
        return c;
    if (named_color(buf, &c))
        return c;
    return c;
}

static double chan(double v)
{
    v /= 255.0;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) * ((v + 0.055) / 1.055);
}

double color_lum(const color_t *c)
{
    return 0.2126 * chan((double)c->r) + 0.7152 * chan((double)c->g) +
           0.0722 * chan((double)c->b);
}

double color_ratio(const color_t *a, const color_t *b)
{
    double la = color_lum(a), lb = color_lum(b);
    double hi = la > lb ? la : lb;
    double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

void color_hex(const color_t *c, char out[16])
{
    snprintf(out, 16, "#%02x%02x%02x", c->r, c->g, c->b);
}

int color_eq(const color_t *a, const color_t *b)
{
    return a->valid && b->valid && a->r == b->r && a->g == b->g &&
           a->b == b->b && a->a == b->a;
}

color_t color_composite(color_t fg, color_t bg)
{
    double fa = fg.a / 255.0;
    color_t out;
    out.valid = 1;
    out.r = (int)(fg.r * fa + bg.r * (1 - fa) + 0.5);
    out.g = (int)(fg.g * fa + bg.g * (1 - fa) + 0.5);
    out.b = (int)(fg.b * fa + bg.b * (1 - fa) + 0.5);
    out.a = 255;
    return out;
}
