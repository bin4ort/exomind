/* exoqms-code: tokenizer (lexical, line/col tracked, preprocessor-aware). */
#include "code.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *KEYWORDS[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "_Bool", "bool", "ssize_t", "size_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "off_t", "FILE", "errno_t", "time_t", NULL};

static int is_keyword(const char *s, size_t n)
{
    for (int i = 0; KEYWORDS[i]; i++) {
        if (strlen(KEYWORDS[i]) == n && memcmp(KEYWORDS[i], s, n) == 0)
            return 1;
    }
    return 0;
}

void tokvec_init(tokvec_t *tv)
{
    tv->ntok = 0;
    tv->cap = 1024;
    tv->toks = calloc(tv->cap, sizeof(tok_t));
}

void tokvec_free(tokvec_t *tv)
{
    free(tv->toks);
    tv->toks = NULL;
    tv->ntok = 0;
    tv->cap = 0;
}

static void tok_push(tokvec_t *tv, tok_type t, const char *s, size_t n,
                     int line, int col)
{
    if (tv->ntok >= tv->cap) {
        tv->cap *= 2;
        tok_t *nw = realloc(tv->toks, tv->cap * sizeof(tok_t));
        if (!nw) {
            tv->ntok = 0;
            return;
        }
        tv->toks = nw;
    }
    tok_t *tk = &tv->toks[tv->ntok++];
    tk->type = t;
    tk->line = line;
    tk->col = col;
    tk->len = n > sizeof tk->text - 1 ? sizeof tk->text - 1 : n;
    memcpy(tk->text, s, tk->len);
    tk->text[tk->len] = 0;
}

/* bounded substring search (memmem is a GNU extension) */
static int has_nonnull(const char *p, size_t n)
{
    static const char NEEDLE[] = "@nonnull";
    if (n < sizeof NEEDLE - 1)
        return 0;
    for (size_t i = 0; i + sizeof NEEDLE - 1 <= n; i++) {
        if (memcmp(p + i, NEEDLE, sizeof NEEDLE - 1) == 0)
            return 1;
    }
    return 0;
}

int tokenize_file(const char *path, tokvec_t *tv)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    static const size_t CAP = 8u * 1024u * 1024u;
    char *buf = malloc(CAP + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, CAP, f);
    if (n == 0 && ferror(f)) {
        /* read error: not just EOF - refuse to tokenize garbage */
        free(buf);
        fclose(f);
        return -1;
    }
    buf[n] = 0;
    fclose(f);

    int line = 1, col = 1;
    size_t pos = 0;

    while (pos < n) {
        int sl = line, sc = col;
        /* preprocessor line */
        if (col == 1 && buf[pos] == '#') {
            while (pos < n && buf[pos] != '\n')
                pos++;
            if (pos < n) {
                pos++;
                line++;
                col = 1;
            }
            continue;
        }
        /* comments */
        if (buf[pos] == '/' && pos + 1 < n && buf[pos + 1] == '/') {
            size_t start = pos;
            while (pos < n && buf[pos] != '\n')
                pos++;
            if (has_nonnull(buf + start, pos - start)) {
                tok_push(tv, T_OP, "@nonnull", 8, sl, sc);
            }
            if (pos < n) {
                pos++;
                line++;
                col = 1;
            }
            continue;
        }
        if (buf[pos] == '/' && pos + 1 < n && buf[pos + 1] == '*') {
            size_t start = pos;
            pos += 2;
            while (pos + 1 < n && !(buf[pos] == '*' && buf[pos + 1] == '/')) {
                if (buf[pos] == '\n') {
                    line++;
                    col = 1;
                } else {
                    col++;
                }
                pos++;
            }
            if (has_nonnull(buf + start, pos - start)) {
                tok_push(tv, T_OP, "@nonnull", 8, sl, sc);
            }
            if (pos + 1 < n)
                pos += 2;
            continue;
        }
        if (isspace((unsigned char)buf[pos])) {
            if (buf[pos] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
            pos++;
            continue;
        }

        int ch = (unsigned char)buf[pos];

        if (isalpha(ch) || ch == '_') {
            size_t start = pos;
            while (pos < n && (isalnum((unsigned char)buf[pos]) ||
                               buf[pos] == '_'))
                pos++;
            size_t len = pos - start;
            col += (int)len;
            tok_type t = is_keyword(buf + start, len) ? T_KEYWORD : T_IDENT;
            tok_push(tv, t, buf + start, len, sl, sc);
            continue;
        }
        if (isdigit(ch) ||
            (ch == '.' && pos + 1 < n && isdigit((unsigned char)buf[pos + 1]))) {
            size_t start = pos;
            while (pos < n && (isalnum((unsigned char)buf[pos]) ||
                               buf[pos] == '.'))
                pos++;
            tok_push(tv, T_NUMBER, buf + start, pos - start, sl, sc);
            col += (int)(pos - start);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            int quote = ch;
            size_t start = pos;
            pos++;
            col++;
            while (pos < n && buf[pos] != quote) {
                if (buf[pos] == '\\' && pos + 1 < n)
                    pos++;
                pos++;
                col++;
            }
            if (pos < n) {
                pos++;
                col++;
            }
            tok_push(tv, quote == '"' ? T_STRING : T_CHAR, buf + start,
                     pos - start, sl, sc);
            continue;
        }

        /* operators / punctuation (2-char first) */
        static const char *OPS2[] = {"==", "!=", "<=", ">=", "&&", "||",
                                     "->", "::", "++", "--", "+=", "-=",
                                     "*=", "/=", "%=", "&=", "|=", "^=",
                                     "<<", ">>"};
        int matched = 0;
        for (size_t i = 0; i < sizeof OPS2 / sizeof OPS2[0]; i++) {
            size_t l = strlen(OPS2[i]);
            if (pos + l <= n && memcmp(buf + pos, OPS2[i], l) == 0) {
                tok_push(tv, T_OP, OPS2[i], l, sl, sc);
                pos += l;
                col += (int)l;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            char one[2] = {(char)ch, 0};
            tok_type t = T_OP;
            if (ch == '(' || ch == ')' || ch == '{' || ch == '}' ||
                ch == ';' || ch == ',' || ch == '[' || ch == ']')
                t = T_PUNCT;
            tok_push(tv, t, one, 1, sl, sc);
            pos++;
            col++;
        }
    }
    tok_push(tv, T_EOF, "", 0, line, col);
    free(buf);
    return 0;
}
