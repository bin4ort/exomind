/* exodoc main: CLI, audit orchestration, exomind dogfooding. */
#include "exodoc.h"
#include "../../common/exo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int audit_run(const char *manifest, const char *base,
                     const char *exomind, const char *outfile, int live,
                     int json);
static int console_run(const char *spec);

static void usage(void)
{
    printf("exodoc v%s — the documentation auditor for the AI-native stack\n"
           "usage: exodoc audit [--stack <manifest>] [--base <dir>]\n"
           "                    [--exomind http://127.0.0.1:7654]\n"
           "                    [--out <file>] [--live] [--json]\n"
           "  --stack   stack manifest (default docs/stack.tsv)\n"
           "  --base    base dir for component dirs (default .)\n"
           "  --exomind write exodoc:audit:* keys + note to this exomind\n"
           "  --out     also write the report to this file\n"
           "  --live    crawl daemons, verify version + API conformance\n"
           "  --json    machine-readable JSON output\n"
           "operation (same console grammar as the stack's daemons):\n"
           "  exodoc /audit?live=1&stack=...&base=...&exomind=...&out=...\n"
           "           one audit run; values literal, & separates params,\n"
           "           flags accept off values 0/false/no/off\n",
           EXODOC_VERSION);
}

/* parse http://host[:port]; returns 0 on success */
int exo_parse_url(const char *url, char *host, size_t hostsz, int *port)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0)
        return -1;
    p += 7;
    size_t hl = 0;
    while (p[hl] && p[hl] != ':' && p[hl] != '/')
        hl++;
    if (hl == 0 || hl >= hostsz)
        return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    p += hl;
    *port = 7654;
    if (*p == ':') {
        char tmp[16];
        p++;
        size_t i = 0;
        while (p[i] && p[i] != '/' && i < sizeof tmp - 1) {
            tmp[i] = p[i];
            i++;
        }
        tmp[i] = 0;
        *port = atoi(tmp);
        if (*port <= 0 || *port > 65535)
            return -1;
    }
    return 0;
}

/* store key/value in exomind via JSON body (avoids form parsing) */
int exo_persist(const char *url, const char *key, const char *value,
                char *err, size_t errsz)
{
    char host[128];
    int port;
    if (exo_parse_url(url, host, sizeof host, &port) != 0) {
        snprintf(err, errsz, "bad exomind url");
        return -1;
    }
    char *ke = json_escape(key, strlen(key));
    char *ve = json_escape(value, strlen(value));
    char target[512];
    snprintf(target, sizeof target, "/set?key=%s", key);
    buf_t body = {0};
    buf_printf(&body, "{\"key\":\"%s\",\"value\":\"%s\",\"ttl\":0}", ke, ve);
    free(ke);
    free(ve);
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    int rc = http_post_json(host, port, target, body.p, body.len, &resp,
                            &rlen, &status, err, errsz);
    buf_free(&body);
    if (rc != 0)
        return -1;
    free(resp);
    if (status != 200) {
        snprintf(err, errsz, "exomind set %s failed (status %d)", key, status);
        return -1;
    }
    return 0;
}

int exo_note(const char *url, const char *text, char *err, size_t errsz)
{
    char host[128];
    int port;
    if (exo_parse_url(url, host, sizeof host, &port) != 0) {
        snprintf(err, errsz, "bad exomind url");
        return -1;
    }
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    int rc = http_post_json(host, port, "/note", text, strlen(text), &resp,
                            &rlen, &status, err, errsz);
    if (rc != 0)
        return -1;
    free(resp);
    if (status != 200) {
        snprintf(err, errsz, "exomind note failed (status %d)", status);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("exodoc v%s\n", EXODOC_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    if (argc >= 2 && argv[1][0] == '/')
        return console_run(argv[1]);
    if (strcmp(argv[1], "audit") != 0) {
        fprintf(stderr, "exodoc: unknown command '%s'\n", argv[1]);
        usage();
        return 2;
    }
    const char *manifest = "docs/stack.tsv";
    const char *base = ".";
    const char *exomind = NULL;
    const char *outfile = NULL;
    int live = 0, json = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--live") == 0)
            live = 1;
        else if (strcmp(a, "--json") == 0)
            json = 1;
        else if (strcmp(a, "--stack") == 0 || strcmp(a, "--base") == 0 ||
                 strcmp(a, "--exomind") == 0 || strcmp(a, "--out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "exodoc: %s needs a value\n", a);
                return 2;
            }
            const char *v = argv[++i];
            if (strcmp(a, "--stack") == 0)
                manifest = v;
            else if (strcmp(a, "--base") == 0)
                base = v;
            else if (strcmp(a, "--exomind") == 0)
                exomind = v;
            else
                outfile = v;
        } else {
            fprintf(stderr, "exodoc: unknown option '%s'\n", a);
            usage();
            return 2;
        }
    }

    return audit_run(manifest, base, exomind, outfile, live, json);
}

/* one audit run with resolved options; shared by the `audit` subcommand
 * and the /audit console operation (same checks, same exit codes) */
static int audit_run(const char *manifest, const char *base,
                     const char *exomind, const char *outfile, int live,
                     int json)
{
    stack st = {0};
    snprintf(st.manifest_path, sizeof st.manifest_path, "%s", manifest);
    snprintf(st.base, sizeof st.base, "%s", base);
    if (manifest_parse(manifest, &st) != 0) {
        fprintf(stderr, "exodoc: error: manifest not found: %s\n", manifest);
        return 1;
    }
    if (st.n == 0) {
        fprintf(stderr, "exodoc: warning: manifest %s has no components\n",
                manifest);
    }
    audit_components(&st, live);

    if (json)
        report_json(&st, live, stdout);
    else
        report_human(&st, live, stdout);
    if (outfile) {
        FILE *f = fopen(outfile, "w");
        if (f) {
            if (json)
                report_json(&st, live, f);
            else
                report_human(&st, live, f);
            fclose(f);
        } else {
            fprintf(stderr, "exodoc: warning: cannot write %s: %s\n", outfile,
                    strerror(errno));
        }
    }

    if (exomind) {
        int p, fa, s;
        int sc = stack_totals(&st, &p, &fa, &s);
        long ts = (long)time(NULL);
        char err[256];
        for (size_t i = 0; i < st.n; i++) {
            comp_t *c = &st.comps[i];
            char key[128];
            snprintf(key, sizeof key, "exodoc:audit:%ld:%s", ts, c->name);
            buf_t v = {0};
            buf_printf(&v, "{\"score\":%d,\"pass\":%d,\"fail\":%d,"
                           "\"skip\":%d}",
                       c->score, c->pass, c->fail, c->skip);
            if (exo_persist(exomind, key, v.p, err, sizeof err) != 0)
                fprintf(stderr, "exodoc: warning: %s\n", err);
            buf_free(&v);
        }
        char note[512];
        snprintf(note, sizeof note,
                 "exodoc audit %ld (%s): %d pass, %d fail, %d skip, score "
                 "%d%% (manifest %s, base %s)",
                 ts, live ? "live" : "doc", p, fa, s, sc, manifest, base);
        if (exo_note(exomind, note, err, sizeof err) != 0)
            fprintf(stderr, "exodoc: warning: %s\n", err);
    }
    return 0;
}

static int flag_is_on(const char *v)
{
    return !(!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") ||
             !strcmp(v, "off"));
}

/* console-mode operation: /audit?live=1&stack=...&base=...&exomind=...&
 * out=...&json=1 — the /op?k=v console grammar shared by the whole stack,
 * mapped onto the audit subcommand. Exit codes = the subcommand's. */
static int console_run(const char *spec)
{
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", spec);
    char *q = strchr(buf, '?');
    if (q)
        *q = 0;
    if (strcmp(buf, "/audit") != 0) {
        fprintf(stderr, "exodoc: unknown operation %s\n", buf);
        usage();
        return 2;
    }
    const char *manifest = "docs/stack.tsv";
    const char *base = ".";
    const char *exomind = NULL;
    const char *outfile = NULL;
    int live = 0, json = 0;
    if (q) {
        for (char *kv = strtok(q + 1, "&"); kv; kv = strtok(NULL, "&")) {
            char *eq = strchr(kv, '=');
            if (eq)
                *eq = 0;
            const char *k = kv;
            const char *v = eq ? eq + 1 : "1";
            if (!strcmp(k, "live"))
                live = flag_is_on(v);
            else if (!strcmp(k, "json"))
                json = flag_is_on(v);
            else if (!strcmp(k, "stack"))
                manifest = v;
            else if (!strcmp(k, "base"))
                base = v;
            else if (!strcmp(k, "exomind"))
                exomind = v;
            else if (!strcmp(k, "out"))
                outfile = v;
            else {
                fprintf(stderr, "exodoc: /audit: unknown parameter %s\n", k);
                usage();
                return 2;
            }
        }
    }
    return audit_run(manifest, base, exomind, outfile, live, json);
}
