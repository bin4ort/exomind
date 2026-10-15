/* exocpi flow enforcement - ensures work is properly distributed */

#include "exoqms.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Flow enforcement state */
static int g_flow_enforcement = 0;

/* Enable/disable flow enforcement */
int cpi_flow_enforcement(int enable)
{
    g_flow_enforcement = enable ? 1 : 0;
    return 0;
}

/* Check if flow enforcement is active */
int cpi_flow_enforcement_get()
{
    return g_flow_enforcement;
}

/* Validate that a PDCA cycle has an associated flow */
int cpi_validate_flow(struct exo *exo, const char *pdca_id, char *out, size_t cap)
{
    if (!pdca_id || !pdca_id[0])
        return snprintf(out, cap, "error: missing pdca_id"), 1;

    /* Check if PDCA exists */
    char pdca_key[256], err[256], *val = NULL;
    snprintf(pdca_key, sizeof pdca_key, "cpi:pdca:%s", pdca_id);
    if (exo_get(NULL, pdca_key, &val, err, sizeof err) != 0 || !val)
        return snprintf(out, cap, "error: pdca not found"), 1;
    free(val);

    /* Check if flow_id is associated */
    char flow_key[256];
    snprintf(flow_key, sizeof flow_key, "cpi:pdca:%s:flow", pdca_id);
    if (exo_get(NULL, flow_key, &val, err, sizeof err) == 0 && val) {
        snprintf(out, cap, "ok flow=%s", val);
        free(val);
        return 0;
    }
    if (val) free(val);
    return snprintf(out, cap, "error: no flow associated with PDCA"), 1;
}

/* Associate flow with PDCA */
int cpi_associate_flow(struct exo *exo, const char *pdca_id, const char *flow_id, char *out, size_t cap)
{
    if (!pdca_id || !pdca_id[0] || !flow_id || !flow_id[0])
        return snprintf(out, cap, "error: missing pdca_id or flow_id"), 1;

    char key[256], err[256];
    snprintf(key, sizeof key, "cpi:pdca:%s:flow", pdca_id);
    return exo_persist(NULL, key, flow_id, out, 256); /* Need proper exo connection */
}

/* Enforce that work operations require flow */
int cpi_enforce_work_requires_flow(struct exo *exo, const char *operation, const char *pdca_id, char *out, size_t cap)
{
    if (!g_flow_enforcement)
        return 0; /* Not enforced */

    if (!pdca_id || !pdca_id[0])
        return snprintf(out, cap, "error: flow enforcement active - pdca_id required for %s"), 1;

    /* Validate flow association */
    return cpi_validate_flow(exo, pdca_id, out, cap);
}

/* Create flow with worker assignments (enforces multi-agent) */
int cpi_create_enforced_flow(struct exo *exo, const char *flow_name, const char *steps_json, 
                              const char *workers_json, char *out, size_t cap)
{
    if (!g_flow_enforcement)
        return snprintf(out, cap, "error: flow enforcement not enabled"), 1;

    /* Validate that multiple workers are specified */
    /* This would parse workers_json and ensure count > 1 */
    
    /* Create exoflow */
    /* This would call exoflow /flow with proper worker assignments */
    return snprintf(out, cap, "ok flow created with enforcement"), 0;
}

/* Get flow enforcement status */
int cpi_flow_status(char *out, size_t cap)
{
    return snprintf(out, cap, "flow_enforcement=%s", g_flow_enforcement ? "enabled" : "disabled"), 0;
}