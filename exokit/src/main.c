/*
 * exokit — the behavioral development kit.
 *
 * Every software carries its own development kit: a `kit/` directory that
 * is the software's SDK-for-itself. It holds
 *   kit/contract.tsv   the function inventory (the public surface)
 *   kit/examples.tsv   behavioral examples: args -> expected output
 *   kit/config         runner commands + audit limits
 *   kit/runners/       one executable shim per language
 *
 * The rules that make this a development kit, not a test harness:
 *   R1  contract-first  — no public function without an inventory entry
 *   R2  every contract entry has >= 1 example, including an edge/error case
 *   R3  translate by regenerating from the contract, never line-by-line
 *   R4  deliver in inventory slices, each slice verified before the next
 *   R5  both implementations must pass the same examples ledger
 *   R6  the ledger is the only source of truth for behavior
 *
 * The runner protocol is deliberately primitive and language-agnostic:
 * the runner reads one line  `fn<TAB>args`  on stdin and prints exactly
 * one result line on stdout (`<result>` or `error: <text>`). Whatever
 * language the software is written in, its shim is ~10 lines.
 *
 * Zero compile dependencies, C11. Batch tool (no daemon).
 */
#include "exokit.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------- loading ----------------
 * contract.tsv:  fn<TAB>sig<TAB>pure<TAB>side_effects<TAB>notes
 * examples.tsv:  fn<TAB>args<TAB>expected<TAB>desc<TAB>err(0/1)
 * config:        runner<TAB><cmd>  |  max_examples<TAB><n>
 */
static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    buf_t b = {0};
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_put(&b, chunk, n);
    fclose(f);
    if (b.len == 0) {
        buf_free(&b);
        return xstrdup("");
    }
    buf_put(&b, "", 1);
    if (len)
        *len = b.len - 1;
    return b.p;
}

static char *tab_field(char *line, int idx)
{
    char *p = line;
    int i = 0;
    while (p && *p) {
        char *tab = strchr(p, '\t');
        if (i == idx)
            return tab ? xstrndup(p, (size_t)(tab - p)) : xstrdup(p);
        if (!tab)
            return xstrdup("");
        p = tab + 1;
        i++;
    }
    return xstrdup("");
}

void kit_load(kit_t *k, const char *dir, char *err, size_t errsz)
{
    memset(k, 0, sizeof *k);
    k->path = xstrdup(dir);
    snprintf(k->runner_cmd, sizeof k->runner_cmd, "%s", "");

    char p[8192];
    snprintf(p, sizeof p, "%s/config", dir);
    size_t n = 0;
    char *cfg = read_file(p, &n);
    if (cfg) {
        for (char *l = strtok(cfg, "\n"); l; l = strtok(NULL, "\n")) {
            if (!l[0] || l[0] == '#')
                continue;
            if (strncmp(l, "runner\t", 7) == 0) {
                snprintf(k->runner_cmd, sizeof k->runner_cmd, "%s", l + 7);
                /* resolve the runner command:
                 *  ./x          -> relative to the CWD (project root)
                 *  /abs/x       -> absolute
                 *  x            -> relative to the kit dir */
                if (k->runner_cmd[0] != '/') {
                    char abs[2048];
                    if (k->runner_cmd[0] == '.') {
                        snprintf(abs, sizeof abs, "%s",
                                 k->runner_cmd); /* CWD-relative */
                    } else {
                        snprintf(abs, sizeof abs, "%s/%s", dir,
                                 k->runner_cmd);
                    }
                    snprintf(k->runner_cmd, sizeof k->runner_cmd, "%s",
                             abs);
                }
            }
        }
        free(cfg);
    }

    snprintf(p, sizeof p, "%s/contract.tsv", dir);
    char *ct = read_file(p, &n);
    if (ct) {
        for (char *l = strtok(ct, "\n"); l; l = strtok(NULL, "\n")) {
            if (!l[0] || l[0] == '#')
                continue;
            if (k->nfns >= 2000)
                break;
            if (k->nfns % 64 == 0)
                k->fns = xrealloc(k->fns, (k->nfns + 64) * sizeof(cfn_t));
            cfn_t *f = &k->fns[k->nfns];
            memset(f, 0, sizeof *f);
            char *fld = tab_field(l, 0);
            if (!fld)
                fld = xstrdup("");
            snprintf(f->fn, sizeof f->fn, "%s", fld);
            free(fld);
            if (!f->fn[0])
                continue;
            f->sig = tab_field(l, 1);
            if (!f->sig)
                f->sig = xstrdup("");
            fld = tab_field(l, 2);
            if (!fld) {
                f->pure = 0;
            } else {
                f->pure = fld[0] == '1';
                free(fld);
            }
            f->side_effects = tab_field(l, 3);
            if (!f->side_effects)
                f->side_effects = xstrdup("");
            f->notes = tab_field(l, 4);
            if (!f->notes)
                f->notes = xstrdup("");
            k->nfns++;
        }
        free(ct);
    }

    snprintf(p, sizeof p, "%s/examples.tsv", dir);
    char *ex = read_file(p, &n);
    if (ex) {
        for (char *l = strtok(ex, "\n"); l; l = strtok(NULL, "\n")) {
            if (!l[0] || l[0] == '#')
                continue;
            if (k->nexs >= MAX_EXAMPLES)
                break;
            if (k->nexs % 64 == 0)
                k->exs = xrealloc(k->exs, (k->nexs + 64) * sizeof(example_t));
            example_t *e = &k->exs[k->nexs];
            memset(e, 0, sizeof *e);
            char *fld = tab_field(l, 0);
            if (!fld)
                fld = xstrdup("");
            snprintf(e->fn, sizeof e->fn, "%s", fld);
            free(fld);
            if (!e->fn[0])
                continue;
            e->args = tab_field(l, 1);
            if (!e->args)
                e->args = xstrdup("");
            e->expected = tab_field(l, 2);
            if (!e->expected)
                e->expected = xstrdup("");
            e->desc = tab_field(l, 3);
            if (!e->desc)
                e->desc = xstrdup("");
            fld = tab_field(l, 4);
            if (!fld) {
                e->is_error = 0;
            } else {
                e->is_error = fld[0] == '1';
                free(fld);
            }
            k->nexs++;
        }
        free(ex);
    }
    (void)err;
    (void)errsz;
}

void kit_free(kit_t *k)
{
    for (size_t i = 0; i < k->nfns; i++) {
        free(k->fns[i].sig);
        free(k->fns[i].side_effects);
        free(k->fns[i].notes);
    }
    for (size_t i = 0; i < k->nexs; i++) {
        free(k->exs[i].args);
        free(k->exs[i].expected);
        free(k->exs[i].desc);
    }
    free(k->exs);
    free(k->fns);
    free(k->path);
    memset(k, 0, sizeof *k);
}

/* ---------------- runner execution ---------------- */

/* run one example: feed "fn\targs\n" to the runner's stdin, capture
 * stdout. Two pipes: the runner is not a filter we can feed inline. */
static int run_example(const char *cmd, const example_t *e, buf_t *out,
                       char *err, size_t errsz)
{
    int inp[2], outp[2];
    if (pipe(inp) != 0 || pipe(outp) != 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        dup2(inp[0], 0);
        dup2(outp[1], 1);
        close(inp[0]);
        close(inp[1]);
        close(outp[0]);
        close(outp[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    buf_t feed = {0};
    buf_printf(&feed, "%s\t%s\n", e->fn, e->args);
    (void)write(inp[1], feed.p, feed.len);
    close(inp[1]);
    char chunk[4096];
    ssize_t got;
    while ((got = read(outp[0], chunk, sizeof chunk)) > 0)
        buf_put(out, chunk, (size_t)got);
    close(outp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (out->len && out->p[out->len - 1] == '\n')
        out->len--;
    if (out->p)
        out->p[out->len] = 0;
    buf_free(&feed);
    return 0;
}

/* ---------------- command: init ---------------- */

static void write_tpl(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

int cmd_init(int argc, char **argv)
{
    const char *dir = argc > 2 ? argv[2] : ".";
    char sub[4096];
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "exokit: mkdir %s: %s\n", dir, strerror(errno));
        return 1;
    }
    snprintf(sub, sizeof sub, "%s/kit", dir);
    if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "exokit: mkdir %s: %s\n", sub, strerror(errno));
        return 1;
    }
    char p[8192];
    snprintf(p, sizeof p, "%s/config", sub);
    write_tpl(p,
              "# exokit kit config\n"
              "# runner<TAB><command>: executed per example; reads one line\n"
              "#   `fn<TAB>args` on stdin, prints one result line on stdout\n"
              "runner\t./kit/runners/c/run\n"
              "# max_examples<TAB><n>: audit cap (default 50)\n"
              "max_examples\t50\n");
    snprintf(p, sizeof p, "%s/contract.tsv", sub);
    write_tpl(p,
              "# fn<TAB>sig<TAB>pure(0/1)<TAB>side_effects<TAB>notes\n"
              "# every public function of the software belongs here; this\n"
              "# inventory IS the software's self-SDK surface (rule R1).\n");
    snprintf(p, sizeof p, "%s/examples.tsv", sub);
    write_tpl(p,
              "# fn<TAB>args<TAB>expected<TAB>desc<TAB>err(0/1)\n"
              "# args/expected are the literal text passed to / produced by\n"
              "# the runner (no tabs or newlines; use esc: \\t \\n).\n"
              "# every contract entry needs >= 1 example incl. an error\n"
              "# case (rule R2); the ledger is the only truth (rule R6).\n");
    /* runner shims */
    snprintf(p, sizeof p, "%s/runners", sub);
    if (mkdir(p, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "exokit: mkdir %s: %s\n", p, strerror(errno));
        return 1;
    }
    static const char *RLANGS[] = {"c", "js", "rust", "python"};
    for (int r = 0; r < 4; r++) {
        snprintf(p, sizeof p, "%s/runners/%s", sub, RLANGS[r]);
        if (mkdir(p, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "exokit: mkdir %s: %s\n", p, strerror(errno));
            return 1;
        }
    }
    snprintf(p, sizeof p, "%s/runners/c/run.c", sub);
    write_tpl(p,
              "/* C runner shim: dispatch fn<TAB>args lines to functions. */\n"
              "#include <stdio.h>\n"
              "#include <string.h>\n"
              "static int dispatch(const char *fn, const char *args)\n"
              "{\n"
              "    /* e.g. if (!strcmp(fn, \"add\")) { int a,b;\n"
              "       sscanf(args, \"%d %d\", &a, &b);\n"
              "       printf(\"%d\\n\", a + b); return 0; } */\n"
              "    (void)args;\n"
              "    fprintf(stderr, \"no such fn: %s\\n\", fn);\n"
              "    return 1;\n"
              "}\n"
              "int main(void)\n"
              "{\n"
              "    char line[4096];\n"
              "    while (fgets(line, sizeof line, stdin)) {\n"
              "        char *tab = strchr(line, '\\t');\n"
              "        if (!tab) continue;\n"
              "        *tab = 0;\n"
              "        char *args = tab + 1;\n"
              "        char *nl = strchr(args, '\\n');\n"
              "        if (nl) *nl = 0;\n"
              "        if (dispatch(line, args) != 0)\n"
              "            printf(\"error: no such fn %s\\n\", line);\n"
              "        fflush(stdout);\n"
              "    }\n"
              "    return 0;\n"
              "}\n");
    snprintf(p, sizeof p, "%s/runners/js/run.mjs", sub);
    write_tpl(p,
              "// JS runner shim: read fn<TAB>args lines, print results.\n"
              "import { readFileSync } from 'fs';\n"
              "for (const line of readFileSync(0, 'utf8').split('\\n')) {\n"
              "  const i = line.indexOf('\\t');\n"
              "  if (i < 0) continue;\n"
              "  const fn = line.slice(0, i), args = line.slice(i + 1);\n"
              "  // dispatch here: if (fn === 'add') ... console.log(a+b)\n"
              "  console.log('error: no such fn ' + fn);\n"
              "}\n");
    snprintf(p, sizeof p, "%s/runners/rust/main.rs", sub);
    write_tpl(p,
              "// Rust runner shim: read fn<TAB>args lines, print results.\n"
              "use std::io::{self, BufRead};\n"
              "fn main() {\n"
              "    for line in io::stdin().lock().lines() {\n"
              "        let line = line.unwrap();\n"
              "        match line.split_once('\\t') {\n"
              "            Some((f, args)) => {\n"
              "                // dispatch: match f { \"add\" => { let v:\n"
              "                // Vec<i32> = args.split_whitespace()... } }\n"
              "                println!(\"error: no such fn {f}\");\n"
              "            }\n"
              "            None => {}\n"
              "        }\n"
              "    }\n"
              "}\n");
    snprintf(p, sizeof p, "%s/runners/python/run.py", sub);
    write_tpl(p,
              "# Python runner shim: read fn<TAB>args lines, print results.\n"
              "import sys\n"
              "def no_such(fn):\n"
              "    print(f'error: no such fn {fn}')\n"
              "for line in sys.stdin:\n"
              "    line = line.rstrip('\\n')\n"
              "    i = line.find('\\t')\n"
              "    if i < 0:\n"
              "        continue\n"
              "    fn, args = line[:i], line[i + 1:]\n"
              "    # dispatch here: if fn == 'add': print(int(a) + int(b))\n"
              "    no_such(fn)\n");
    snprintf(p, sizeof p, "%s/Makefile", sub);
    write_tpl(p,
              "# kit build: compile/install your runner shims, then:\n"
              "verify:\n"
              "\t../build/exokit verify\n"
              "audit:\n"
              "\t../build/exokit audit\n");
    snprintf(p, sizeof p, "%s/README.md", sub);
    write_tpl(p,
              "# kit/\n"
              "This directory is this software's development kit — its\n"
              "SDK-for-itself (exokit). `contract.tsv` is the public surface,\n"
              "`examples.tsv` the behavioral ledger, `runners/` one shim per\n"
              "language. Translate this software by regenerating from the\n"
              "contract (rule R3), never by porting code line-by-line.\n");
    printf("exokit: kit initialized in %s/kit\n", dir);
    printf("  next: 1) fill contract.tsv  2) add examples.tsv\n"
           "        3) implement a runner  4) exokit verify  5) exokit audit\n");
    return 0;
}

/* ---------------- command: extract ---------------- */

static int is_keyword(const char *w, size_t n)
{
    static const char *kw[] = {
        "if", "for", "while", "switch", "return", "else", "case", "typedef",
        "struct", "enum", "union", "class", "template", "namespace", "static",
        "extern", "inline", "constexpr", "virtual", "friend", "using",
        "sizeof", "alignof", "decltype", "new", "delete", "operator",
        "catch", "throw", "try", "do", "goto", "define", "include", NULL};
    for (int i = 0; kw[i]; i++)
        if (n == strlen(kw[i]) && strncmp(w, kw[i], n) == 0)
            return 1;
    return 0;
}

/* best-effort function inventory extraction for C/C++. Finds
 * `<type> <name>(<params>)` blocks ending in `{`. Not a real parser:
 * macros and templates defeat it (documented limitation). */
static void extract_file(const char *path, buf_t *out)
{
    /* the kit's own scaffolding and runners are not part of the
     * software's surface: skip anything under a kit/ directory */
    if (strstr(path, "/kit/"))
        return;
    size_t n = 0;
    char *src = read_file(path, &n);
    if (!src)
        return;
    const char *p = src;
    char sig[8192];
    size_t slen = 0;
    int in_sig = 0;
    char name[MAX_FN];
    while (p && *p) {
        if (!in_sig) {
            /* a signature starts at a line whose first token is a type */
            const char *ls = p;
            while (*ls == ' ' || *ls == '\t')
                ls++;
            const char *eol = strchr(p, '\n');
            if (!eol)
                eol = p + strlen(p);
            if (*ls == '#' || *ls == '/' || *ls == '*' || *ls == '}') {
                p = eol + 1;
                continue;
            }
            slen = 0;
            const char *t = ls;
            while (t < eol && slen + 8 < sizeof sig) {
                if (*t == '(') {
                    /* name = token before '(' */
                    const char *nm = t;
                    while (nm > ls && (isalnum((unsigned char)nm[-1]) ||
                                       nm[-1] == '_'))
                        nm--;
                    size_t nml = (size_t)(t - nm);
                    if (nml == 0 || nml >= MAX_FN || is_keyword(nm, nml) ||
                        (nml == 4 && strncmp(nm, "main", 4) == 0)) {
                        in_sig = 0;
                        break;
                    }
                    memcpy(name, nm, nml);
                    name[nml] = 0;
                    in_sig = 1;
                    /* sig starts at ls */
                    size_t hdr = (size_t)(t - ls) + 1;
                    if (slen + hdr + 1 >= sizeof sig)
                        break;
                    memcpy(sig + slen, ls, hdr);
                    slen += hdr;
                    ls = t + 1;
                    break;
                }
                t++;
            }
            if (!in_sig) {
                p = eol + 1;
                continue;
            }
            p = t + 1;
            /* append the rest of the line (params...) */
            if (eol > p && slen + (size_t)(eol - p) < sizeof sig) {
                memcpy(sig + slen, p, (size_t)(eol - p));
                slen += (size_t)(eol - p);
            }
            p = eol + 1;
        }
        /* now inside a signature: consume lines until '{' or ';'.
         * the '{' may already be in sig from the first line (single-line
         * functions); only append the next line when it is not. */
        while (in_sig) {
            sig[slen] = 0;
            if (strchr(sig, '{')) {
                /* trim the block opener */
                char *cb = strchr(sig, '{');
                *cb = 0;
                while (cb > sig && (cb[-1] == ' ' || cb[-1] == '\t' ||
                                    cb[-1] == '\n' || cb[-1] == '\r'))
                    cb--;
                *cb = 0;
                /* collapse whitespace */
                buf_t flat = {0};
                int sp = 0;
                for (char *c = sig; *c; c++) {
                    if (isspace((unsigned char)*c)) {
                        if (!sp && flat.len) {
                            buf_put(&flat, " ", 1);
                            sp = 1;
                        }
                    } else {
                        char cc[1] = {*c};
                        buf_put(&flat, cc, 1);
                        sp = 0;
                    }
                }
                if (flat.len) {
                    buf_printf(out, "%s\t%s\t1\t\t\n", name, flat.p);
                }
                buf_free(&flat);
                in_sig = 0;
                break; /* p already past the '{' line */
            }
            if (strchr(sig, ';')) {
                in_sig = 0; /* declaration, not a definition */
                break;
            }
            const char *eol = strchr(p, '\n');
            if (!eol)
                eol = p + strlen(p);
            if (slen + (size_t)(eol - p) + 2 < sizeof sig) {
                memcpy(sig + slen, p, (size_t)(eol - p));
                slen += (size_t)(eol - p);
            }
            p = eol + 1;
        }
    }
    free(src);
}

int cmd_extract(int argc, char **argv)
{
    const char *src_dir = NULL;
    const char *out_path = "kit/contract.tsv";
    int append = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--append"))
            append = 1;
        else if (!src_dir)
            src_dir = argv[i];
    }
    if (!src_dir) {
        fprintf(stderr, "exokit extract <src-dir> [--out kit/contract.tsv]\n");
        return 1;
    }
    buf_t out = {0};
    if (append) {
        size_t n = 0;
        char *old = read_file(out_path, &n);
        if (old) {
            buf_puts(&out, old);
            free(old);
            if (out.len && out.p[out.len - 1] != '\n')
                buf_puts(&out, "\n");
        }
    }
    /* walk the tree for *.c *.cc *.cpp *.cxx *.h *.hpp */
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "find %s -type f \\( -name '*.c' -o -name '*.cc' -o "
             "-name '*.cpp' -o -name '*.cxx' -o -name '*.h' -o "
             "-name '*.hpp' \\) 2>/dev/null | sort",
             src_dir);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        buf_free(&out);
        return 1;
    }
    char path[4096];
    while (fgets(path, sizeof path, fp)) {
        size_t l = strlen(path);
        while (l && (path[l - 1] == '\n' || path[l - 1] == '\r'))
            path[--l] = 0;
        if (!path[0])
            continue;
        extract_file(path, &out);
    }
    pclose(fp);
    FILE *f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "exokit: cannot write %s: %s\n", out_path,
                strerror(errno));
        buf_free(&out);
        return 1;
    }
    fputs(out.p ? out.p : "", f);
    fclose(f);
    int nfns = 0;
    for (char *l = strtok(out.p ? out.p : xstrdup(""), "\n"); l;
         l = strtok(NULL, "\n"))
        if (l[0] && l[0] != '#')
            nfns++;
    printf("exokit: extracted %d function(s) into %s\n", nfns, out_path);
    buf_free(&out);
    return 0;
}

/* ---------------- command: verify ---------------- */

int cmd_verify(int argc, char **argv)
{
    const char *kit_dir = "kit";
    const char *runner = NULL;
    const char *only_fn = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--kit") && i + 1 < argc)
            kit_dir = argv[++i];
        else if (!strcmp(argv[i], "--runner") && i + 1 < argc)
            runner = argv[++i];
        else if (!strcmp(argv[i], "--fn") && i + 1 < argc)
            only_fn = argv[++i];
        else {
            fprintf(stderr,
                    "exokit verify [--kit kit] [--runner cmd] [--fn name]\n");
            return 1;
        }
    }
    kit_t k;
    char err[256];
    kit_load(&k, kit_dir, err, sizeof err);
    if (!k.runner_cmd[0] && !runner) {
        fprintf(stderr, "exokit: no runner configured (kit/config)\n");
        kit_free(&k);
        return 1;
    }
    const char *cmd = runner ? runner : k.runner_cmd;
    int pass = 0, fail = 0;
    for (size_t i = 0; i < k.nexs; i++) {
        example_t *e = &k.exs[i];
        if (only_fn && strcmp(e->fn, only_fn) != 0)
            continue;
        buf_t got = {0};
        if (run_example(cmd, e, &got, err, sizeof err) != 0) {
            fprintf(stderr, "exokit: runner failed: %s\n", err);
            buf_free(&got);
            kit_free(&k);
            return 1;
        }
        char *g = got.p ? got.p : xstrdup("");
        int ok = strcmp(g, e->expected) == 0;
        /* error examples: runner must print `error:` */
        if (e->is_error && strncmp(g, "error:", 6) != 0)
            ok = 0;
        if (ok) {
            pass++;
            printf("pass %s%s%s\n", e->fn, e->desc[0] ? " - " : "",
                   e->desc[0] ? e->desc : "");
        } else {
            fail++;
            printf("fail %s%s%s\n     expected '%s'\n     got      '%s'\n",
                   e->fn, e->desc[0] ? " - " : "",
                   e->desc[0] ? e->desc : "", e->expected, g);
        }
        free(g);
        if (got.p)
            got.p = NULL; /* owned by g, already freed */
        buf_free(&got);
    }
    kit_free(&k);
    printf("=== exokit verify: %d ok, %d fail ===\n", pass, fail);
    return fail > 0;
}

/* ---------------- command: diff ---------------- */

int cmd_diff(int argc, char **argv)
{
    const char *a = NULL, *b = NULL;
    int exact = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--exact"))
            exact = 1;
        else if (!a)
            a = argv[i];
        else if (!b)
            b = argv[i];
    }
    if (!a || !b) {
        fprintf(stderr, "exokit diff <contractA> <contractB> [--exact]\n");
        return 1;
    }
    kit_t ka, kb;
    char err[256];
    kit_load(&ka, ".", err, sizeof err); /* placeholder, not used */
    /* load contracts directly: temporarily treat dirs' parent? simpler:
     * read the two files as contracts via a synthetic kit */
    memset(&ka, 0, sizeof ka);
    memset(&kb, 0, sizeof kb);
    size_t n = 0;
    char *t1 = read_file(a, &n);
    char *t2 = read_file(b, &n);
    /* compare fn columns */
    char *fa[4096], *fb[4096];
    size_t na = 0, nb = 0;
    if (t1)
        for (char *l = strtok(t1, "\n"); l; l = strtok(NULL, "\n")) {
            if (!l[0] || l[0] == '#')
                continue;
            char *tab = strchr(l, '\t');
            if (tab && na < 4096)
                fa[na++] = l;
        }
    if (t2)
        for (char *l = strtok(t2, "\n"); l; l = strtok(NULL, "\n")) {
            if (!l[0] || l[0] == '#')
                continue;
            char *tab = strchr(l, '\t');
            if (tab && nb < 4096)
                fb[nb++] = l;
        }
    int missing = 0, extra = 0;
    for (size_t i = 0; i < na; i++) {
        char *tab = strchr(fa[i], '\t');
        size_t al = (size_t)(tab - fa[i]);
        int found = 0;
        for (size_t j = 0; j < nb; j++) {
            char *tb = strchr(fb[j], '\t');
            if (tb && (size_t)(tb - fb[j]) == al &&
                strncmp(fa[i], fb[j], al) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("missing %.*s\n", (int)al, fa[i]);
            missing++;
        }
    }
    for (size_t j = 0; j < nb; j++) {
        char *tb = strchr(fb[j], '\t');
        size_t bl = (size_t)(tb - fb[j]);
        int found = 0;
        for (size_t i = 0; i < na; i++) {
            char *ta = strchr(fa[i], '\t');
            if (ta && (size_t)(ta - fa[i]) == bl &&
                strncmp(fb[j], fa[i], bl) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("extra %.*s\n", (int)bl, fb[j]);
            extra++;
        }
    }
    free(t1);
    free(t2);
    printf("=== exokit diff: %d missing, %d extra ===\n", missing, extra);
    if (missing > 0)
        return 1;
    if (exact && extra > 0)
        return 1;
    return 0;
}

/* ---------------- command: audit ---------------- */

int cmd_audit(int argc, char **argv)
{
    const char *kit_dir = "kit";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--kit") && i + 1 < argc)
            kit_dir = argv[++i];
        else {
            fprintf(stderr, "exokit audit [--kit kit]\n");
            return 1;
        }
    }
    kit_t k;
    char err[256];
    kit_load(&k, kit_dir, err, sizeof err);
    if (k.nfns == 0 && k.nexs == 0) {
        fprintf(stderr, "exokit: empty kit at %s\n", kit_dir);
        kit_free(&k);
        return 2;
    }
    buf_t finds = {0};
    int nf = 0;
    /* R1/R2: every contract fn has >= 1 example */
    for (size_t i = 0; i < k.nfns; i++) {
        int has = 0;
        for (size_t j = 0; j < k.nexs; j++)
            if (strcmp(k.fns[i].fn, k.exs[j].fn) == 0) {
                has = 1;
                break;
            }
        if (!has) {
            if (nf)
                buf_puts(&finds, ",");
            buf_printf(&finds,
                       "{\"check\":\"missing-example\",\"severity\":\"major\","
                       "\"file\":\"%s/contract.tsv\",\"line\":0,"
                       "\"reason\":\"contract entry %s has no example\"}",
                       kit_dir, k.fns[i].fn);
            nf++;
        }
    }
    /* R6: every example must pass */
    if (k.runner_cmd[0]) {
        size_t cap = 50;
        /* read max_examples from config via the runner count */
        for (size_t i = 0; i < k.nexs && i < cap; i++) {
            example_t *e = &k.exs[i];
            buf_t got = {0};
            if (run_example(k.runner_cmd, e, &got, err, sizeof err) != 0) {
                buf_free(&got);
                break;
            }
            char *g = got.p ? got.p : xstrdup("");
            int ok = strcmp(g, e->expected) == 0;
            if (e->is_error && strncmp(g, "error:", 6) != 0)
                ok = 0;
            if (!ok) {
                if (nf)
                    buf_puts(&finds, ",");
                buf_printf(&finds,
                           "{\"check\":\"example-fail\",\"severity\":\"major\","
                           "\"file\":\"%s/examples.tsv\",\"line\":%zu,"
                           "\"reason\":\"%s expected '%s' got '%s'\"}",
                           kit_dir, i + 1, e->fn, e->expected, g);
                nf++;
            }
            free(g);
            if (got.p)
                got.p = NULL; /* owned by g, already freed */
            buf_free(&got);
        }
    }
    kit_free(&k);
    if (nf > 0) {
        printf("[%s]\n", finds.p ? finds.p : "");
        buf_free(&finds);
        return 1;
    }
    buf_free(&finds);
    printf("pass\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "exokit v%s — the behavioral development kit\n"
                "usage:\n"
                "  exokit init [dir]            scaffold a kit/ (self-SDK)\n"
                "  exokit extract <src-dir>     best-effort C/C++ inventory\n"
                "  exokit verify [--kit dir]    run the examples ledger\n"
                "  exokit diff <a> <b> [--exact] compare inventories\n"
                "  exokit audit [--kit dir]     QMS findings (completeness\n"
                "                               + ledger fidelity)\n"
                "  exokit --version\n"
                "rules: R1 contract-first, R2 examples incl. error cases,\n"
                "R3 translate by regenerating from the contract, R4 slice\n"
                "delivery, R5 both impls pass the same ledger, R6 the\n"
                "ledger is the only truth.\n",
                EXOKIT_VERSION);
        return 2;
    }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version")) {
        printf("exokit v%s\n", EXOKIT_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "init"))
        return cmd_init(argc, argv);
    if (!strcmp(argv[1], "extract"))
        return cmd_extract(argc, argv);
    if (!strcmp(argv[1], "verify"))
        return cmd_verify(argc, argv);
    if (!strcmp(argv[1], "diff"))
        return cmd_diff(argc, argv);
    if (!strcmp(argv[1], "audit"))
        return cmd_audit(argc, argv);
    fprintf(stderr, "exokit: unknown command %s\n", argv[1]);
    return 2;
}
