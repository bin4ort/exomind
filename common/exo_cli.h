/*
 * exo-cli — common CLI utilities for exomind stack modules.
 *
 * Provides:
 *   - Tab completion (via shell integration)
 *   - Command history with arrow keys
 *   - Better error messages
 *   - Progress indicators
 *   - Caching
 *
 * Zero dependencies, C11. Compiled into each module binary.
 */
#ifndef EXO_CLI_H
#define EXO_CLI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- tab completion ---- */

/* Register completion handlers. The shell calls <module> --completion <prefix>
 * and we print matching completions, one per line. */
typedef struct {
    const char *name;      /* command name, e.g. "exosched" */
    const char *spec;      /* the module's usage guide (for --help) */
    const char *commands[16]; /* list of commands for tab completion */
} exo_cli_module_t;

void exo_cli_register(const exo_cli_module_t *modules, size_t n);

/* Check if this is a completion request. Returns 1 if we should print
 * completions and 0 if not. */
int exo_cli_handle_completion(int argc, char **argv);

/* ---- command history ---- */

/* Write the current command to history file. */
void exo_cli_history_write(const char *cmd);

/* Read commands from history file. Returns the number of commands read.
 * The commands are stored in an array of strings. */
size_t exo_cli_history_read(const char **commands);

/* ---- progress indicators ---- */

/* Simple progress bar. Shows [████████░░░░░░] 65% ... */
void exo_cli_progress_bar(double progress, const char *label);

/* Spinner for long operations. Shows "loading..." with a rotating character.
 * Call with 1 to start, 0 to stop. */
void exo_cli_spinner(int show);

/* ---- caching ---- */

/* Cache key for a given operation. Returns the cache key path. */
const char *exo_cli_cache_key(const char *operation, const char *params);

/* Check if cache exists and is valid. Returns 1 if valid, 0 if not. */
int exo_cli_cache_check(const char *key, long max_age_secs);

/* Get cached value. Returns the cached value or NULL. */
const char *exo_cli_cache_get(const char *key);

/* Save value to cache. */
void exo_cli_cache_set(const char *key, const char *value);

/* Clear cache. */
void exo_cli_cache_clear(void);

/* ---- error handling ---- */

/* Print a user-friendly error message. Includes actionable context. */
void exo_cli_error(const char *message, const char *hint);

/* Print a warning message. */
void exo_cli_warn(const char *message, const char *hint);

/* Print a fatal error message. */
void exo_cli_fatal(const char *message);

/* ---- onboarding / getting started ---- */

/* Show the getting started guide. */
void exo_cli_show_getting_started(void);

/* Show a tutorial. Available tutorials: getting_started, memory_basics, workflow_basics */
void exo_cli_show_tutorial(const char *tutorial_name);

#endif
