/* exocpi C runner for exokit verification */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Forward declarations of CPI functions */
int cpi_pdca_create(void *exo, const char *problem, const char *target,
                    const char *metric, const char *hypothesis, char *out, size_t cap);
int cpi_pdca_get(void *exo, const char *id, char *out, size_t cap);
int cpi_pdca_list(void *exo, char *out, size_t cap);
int cpi_plan(void *exo, const char *pdca_id, const char *problem,
             const char *target, const char *metric, const char *hypothesis,
             char *out, size_t cap);
int cpi_do(void *exo, const char *pdca_id, const char *step, char *out, size_t cap);
int cpi_check(void *exo, const char *pdca_id, double measurement, char *out, size_t cap);
int cpi_act(void *exo, const char *pdca_id, const char *decision,
            const char *note, char *out, size_t cap);

int cpi_checksheet_create(void *exo, const char *template, char *out, size_t cap);
int cpi_pareto(void *exo, const char *source, char *out, size_t cap);
int cpi_fishbone(void *exo, const char *problem, const char *categories, char *out, size_t cap);
int cpi_histogram(void *exo, const char *metric, char *out, size_t cap);
int cpi_control(void *exo, const char *metric, double value, char *out, size_t cap);
int cpi_scatter(void *exo, const char *x_metric, const char *y_metric, char *out, size_t cap);
int cpi_run_chart(void *exo, const char *metric, char *out, size_t cap);
int cpi_flowchart(void *exo, const char *flow_id, char *out, size_t cap);

int cpi_poka_register(void *exo, const char *name, const char *method,
                      int prevention, const char *desc, char *out, size_t cap);
int cpi_poka_list(void *exo, char *out, size_t cap);
int cpi_poka_effectiveness(void *exo, const char *period, char *out, size_t cap);

int cpi_kanban_board_create(void *exo, const char *flow, const char *columns,
                            const char *wip_limits, const char *sle, char *out, size_t cap);
int cpi_kanban_pull(void *exo, const char *board, const char *worker, char *out, size_t cap);
int cpi_kanban_metrics(void *exo, const char *board, char *out, size_t cap);

int cpi_5s_audit(void *exo, const char *agent, const char *criteria, char *out, size_t cap);

int cpi_kaizen_suggest(void *exo, const char *agent, const char *title,
                       const char *problem, const char *suggestion, const char *category,
                       char *out, size_t cap);
int cpi_kaizen_implement(void *exo, const char *suggestion_id,
                         const char *pdca_id, char *out, size_t cap);

int cpi_standard_sync(void *exo, const char *kit_path, char *out, size_t cap);
int cpi_standard_drift(void *exo, char *out, size_t cap);

int cpi_report(void *exo, const char *period, char *out, size_t cap);

/* Mock exo connection for testing */
typedef struct { int dummy; } mock_exo_t;

static mock_exo_t g_mock_exo = {0};

int main(void)
{
    mock_exo_t *exo = &g_mock_exo;
    char out[8192];
    int pass = 0, fail = 0;

#define TEST(fn, args, expected, desc) \
    do { \
        char out[8192] = {0}; \
        int rc = fn(args, out, sizeof out); \
        int ok = (strstr(out, expected) != NULL); \
        if (ok) { printf("pass %s - %s\n", #fn, desc); pass++; } \
        else { printf("fail %s - %s: got '%s'\n", #fn, desc, out); fail++; } \
    } while (0)

    /* PDCA tests */
    TEST(cpi_pdca_create, (&g_mock_exo, "high rework", "rework<5%", "metric:rework", "hypothesis", out, sizeof out),
         "ok cpi:pdca:", "create PDCA");
    TEST(cpi_pdca_create, (&g_mock_exo, "", "target", "metric", "hyp", out, sizeof out),
         "error: missing problem", "create PDCA missing problem");
    TEST(cpi_pdca_get, (&g_mock_exo, "nonexistent", out, sizeof out),
         "error: not found", "get nonexistent");
    TEST(cpi_pdca_list, (&g_mock_exo, out, sizeof out), "cpi:pdca:", "list PDCA");

    TEST(cpi_plan, (&g_mock_exo, "test-id", "prob", "target", "metric", "hyp", out, sizeof out),
         "ok cpi:plan:", "plan phase");
    TEST(cpi_do, (&g_mock_exo, "test-id", "step1", out, sizeof out),
         "ok cpi:do:", "do phase");
    TEST(cpi_check, (&g_mock_exo, "test-id", 4.8, out, sizeof out),
         "ok cpi:check:", "check phase");
    TEST(cpi_act, (&g_mock_exo, "test-id", "standardize", "note", out, sizeof out),
         "ok cpi:act:", "act standardize");
    TEST(cpi_act, (&g_mock_exo, "test-id", "replan", "note", out, sizeof out),
         "ok cpi:act:", "act replan");

    /* QC tools */
    TEST(cpi_checksheet_create, (&g_mock_exo, "defects", out, sizeof out),
         "ok cpi:checksheet:", "checksheet");
    TEST(cpi_pareto, (&g_mock_exo, "nc:findings", out, sizeof out),
         "component-tests=", "pareto");
    TEST(cpi_fishbone, (&g_mock_exo, "high rework", "method,material", out, sizeof out),
         "graph TD", "fishbone");
    TEST(cpi_histogram, (&g_mock_exo, "metric:rework", out, sizeof out),
         "bins:", "histogram");
    TEST(cpi_control, (&g_mock_exo, "metric:rework", 4.8, out, sizeof out),
         "CL=", "control chart");
    TEST(cpi_scatter, (&g_mock_exo, "wip", "cycle_time", out, sizeof out),
         "r=", "scatter");
    TEST(cpi_run_chart, (&g_mock_exo, "metric:rework", out, sizeof out),
         "points:", "run chart");
    TEST(cpi_flowchart, (&g_mock_exo, "flow-1", out, sizeof out),
         "graph TD", "flowchart");

    /* Poka-yoke */
    TEST(cpi_poka_register, (&g_mock_exo, "test_poka", "motion-step", 1, "desc", out, sizeof out),
         "ok cpi:poka:test_poka", "poka register");
    TEST(cpi_poka_list, (&g_mock_exo, out, sizeof out), "cpi:poka:", "poka list");

    /* Kanban */
    TEST(cpi_kanban_board_create, (&g_mock_exo, "flow-1", "a,b,c", "", "", out, sizeof out),
         "ok cpi:kanban:flow-1", "kanban create");
    TEST(cpi_kanban_pull, (&g_mock_exo, "flow-1", "worker1", out, sizeof out),
         "card=", "kanban pull");

    /* 5S, Kaizen, Standard, Report */
    TEST(cpi_5s_audit, (&g_mock_exo, "agent1", "criteria", out, sizeof out),
         "score=", "5s audit");
    TEST(cpi_kaizen_suggest, (&g_mock_exo, "agent1", "title", "prob", "sugg", "process", out, sizeof out),
         "ok cpi:kaizen:", "kaizen suggest");
    TEST(cpi_standard_sync, (&g_mock_exo, "kit", out, sizeof out),
         "synced", "standard sync");
    TEST(cpi_report, (&g_mock_exo, "monthly", out, sizeof out),
         "PDCA Cycles:", "report");

    printf("=== exocpi runner: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}