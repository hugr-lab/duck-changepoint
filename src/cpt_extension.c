#include "duckdb_extension.h"

#include "cpt_functions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

//===--------------------------------------------------------------------===//
// cpt_max_evaluations
//===--------------------------------------------------------------------===//

bool cpt_register_settings(duckdb_connection connection) {
	duckdb_config_option option = duckdb_create_config_option();
	duckdb_config_option_set_name(option, CPT_MAX_EVALUATIONS_SETTING);
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_config_option_set_type(option, bigint);
	duckdb_destroy_logical_type(&bigint);
	duckdb_value def = duckdb_create_int64(CPT_MAX_EVALUATIONS_DEFAULT);
	duckdb_config_option_set_default_value(option, def);
	duckdb_destroy_value(&def);
	duckdb_config_option_set_description(
	    option, "Maximum work per cpt_* call on one series, in segment cost evaluations (an l1 evaluation "
	            "counts log2(n)); 0 = no limit. Bounds run time, since a running call cannot be interrupted.");
	bool ok = duckdb_register_config_option(connection, option) == DuckDBSuccess;
	duckdb_destroy_config_option(&option);
	return ok;
}

static void *copy_int64(void *data) {
	int64_t *copy = (int64_t *)malloc(sizeof(int64_t));
	if (copy) {
		*copy = *(int64_t *)data;
	}
	return copy;
}

// Bind: capture cpt_max_evaluations for this query.
static void BindBudget(duckdb_bind_info info) {
	int64_t max_evals = CPT_MAX_EVALUATIONS_DEFAULT;
	duckdb_client_context context = NULL;
	duckdb_scalar_function_get_client_context(info, &context);
	if (context) {
		duckdb_config_option_scope scope;
		duckdb_value value = duckdb_client_context_get_config_option(context, CPT_MAX_EVALUATIONS_SETTING, &scope);
		if (value) {
			max_evals = duckdb_get_int64(value);
			duckdb_destroy_value(&value);
		}
		duckdb_destroy_client_context(&context);
	}
	int64_t *data = (int64_t *)malloc(sizeof(int64_t));
	if (!data) {
		duckdb_scalar_function_bind_set_error(info, "cpt: out of memory");
		return;
	}
	*data = max_evals;
	duckdb_scalar_function_set_bind_data(info, data, free);
	duckdb_scalar_function_set_bind_data_copy(info, copy_int64);
}

cpt_budget cpt_function_budget(duckdb_function_info info) {
	cpt_budget budget = {CPT_MAX_EVALUATIONS_DEFAULT, 0};
	int64_t *data = (int64_t *)duckdb_scalar_function_get_bind_data(info);
	if (data) {
		budget.max_evals = *data;
	}
	return budget;
}

// What to try instead when a function hits the work limit.
// A string literal, not a const variable: MSVC (C2099) only accepts constant expressions here.
#define CPT_PELT_HINT " PELT approaches O(n^2) on long series with few change points; cpt_binseg is O(n log n)."

static const char *budget_hint(const char *function) {
	static const struct {
		const char *function;
		const char *hint;
	} HINTS[] = {{"cpt_pelt", CPT_PELT_HINT},
	             {"cpt_crops", CPT_PELT_HINT},
	             {"cpt_dynp", " cpt_dynp is O(n_bkps * n^2); cpt_pelt or cpt_binseg scale better."}};
	for (size_t i = 0; i < sizeof(HINTS) / sizeof(HINTS[0]); i++) {
		if (strcmp(function, HINTS[i].function) == 0) {
			return HINTS[i].hint;
		}
	}
	return "";
}

void cpt_report_error(duckdb_function_info info, const char *function, int rc, const char *err) {
	char msg[CPT_ERR_LEN * 3];
	if (rc == CPT_BUDGET_EXCEEDED) {
		snprintf(msg, sizeof(msg),
		         "%s: %s. A running cpt_* call cannot be interrupted, so its work is capped by "
		         "SET " CPT_MAX_EVALUATIONS_SETTING " (0 = no limit).%s",
		         function, err, budget_hint(function));
	} else {
		snprintf(msg, sizeof(msg), "%s", err);
	}
	duckdb_scalar_function_set_error(info, msg);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

bool cpt_add_overload(duckdb_scalar_function_set set, const char *name, duckdb_logical_type *params, idx_t n_params,
                      duckdb_logical_type ret, duckdb_scalar_function_t fn) {
	duckdb_scalar_function f = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(f, name);
	for (idx_t i = 0; i < n_params; i++) {
		duckdb_scalar_function_add_parameter(f, params[i]);
	}
	duckdb_scalar_function_set_return_type(f, ret);
	duckdb_scalar_function_set_function(f, fn);
	duckdb_scalar_function_set_bind(f, BindBudget);
	// NULL arguments mean "use the default" for optional parameters, so the callback sees them.
	duckdb_scalar_function_set_special_handling(f);
	bool ok = duckdb_add_scalar_function_to_set(set, f) == DuckDBSuccess;
	duckdb_destroy_scalar_function(&f);
	return ok;
}

static bool (*const REGISTRATIONS[])(duckdb_connection) = {
    cpt_register_settings, cpt_register_pelt,  cpt_register_binseg,
    cpt_register_dynp,     cpt_register_crops, cpt_register_segment_stats,
};

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info,
                            struct duckdb_extension_access *access) {
	for (size_t i = 0; i < sizeof(REGISTRATIONS) / sizeof(REGISTRATIONS[0]); i++) {
		if (!REGISTRATIONS[i](connection)) {
			access->set_error(info, "cpt: failed to register functions");
			return false;
		}
	}
	return true;
}
