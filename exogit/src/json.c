/* exogit: minimal, dependency-free JSON helpers for parsing GitHub/GitLab
 * API responses (arrays of objects with nested fields). Not a general
 * parser — just enough to extract the strings we need reliably. */
#include "exogit.h"

#include <ctype.h>

/* match the '}' (or ']') balancing the brace/bracket at doc[open].
 * Returns its index, or (size_t)-1 on failure. Honors strings/escapes. */
static size_t match_close(const char *doc, size_t open)
{
    char openc = doc[open];
    char closec = (openc == '{') ? '}' : ']';
    int depth = 0;
    int in_str = 0, esc = 0;
    for (size_t i = open; doc[i]; i++) {
        char c = doc[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == openc) depth++;
        else if (c == closec) {
            depth--;
            if (depth == 0)
                return i;
        }
    }
    return (size_t)-1;
}

/* locate the value slice for `key` within [start,end). The value may be a
 * string, object, or array; *vs/*ve span it (including the quotes / braces).
 * Returns 1 on success. */
static int json_find_value(const char *doc, size_t start, size_t end,
                           const char *key, size_t *vs, size_t *ve)
{
    size_t klen = strlen(key);
    size_t i = start;
    while (i + klen + 2 <= end) {
        if (doc[i] == '"' && strncmp(doc + i + 1, key, klen) == 0 &&
            doc[i + 1 + klen] == '"') {
            /* require a ':' after the closing quote (skip spaces) */
            size_t p = i + 1 + klen + 1;
            while (p < end && (doc[p] == ' ' || doc[p] == '\t' ||
                               doc[p] == '\n' || doc[p] == '\r'))
                p++;
            if (p < end && doc[p] == ':') {
                /* skip spaces after ':' */
                size_t v = p + 1;
                while (v < end && (doc[v] == ' ' || doc[v] == '\t' ||
                                   doc[v] == '\n' || doc[v] == '\r'))
                    v++;
                if (v >= end)
                    return 0;
                if (doc[v] == '{' || doc[v] == '[') {
                    size_t close = match_close(doc, v);
                    if (close == (size_t)-1)
                        return 0;
                    *vs = v;
                    *ve = close + 1;
                    return 1;
                }
                if (doc[v] == '"') {
                    *vs = v;
                    *ve = v + 1;
                    return 1; /* caller extracts string body */
                }
                /* number/bool/null: take until , } ] */
                size_t e = v;
                while (e < end && doc[e] != ',' && doc[e] != '}' &&
                       doc[e] != ']')
                    e++;
                *vs = v;
                *ve = e;
                return 1;
            }
        }
        i++;
    }
    return 0;
}

/* decode a JSON string starting at doc[open] (which must be '"') into out.
 * Returns 1 on success, 0 if not a string. */
static int json_decode_str(const char *doc, size_t open, char *out, size_t cap)
{
    if (doc[open] != '"')
        return 0;
    size_t w = 0, i = open + 1;
    while (doc[i] && doc[i] != '"') {
        char c = doc[i];
        if (c == '\\') {
            char n = doc[i + 1];
            switch (n) {
                case 'n': c = '\n'; i += 2; break;
                case 't': c = '\t'; i += 2; break;
                case 'r': c = '\r'; i += 2; break;
                case 'b': c = '\b'; i += 2; break;
                case 'f': c = '\f'; i += 2; break;
                case '/': c = '/'; i += 2; break;
                case '\\': c = '\\'; i += 2; break;
                case '"': c = '"'; i += 2; break;
                case 'u': {
                    if (!isxdigit((unsigned char)doc[i+2]) ||
                        !isxdigit((unsigned char)doc[i+3]) ||
                        !isxdigit((unsigned char)doc[i+4]) ||
                        !isxdigit((unsigned char)doc[i+5])) { i += 2; break; }
                    unsigned long v = 0;
                    for (int k = 0; k < 4; k++)
                        v = v * 16 + (unsigned long)(doc[i+2+k] >= 'a'
                                ? doc[i+2+k] - 'a' + 10
                                : doc[i+2+k] >= 'A'
                                ? doc[i+2+k] - 'A' + 10
                                : doc[i+2+k] - '0');
                    i += 6;
                    if (v < 0x80) { if (w + 1 < cap) out[w++] = (char)v; }
                    else if (v < 0x800) {
                        if (w + 2 < cap) {
                            out[w++] = (char)(0xC0 | (v >> 6));
                            out[w++] = (char)(0x80 | (v & 0x3F));
                        }
                    } else {
                        if (w + 3 < cap) {
                            out[w++] = (char)(0xE0 | (v >> 12));
                            out[w++] = (char)(0x80 | ((v >> 6) & 0x3F));
                            out[w++] = (char)(0x80 | (v & 0x3F));
                        }
                    }
                    continue;
                }
                default: i += 2; break;
            }
            if (w + 1 < cap) out[w++] = c;
        } else {
            if (w + 1 < cap) out[w++] = c;
            i++;
        }
    }
    if (w + 1 < cap) out[w] = 0;
    else if (cap) out[cap - 1] = 0;
    return 1;
}

int json_str_in(const char *doc, size_t start, size_t end, const char *key,
                char *out, size_t cap)
{
    size_t vs, ve;
    if (!json_find_value(doc, start, end, key, &vs, &ve))
        return 0;
    if (doc[vs] != '"')
        return 0; /* value is not a string */
    return json_decode_str(doc, vs, out, cap);
}

int json_str(const char *doc, const char *key, char *out, size_t cap)
{
    return json_str_in(doc, 0, doc ? strlen(doc) : 0, key, out, cap);
}

int json_find_slice(const char *doc, size_t start, size_t end,
                    const char *key, size_t *s, size_t *e)
{
    return json_find_value(doc, start, end, key, s, e);
}

int json_array_object(const char *doc, size_t *cursor, size_t *start,
                      size_t *end)
{
    size_t i = *cursor;
    int in_str = 0, esc = 0, obj_depth = 0;
    for (; doc[i]; i++) {
        char c = doc[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') {
            int was = obj_depth;
            obj_depth++;
            /* a top-level array element is an object seen while not already
             * inside another object (was == 0). This holds whether we start
             * scanning at the '[' or resume after a previous element. */
            if (was == 0) {
                size_t close = match_close(doc, i);
                if (close == (size_t)-1)
                    return 0;
                *start = i;
                *end = close + 1;
                *cursor = close + 1;
                return 1;
            }
        } else if (c == '}') { if (obj_depth) obj_depth--; }
    }
    return 0;
}
