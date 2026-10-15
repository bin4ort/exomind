/* exocpi — Continuous Process Improvement
 *
 * Core types and function declarations for the CPI engine.
 * Integrates with exomind, exosched, exoflow, exoqms, exokit.
 */

#ifndef CPI_H
#define CPI_H

#include <stddef.h>
#include <stdint.h>

/* Forward declaration from exoqms.h */
struct exo;

/* PDCA cycle states */
typedef enum {
    CPI_PDCA_PLAN = 0,
    CPI_PDCA_DO,
    CPI_PDCA_CHECK,
    CPI_PDCA_ACT,
    CPI_PDCA_DONE
} cpi_pdca_state_t;

/* PDCA cycle record */
typedef struct {
    char id[64];
    char problem[1024];
    char target[256];
    char metric[256];
    char hypothesis[1024];
    cpi_pdca_state_t state;
    char plan_id[64];
    char do_id[64];
    char check_id[64];
    char act_id[64];
    int64_t created;
    int64_t updated;
} cpi_pdca_t;

/* QC tool types */
typedef enum {
    CPI_QC_CHECKSHEET = 0,
    CPI_QC_PARETO,
    CPI_QC_FISHBONE,
    CPI_QC_HISTOGRAM,
    CPI_QC_CONTROL,
    CPI_QC_SCATTER,
    CPI_QC_RUN_CHART,
    CPI_QC_FLOWCHART
} cpi_qc_type_t;

/* Kanban board */
typedef struct {
    char id[64];
    char flow[64];
    char columns[512];
    char wip_limits[512];
    char sle[512];
    char policies[1024];
} cpi_kanban_board_t;

/* Kanban card */
typedef struct {
    char id[64];
    char board_id[64];
    char column[64];
    char title[256];
    char assignee[64];
    int64_t created;
    int64_t started;
    int blocked;
    char blocker_reason[256];
} cpi_kanban_card_t;

/* Poka-yoke device */
typedef struct {
    char id[64];
    char name[128];
    char method[32];        /* contact, fixed-value, motion-step */
    int prevention;         /* 1 = prevention, 0 = detection */
    char description[512];
    int64_t created;
    int catches;
    int opportunities;
} cpi_poka_t;

/* Kaizen suggestion */
typedef struct {
    char id[64];
    char agent[64];
    char title[256];
    char problem[1024];
    char suggestion[1024];
    char category[32];      /* process, tool, skill, waste */
    char status[32];        /* pending, approved, implemented, rejected */
    char pdca_id[64];
    int64_t created;
} cpi_kaizen_t;

/* Standard work */
typedef struct {
    char id[64];
    char kit_path[256];
    char function[128];
    char current_standard[1024];
    int64_t verified;
    int drift;
} cpi_standard_t;

/* PDCA engine */
int cpi_pdca_create(struct exo *exo, const char *problem, const char *target,
                    const char *metric, const char *hypothesis, char *out, size_t cap);
int cpi_pdca_get(struct exo *exo, const char *id, char *out, size_t cap);
int cpi_pdca_list(struct exo *exo, char *out, size_t cap);

int cpi_plan(struct exo *exo, const char *pdca_id, const char *problem,
             const char *target, const char *metric, const char *hypothesis,
             char *out, size_t cap);
int cpi_do(struct exo *exo, const char *pdca_id, const char *step,
           char *out, size_t cap);
int cpi_check(struct exo *exo, const char *pdca_id, double measurement,
              char *out, size_t cap);
int cpi_act(struct exo *exo, const char *pdca_id, const char *decision,
            const char *note, char *out, size_t cap);

/* QC tools */
int cpi_qc_run(struct exo *exo, cpi_qc_type_t type, const char *params,
               char *out, size_t cap);
int cpi_checksheet_create(struct exo *exo, const char *template, char *out, size_t cap);
int cpi_pareto(struct exo *exo, const char *source, char *out, size_t cap);
int cpi_fishbone(struct exo *exo, const char *problem, const char *categories, char *out, size_t cap);
int cpi_histogram(struct exo *exo, const char *metric, char *out, size_t cap);
int cpi_control(struct exo *exo, const char *metric, double value, char *out, size_t cap);
int cpi_scatter(struct exo *exo, const char *x_metric, const char *y_metric, char *out, size_t cap);
int cpi_run_chart(struct exo *exo, const char *metric, char *out, size_t cap);
int cpi_flowchart(struct exo *exo, const char *flow_id, char *out, size_t cap);

/* Kanban */
int cpi_kanban_board_create(struct exo *exo, const char *flow,
                            const char *columns, const char *wip_limits,
                            const char *sle, char *out, size_t cap);
int cpi_kanban_pull(struct exo *exo, const char *board, const char *worker,
                    char *out, size_t cap);
int cpi_kanban_metrics(struct exo *exo, const char *board, char *out, size_t cap);

/* Poka-yoke */
int cpi_poka_register(struct exo *exo, const char *name, const char *method,
                      int prevention, const char *desc, char *out, size_t cap);
int cpi_poka_list(struct exo *exo, char *out, size_t cap);
int cpi_poka_effectiveness(struct exo *exo, const char *period, char *out, size_t cap);

/* 5S */
int cpi_5s_audit(struct exo *exo, const char *agent, const char *criteria,
                 char *out, size_t cap);

/* Kaizen */
int cpi_kaizen_suggest(struct exo *exo, const char *agent, const char *title,
                       const char *problem, const char *suggestion, const char *category,
                       char *out, size_t cap);
int cpi_kaizen_implement(struct exo *exo, const char *suggestion_id,
                         const char *pdca_id, char *out, size_t cap);

/* Standard work */
int cpi_standard_sync(struct exo *exo, const char *kit_path, char *out, size_t cap);
int cpi_standard_drift(struct exo *exo, char *out, size_t cap);

/* Report */
int cpi_report(struct exo *exo, const char *period, char *out, size_t cap);

/* CLI dispatch (called from cpi_main.c) */
int cpi_dispatch(struct exo *exo, const char *path, const char *body,
                 char *out, size_t cap);

/* Agent/Session management */
int exo_agent_register(struct exo *exo, const char *session_id, const char *agent_id,
                       const char *role, char *out, size_t cap);
int exo_agent_heartbeat(struct exo *exo, const char *session_id, const char *agent_id, char *out, size_t cap);
int exo_agent_list(struct exo *exo, const char *session_id, char *out, size_t cap);
int exo_agent_get(struct exo *exo, const char *session_id, const char *agent_id, char *out, size_t cap);
int exo_agent_check(struct exo *exo, const char *session_id, const char *agent_id);

/* Session context management */
int exo_session_ctx_set(struct exo *exo, const char *session_id, const char *agent_id, char *out, size_t cap);
int exo_session_ctx_clear(char *out, size_t cap);
int exo_session_ctx_get(char *out, size_t cap);
int exo_session_ctx_autoscope(int enable);
int exo_session_ctx_autoscope_get();

/* Session-scoped memory operations */
int exo_session_set(struct exo *exo, const char *key, const char *value, char *out, size_t cap);
int exo_session_get(struct exo *exo, const char *key, char *out, size_t cap);
int exo_session_list(struct exo *exo, const char *prefix, char *out, size_t cap);
int exo_session_search(struct exo *exo, const char *query, char *out, size_t cap);
int exo_session_note(struct exo *exo, const char *text, char *out, size_t cap);
int exo_session_autoscope(int enable);
int exo_session_autoscope_get();

/* Flow enforcement */
int cpi_flow_enforcement(int enable);
int cpi_flow_enforcement_get();
int cpi_validate_flow(struct exo *exo, const char *pdca_id, char *out, size_t cap);
int cpi_associate_flow(struct exo *exo, const char *pdca_id, const char *flow_id, char *out, size_t cap);
int cpi_enforce_work_requires_flow(struct exo *exo, const char *operation, const char *pdca_id, char *out, size_t cap);
int cpi_create_enforced_flow(struct exo *exo, const char *flow_name, const char *steps_json,
                             const char *workers_json, char *out, size_t cap);
int cpi_flow_status(char *out, size_t cap);

/* Simplified usage validation */
int cpi_validate_simple_usage(const char *argv0, int argc, char **argv, char *out, size_t cap);
int cpi_suggest_simpler(const char *cmd, char *out, size_t cap);
int cpi_is_bare_command(int argc, char **argv);

#endif