/* exo-cli implementation. See exo_cli.h for the API contract. */
#include "exo_cli.h"
#include "exo.h"

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- tab completion ---- */

static const exo_cli_module_t *g_modules[32];
static size_t g_nmodules = 0;

void exo_cli_register(const exo_cli_module_t *modules, size_t n)
{
    for (size_t i = 0; i < n && g_nmodules < 32; i++)
        g_modules[g_nmodules++] = &modules[i];
}

int exo_cli_handle_completion(int argc, char **argv)
{
    /* Check if this is a completion request */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--completion")) {
            if (i + 1 < argc) {
                const char *prefix = argv[++i];
                /* Print matching completions */
                for (size_t j = 0; j < g_nmodules; j++) {
                    const char *cmd = g_modules[j]->name;
                    if (!strncmp(cmd, prefix, strlen(prefix))) {
                        printf("%s\n", cmd);
                    }
                    /* Print commands for this module */
                    /* commands is a variable-sized array, so we can't check for NULL.
                     * We check if the first element is non-empty. */
                    if (g_modules[j]->commands[0]) {
                        for (size_t k = 0; k < 16; k++) {
                            if (!g_modules[j]->commands[k])
                                break;
                            const char *c = g_modules[j]->commands[k];
                            if (!strncmp(c, prefix, strlen(prefix))) {
                                printf("%s\n", c);
                            }
                        }
                    }
                }
                /* Also add common flags */
                const char *flags[] = {
                    "--help", "-h", "--version", "-v", "--body",
                    "--host", "--port", "--token", "--tokens",
                    "--keys", "--rate-limit", "--log-level",
                    "--serve", "--completion", "--tutorial",
                    "--getting-started"
                };
                for (size_t k = 0; k < 12; k++) {
                    if (!strncmp(flags[k], prefix, strlen(prefix))) {
                        printf("%s\n", flags[k]);
                    }
                }
            }
            return 1;
        }
    }
    return 0;
}

/* ---- command history ---- */

void exo_cli_history_write(const char *cmd)
{
    const char *home = getenv("HOME");
    if (!home)
        return;
    const char *history_file = getenv("EXOMIND_HISTORY");
    if (!history_file)
        history_file = ".exomind_history";
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", home, history_file);

    /* Limit history size to 10000 lines: rewrite the file keeping only the
     * most recent entries, then append the new command. */
    const long max_lines = 10000;
    char **lines = NULL;
    long n = 0, cap = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            if (n >= cap) {
                cap = cap ? cap * 2 : 1024;
                lines = (char **)realloc((void *)lines, cap * sizeof(char *));
                if (!lines) {
                    fclose(f);
                    return;
                }
            }
            lines[n++] = strdup(line);
        }
        fclose(f);
    }

    long start = 0;
    if (n > max_lines)
        start = n - max_lines;

    char tmp[4192];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *tmp_f = fopen(tmp, "w");
    if (!tmp_f) {
        /* Fallback: append directly (no size limit). */
        tmp_f = fopen(path, "a");
        if (tmp_f) {
            fprintf(tmp_f, "%s\n", cmd);
            fclose(tmp_f);
        }
        for (long i = 0; i < n; i++)
            free(lines[i]);
        free(lines);
        return;
    }
    for (long i = start; i < n; i++)
        fprintf(tmp_f, "%s", lines[i]);
    fprintf(tmp_f, "%s\n", cmd);
    fclose(tmp_f);
    for (long i = 0; i < n; i++)
        free(lines[i]);
    free(lines);
    rename(tmp, path);
}

size_t exo_cli_history_read(const char **commands)
{
    const char *home = getenv("HOME");
    if (!home)
        return 0;
    const char *history_file = getenv("EXOMIND_HISTORY");
    if (!history_file)
        history_file = ".exomind_history";
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", home, history_file);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = 0;
    char line[4096];
    while (fgets(line, sizeof line, f) && n < 1000) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (l && line[0] != '#') {
            commands[n++] = line;
        }
    }
    fclose(f);
    return n;
}

/* ---- progress indicators ---- */

void exo_cli_progress_bar(double progress, const char *label)
{
    int width = 50;
    int filled = (int)(progress * width);
    if (filled > width)
        filled = width;
    if (filled < 0)
        filled = 0;
    printf("\r[");
    for (int i = 0; i < width; i++) {
        if (i < filled)
            printf("\033[32m█\033[0m");
        else
            printf("░");
    }
    printf("] %3.0f%% %s", progress * 100, label);
    fflush(stdout);
}

void exo_cli_spinner(int show)
{
    static const char *chars = "\033[32m/\033[0m\033[32m-\033[0m\033[32m\\\033[0m\033[32m|\033[0m";
    static int idx = 0;
    if (show) {
        printf("\r%c loading...", chars[idx % 4]);
        idx++;
        fflush(stdout);
    } else {
        printf("\r\033[2K");
        fflush(stdout);
    }
}

/* ---- caching ---- */

static char *g_cache_dir = NULL;

static char *get_cache_dir(void)
{
    if (g_cache_dir)
        return g_cache_dir;
    const char *cache_home = getenv("XDG_CACHE_HOME");
    if (!cache_home)
        cache_home = getenv("XDG_CONFIG_HOME");
    if (!cache_home) {
        const char *home = getenv("HOME");
        if (!home)
            return NULL;
        static char *default_dir;
        if (!default_dir) {
            default_dir = ".exomind_cache";
            static char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s/%s", home, default_dir);
            g_cache_dir = tmp;
        }
        return g_cache_dir;
    }
    static char *default_dir;
    if (!default_dir) {
        default_dir = ".exomind_cache";
        static char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s/%s", cache_home, default_dir);
        g_cache_dir = tmp;
    }
    return g_cache_dir;
}

const char *exo_cli_cache_key(const char *operation, const char *params)
{
    /* Simple key generation: hash the operation and params */
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s_%s", operation, params);
    unsigned int hash = 0;
    for (const char *p = tmp; *p; p++)
        hash = hash * 31 + *p;
    static char *result;
    result = (char *)malloc(33);
    snprintf(result, 33, "%08x", hash);
    return result;
}

int exo_cli_cache_check(const char *key, long max_age_secs)
{
    char *dir = get_cache_dir();
    if (!dir)
        return 0;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, key);
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    time_t now;
    time_t cache_time;
    if (time(&now) != 0)
        return 0;
    cache_time = (time_t)st.st_ctime;
    if (now - cache_time > max_age_secs)
        return 0;
    return 1;
}

const char *exo_cli_cache_get(const char *key)
{
    char *dir = get_cache_dir();
    if (!dir)
        return NULL;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, key);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char *value = NULL;
    size_t n = 0, cap = 0;
    char chunk[8192];
    while (fread(chunk, 1, sizeof chunk, f) > 0) {
        cap += 8192;
        value = (char *)realloc((void *)value, cap);
        if (!value) {
            fclose(f);
            return NULL;
        }
        memcpy(value + n, chunk, sizeof chunk);
        n += sizeof chunk;
    }
    fclose(f);
    if (value) {
        value[n] = 0;
        return value;
    }
    return NULL;
}

void exo_cli_cache_set(const char *key, const char *value)
{
    char *dir = get_cache_dir();
    if (!dir)
        return;
    /* Create cache directory if it doesn't exist */
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return;
    /* Write value to cache file */
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, key);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%s", value);
    fclose(f);
}

void exo_cli_cache_clear(void)
{
    char *dir = get_cache_dir();
    if (!dir)
        return;
    /* Remove all files in cache directory */
    char path[4096];
    snprintf(path, sizeof path, "%s/*", dir);
    glob_t g;
    if (glob(path, 0, 0, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            if (g.gl_pathv[i][0] != '.')
                unlink(g.gl_pathv[i]);
        }
    }
    free(g.gl_pathv);
}

/* ---- error handling ---- */

void exo_cli_error(const char *message, const char *hint)
{
    fprintf(stderr, "\033[31mError:\033[0m %s\n", message);
    if (hint)
        fprintf(stderr, "  Hint: %s\n", hint);
    fprintf(stderr, "  Run `--help` or `--completion <prefix>` for usage guidance.\n\n");
}

void exo_cli_warn(const char *message, const char *hint)
{
    fprintf(stderr, "\033[33mWarning:\033[0m %s\n", message);
    if (hint)
        fprintf(stderr, "  Hint: %s\n", hint);
    fprintf(stderr, "\n");
}

void exo_cli_fatal(const char *message)
{
    fprintf(stderr, "\033[31mFatal:\033[0m %s\n", message);
    fprintf(stderr, "\n");
    fprintf(stderr, "This is a critical error. Please check your configuration and\n");
    fprintf(stderr, "try again. If the problem persists, please report an issue.\n");
    fprintf(stderr, "\n");
}

/* ---- onboarding / getting started ---- */

void exo_cli_show_getting_started(void)
{
    printf("\033[32m🎓 Getting Started with Exomind\033[0m\n");
    printf("========================================\n\n");
    printf("Welcome to Exomind! This is a powerful AI-native software stack.\n");
    printf("Here's a quick guide to get you started:\n\n");
    printf("1. Install Exomind\n");
    printf("   a) From source: make && make install PREFIX=~/.local\n");
    printf("   b) From packages: download from GitHub release\n\n");
    printf("2. Initialize your workspace\n");
    printf("   exoqms /init --repo ~/projects/my-project\n\n");
    printf("3. Store your first value in exomind\n");
    printf("   exomind /set?key=project:my-project:version --body '1.0.0'\n\n");
    printf("4. Create a reminder\n");
    printf("   exosched /remind --body 'in 90s \"water the plants\"'\n\n");
    printf("5. Start the servers\n");
    printf("   exomind --serve\n");
    printf("   exosched --serve\n");
    printf("   exoflow --serve\n");
    printf("   exoqms --serve\n\n");
    printf("6. Explore the API\n");
    printf("   exomind --help\n");
    printf("   exoqms --help\n\n");
    printf("For more information, see the README.md in your exomind directory.\n");
    printf("Enjoy learning and using Exomind! 🎓\n\n");
}

/* ---- interactive tutorial mode ---- */

void exo_cli_show_tutorial(const char *tutorial_name)
{
    printf("\033[32m📚 Tutorial: %s\033[0m\n", tutorial_name);
    printf("========================================\n\n");
    if (!strcmp(tutorial_name, "getting_started")) {
        exo_cli_show_getting_started();
    } else if (!strcmp(tutorial_name, "memory_basics")) {
        printf("Tutorial: Memory Basics\n");
        printf("========================================\n\n");
        printf("1. Save a value:\n");
        printf("   exomind /set?key=user:name --body 'John'\n\n");
        printf("2. Load a value:\n");
        printf("   exomind /get?key=user:name\n\n");
        printf("3. Delete a value:\n");
        printf("   exomind /del?key=user:name\n\n");
        printf("4. Search for values:\n");
        printf("   exomind /search?q=user\n\n");
        printf("5. List all keys:\n");
        printf("   exomind /list\n\n");
    } else if (!strcmp(tutorial_name, "workflow_basics")) {
        printf("Tutorial: Workflow Basics\n");
        printf("========================================\n\n");
        printf("1. Start the servers:\n");
        printf("   exomind --serve\n");
        printf("   exosched --serve\n");
        printf("   exoflow --serve\n");
        printf("   exoqms --serve\n\n");
        printf("2. Create a reminder:\n");
        printf("   exosched /remind --body 'in 90s \"water the plants\"'\n\n");
        printf("3. Create a flow:\n");
        printf("   exoflow /create --body 'build-then-deploy'\n\n");
        printf("4. Run the flow:\n");
        printf("   exoflow /run?flow=build-then-deploy\n\n");
    } else {
        printf("Tutorial not found: %s\n", tutorial_name);
        printf("Available tutorials: getting_started, memory_basics, workflow_basics\n");
    }
}
