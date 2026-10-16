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

    /* read source version from repo for accurate banner */
    const char *src = update_src();
    char sver[64] = {0};
    char version_h[4096];
    snprintf(version_h, sizeof version_h, "%s/src/version.h", src);
    FILE *f = fopen(version_h, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "EXOMIND_VERSION")) {
                char *start = strchr(line, '"');
                if (start) {
                    start++;
                    char *end = strchr(start, '"');
                    if (end) {
                        *end = 0;
                        snprintf(sver, sizeof sver, "%s", start);
                    }
                }
                break;
            }
        }
        fclose(f);
    }
    if (!sver[0])
        snprintf(sver, sizeof sver, "%s", EXOMIND_VERSION);

    fprintf(stderr,
            "exomind-server v%s: update available (git %s -> %s, %s "
            "commit%s behind origin/%s)\n"
            "  run '%s --update' to fetch, rebuild and reinstall into "
            "%s/bin\n"
            "  (EXO_UPDATE_CHECK=0 silences this notice)\n",
            sver, from, to, cnt, strcmp(cnt, "1") ? "s" : "", br,
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
    if (!realpath(argv0, rp)) {
        /* argv0 often names a path that no longer exists (the binary was moved,
         * removed, or is only a contract fixture): keep the literal path so the
         * prefix stays derivable instead of failing on the host's filesystem. */
        snprintf(rp, sizeof rp, "%s", argv0);
    }
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

/* fallback prefix detection: try common locations */
static int update_prefix_fallback(char *buf, size_t bufsz)
{
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, bufsz, "%s/.local", home);
        return 0;
    }
    return -1;
}

static int cmp_version(const char *a, const char *b)
{
    if (!a || !b)
        return 1;
    int va[3] = {0}, vb[3] = {0};
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}

static int get_binary_version(char *buf, size_t bufsz)
{
    char rp[4096];
    ssize_t n = readlink("/proc/self/exe", rp, sizeof rp - 1);
    if (n <= 0)
        return -1;
    rp[n] = '\0';
    char cmd[EXO_CMD_CAP];
    snprintf(cmd, sizeof cmd,
             "strings '%s' 2>/dev/null | grep -oE 'exomind v?[0-9]+\\.[0-9]+\\.[0-9]+(-alpha\\.[0-9]+)?' | tail -1 | grep -oE '[0-9]+\\.[0-9]+\\.[0-9]+(-alpha\\.[0-9]+)?' | head -1",
             rp);
    char tmp[64];
    if (sh_out(cmd, tmp, sizeof tmp) != 0 || !tmp[0])
        return -1;
    snprintf(buf, bufsz, "%s", tmp);
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
        /* read source version from repo's version.h (not the running binary's) */
        char sver[64] = {0};
        char version_h[4096];
        snprintf(version_h, sizeof version_h, "%s/src/version.h", src);
        FILE *f = fopen(version_h, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "EXOMIND_VERSION")) {
                    char *start = strchr(line, '"');
                    if (start) {
                        start++;
                        char *end = strchr(start, '"');
                        if (end) {
                            *end = 0;
                            snprintf(sver, sizeof sver, "%s", start);
                        }
                    }
                    break;
                }
            }
            fclose(f);
        }
        if (!sver[0])
            snprintf(sver, sizeof sver, "%s", EXOMIND_VERSION); /* fallback */

        char bver[64] = {0};
        if (get_binary_version(bver, sizeof bver) == 0) {
            int vcmp = cmp_version(bver, sver);
            if (vcmp == 0) {
                printf("exomind: up to date (v%s at %s)\n", sver, old);
                return 0;
            }
            if (vcmp < 0) {
                printf("exomind: binary version %s < source v%s, rebuilding\n",
                       bver, sver);
            } else {
                printf("exomind: binary version %s != source v%s, rebuilding\n",
                       bver, sver);
            }
        } else {
            printf("exomind: cannot read binary version, rebuilding\n");
        }
    }
char ahead[64] = {0}, cnt[64] = {0};
    int ahead_cnt = 0;

    /* only stash if there are actual local changes (including untracked) */
    int stash_ok = 0;
    char stash_check[128];
    if (sh_out("git -C '%s' status --porcelain 2>/dev/null", stash_check,
               sizeof stash_check, sqdir) == 0 && stash_check[0]) {
        stash_ok = sh("git -C '%s' stash push --include-untracked -m exomind-update >/dev/null 2>&1",
                      sqdir) == 0;
    }

    if (sh_out("git -C '%s' rev-list --count origin/%s..HEAD 2>/dev/null",
               ahead, sizeof ahead, sqdir, branch) != 0)
        ahead[0] = '\0';
    ahead_cnt = ahead[0] ? atoi(ahead) : 0;
    if (sh_out("git -C '%s' rev-list --count HEAD..origin/%s 2>/dev/null",
               cnt, sizeof cnt, sqdir, branch) != 0)
        snprintf(cnt, sizeof cnt, "?");
    int pull_rc;
    if (ahead_cnt) {
        pull_rc = sh("git -C '%s' pull --rebase --quiet origin %s >/dev/null 2>&1",
                      sqdir, branch);
    } else {
        pull_rc = sh("git -C '%s' pull --ff-only --quiet origin %s >/dev/null 2>&1",
                      sqdir, branch);
    }
    if (stash_ok) {
        /* verify stash was actually created (not empty) */
        char stash_list[64];
        if (sh_out("git -C '%s' stash list 2>/dev/null | grep -c 'exomind-update'",
                   stash_list, sizeof stash_list, sqdir) == 0 &&
            stash_list[0] && atoi(stash_list) > 0) {
            if (sh("git -C '%s' stash pop --index >/dev/null 2>&1", sqdir) != 0) {
                fprintf(stderr,
                        "exomind: warning: could not restore stashed changes "
                        "(conflict during pop) — run 'git stash pop' in %s to recover\n",
                        src);
            }
        }
    }
    if (pull_rc != 0) {
        fprintf(stderr,
                "exomind: error: git pull failed (conflicts or "
                "uncommitted changes?) — resolve in %s and re-run\n",
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
        if (update_prefix_fallback(prefix, sizeof prefix) != 0) {
            fprintf(stderr,
                    "exomind: error: cannot derive install prefix from %s — "
                    "set EXO_UPDATE_PREFIX\n",
                    argv0);
            return 1;
        }
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

static const char *detect_pm(void)
{
    if (access("/usr/bin/pacman", X_OK) == 0)
        return "pacman";
    if (access("/usr/bin/apt-get", X_OK) == 0)
        return "apt";
    if (access("/usr/bin/yum", X_OK) == 0)
        return "yum";
    if (access("/usr/bin/dnf", X_OK) == 0)
        return "dnf";
    if (access("/usr/bin/paru", X_OK) == 0)
        return "paru";
    if (access("/usr/bin/yay", X_OK) == 0)
        return "yay";
    return NULL;
}

static int pm_install(const char *pm, const char *pkg)
{
    if (!strcmp(pm, "pacman"))
        return sh("sudo pacman -Sy --noconfirm %s 2>/dev/null", pkg);
    if (!strcmp(pm, "paru") || !strcmp(pm, "yay"))
        return sh("sudo %s -Sy --noconfirm %s 2>/dev/null", pm, pkg);
    if (!strcmp(pm, "apt") || !strcmp(pm, "apt-get"))
        return sh("sudo apt-get update -qq >/dev/null 2>&1 && sudo apt-get install -y %s 2>/dev/null",
                  pkg);
    if (!strcmp(pm, "yum"))
        return sh("sudo yum install -y %s 2>/dev/null", pkg);
    if (!strcmp(pm, "dnf"))
        return sh("sudo dnf install -y %s 2>/dev/null", pkg);
    return -1;
}

int exo_update_prebuilt(const char *argv0)
{
    const char *pm = detect_pm();
    if (!pm) {
        fprintf(stderr,
                "exomind: error: no supported package manager found (need "
                "pacman, apt, yum, dnf, paru, or yay)\n");
        return 1;
    }
    const char *pkg = getenv("EXO_PREBUILT_PKG");
    if (!pkg || !*pkg)
        pkg = "exomind";
    char ver[64];
    snprintf(ver, sizeof ver, "exomind-%s", EXOMIND_VERSION);
    printf("exomind: installing %s v%s via %s ...\n", pkg, EXOMIND_VERSION,
           pm);
    if (pm_install(pm, pkg) != 0) {
        fprintf(stderr,
                "exomind: error: %s install of '%s' failed — "
                "try manually or use --update to build from source\n",
                pm, pkg);
        return 1;
    }
    printf("exomind: %s v%s installed via %s\n", pkg, EXOMIND_VERSION, pm);
    printf("exomind: restart '%s' to apply (hint: systemctl --user restart "
           "exo-exomind)\n",
           strrchr(argv0, '/') ? strrchr(argv0, '/') + 1 : argv0);
    return 0;
}
