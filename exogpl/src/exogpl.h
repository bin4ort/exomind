/* exogpl: GPL v3.0 compliance checker — license scanner, dependency analyzer,
   copyright tracker, distribution validator, linking checker, report generator. */
#ifndef EXOGPL_H
#define EXOGPL_H

#include <stddef.h>
#include <stdint.h>

/* --------------- build version (must match EXO_VERSION) --------------- */

#ifndef EXO_VERSION
#define EXO_VERSION "0.1.0"
#endif

/* --------------- data structures ------------------------------------ */

#define MAX_PATH_LEN 4096
#define MAX_LINE_LEN 2048
#define MAX_FILES 10000
#define MAX_LICENSES 256
#define MAX_CONTRIBUTORS 10000

typedef struct {
    char path[MAX_PATH_LEN];
    char license[MAX_LINE_LEN];  /* detected license text or name */
    char copyright[MAX_LINE_LEN]; /* copyright notice if found */
    int  has_gpl_header;         /* 1 if GPL v3.0 header found */
    int  is_compliant;           /* 1 if compliant with GPL v3.0 */
    int  error;                  /* error flag */
} file_report_t;

typedef struct {
    char name[MAX_LINE_LEN];
    char version[MAX_LINE_LEN];
    char license[MAX_LINE_LEN];
    int  is_gpl_compatible;  /* 1 if compatible with GPL v3.0 */
    int  is_third_party;     /* 1 if third-party dependency */
} dependency_t;

typedef struct {
    char name[MAX_LINE_LEN];
    char email[MAX_LINE_LEN];
    char contribution[MAX_LINE_LEN];
    char year[MAX_LINE_LEN];
} contributor_t;

typedef struct {
    file_report_t files[MAX_FILES];
    int           file_count;

    dependency_t  deps[MAX_LICENSES];
    int           dep_count;

    contributor_t contributors[MAX_CONTRIBUTORS];
    int           contributor_count;

    int  gpl_v3_missing;    /* count of files missing GPL header */
    int  gpl_v3_compliant;  /* count of compliant files */
    int  gpl_v3_noncompliant; /* count of non-compliant files */
    int  incompatible_deps; /* count of deps incompatible with GPL v3.0 */
    int  total_errors;      /* total errors detected */

    char target_path[MAX_PATH_LEN]; /* path being audited */
} audit_result_t;

/* --------------- API ------------------------------------------------ */

/* spec() returns the API guide (same as GET /) */
const char *exogpl_spec(void);

/* ping() returns "pong" for liveness */
const char *exogpl_ping(void);

/* audit_path() audits a directory for GPL compliance.
   Sets result->error on failure. Returns the result pointer. */
audit_result_t *exogpl_audit_path(const char *path, audit_result_t *result);

/* audit_file() audits a single file for GPL compliance.
   Fills in file_report_t fields. Returns 0 on success. */
int exogpl_audit_file(const char *path, file_report_t *report);

/* generate_report() formats the audit result as plain text.
   Returns a static buffer (caller must not free). */
const char *exogpl_generate_report(audit_result_t *result);

/* scan_gpl_header() scans a file for GPL v3.0 license header.
   Returns 1 if found, 0 if not. */
int exogpl_scan_gpl_header(const char *path);

/* check_dependency() checks if a dependency license is GPL v3.0 compatible.
   Returns 1 if compatible, 0 if not or unknown. */
int exogpl_check_dependency(const char *license_name);

/* --------------- console ops ---------------------------------------- */

/* dispatch() handles console operations (e.g., /audit, /report, /scan).
   Returns the response string (static buffer). */
const char *exogpl_dispatch(const char *path, const char *body);

#endif /* EXOGPL_H */
