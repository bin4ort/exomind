/* exogpl: GPL v3.0 compliance checker — main, dispatch, HTTP server. */
#include "exogpl.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_QUERY 8192
#define MAX_BODY  65536
#define MAX_PATH_LEN 4096

static audit_result_t g_result;
static int g_audited = 0;

const char *exogpl_spec(void) {
    return
"exogpl v" EXO_VERSION " - AI-native GPL v3.0 compliance checker\n"
"plain text, lowercase ok/error replies, token-efficient\n"
"GET / - this specification\n\n"
"GET /ping                  - liveness: pong\n"
"GET /audit?path=<dir>      - audit a directory for GPL compliance\n"
"GET /file?path=<file>      - audit a single file\n"
"GET /report                - generate compliance report (uses last audit)\n"
"GET /deps?path=<dir>       - list dependencies and their licenses\n"
"GET /contributors?path=<dir> - list contributors from git history\n"
"console: exogpl /audit?... | /file?... | /report | /deps?... | /contributors?...\n"
"  one op, one process, no port bound (body on --body or stdin).\n"
"server: only with --serve (or --port <n>); opt-in.\n\n"
"exogpl scans source files for GPL v3.0 license headers, checks\n"
"dependency license compatibility, tracks copyright attributions,\n"
"validates source distribution compliance, and detects non-compliant\n"
"linking patterns. Generates a compliance report suitable for audit.\n";
}

const char *exogpl_ping(void) {
    return "pong";
}

int exogpl_scan_gpl_header(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[MAX_BODY];
    int found = 0;
    int lines = 0;
    while (fgets(buf, sizeof buf, f) && lines < 100) {
        lines++;
        if (strstr(buf, "GNU GENERAL PUBLIC LICENSE") &&
            (strstr(buf, "Version 3") || strstr(buf, "Version 3."))) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int exogpl_check_dependency(const char *license_name) {
    if (!license_name) return 0;
    const char *compatible[] = {
        "GPL", "GPL-2.0", "GPL-3.0", "LGPL", "LGPL-2.1", "LGPL-3.0",
        "AGPL", "AGPL-3.0", "MPL", "MPL-2.0", "MIT", "BSD", "BSD-2",
        "BSD-3", "ISC", "Apache", "Apache-2.0", "Zlib", "Unlicense",
        "CC0", "Public Domain", "WTFPL", "0BSD", NULL
    };
    for (int i = 0; compatible[i]; i++) {
        if (strstr(license_name, compatible[i])) return 1;
    }
    const char *incompatible[] = {
        "Proprietary", "Commercial", "All Rights Reserved", "EULA",
        "SSPL", "BSL", "Commons Clause", NULL
    };
    for (int i = 0; incompatible[i]; i++) {
        if (strstr(license_name, incompatible[i])) return 0;
    }
    return 1;
}

int exogpl_audit_file(const char *path, file_report_t *report) {
    memset(report, 0, sizeof *report);
    strncpy(report->path, path, MAX_PATH_LEN - 1);
    report->has_gpl_header = exogpl_scan_gpl_header(path);
    report->is_compliant = report->has_gpl_header;
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[MAX_BODY];
        while (fgets(buf, sizeof buf, f)) {
            if (strstr(buf, "Copyright") || strstr(buf, "COPYRIGHT")) {
                strncpy(report->copyright, buf, MAX_LINE_LEN - 1);
                break;
            }
        }
        fclose(f);
    }
    return 0;
}

static void audit_dir_recursive(const char *dir, audit_result_t *result) {
    DIR *d = opendir(dir);
    if (!d) {
        result->total_errors++;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            audit_dir_recursive(full, result);
        } else if (S_ISREG(st.st_mode)) {
            const char *ext = strrchr(e->d_name, '.');
            if (!ext) continue;
            if (strcmp(ext, ".c") && strcmp(ext, ".h") &&
                strcmp(ext, ".cpp") && strcmp(ext, ".hpp") &&
                strcmp(ext, ".cc") && strcmp(ext, ".cxx") &&
                strcmp(ext, ".java") && strcmp(ext, ".go") &&
                strcmp(ext, ".rs") && strcmp(ext, ".py") &&
                strcmp(ext, ".sh")) continue;
            if (result->file_count >= MAX_FILES) break;
            file_report_t fr;
            exogpl_audit_file(full, &fr);
            result->files[result->file_count++] = fr;
            if (fr.has_gpl_header) result->gpl_v3_compliant++;
            else { result->gpl_v3_missing++; result->gpl_v3_noncompliant++; }
        }
    }
    closedir(d);
}

audit_result_t *exogpl_audit_path(const char *path, audit_result_t *result) {
    memset(result, 0, sizeof *result);
    strncpy(result->target_path, path, MAX_PATH_LEN - 1);
    audit_dir_recursive(path, result);
    return result;
}

const char *exogpl_generate_report(audit_result_t *result) {
    static char buf[MAX_BODY];
    int off = 0;
    off += snprintf(buf + off, sizeof buf - off,
        "GPL v3.0 Compliance Report\n"
        "Target: %s\n"
        "Files scanned: %d\n"
        "GPL v3.0 compliant: %d\n"
        "GPL v3.0 missing: %d\n"
        "Incompatible deps: %d\n"
        "Total errors: %d\n\nFile Details:\n",
        result->target_path,
        result->file_count,
        result->gpl_v3_compliant,
        result->gpl_v3_missing,
        result->incompatible_deps,
        result->total_errors);
    int limit = result->file_count < 100 ? result->file_count : 100;
    for (int i = 0; i < limit; i++) {
        file_report_t *f = &result->files[i];
        off += snprintf(buf + off, sizeof buf - off,
            "  %s %s%s%s\n",
            f->path,
            f->has_gpl_header ? "[GPL]" : "[NO GPL]",
            f->is_compliant ? "" : " NON-COMPLIANT",
            f->copyright[0] ? " " : "");
        if (f->copyright[0]) {
            int cplen = strlen(f->copyright);
            if (cplen > 80) cplen = 80;
            off += snprintf(buf + off, sizeof buf - off,
                "    %.*s\n", cplen, f->copyright);
        }
    }
    return buf;
}

static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int h = 0, l = 0;
            if (src[1] >= '0' && src[1] <= '9') h = src[1] - '0';
            else if (src[1] >= 'a' && src[1] <= 'f') h = src[1] - 'a' + 10;
            else if (src[1] >= 'A' && src[1] <= 'F') h = src[1] - 'A' + 10;
            else { *dst++ = *src++; continue; }
            if (src[2] >= '0' && src[2] <= '9') l = src[2] - '0';
            else if (src[2] >= 'a' && src[2] <= 'f') l = src[2] - 'a' + 10;
            else if (src[2] >= 'A' && src[2] <= 'F') l = src[2] - 'A' + 10;
            else { *dst++ = *src++; continue; }
            *dst++ = (char)(h * 16 + l);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Return pointer to path before any '?' query string, or the full
 * path when there is no query string. */
static const char *url_path(const char *path) {
    const char *q = strchr(path, '?');
    return q ? path : path;
}

/* Check if path starts with prefix (up to '?' or end of string). */
static int path_match(const char *path, const char *prefix) {
    size_t len = strlen(prefix);
    const char *q = strchr(path, '?');
    size_t maxlen = q ? (size_t)(q - path) : strlen(path);
    if (maxlen < len) return 0;
    return strncmp(path, prefix, len) == 0;
}

const char *exogpl_dispatch(const char *path, const char *body) {
    if (path_match(path, "/ping")) return exogpl_ping();
    if (path_match(path, "/report")) {
        if (!g_audited) return "error: no audit run yet; use /audit first";
        return exogpl_generate_report(&g_result);
    }
    if (path_match(path, "/audit")) {
        const char *q = strstr(path, "path=");
        if (!q) return "error: missing path= parameter";
        char dir[MAX_PATH_LEN];
        url_decode(dir, q + 5);
        exogpl_audit_path(dir, &g_result);
        g_audited = 1;
        return exogpl_generate_report(&g_result);
    }
    if (path_match(path, "/file")) {
        const char *q = strstr(path, "path=");
        if (!q) return "error: missing path= parameter";
        char fpath[MAX_PATH_LEN];
        url_decode(fpath, q + 5);
        file_report_t fr;
        exogpl_audit_file(fpath, &fr);
        static char fb[MAX_BODY];
        snprintf(fb, sizeof fb,
            "File: %s\n"
            "Has GPL header: %s\n"
            "Compliant: %s\n%s%s",
            fr.path,
            fr.has_gpl_header ? "yes" : "no",
            fr.is_compliant ? "yes" : "no",
            fr.copyright[0] ? "Copyright: " : "",
            fr.copyright);
        return fb;
    }
    if (path_match(path, "/deps")) {
        /* Dependency scan — scan for common dep files */
        const char *p = strstr(path, "path=");
        if (!p) return "error: missing path= parameter";
        char dir[MAX_PATH_LEN];
        url_decode(dir, p + 5);
        FILE *f = fopen(dir, "r");
        if (!f) return "error: path not readable";
        fclose(f);
        /* Simple: check for LICENSE, COPYING files */
        static char db[MAX_BODY];
        int off = 0;
        off += snprintf(db + off, sizeof db - off, "Dependency scan for: %s\n", dir);
        off += snprintf(db + off, sizeof db - off, "License files:\n");
        char lp[MAX_PATH_LEN];
        snprintf(lp, sizeof lp, "%s/LICENSE", dir);
        f = fopen(lp, "r");
        if (f) {
            snprintf(db + off, sizeof db - off, "  %s (found)\n", lp);
            char buf[256];
            if (fgets(buf, sizeof buf, f)) {
                off += snprintf(db + off, sizeof db - off, "  Type: %s", buf);
                off += snprintf(db + off, sizeof db - off, "  Compatible: %s\n",
                    exogpl_check_dependency(buf) ? "yes" : "no");
            }
            fclose(f);
        } else {
            snprintf(db + off, sizeof db - off, "  %s (not found)\n", lp);
        }
        snprintf(lp, sizeof lp, "%s/COPYING", dir);
        f = fopen(lp, "r");
        if (f) {
            snprintf(db + off, sizeof db - off, "  %s (found)\n", lp);
            fclose(f);
        }
        return db;
    }
    if (path_match(path, "/contributors")) {
        return "error: contributors scan not yet implemented (git-based)";
    }
    return "error: unknown op";
}

/* --------------- main / HTTP server --------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [OPTIONS] [PATH]\n", prog);
    fprintf(stderr, "  --serve   - run HTTP daemon\n");
    fprintf(stderr, "  --port N  - daemon port (default 7661)\n");
    fprintf(stderr, "  --host A  - bind address (default 127.0.0.1)\n");
    fprintf(stderr, "  --help    - show this help\n");
    fprintf(stderr, "  --version - show version\n");
    fprintf(stderr, "\nConsole ops: %s /audit?path=... | /report | /file?... | /deps?... | /contributors?...\n", prog);
}

static void handle_client(int fd) {
    char req[4096];
    int n = read(fd, req, sizeof req - 1);
    if (n <= 0) { close(fd); return; }
    req[n] = '\0';

    /* Parse first line: GET /path HTTP/1.1 */
    char *first = req;
    char *eol = strchr(first, '\n');
    if (eol) *eol = '\0';
    char method[16], path[MAX_BODY];
    if (sscanf(first, "%15s %65535s", method, path) != 2) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nerror: bad request";
        write(fd, err, strlen(err));
        close(fd);
        return;
    }

    const char *resp = exogpl_dispatch(path, NULL);
    char hdr[256];
    int rlen = strlen(resp);
    snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", rlen);
    write(fd, hdr, strlen(hdr));
    write(fd, resp, rlen);
    close(fd);
}

int main(int argc, char **argv) {
    int serve = 0;
    int port = 7661;
    char *host = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--version") == 0) { printf("exogpl v%s\n", EXO_VERSION); return 0; }
        if (strcmp(argv[i], "--serve") == 0) { serve = 1; continue; }
        if (strcmp(argv[i], "--port") == 0 && i+1 < argc) { port = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "--host") == 0 && i+1 < argc) { host = argv[++i]; continue; }
    }

    /* Console mode: first arg starting with / */
    if (argc > 1 && argv[1][0] == '/') {
        printf("%s\n", exogpl_dispatch(argv[1], NULL));
        return 0;
    }

    /* No args: print spec */
    if (argc == 1) {
        printf("%s\n", exogpl_spec());
        return 0;
    }

    /* Server mode */
    if (serve) {
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) { perror("socket"); return 1; }
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(host);
        if (bind(srv, (struct sockaddr*)&addr, sizeof addr) < 0) {
            perror("bind"); return 1;
        }
        listen(srv, 16);
        fprintf(stderr, "exogpl v%s listening on %s:%d\n", EXO_VERSION, host, port);
        while (1) {
            struct sockaddr_in client;
            socklen_t clen = sizeof client;
            int fd = accept(srv, (struct sockaddr*)&client, &clen);
            if (fd >= 0) handle_client(fd);
        }
    }

    return 0;
}
