/* exocpi — Continuous Process Improvement CLI */

#include "exoqms.h"
#include "cpi.h"
#include "util.h"
#include "store.h"
#include "../common/exo.h"
#include "../common/exo_cli.h"
#include "version.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0)
{
    printf("exocpi v" EXOMIND_VERSION " — Continuous Process Improvement\n"
           "usage: %s [options]\n"
           "  --exomind URL    exomind endpoint (default http://127.0.0.1:7654)\n"
           "  --exosched URL   exosched endpoint (default http://127.0.0.1:7655)\n"
           "  --exoflow URL    exoflow endpoint (default http://127.0.0.1:7656)\n"
           "  --exoqms URL     exoqms endpoint (default http://127.0.0.1:7657)\n"
           "  --exokit PATH    exokit binary (default ~/.local/bin/exokit)\n"
           "  --kit DIR        kit directory for standard work (default .)\n"
           "  --json           JSON output\n"
           "  <operation>      run ONE CPI operation:\n"
           "                   exocpi /plan?problem=...&target=...&metric=...&hypothesis=...\n"
           "                   exocpi /do?pdca=...&step=...\n"
           "                   exocpi /check?pdca=...&measurement=...\n"
           "                   exocpi /act?pdca=...&decision=standardize|replan&note=...\n"
           "                   exocpi /pdca?list=1|id=...\n"
           "                   exocpi /checksheet?template=...\n"
           "                   exocpi /pareto?source=...\n"
           "                   exocpi /fishbone?problem=...&categories=...\n"
           "                   exocpi /histogram?metric=...\n"
           "                   exocpi /control?metric=...&value=...\n"
           "                   exocpi /scatter?x=...&y=...\n"
           "                   exocpi /run-chart?metric=...\n"
           "                   exocpi /flowchart?flow=...\n"
           "                   exocpi /poka?register=1|list=1|effectiveness=1...\n"
           "                   exocpi /kanban?board=...&create=1|pull=1|metrics=1...\n"
           "                   exocpi /5s?audit=1&agent=...&criteria=...\n"
           "                   exocpi /kaizen?suggest=1|implement=1...\n"
           "                   exocpi /standard?sync=1|drift=1&kit=...\n"
           "                   exocpi /report?period=monthly|quarterly\n"
           "  --body <text>    request body\n"
           "  --help           show this guide\n"
           "  --version        show version\n",
           argv0);
}

static struct exo *g_exo = NULL;
static int g_json = 0;

static int connect_backends(void)
{
    const char *exomind_url = getenv("EXO_EXOMIND_URL") ? getenv("EXO_EXOMIND_URL") : "http://127.0.0.1:7654";
    char err[256];

    g_exo = xmalloc(sizeof *g_exo);
    if (exo_init(g_exo, getenv("EXO_EXOMIND_URL") ? getenv("EXO_EXOMIND_URL") : "http://127.0.0.1:7654", err, sizeof err) != 0) {
        fprintf(stderr, "exocpi: cannot connect to exomind: %s\n", err);
        return -1;
    }
    return 0;
}

static void disconnect_backends(void)
{
    if (g_exo) {
        free(g_exo);
        g_exo = NULL;
    }
}

static int parse_qs(const char *path, const char *p, char *val, size_t cap)
{
    const char *q = strchr(path, '?');
    if (!q) return 0;
    q++;
    const char *found = strstr(q, p);
    if (!found) return 0;
    found += strlen(p);
    if (*found != '=') return 0;
    found++;
    size_t i = 0;
    while (found[i] && found[i] != '&' && found[i] != ' ' && i < cap - 1) {
        if (found[i] == '%' && found[i+1] && found[i+2]) {
            if (found[i+1] == '2' && found[i+2] == '0') {
                val[i] = ' ';
                i++;
                found += 3;
                continue;
            }
        }
        val[i++] = found[i];
    }
    val[i] = 0;
    return 1;
}

int cpi_dispatch(struct exo *exo, const char *path, const char *body,
                 char *out, size_t cap)
{
    char p[512], q[1536];
    snprintf(p, sizeof p, "%s", path);
    char *qs = strchr(p, '?');
    if (qs) { *qs = 0; snprintf(q, sizeof q, "%s", qs + 1); } else q[0] = 0;

    /* PDCA */
    if (!strcmp(p, "/plan")) {
        char problem[1024]="", target[256]="", metric[256]="", hypothesis[1024]="";
        parse_qs(path, "problem", problem, sizeof problem);
        parse_qs(path, "target", target, sizeof target);
        parse_qs(path, "metric", metric, sizeof metric);
        parse_qs(path, "hypothesis", hypothesis, sizeof hypothesis);
        return cpi_pdca_create(exo, problem[0]?problem:0, target[0]?target:0,
                        metric[0]?metric:0, hypothesis[0]?hypothesis:0, out, cap);
    }
    if (!strcmp(p, "/do")) {
        char pdca_id[128]="", step[256]="";
        parse_qs(path, "pdca", pdca_id, sizeof pdca_id);
        parse_qs(path, "step", step, sizeof step);
        return cpi_do(exo, pdca_id[0]?pdca_id:0, step[0]?step:0, out, cap);
    }
    if (!strcmp(p, "/check")) {
        char pdca_id[128]="", meas_str[64]="";
        parse_qs(path, "pdca", pdca_id, sizeof pdca_id);
        parse_qs(path, "measurement", meas_str, sizeof meas_str);
        double measurement = meas_str[0] ? atof(meas_str) : 0;
        return cpi_check(exo, pdca_id[0]?pdca_id:0, measurement, out, cap);
    }
    if (!strcmp(p, "/act")) {
        char pdca_id[128]="", decision[64]="", note[1024]="";
        parse_qs(path, "pdca", pdca_id, sizeof pdca_id);
        parse_qs(path, "decision", decision, sizeof decision);
        parse_qs(path, "note", note, sizeof note);
        return cpi_act(exo, pdca_id[0]?pdca_id:0, decision[0]?decision:0,
                       note[0]?note:0, out, cap);
    }
    if (!strcmp(p, "/pdca")) {
        char id[128]="", list[8]="";
        parse_qs(path, "id", id, sizeof id);
        parse_qs(path, "list", list, sizeof list);
        if (list[0] && !strcmp(list, "1")) return cpi_pdca_list(exo, out, cap);
        if (id[0]) return cpi_pdca_get(exo, id, out, cap);
        return snprintf(out, cap, "error: specify id or list=1"), 1;
    }

    /* QC tools */
    if (!strcmp(p, "/checksheet")) {
        char tmpl[128]=""; parse_qs(path, "template", tmpl, sizeof tmpl);
        return cpi_checksheet_create(exo, tmpl[0]?tmpl:0, out, cap);
    }
    if (!strcmp(p, "/pareto")) {
        char src[256]=""; parse_qs(path, "source", src, sizeof src);
        return cpi_pareto(exo, src[0]?src:0, out, cap);
    }
    if (!strcmp(p, "/fishbone")) {
        char prob[1024]="", cats[512]=""; parse_qs(path, "problem", prob, sizeof prob);
        parse_qs(path, "categories", cats, sizeof cats);
        return cpi_fishbone(exo, prob[0]?prob:0, cats[0]?cats:0, out, cap);
    }
    if (!strcmp(p, "/histogram")) {
        char metric[256]=""; parse_qs(path, "metric", metric, sizeof metric);
        return cpi_histogram(exo, metric[0]?metric:0, out, cap);
    }
    if (!strcmp(p, "/control")) {
        char metric[256]="", val_str[64]=""; parse_qs(path, "metric", metric, sizeof metric);
        parse_qs(path, "value", val_str, sizeof val_str);
        double v = val_str[0] ? atof(val_str) : 0;
        return cpi_control(exo, metric[0]?metric:0, v, out, cap);
    }
    if (!strcmp(p, "/scatter")) {
        char x[256]="", y[256]=""; parse_qs(path, "x", x, sizeof x); parse_qs(path, "y", y, sizeof y);
        return cpi_scatter(exo, x[0]?x:0, y[0]?y:0, out, cap);
    }
    if (!strcmp(p, "/run-chart")) {
        char metric[256]=""; parse_qs(path, "metric", metric, sizeof metric);
        return cpi_run_chart(exo, metric[0]?metric:0, out, cap);
    }
    if (!strcmp(p, "/flowchart")) {
        char flow[128]=""; parse_qs(path, "flow", flow, sizeof flow);
        return cpi_flowchart(exo, flow[0]?flow:0, out, cap);
    }

    /* Poka-yoke */
    if (!strcmp(p, "/poka")) {
        char reg[8]="", list[8]="", eff[64]="";
        parse_qs(path, "register", reg, sizeof reg);
        parse_qs(path, "list", list, sizeof list);
        parse_qs(path, "effectiveness", eff, sizeof eff);
        if (reg[0] && !strcmp(reg, "1")) {
            char name[128]="", method[32]="", prev[8]="", desc[512]="";
            parse_qs(path, "name", name, sizeof name);
            parse_qs(path, "method", method, sizeof method);
            parse_qs(path, "prevention", prev, sizeof prev);
            parse_qs(path, "desc", desc, sizeof desc);
            return cpi_poka_register(exo, name[0]?name:0, method[0]?method:0,
                                     prev[0]?atoi(prev):0, desc[0]?desc:0, out, cap);
        }
        if (list[0] && !strcmp(list, "1")) return cpi_poka_list(exo, out, cap);
        if (eff[0]) return cpi_poka_effectiveness(exo, eff, out, cap);
        return snprintf(out, cap, "error: specify register=1, list=1, or effectiveness=..."), 1;
    }

    /* Kanban */
    if (!strcmp(p, "/kanban")) {
        char board[128]="", create[8]="", pull[8]="", metrics[8]="", worker[64]="", cols[512]="", wip[512]="", sle[512]="";
        parse_qs(path, "board", board, sizeof board);
        parse_qs(path, "create", create, sizeof create);
        parse_qs(path, "pull", pull, sizeof pull);
        parse_qs(path, "metrics", metrics, sizeof metrics);
        parse_qs(path, "worker", worker, sizeof worker);
        parse_qs(path, "columns", cols, sizeof cols);
        parse_qs(path, "wip_limits", wip, sizeof wip);
        parse_qs(path, "sle", sle, sizeof sle);
        if (create[0] && !strcmp(create, "1"))
            return cpi_kanban_board_create(exo, board[0]?board:0, cols[0]?cols:0, wip[0]?wip:0, sle[0]?sle:0, out, cap);
        if (pull[0] && !strcmp(pull, "1"))
            return cpi_kanban_pull(exo, board[0]?board:0, worker[0]?worker:0, out, cap);
        if (metrics[0] && !strcmp(metrics, "1"))
            return cpi_kanban_metrics(exo, board[0]?board:0, out, cap);
        return snprintf(out, cap, "error: specify create=1, pull=1, or metrics=1"), 1;
    }

    /* 5S */
    if (!strcmp(p, "/5s")) {
        char audit[8]="", agent[64]="", criteria[512]="";
        parse_qs(path, "audit", audit, sizeof audit);
        parse_qs(path, "agent", agent, sizeof agent);
        parse_qs(path, "criteria", criteria, sizeof criteria);
        if (audit[0] && !strcmp(audit, "1"))
            return cpi_5s_audit(exo, agent[0]?agent:0, criteria[0]?criteria:0, out, cap);
        return snprintf(out, cap, "error: specify audit=1"), 1;
    }

    /* Kaizen */
    if (!strcmp(p, "/kaizen")) {
        char suggest[8]="", impl[8]="", agent[64]="", title[256]="", prob[1024]="", sugg[1024]="", cat[64]="", sid[128]="", pid[128]="";
        parse_qs(path, "suggest", suggest, sizeof suggest);
        parse_qs(path, "implement", impl, sizeof impl);
        if (suggest[0] && !strcmp(suggest, "1")) {
            parse_qs(path, "agent", agent, sizeof agent);
            parse_qs(path, "title", title, sizeof title);
            parse_qs(path, "problem", prob, sizeof prob);
            parse_qs(path, "suggestion", sugg, sizeof sugg);
            parse_qs(path, "category", cat, sizeof cat);
            return cpi_kaizen_suggest(exo, agent[0]?agent:0, title[0]?title:0, prob[0]?prob:0,
                                      sugg[0]?sugg:0, cat[0]?cat:0, out, cap);
        }
        if (impl[0] && !strcmp(impl, "1")) {
            parse_qs(path, "suggestion_id", sid, sizeof sid);
            parse_qs(path, "pdca_id", pid, sizeof pid);
            return cpi_kaizen_implement(exo, sid[0]?sid:0, pid[0]?pid:0, out, cap);
        }
        return snprintf(out, cap, "error: specify suggest=1 or implement=1"), 1;
    }

    /* Standard work */
    if (!strcmp(p, "/standard")) {
        char sync[8]="", drift[8]="", kit[256]="";
        parse_qs(path, "sync", sync, sizeof sync);
        parse_qs(path, "drift", drift, sizeof drift);
        parse_qs(path, "kit", kit, sizeof kit);
        if (sync[0] && !strcmp(sync, "1"))
            return cpi_standard_sync(exo, kit[0]?kit:0, out, cap);
        if (drift[0] && !strcmp(drift, "1"))
            return cpi_standard_drift(exo, out, cap);
        return snprintf(out, cap, "error: specify sync=1 or drift=1"), 1;
    }

    /* Report */
    if (!strcmp(p, "/report")) {
        char period[64]=""; parse_qs(path, "period", period, sizeof period);
        return cpi_report(exo, period[0]?period:0, out, cap);
    }

    /* Agent/Session management */
    if (!strcmp(p, "/agent")) {
        char reg[8]="", list[8]="", get[8]="", hb[8]="", session[64]="", agent[64]="", role[32]="";
        parse_qs(path, "register", reg, sizeof reg);
        parse_qs(path, "list", list, sizeof list);
        parse_qs(path, "get", get, sizeof get);
        parse_qs(path, "heartbeat", hb, sizeof hb);
        parse_qs(path, "session", session, sizeof session);
        parse_qs(path, "agent", agent, sizeof agent);
        parse_qs(path, "role", role, sizeof role);
        if (reg[0] && !strcmp(reg, "1"))
            return exo_agent_register(exo, session[0]?session:0, agent[0]?agent:0, role[0]?role:0, out, cap);
        if (list[0] && !strcmp(list, "1"))
            return exo_agent_list(exo, session[0]?session:0, out, cap);
        if (get[0] && !strcmp(get, "1"))
            return exo_agent_get(exo, session[0]?session:0, agent[0]?agent:0, out, cap);
        if (hb[0] && !strcmp(hb, "1"))
            return exo_agent_heartbeat(exo, session[0]?session:0, agent[0]?agent:0, out, cap);
        return snprintf(out, cap, "error: specify register=1, list=1, get=1, or heartbeat=1"), 1;
    }

    /* Session management */
    if (!strcmp(p, "/session")) {
        char set[8]="", clear[8]="", get[8]="", session[64]="", agent[64]="";
        parse_qs(path, "set", set, sizeof set);
        parse_qs(path, "clear", clear, sizeof clear);
        parse_qs(path, "get", get, sizeof get);
        parse_qs(path, "session", session, sizeof session);
        parse_qs(path, "agent", agent, sizeof agent);
        if (set[0] && !strcmp(set, "1"))
            return exo_session_ctx_set(exo, session[0]?session:0, agent[0]?agent:0, out, cap);
        if (clear[0] && !strcmp(clear, "1"))
            return exo_session_ctx_clear(out, cap);
        if (get[0] && !strcmp(get, "1"))
            return exo_session_ctx_get(out, cap);
        return snprintf(out, cap, "error: specify set=1, clear=1, or get=1"), 1;
    }

    /* Flow enforcement */
    if (!strcmp(p, "/flow")) {
        char enable[8]="", disable[8]="", status[8]="", validate[8]="", pdca[64]="", flow[64]="", create[8]="", steps[512]="", workers[512]="";
        parse_qs(path, "enable", enable, sizeof enable);
        parse_qs(path, "disable", disable, sizeof disable);
        parse_qs(path, "status", status, sizeof status);
        parse_qs(path, "validate", validate, sizeof validate);
        parse_qs(path, "pdca", pdca, sizeof pdca);
        parse_qs(path, "flow", flow, sizeof flow);
        parse_qs(path, "create", create, sizeof create);
        parse_qs(path, "steps", steps, sizeof steps);
        parse_qs(path, "workers", workers, sizeof workers);
        if (enable[0] && !strcmp(enable, "1")) return cpi_flow_enforcement(1), snprintf(out, cap, "ok flow enforcement enabled"), 0;
        if (disable[0] && !strcmp(disable, "1")) return cpi_flow_enforcement(0), snprintf(out, cap, "ok flow enforcement disabled"), 0;
        if (status[0] && !strcmp(status, "1")) return cpi_flow_status(out, cap);
        if (validate[0] && !strcmp(validate, "1")) return cpi_validate_flow(exo, pdca[0]?pdca:0, out, cap);
        if (create[0] && !strcmp(create, "1"))
            return cpi_create_enforced_flow(exo, pdca[0]?pdca:0, steps[0]?steps:0, workers[0]?workers:0, out, cap);
        return snprintf(out, cap, "error: specify enable=1, disable=1, status=1, validate=1, or create=1"), 1;
    }

    /* Simple usage validation */
    if (!strcmp(p, "/simple")) {
        char validate[8]="", suggest[8]="", bare[8]="";
        parse_qs(path, "validate", validate, sizeof validate);
        parse_qs(path, "suggest", suggest, sizeof suggest);
        parse_qs(path, "bare", bare, sizeof bare);
        if (validate[0] && !strcmp(validate, "1"))
            return cpi_validate_simple_usage("exocpi", 0, NULL, out, cap);
        if (suggest[0] && !strcmp(suggest, "1"))
            return cpi_suggest_simpler("", out, cap);
        if (bare[0] && !strcmp(bare, "1"))
            return snprintf(out, cap, "ok bare=%d", cpi_is_bare_command(0, NULL)), 0;
        return snprintf(out, cap, "error: specify validate=1, suggest=1, or bare=1"), 1;
    }

    return snprintf(out, cap, "error: unknown operation %s", p), 1;
}

int main(int argc, char **argv)
{
    const char *console_path = NULL;
    const char *body_arg = "";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--exomind") && i + 1 < argc) {
            setenv("EXO_EXOMIND_URL", argv[++i], 1);
        } else if (!strcmp(a, "--exosched") && i + 1 < argc) {
            setenv("EXO_EXOSCHED_URL", argv[++i], 1);
        } else if (!strcmp(a, "--exoflow") && i + 1 < argc) {
            setenv("EXO_EXOFLOW_URL", argv[++i], 1);
        } else if (!strcmp(a, "--exoqms") && i + 1 < argc) {
            setenv("EXO_EXOQMS_URL", argv[++i], 1);
        } else if (!strcmp(a, "--exokit") && i + 1 < argc) {
            /* ignored - we use local functions */
        } else if (!strcmp(a, "--kit") && i + 1 < argc) {
            /* ignored - we use local functions */
        } else if (!strcmp(a, "--json")) {
            /* ignored for now */
        } else if (!strcmp(a, "--body") && i + 1 < argc) {
            /* body not used in local mode */
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("exocpi v%s\n", EXOMIND_VERSION);
            return 0;
        } else if (a[0] == '/') {
            console_path = a;
        } else {
            fprintf(stderr, "exocpi: unknown argument %s\n", a);
            return 1;
        }
    }

    if (!console_path) {
        usage(argv[0]);
        return 0;
    }

    if (connect_backends() != 0)
        return 1;

    char out[65536];
    int rc = cpi_dispatch(g_exo, console_path, body_arg, out, sizeof out);

    if (g_json) {
        printf("{\"result\":%s,\"exit\":%d}\n", out[0] ? out : "\"\"", rc);
    } else {
        printf("%s", out);
    }

    if (g_exo) free(g_exo);
    return rc;
}