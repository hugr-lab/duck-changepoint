#pragma once

#include "duckdb_extension.h"

// The C API has no DEFAULT parameters, so every function is registered as a scalar function set
// with one overload per trailing-argument count. All overloads of a function share one callback,
// which checks the column count of the input chunk.

// Add one overload `name(params...) -> ret` to `set`. Returns false on failure.
bool cpt_add_overload(duckdb_scalar_function_set set, const char *name, duckdb_logical_type *params, idx_t n_params,
                      duckdb_logical_type ret, duckdb_scalar_function_t fn);

#include "algo.h"

// Setting that bounds the work of one call (one series): see cpt_budget in algo.h.
#define CPT_MAX_EVALUATIONS_SETTING "cpt_max_evaluations"
#define CPT_MAX_EVALUATIONS_DEFAULT 1000000000LL

bool cpt_register_settings(duckdb_connection connection);

// Fresh budget for one series, from the setting captured at bind time.
cpt_budget cpt_function_budget(duckdb_function_info info);

// Report an algorithm failure. rc == CPT_BUDGET_EXCEEDED gets a hint about the setting.
void cpt_report_error(duckdb_function_info info, const char *function, int rc, const char *err);

bool cpt_register_pelt(duckdb_connection connection);
bool cpt_register_binseg(duckdb_connection connection);
bool cpt_register_dynp(duckdb_connection connection);
bool cpt_register_crops(duckdb_connection connection);
bool cpt_register_segment_stats(duckdb_connection connection);
bool cpt_register_online(duckdb_connection connection);
