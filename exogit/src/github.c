/* exogit: code-research operations against GitHub / GitLab. */
#include "exogit.h"

#include <ctype.h>

/* ---- shared small helpers ---- */

/* query param extractor (query is already URL-decoded by the caller for
 * console mode; for server mode we decode %XX here). */
static int qp(const char *query, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq)
            break;
        if ((size_t)(eq - p) == nlen && strncmp(p, name, nlen) == 0) {
            const char *v = eq + 1;
            const char *amp = strchr(v, '&');
            size_t vn = amp ? (size_t)(amp - v) : strlen(v);
            size_t w = 0;
            for (size_t i = 0; i < vn && w + 1 < cap; i++) {
                if (v[i] == '%' && i + 2 < vn && isxdigit((unsigned char)v[i+1])
                    && isxdigit((unsigned char)v[i+2])) {
                    char hb[3] = {v[i+1], v[i+2], 0};
                    out[w++] = (char)strtol(hb, NULL, 16);
                    i += 2;
                    continue;
                }
                if (v[i] == '+') out[w++] = ' ';
                else out[w++] = v[i];
            }
            out[w] = 0;
            return 1;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

/* minimal URL-encoder for building request URLs (space->%20, plus a few
 * reserved chars). Does NOT encode '/' (callers handle path slashes). */
static void url_encode(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; in[i] && w + 4 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            out[w++] = (char)c;
        } else if (c == ' ') {
            out[w++] = '%'; out[w++] = '2'; out[w++] = '0';
        } else {
            static const char *hex = "0123456789ABCDEF";
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0xF];
        }
    }
    out[w] = 0;
}

/* gitlab needs project paths with '/' -> '%2F' in the API URL */
static void url_encode_slash(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; in[i] && w + 4 < cap; i++) {
        if (in[i] == '/') {
            out[w++] = '%'; out[w++] = '2'; out[w++] = 'F';
        } else {
            char tmp[8];
            char one[2] = { in[i], 0 };
            url_encode(one, tmp, sizeof tmp);
            size_t k = 0;
            while (tmp[k] && w + 1 < cap) out[w++] = tmp[k++];
        }
    }
    out[w] = 0;
}

/* ---- repo ref parser ---- */

int parse_repo_ref(const char *raw, char *host, size_t hostcap, char *owner,
                   size_t ownercap, char *repo, size_t repocap, char *branch,
                   size_t branchcap, char *path, size_t pathcap,
                   int *is_gitlab)
{
    host[0] = owner[0] = repo[0] = branch[0] = path[0] = 0;
    *is_gitlab = 0;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", raw);
    /* strip scheme */
    if (!strncmp(buf, "https://", 8)) memmove(buf, buf + 8, strlen(buf + 8) + 1);
    else if (!strncmp(buf, "http://", 7)) memmove(buf, buf + 7, strlen(buf + 7) + 1);
    char *slash = strchr(buf, '/');
    if (!slash)
        return 0; /* need at least owner/repo */
    *slash = 0;
    char *firstseg = buf;
    char *rest = slash + 1;
    int has_host = (strchr(firstseg, '.') != NULL);
    if (has_host) {
        snprintf(host, hostcap, "%s", firstseg);
        if (strstr(host, "gitlab.com"))
            *is_gitlab = 1;
        char *o2 = strchr(rest, '/');
        if (!o2) {
            snprintf(owner, ownercap, "%s", rest);
            return 1;
        }
        size_t ol = (size_t)(o2 - rest);
        if (ol >= ownercap) ol = ownercap - 1;
        memcpy(owner, rest, ol);
        owner[ol] = 0;
        rest = o2 + 1;
    } else {
        /* shorthand: first segment is the owner */
        snprintf(owner, ownercap, "%s", firstseg);
    }
    /* now rest = repo[/verb/branch/path...] */
    char *r2 = strchr(rest, '/');
    if (!r2) {
        snprintf(repo, repocap, "%s", rest);
        return 1;
    }
    size_t rl = (size_t)(r2 - rest);
    if (rl >= repocap) rl = repocap - 1;
    memcpy(repo, rest, rl);
    repo[rl] = 0;
    rest = r2 + 1;
    /* strip verbs */
    if (!strncmp(rest, "blob/", 5)) rest += 5;
    else if (!strncmp(rest, "tree/", 5)) rest += 5;
    else if (!strncmp(rest, "raw/", 4)) rest += 4;
    else if (!strncmp(rest, "-/blob/", 7)) rest += 7;
    else if (!strncmp(rest, "-/raw/", 6)) rest += 6;
    else if (!strncmp(rest, "-/tree/", 7)) rest += 7;
    char *b2 = strchr(rest, '/');
    if (!b2) {
        snprintf(branch, branchcap, "%s", rest);
        return 1;
    }
    size_t bl = (size_t)(b2 - rest);
    if (bl >= branchcap) bl = branchcap - 1;
    memcpy(branch, rest, bl);
    branch[bl] = 0;
    rest = b2 + 1;
    snprintf(path, pathcap, "%s", rest);
    return 1;
}

/* ---- /file : fetch one raw source file, token-efficient ---- */

void exogit_file(dout_t *d, net_t *net, const char *query)
{
    char url[8192], branch[256], owner[256], repo[256], host[256], path[256];
    int is_gl = 0;
    char rawurl[16384] = {0};
    int have_url = qp(query, "url", url, sizeof url);
    if (have_url) {
        if (!parse_repo_ref(url, host, sizeof host, owner, sizeof owner,
                            repo, sizeof repo, branch, sizeof branch, path,
                            sizeof path, &is_gl)) {
            /* maybe it's already a raw URL */
            if (strstr(url, "raw.githubusercontent.com") ||
                strstr(url, "/raw/") || strstr(url, "gitlabstatic") ||
                strstr(url, "://")) {
                snprintf(rawurl, sizeof rawurl, "%s", url);
                is_gl = strstr(url, "gitlab.com") != NULL;
            } else {
                d->status = 400;
                buf_puts(&d->body, "error: cannot parse repo url\n");
                return;
            }
        }
    } else {
        if (!qp(query, "repo", repo, sizeof repo) ||
            !qp(query, "path", path, sizeof path)) {
            d->status = 400;
            buf_puts(&d->body,
                     "error: need url=<repo url> or repo=<o/r> & path=<file>\n");
            return;
        }
        /* repo is "<owner>/<name>" (or just "<name>" if owner supplied
         * separately); split the owner out so the raw URL is well-formed. */
        char *slash = strchr(repo, '/');
        if (slash) {
            size_t ol = (size_t)(slash - repo);
            if (ol >= sizeof owner) ol = sizeof owner - 1;
            memcpy(owner, repo, ol);
            owner[ol] = 0;
            memmove(repo, slash + 1, strlen(slash + 1) + 1);
        }
        qp(query, "branch", branch, sizeof branch);
        is_gl = strstr(repo, "gitlab") != NULL;
    }
    if (!rawurl[0]) {
        if (!branch[0])
            snprintf(branch, sizeof branch, "main");
        char encpath[4096];
        url_encode(path, encpath, sizeof encpath);
        if (is_gl) {
            char encr[4096];
            url_encode_slash(repo, encr, sizeof encr);
            snprintf(rawurl, sizeof rawurl,
                     "https://gitlab.com/api/v4/projects/%s/repository/files/%s/raw?ref=%s",
                     encr, encpath, branch);
        } else {
            snprintf(rawurl, sizeof rawurl,
                     "https://raw.githubusercontent.com/%s/%s/%s/%s",
                     owner, repo, branch, path);
        }
    }
    char mx[32];
    long max = 60000;
    if (qp(query, "max", mx, sizeof mx)) {
        long m = strtol(mx, NULL, 10);
        if (m > 0) max = m;
    }
    int linenos = 1;
    char ln[8];
    if (qp(query, "linenos", ln, sizeof ln) && ln[0] == '0')
        linenos = 0;

    resp_t r;
    char err[256];
    if (net_fetch(net, rawurl, &r, err, sizeof err) != 0 || r.status != 200 ||
        !r.body) {
        d->status = 502;
        buf_printf(&d->body, "error: fetch failed (%s)%s\n",
                   err[0] ? err : "no body",
                   is_gl ? "" : " (branch may not be main; pass branch=)");
        resp_free(&r);
        return;
    }
    /* token-efficient output: line numbers, truncate AT max bytes (no
     * overshoot beyond the cap) */
    size_t shown = 0;
    unsigned long lnno = 0;
    const char *p = r.body;
    const char *end = r.body + r.blen;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);
        size_t overhead = (linenos ? 7 : 0) + 1;
        if (shown + llen + overhead > (size_t)max)
            break;
        lnno++;
        if (linenos)
            buf_printf(&d->body, "%5lu  ", lnno);
        buf_putsn(&d->body, p, llen);
        buf_puts(&d->body, "\n");
        shown += llen + overhead;
        p = eol ? eol + 1 : end;
    }
    if (p < end)
        buf_printf(&d->body, "... (%lu more lines truncated at %ld bytes)\n",
                   (unsigned long)(end - p), max);
    resp_free(&r);
}

/* ---- default-branch resolution (so repo-mapping needs no branch arg) ---- */

static void resolve_default_branch(net_t *net, const char *repo, int is_gl,
                                   char *branch, size_t bcap)
{
    if (branch[0])
        return;
    char api[16384];
    if (is_gl) {
        char encr[4096];
        url_encode_slash(repo, encr, sizeof encr);
        snprintf(api, sizeof api, "https://gitlab.com/api/v4/projects/%s",
                 encr);
    } else {
        snprintf(api, sizeof api, "https://api.github.com/repos/%s", repo);
    }
    resp_t r;
    char err[256];
    if (net_fetch(net, api, &r, err, sizeof err) == 0 && r.status == 200 &&
        r.body) {
        char db[256];
        if (json_str_in(r.body, 0, r.blen, "default_branch", db,
                        sizeof db) &&
            db[0])
            snprintf(branch, bcap, "%s", db);
        resp_free(&r);
    }
    /* on any failure branch stays empty -> caller falls back to "main" */
}

/* ---- /repo : list a repository tree ---- */

void exogit_repo(dout_t *d, net_t *net, const char *query)
{
    char repo[1024], branch[256], owner[256], host[256], path[256];
    int is_gl = 0;
    if (!qp(query, "repo", repo, sizeof repo) &&
        !qp(query, "url", repo, sizeof repo)) {
        d->status = 400;
        buf_puts(&d->body, "error: need repo=<o/r> or url=<\n");
        return;
    }
    if (strstr(repo, "/") && strstr(repo, ".")) {
        /* looks like a URL */
        parse_repo_ref(repo, host, sizeof host, owner, sizeof owner, repo,
                       sizeof repo, branch, sizeof branch, path, sizeof path,
                       &is_gl);
    }
    qp(query, "branch", branch, sizeof branch);
    resolve_default_branch(net, repo, is_gl, branch, sizeof branch);
    if (!branch[0])
        snprintf(branch, sizeof branch, "main");
    char api[16384];
    if (is_gl) {
        char encr[4096];
        url_encode_slash(repo, encr, sizeof encr);
        char encp[4096];
        url_encode(path, encp, sizeof encp);
        snprintf(api, sizeof api,
                 "https://gitlab.com/api/v4/projects/%s/repository/tree?ref=%s&path=%s&per_page=100",
                 encr, branch, encp);
    } else {
        char encp[4096];
        url_encode(path, encp, sizeof encp);
        snprintf(api, sizeof api,
                 "https://api.github.com/repos/%s/contents/%s?ref=%s",
                 repo, encp, branch);
    }
    resp_t r;
    char err[256];
    if (net_fetch(net, api, &r, err, sizeof err) != 0 || r.status != 200 ||
        !r.body) {
        d->status = 502;
        buf_printf(&d->body, "error: repo listing failed (%s)\n",
                   err[0] ? err : "no body");
        resp_free(&r);
        return;
    }
    /* array of objects with name + type (+ path). print compact. */
    size_t cur = 0, s, e;
    int n = 0;
    while (json_array_object(r.body, &cur, &s, &e)) {
        char name[1024], type[64];
        if (!json_str_in(r.body, s, e, "name", name, sizeof name))
            name[0] = 0;
        if (!json_str_in(r.body, s, e, "type", type, sizeof type))
            type[0] = 0;
        buf_printf(&d->body, "%-6s %s\n", type, name);
        n++;
    }
    if (n == 0)
        buf_printf(&d->body, "(empty or rate-limited; status %d)\n", r.status);
    resp_free(&r);
}

/* ---- /tree : full repository file listing (GitHub recursive git tree) ----
 * One request yields the entire repo's file inventory — the map an agent
 * needs before deciding which files to /file or /commits. Recursive beats
 * the paginated contents API (which caps at 100 per dir). */

void exogit_tree(dout_t *d, net_t *net, const char *query)
{
    char repo[1024], branch[256], owner[256], host[256], path[256];
    int is_gl = 0;
    if (!qp(query, "repo", repo, sizeof repo) &&
        !qp(query, "url", repo, sizeof repo)) {
        d->status = 400;
        buf_puts(&d->body, "error: need repo=<o/r> or url=<\n");
        return;
    }
    if (strstr(repo, "/") && strstr(repo, "."))
        parse_repo_ref(repo, host, sizeof host, owner, sizeof owner, repo,
                       sizeof repo, branch, sizeof branch, path, sizeof path,
                       &is_gl);
    qp(query, "branch", branch, sizeof branch);
    resolve_default_branch(net, repo, is_gl, branch, sizeof branch);
    if (!branch[0])
        snprintf(branch, sizeof branch, "main");

    char api[16384];
    if (is_gl) {
        char encr[4096];
        url_encode_slash(repo, encr, sizeof encr);
        snprintf(api, sizeof api,
                 "https://gitlab.com/api/v4/projects/%s/repository/tree?"
                 "ref=%s&recursive=true&per_page=100",
                 encr, branch);
    } else {
        snprintf(api, sizeof api,
                 "https://api.github.com/repos/%s/git/trees/%s?recursive=1",
                 repo, branch);
    }
    resp_t r;
    char err[256];
    if (net_fetch(net, api, &r, err, sizeof err) != 0 || r.status != 200 ||
        !r.body) {
        d->status = 502;
        buf_printf(&d->body, "error: tree failed (%s)\n",
                   err[0] ? err : "no body");
        resp_free(&r);
        return;
    }
    /* GitHub nests the array under "tree"; GitLab returns it top-level. */
    size_t ts = 0, te = r.blen;
    json_find_slice(r.body, 0, r.blen, "tree", &ts, &te);
    size_t cur = 0, s, e;
    int n = 0;
    while (json_array_object(r.body + ts, &cur, &s, &e)) {
        size_t as = ts + s, ae = ts + e;
        char name[2048], type[64], size[32];
        if (!json_str_in(r.body, as, ae, "path", name, sizeof name))
            json_str_in(r.body, as, ae, "name", name, sizeof name);
        if (!json_str_in(r.body, as, ae, "type", type, sizeof type))
            type[0] = 0;
        json_str_in(r.body, as, ae, "size", size, sizeof size);
        buf_printf(&d->body, "%-5s %-10s %s\n", type, size, name);
        n++;
    }
    if (n == 0)
        buf_printf(&d->body, "(empty or rate-limited; status %d)\n", r.status);
    char trunc[16];
    if (json_str_in(r.body, 0, r.blen, "truncated", trunc, sizeof trunc) &&
        trunc[0] == 't')
        buf_puts(&d->body,
                 "(note: listing truncated by GitHub; narrow with path=)\n");
    resp_free(&r);
}

/* ---- diff context stripping (token efficiency) ---- */

static void strip_diff_context(const char *patch, buf_t *out)
{
    /* keep: hunk headers (@@), added/removed lines (+/-), and the file
     * header (diff --git / +++ / ---). Drop unchanged context lines. */
    const char *p = patch;
    const char *end = patch + strlen(patch);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (llen == 0) { p = eol ? eol + 1 : end; continue; }
        char c = p[0];
        int keep = (c == '+' || c == '-' || c == '@');
        if (!keep && llen >= 3 && strncmp(p, "diff", 4) == 0) keep = 1;
        if (!keep && llen >= 3 && strncmp(p, "+++", 3) == 0) keep = 1;
        if (!keep && llen >= 3 && strncmp(p, "---", 3) == 0) keep = 1;
        if (keep)
            buf_putsn(out, p, llen + 1); /* include newline */
        p = eol ? eol + 1 : end;
    }
}

/* ---- /commits : the design-evolution extractor (flagship) ---- */

void exogit_commits(dout_t *d, net_t *net, const char *query)
{
    char repo[1024], branch[256], owner[256], host[256], path[256];
    int is_gl = 0;
    if (!qp(query, "repo", repo, sizeof repo) &&
        !qp(query, "url", repo, sizeof repo)) {
        d->status = 400;
        buf_puts(&d->body, "error: need repo=<o/r> or url=<\n");
        return;
    }
    if (strstr(repo, "/") && strstr(repo, ".")) {
        parse_repo_ref(repo, host, sizeof host, owner, sizeof owner, repo,
                       sizeof repo, branch, sizeof branch, path, sizeof path,
                       &is_gl);
    }
    if (!qp(query, "path", path, sizeof path)) {
        d->status = 400;
        buf_puts(&d->body, "error: need path=<file> (the file to trace)\n");
        return;
    }
    qp(query, "branch", branch, sizeof branch);
    if (!branch[0])
        snprintf(branch, sizeof branch, "main");
    char nbuf[16];
    int n = 10;
    if (qp(query, "n", nbuf, sizeof nbuf)) {
        n = atoi(nbuf);
        if (n < 1) n = 1;
        if (n > 30) n = 30;
    }

    /* 1) list commits touching this file */
    char listapi[16384];
    if (is_gl) {
        char encr[4096];
        url_encode_slash(repo, encr, sizeof encr);
        char encp[4096];
        url_encode(path, encp, sizeof encp);
        snprintf(listapi, sizeof listapi,
                 "https://gitlab.com/api/v4/projects/%s/repository/commits?path=%s&ref_name=%s&per_page=%d",
                 encr, encp, branch, n);
    } else {
        char encp[4096];
        url_encode(path, encp, sizeof encp);
        snprintf(listapi, sizeof listapi,
                 "https://api.github.com/repos/%s/commits?path=%s&per_page=%d",
                 repo, encp, n);
    }
    resp_t list;
    char err[256];
    if (net_fetch(net, listapi, &list, err, sizeof err) != 0 ||
        list.status != 200 || !list.body) {
        d->status = 502;
        buf_printf(&d->body,
                   "error: commit list failed (%s). For GitHub, set "
                   "EXOGIT_TOKEN (unauthenticated is rate-limited to 60/hr).\n",
                   err[0] ? err : "no body");
        resp_free(&list);
        return;
    }

    size_t cur = 0, s, e;
    int count = 0;
    while (json_array_object(list.body, &cur, &s, &e)) {
        char sha[64], msg[2048];
        /* github: sha at top, message nested under "commit".
         * gitlab: id at top, message at top. */
        if (!json_str_in(list.body, s, e, "sha", sha, sizeof sha))
            json_str_in(list.body, s, e, "id", sha, sizeof sha);
        if (!json_str_in(list.body, s, e, "message", msg, sizeof msg)) {
            size_t cs, ce;
            if (json_find_slice(list.body, s, e, "commit", &cs, &ce))
                json_str_in(list.body, cs, ce, "message", msg, sizeof msg);
        }
        /* trim message to first line for the header */
        char *nl = strchr(msg, '\n');
        if (nl) *nl = 0;

        buf_printf(&d->body, "\n### commit %s\n%s\n", sha, msg);

        /* 2) fetch the commit detail to get the diff for THIS file */
        char detail[16384];
        if (is_gl) {
            char encr[4096];
            url_encode_slash(repo, encr, sizeof encr);
            snprintf(detail, sizeof detail,
                     "https://gitlab.com/api/v4/projects/%s/repository/commits/%s/diff",
                     encr, sha);
        } else {
            snprintf(detail, sizeof detail,
                     "https://api.github.com/repos/%s/commits/%s", repo, sha);
        }
        resp_t det;
        char derr[256];
        if (net_fetch(net, detail, &det, derr, sizeof derr) == 0 &&
            det.status == 200 && det.body) {
            /* the files array lives inside the detail object */
            size_t fs, fe;
            if (json_find_slice(det.body, 0, det.blen, "files", &fs, &fe) ||
                json_find_slice(det.body, 0, det.blen, "diffs", &fs, &fe)) {
                size_t dc = 0, ds, de;
                int found = 0;
                while (json_array_object(det.body + fs, &dc, &ds, &de)) {
                    /* ds/de are offsets within det.body+fs; make absolute */
                    size_t abs = fs + ds;
                    size_t abe = fs + de;
                    char fname[2048], patch[1 << 16], status[32];
                    json_str_in(det.body, abs, abe, "filename", fname,
                                sizeof fname);
                    if (!fname[0])
                        json_str_in(det.body, abs, abe, "new_path", fname,
                                    sizeof fname);
                    json_str_in(det.body, abs, abe, "patch", patch,
                                sizeof patch);
                    json_str_in(det.body, abs, abe, "status", status,
                                sizeof status);
                    /* match this file */
                    const char *base = strrchr(path, '/');
                    base = base ? base + 1 : path;
                    const char *fbase = strrchr(fname, '/');
                    fbase = fbase ? fbase + 1 : fname;
                    if (strcmp(fname, path) == 0 ||
                        strcmp(fbase, base) == 0) {
                        found = 1;
                        buf_printf(&d->body, "  [%s] %s\n", status, fname);
                        if (patch[0]) {
                            buf_puts(&d->body,
                                     "  --- changed lines only (context stripped) ---\n");
                            strip_diff_context(patch, &d->body);
                            buf_puts(&d->body, "\n");
                        } else {
                            buf_puts(&d->body,
                                     "  (binary or no textual patch)\n");
                        }
                        break;
                    }
                }
                if (!found)
                    buf_puts(&d->body,
                             "  (file not in this commit's patch set)\n");
            } else {
                buf_puts(&d->body, "  (no diff available)\n");
            }
            resp_free(&det);
        } else {
            buf_printf(&d->body, "  (detail fetch failed: %s)\n",
                       derr[0] ? derr : "no body");
            resp_free(&det);
        }
        count++;
    }
    if (count == 0)
        buf_printf(&d->body, "(no commits found; status %d)\n", list.status);
    resp_free(&list);
}

/* ---- /search : find code on GitHub / GitLab ---- */

void exogit_search(dout_t *d, net_t *net, const char *query)
{
    char q[4096];
    if (!qp(query, "q", q, sizeof q) || !q[0]) {
        d->status = 400;
        buf_puts(&d->body, "error: need q=<search terms>\n");
        return;
    }
    char lang[64];
    if (!qp(query, "lang", lang, sizeof lang))
        lang[0] = 0;
    int n = 10;
    char nbuf[16];
    if (qp(query, "n", nbuf, sizeof nbuf)) {
        n = atoi(nbuf);
        if (n < 1) n = 1;
        if (n > 50) n = 50;
    }
    char encq[4096];
    url_encode(q, encq, sizeof encq);
    char encLang[512];
    if (lang[0]) {
        char tl[256];
        url_encode(lang, tl, sizeof tl);
        snprintf(encLang, sizeof encLang, "+language:%s", tl);
    } else encLang[0] = 0;

    if (net->token && net->token[0]) {
        /* authenticated API search (clean, structured) */
        char api[16384];
        snprintf(api, sizeof api,
                 "https://api.github.com/search/code?q=%s%s&per_page=%d",
                 encq, encLang, n);
        resp_t r;
        char err[256];
        if (net_fetch(net, api, &r, err, sizeof err) != 0 ||
            r.status != 200 || !r.body) {
            d->status = 502;
            buf_printf(&d->body,
                       "error: search failed (%s). GitHub code search needs "
                       "EXOGIT_TOKEN.\n", err[0] ? err : "no body");
            resp_free(&r);
            return;
        }
        size_t cur = 0, s, e;
        int cnt = 0;
        while (json_array_object(r.body, &cur, &s, &e)) {
            char full[1024], pth[2048], url[2048];
            json_str_in(r.body, s, e, "full_name", full, sizeof full);
            json_str_in(r.body, s, e, "path", pth, sizeof pth);
            json_str_in(r.body, s, e, "html_url", url, sizeof url);
            buf_printf(&d->body, "%s :: %s\n    %s\n", full, pth, url);
            cnt++;
        }
        if (cnt == 0)
            buf_printf(&d->body, "(no results; status %d)\n", r.status);
        resp_free(&r);
        return;
    }

    /* unauthenticated: scrape the web search page, best-effort */
    char web[16384];
    snprintf(web, sizeof web,
             "https://github.com/search?q=%s%s&type=code&n=%d", encq, encLang,
             n);
    resp_t r;
    char err[256];
    if (net_fetch(net, web, &r, err, sizeof err) != 0 || !r.body) {
        d->status = 502;
        buf_puts(&d->body,
                 "error: web search failed; set EXOGIT_TOKEN for API search\n");
        resp_free(&r);
        return;
    }
    /* extract /blob/ file links and result snippets */
    const char *html = r.body;
    size_t hlen = r.blen;
    int hits = 0;
    for (size_t i = 0; i + 6 < hlen; i++) {
        if (strncmp(html + i, "href=\"", 6) == 0) {
            size_t js = i + 6;
            size_t je = js;
            while (je < hlen && html[je] != '"') je++;
            size_t l = je - js;
            if (l > 6 && strncmp(html + js, "/", 1) == 0 &&
                (strstr(html + js, "/blob/") || strstr(html + js, "::"))) {
                /* print the path portion */
                buf_printf(&d->body, "%.*s\n", (int)l, html + js);
                hits++;
            }
            i = je;
        }
    }
    /* also dump stripped snippets (search-result-file-snippet) */
    for (size_t i = 0; i + 30 < hlen; i++) {
        if (strncmp(html + i, "search-result-file-snippet", 26) == 0) {
            /* find the following > ... </p> */
            size_t p = i + 26;
            while (p < hlen && html[p] != '>') p++;
            if (p >= hlen) break;
            p++;
            size_t pe = p;
            while (pe < hlen && strncmp(html + pe, "</p>", 4) != 0) pe++;
            char txt[4096];
            size_t tl = html_to_text(html + p, pe - p, txt, sizeof txt);
            if (tl > 0) {
                buf_printf(&d->body, "    %.*s\n", (int)tl, txt);
                hits++;
            }
            i = pe;
        }
    }
    if (hits == 0)
        buf_puts(&d->body,
                 "(no parsed hits; GitHub may require a token — set "
                 "EXOGIT_TOKEN.)\n");
    resp_free(&r);
}
