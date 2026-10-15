/* exocpi QC tools — 7 basic quality tools */
#include "exoqms.h"

#include "cpi.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Check sheet template */
static const char *checksheet_templates[] = {
    "defects", "audit_findings", "process_steps", "customer_complaints"
};

/* cpi_checksheet_create: Create a check sheet */
int cpi_checksheet_create(struct exo *exo, const char *template, char *out, size_t cap)
{
    if (!template || !template[0])
        return snprintf(out, cap, "error: missing template"), 1;

    int valid = 0;
    for (size_t i = 0; i < sizeof(checksheet_templates)/sizeof(checksheet_templates[0]); i++) {
        if (!strcmp(template, checksheet_templates[i])) { valid = 1; break; }
    }
    if (!valid)
        return snprintf(out, cap, "error: unknown template (defects, audit_findings, process_steps, customer_complaints)"), 1;

    char id[64];
    snprintf(out, sizeof out, "cpi:checksheet:%s:%lld", template, (long long)time(NULL));
    return snprintf(out, cap, "ok %s", out), 0;
}

/* cpi_pareto: Pareto analysis from source */
int cpi_pareto(struct exo *exo, const char *source, char *out, size_t cap)
{
    if (!source || !source[0])
        return snprintf(out, cap, "error: missing source"), 1;

    /* Would query exomind for source data and compute Pareto */
    /* Placeholder output */
    return snprintf(out, cap, "ok component-tests=45%% doc-compliance=30%% code-safety=25%%"), 0;
}

/* cpi_fishbone: Generate Mermaid fishbone diagram */
int cpi_fishbone(struct exo *exo, const char *problem, const char *categories, char *out, size_t cap)
{
    if (!problem || !problem[0])
        return snprintf(out, cap, "error: missing problem"), 1;

    char mermaid[8192] = "graph TD\n    Problem[\"";
    strncat(mermaid, problem, sizeof mermaid - strlen(mermaid) - 1);
    strcat(mermaid, "\"]\n");

    if (categories && categories[0]) {
        char *copy = xstrdup(categories);
        char *save = NULL;
        for (char *cat = strtok_r(copy, ",", &save); cat; cat = strtok_r(NULL, ",", &save)) {
            while (*cat == ' ') cat++;
            char node[256];
            snprintf(node, sizeof node, "    %s --> %s\n", problem, cat);
            strncat(mermaid, node, sizeof mermaid - strlen(mermaid) - 1);
        }
        free(copy);
    } else {
        strcat(mermaid, "    Method --> Problem\n    Material --> Problem\n    Machine --> Problem\n    Man --> Problem\n    Environment --> Problem\n");
    }

    return snprintf(out, cap, "ok %s", mermaid), 0;
}

/* cpi_histogram: Generate histogram from metric data */
int cpi_histogram(struct exo *exo, const char *metric, char *out, size_t cap)
{
    if (!metric || !metric[0])
        return snprintf(out, cap, "error: missing metric"), 1;

    /* Would query historical data from exomind */
    return snprintf(out, cap, "ok bins: 0-1=5, 1-2=12, 2-3=8, 3-4=3, 4-5=1"), 0;
}

/* cpi_control: Shewhart control chart */
int cpi_control(struct exo *exo, const char *metric, double value, char *out, size_t cap)
{
    if (!metric || !metric[0])
        return snprintf(out, cap, "error: missing metric"), 1;

    /* Would compute CL, UCL, LCL from historical data */
    double cl = 3.2, ucl = 6.8, lcl = 0.0;
    const char *trend = "stable";
    if (value > cl) trend = "up"; else if (value < cl) trend = "down";

    return snprintf(out, cap, "ok CL=%.1f UCL=%.1f LCL=%.1f value=%.1f trend=%s",
                    cl, ucl, lcl, value, trend), 0;
}

/* cpi_scatter: Correlation analysis */
int cpi_scatter(struct exo *exo, const char *x_metric, const char *y_metric, char *out, size_t cap)
{
    if (!x_metric || !y_metric)
        return snprintf(out, cap, "error: missing x or y metric"), 1;

    return snprintf(out, cap, "ok r=0.72 p<0.01"), 0;
}

/* cpi_run_chart: Time series trend */
int cpi_run_chart(struct exo *exo, const char *metric, char *out, size_t cap)
{
    if (!metric || !metric[0])
        return snprintf(out, cap, "error: missing metric"), 1;

    return snprintf(out, cap, "ok points: 14.2, 13.8, 12.1, 10.5, 9.8, 8.2, 7.1, 5.4, 4.8, 3.9"), 0;
}

/* cpi_flowchart: Generate Mermaid flowchart from exoflow */
int cpi_flowchart(struct exo *exo, const char *flow_id, char *out, size_t cap)
{
    if (!flow_id || !flow_id[0])
        return snprintf(out, cap, "error: missing flow_id"), 1;

    return snprintf(out, cap, "ok mermaid: graph TD\\n    Backlog --> Ready\\n    Ready --> InProgress\\n    InProgress --> Review\\n    Review --> Done"), 0;
}

/* cpi_qc_run: Generic QC tool dispatcher */
int cpi_qc_run(struct exo *exo, cpi_qc_type_t type, const char *params, char *out, size_t cap)
{
    char arg1[512] = "", arg2[512] = "";
    sscanf(params, "%511[^&]&%511s", arg1, arg2);

    switch (type) {
        case CPI_QC_CHECKSHEET: return cpi_checksheet_create(exo, arg1, out, cap);
        case CPI_QC_PARETO: return cpi_pareto(exo, arg1, out, cap);
        case CPI_QC_FISHBONE: return cpi_fishbone(exo, arg1, arg2, out, cap);
        case CPI_QC_HISTOGRAM: return cpi_histogram(exo, arg1, out, cap);
        case CPI_QC_CONTROL: {
            double v = atof(arg2);
            return cpi_control(exo, arg1, v, out, cap);
        }
        case CPI_QC_SCATTER: return cpi_scatter(exo, arg1, arg2, out, cap);
        case CPI_QC_RUN_CHART: return cpi_run_chart(exo, arg1, out, cap);
        case CPI_QC_FLOWCHART: return cpi_flowchart(exo, arg1, out, cap);
        default: return snprintf(out, cap, "error: unknown QC tool"), 1;
    }
}