#include "update.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef EXO_REPO_DIR_DEFAULT
#define EXO_REPO_DIR_DEFAULT ""
#endif

#define EXO_CMD_CAP 8192

static int sh(const char *fmt, ...)
{
    char buf[EXO_CMD_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int rc = system(buf);
    if (rc == -1)
        return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int sh_out(const char *fmt, char *out, size_t outsz, ...)
{
    char buf[EXO_CMD_CAP];
    va_list ap;
    va_start(ap, outsz);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FILE *p = popen(buf, "r");
    if (!p)
        return -1;
    size_t n = fread(out, 1, outsz - 1, p);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = 0;
    out[n] = 0;
    int rc = pclose(p);
    return rc == 0 ? 0 : -1;
}

static const char *sq(const char *in, char *buf, size_t bufsz)
{
    size_t i = 0, j = 0;
    if (!in)
        return "";
    while (in[i] && j + 1 < bufsz) {
        if (in[i] != '\'')
            buf[j++] = in[i];
        i++;
    }
    buf[j] = 0;
    return buf;
}

static const char *update_src(void)
{
    const char *d = getenv("EXO_UPDATE_DIR");
    return (d && *d) ? d : EXO_REPO_DIR_DEFAULT;
}

static const char *update_branch(const char *sqdir, char *buf, size_t bufsz)
{
    const char *b = getenv("EXO_UPDATE_BRANCH");
    if (b && *b) {
        snprintf(buf, bufsz, "%s", b);
        return buf;
    }
    if (sh_out("git -C '%s' symbolic-ref --short HEAD 2>/dev/null", buf,
               bufsz, sqdir) != 0 || !buf[0])
        return NULL;
    return buf;
}

int exo_update_available(char *info, size_t infosz)
{
    const char *src = update_src();
    if (!src[0])
        return -1;
    char sqdir[4096];
    sq(src, sqdir, sizeof sqdir);
    char branch[256];
    if (!update_branch(sqdir, branch, sizeof branch))
        return -1;
    if (sh("timeout 8 git -C '%s' fetch --quiet origin %s >/dev/null 2>&1",
           sqdir, branch) != 0)
        return -1;
    char local[128], remote[128];
    if (sh_out("git -C '%s' rev-parse --short HEAD 2>/dev/null", local,
               sizeof local, sqdir) != 0 ||
        sh_out("git -C '%s' rev-parse --short origin/%s 2>/dev/null", remote,
               sizeof remote, sqdir, branch) != 0)
        return -1;
    if (!strcmp(local, remote))
        return 0;
    char n[64];
    if (sh_out("git -C '%s' rev-list --count HEAD..origin/%s 2>/dev/null", n,
               sizeof n, sqdir, branch) != 0)
        return -1;
    snprintf(info, infosz, "%s %s %s %s", local, remote, n, branch);
    return 1;
}

static int update_prefix(char *buf, size_t bufsz, const char *argv0);

void exo_update_banner(const char *argv0)
{
    const char *stop = getenv("EXO_UPDATE_CHECK");
    if (stop && (!strcmp(stop, "0") || !strcmp(stop, "off")
                 || !strcmp(stop, "no")))
        return;
    char info[512];
    if (exo_update_available(info, sizeof info) != 1)
        return;
    char from[64], to[64], cnt[64], br[128];
    if (sscanf(info, "%63s %63s %63s %127s", from, to, cnt, br) != 4)
        return;
    if (!strcmp(from, to))
        return;
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    char prefix[4096];
    if (update_prefix(prefix, sizeof prefix, argv0) != 0)
        snprintf(prefix, sizeof prefix, "<install prefix>");
    fprintf(stderr,
            "exomind-server v%s: update available (git %s -> %s, %s "
            "commit%s behind origin/%s)\n"
            "  run '%s --update' to fetch, rebuild and reinstall into "
            "%s/bin\n"
            "  (EXO_UPDATE_CHECK=0 silences this notice)\n",
            EXOMIND_VERSION, from, to, cnt, strcmp(cnt, "1") ? "s" : "", br,
            base, prefix);
}

static int update_prefix(char *buf, size_t bufsz, const char *argv0)
{
    const char *p = getenv("EXO_UPDATE_PREFIX");
    if (p && *p) {
        snprintf(buf, bufsz, "%s", p);
        return 0;
    }
    char rp[4096];
    if (!realpath(argv0, rp))
        return -1;
    char *slash = strrchr(rp, '/');
    if (!slash)
        return -1;
    *slash = 0;
    slash = strrchr(rp, '/');
    if (!slash)
        return -1;
    *slash = 0;
    snprintf(buf, bufsz, "%s", rp);
    return 0;
}

int exo_update_self(const char *argv0)
{
    const char *src = update_src();
    char sqdir[4096];
    if (!src[0]) {
        fprintf(stderr,
                "exomind: error: no update source dir (build default is "
                "empty) — set EXO_UPDATE_DIR=/path/to/exomind\n");
        return 1;
    }
    sq(src, sqdir, sizeof sqdir);
    char probe[64];
    if (sh_out("git -C '%s' rev-parse --git-dir >/dev/null 2>&1", probe,
               sizeof probe, sqdir) != 0) {
        fprintf(stderr,
                "exomind: error: %s is not a git working tree\n", src);
        return 1;
    }
    char branch[256];
    if (!update_branch(sqdir, branch, sizeof branch)) {
        fprintf(stderr,
                "exomind: error: %s is not a git working tree\n", src);
        return 1;
    }
    char old[128];
    if (sh_out("git -C '%s' rev-parse --short HEAD 2>/dev/null", old,
               sizeof old, sqdir) != 0) {
        fprintf(stderr, "exomind: error: cannot read HEAD in %s\n", src);
        return 1;
    }
    if (sh("timeout 30 git -C '%s' fetch --quiet origin %s >/dev/null 2>&1",
           sqdir, branch) != 0) {
        fprintf(stderr,
                "exomind: error: git fetch failed (network/remote) — "
                "EXO_UPDATE_BRANCH=%s to override the tracked branch\n",
                branch);
        return 1;
    }
    char remote[128];
    if (sh_out("git -C '%s' rev-parse --short origin/%s 2>/dev/null", remote,
               sizeof remote, sqdir, branch) != 0 || !remote[0]) {
        fprintf(stderr, "exomind: error: no origin/%s on the remote\n",
                branch);
        return 1;
    }
    const char *bin = strrchr(argv0, '/');
    bin = bin ? bin + 1 : argv0;
    if (!strcmp(old, remote)) {
        printf("exomind: up to date (v%s at %s)\n", EXOMIND_VERSION, old);
        return 0;
    }
    char ahead[64];
    if (sh_out("git -C '%s' rev-list --count origin/%s..HEAD 2>/dev/null",
               ahead, sizeof ahead, sqdir, branch) == 0 && ahead[0] &&
        strcmp(ahead, "0")) {
        printf("exomind: local %s is %s commit%s ahead of origin/%s — "
               "nothing to pull\n",
               branch, ahead, strcmp(ahead, "1") ? "s" : "", branch);
        return 0;
    }
    char cnt[64];
    if (sh_out("git -C '%s' rev-list --count HEAD..origin/%s 2>/dev/null",
               cnt, sizeof cnt, sqdir, branch) != 0)
        snprintf(cnt, sizeof cnt, "?");
    if (sh("git -C '%s' pull --ff-only --quiet origin %s >/dev/null 2>&1",
           sqdir, branch) != 0) {
        fprintf(stderr,
                "exomind: error: git pull failed (uncommitted local "
                "changes?) — resolve in %s and re-run\n",
                src);
        return 1;
    }
    if (sh("make -C '%s' -j%d 1>&2", sqdir,
           (int)sysconf(_SC_NPROCESSORS_ONLN)) != 0) {
        fprintf(stderr, "exomind: error: build failed in %s\n", src);
        return 1;
    }
    char prefix[4096];
    if (update_prefix(prefix, sizeof prefix, argv0) != 0) {
        fprintf(stderr,
                "exomind: error: cannot derive install prefix from %s — "
                "set EXO_UPDATE_PREFIX\n",
                argv0);
        return 1;
    }
    char sqp[4096];
    sq(prefix, sqp, sizeof sqp);
    if (sh("make -C '%s' install PREFIX='%s' 1>&2", sqdir, sqp) != 0) {
        fprintf(stderr,
                "exomind: error: install into %s/bin failed (permission "
                "denied?) — set EXO_UPDATE_PREFIX to a writable prefix, "
                "e.g. ~/.local\n",
                prefix);
        return 1;
    }
    printf("exomind: updated %s -> %s (%s commit%s, v%s)\n", old, remote,
           cnt, strcmp(cnt, "1") ? "s" : "", EXOMIND_VERSION);
    printf("exomind: binaries installed into %s/bin\n", prefix);
    printf("exomind: restart '%s' to apply (hint: systemctl --user restart "
           "exo-exomind)\n",
           bin);
    return 0;
}