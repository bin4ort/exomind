/* exocpi simplified usage validator - encourages bare commands over explicit defaults */

#include "exoqms.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Common default values that shouldn't be explicitly set */
typedef struct {
    const char *flag;
    const char *default_value;
    const char *description;
} default_flag_t;

static const default_flag_t KNOWN_DEFAULTS[] = {
    {"--host", "127.0.0.1", "bind address"},
    {"--port", "7654", "exomind port"},
    {"--data", "exomind.dat", "data file"},
    {"--rate-limit", "0", "rate limiting (0=disabled)"},
    {"--log-level", "warn", "log level"},
    {"--method", "GET", "HTTP method (auto-detected from path)"},
    {NULL, NULL, NULL}
};

/* Validate command for explicit defaults */
int cpi_validate_simple_usage(const char *argv0, int argc, char **argv, char *out, size_t cap)
{
    size_t off = 0;
    int warnings = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        
        /* Check for explicit default flags */
        for (size_t d = 0; KNOWN_DEFAULTS[d].flag; d++) {
            if (!strcmp(arg, KNOWN_DEFAULTS[d].flag) && i + 1 < argc) {
                if (!strcmp(argv[i + 1], KNOWN_DEFAULTS[d].default_value)) {
                    off += snprintf(out + off, cap - off,
                        "WARNING: Explicit default '%s %s' (default: %s). "
                        "Consider omitting for simpler usage.\n",
                        arg, argv[i + 1], KNOWN_DEFAULTS[d].default_value);
                    warnings++;
                    i++; /* skip value */
                    break;
                }
            }
        }
    }

    if (warnings == 0) {
        return snprintf(out, cap, "ok (no explicit defaults detected)"), 0;
    }
    return snprintf(out, cap, "warnings=%d\n%s", warnings, out), warnings;
}

/* Suggest simpler alternative for a command */
int cpi_suggest_simpler(const char *cmd, char *out, size_t cap)
{
    /* This would analyze the command and suggest bare alternatives */
    return snprintf(out, cap, "ok (suggestion engine not yet implemented)"), 0;
}

/* Check if command uses bare form (no explicit defaults) */
int cpi_is_bare_command(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        for (size_t d = 0; KNOWN_DEFAULTS[d].flag; d++) {
            if (!strcmp(argv[i], KNOWN_DEFAULTS[d].flag) && i + 1 < argc) {
                if (!strcmp(argv[i + 1], KNOWN_DEFAULTS[d].default_value)) {
                    return 0; /* Not bare - uses explicit default */
                }
            }
        }
    }
    return 1; /* Bare command */
}