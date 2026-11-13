#include "duckdb.h"

#include "cpt_functions.h"
#include "cpt_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	            "counts log2(n)); 0 = no limit. A running call also stops when the query is interrupted.");
	bool ok = duckdb_register_config_option(connection, option) == DuckDBSuccess;
	duckdb_destroy_config_option(&option);
	return ok;
}

// What a bind captures for the query: the work limit and the query itself, for interrupts.
typedef struct {
	int64_t max_evals;
	void *query; // cpt_query.h
} bind_data;

static void *copy_bind_data(void *data) {
	bind_data *copy = (bind_data *)malloc(sizeof(bind_data));
	if (copy) {
		*copy = *(bind_data *)data;
	}
	return copy;
}

static int64_t max_evaluations(duckdb_client_context context) {
	duckdb_config_option_scope scope;
	duckdb_value value = context ? duckdb_client_context_get_config_option(context, CPT_MAX_EVALUATIONS_SETTING, &scope)
	                             : NULL;
	int64_t max_evals = value ? duckdb_get_int64(value) : CPT_MAX_EVALUATIONS_DEFAULT;
	duckdb_destroy_value(&value);
	return max_evals;
}

// Bind: capture cpt_max_evaluations and the query.
static void BindBudget(duckdb_bind_info info) {
	bind_data *data = (bind_data *)malloc(sizeof(bind_data));
	if (!data) {
		duckdb_scalar_function_bind_set_error(info, "cpt: out of memory");
		return;
	}
	duckdb_client_context context = NULL;
	duckdb_scalar_function_get_client_context(info, &context);
	data->max_evals = max_evaluations(context);
	data->query = cpt_query_context(context);
	duckdb_destroy_client_context(&context);
	duckdb_scalar_function_set_bind_data(info, data, free);
	duckdb_scalar_function_set_bind_data_copy(info, copy_bind_data);
}

cpt_budget cpt_function_budget(duckdb_function_info info) {
	cpt_budget budget = {CPT_MAX_EVALUATIONS_DEFAULT, 0, cpt_query_interrupted, NULL};
	bind_data *data = (bind_data *)duckdb_scalar_function_get_bind_data(info);
	if (data) {
		budget.max_evals = data->max_evals;
		budget.interrupt_ctx = data->query;
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
		snprintf(msg, sizeof(msg), "%s: %s; raise or remove it with SET " CPT_MAX_EVALUATIONS_SETTING " (0 = no limit).%s",
		         function, err, budget_hint(function));
	} else if (rc == CPT_INTERRUPTED) {
		snprintf(msg, sizeof(msg), "%s: %s", function, err);
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
    cpt_register_dynp,     cpt_register_crops, cpt_register_segment_stats, cpt_register_online,
};

bool cpt_register_all(duckdb_connection connection) {
	for (size_t i = 0; i < sizeof(REGISTRATIONS) / sizeof(REGISTRATIONS[0]); i++) {
		if (!REGISTRATIONS[i](connection)) {
			return false;
		}
	}
	return true;
}
