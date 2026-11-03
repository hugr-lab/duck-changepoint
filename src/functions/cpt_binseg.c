// cpt_binseg(series LIST(DOUBLE)
//            [, cost VARCHAR [, n_bkps BIGINT [, penalty DOUBLE [, min_size BIGINT]]]]) -> LIST(INTEGER)

#include "cpt_call.h"
#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

// The stop rule: n_bkps splits (column 2) or a penalty (column 3), BIC when neither is given.
// *n_bkps is -1 when the penalty rule applies.
static int binseg_stop_rule(duckdb_data_chunk input, idx_t row, int64_t n, int p, int64_t *n_bkps, double *pen,
                            char *err) {
	bool has_n_bkps = cpt_arg_present(input, 2, row);
	bool has_pen = cpt_arg_present(input, 3, row);
	if (has_n_bkps && has_pen) {
		snprintf(err, CPT_ERR_LEN, "cpt_binseg: give either n_bkps or penalty, not both");
		return 1;
	}
	*n_bkps = cpt_arg_bigint(input, 2, row, -1);
	if (has_n_bkps && *n_bkps < 0) {
		snprintf(err, CPT_ERR_LEN, "n_bkps must be >= 0, got %lld", (long long)*n_bkps);
		return 1;
	}
	bool unused;
	*pen = has_pen ? cpt_arg_double(input, 3, row, 0, &unused) : cpt_penalty_named(CPT_PEN_BIC, n, p).beta;
	return cpt_check_penalty(*pen, "penalty", err);
}

static int run_binseg(duckdb_data_chunk input, idx_t row, const cpt_cost *cost, const cpt_series_args *args,
                      cpt_budget *budget, cpt_bkps *out, char *err) {
	int64_t n_bkps;
	double pen;
	if (binseg_stop_rule(input, row, args->n, cost->n_params, &n_bkps, &pen, err)) {
		return 1;
	}
	return cpt_binseg(cost, args->n, n_bkps, pen, args->min_size, budget, out, err);
}

static void BinsegFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	cpt_execute_bkps(info, input, output, "cpt_binseg", (cpt_arg_layout) {1, 4}, run_binseg);
}

bool cpt_register_binseg(duckdb_connection connection) {
	duckdb_logical_type list_double = cpt_type_list_double();
	duckdb_logical_type list_int = cpt_type_list_integer();
	duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);

	duckdb_scalar_function_set set = duckdb_create_scalar_function_set("cpt_binseg");
	duckdb_logical_type params[] = {list_double, varchar, bigint, dbl, bigint};
	bool ok = true;
	for (idx_t k = 1; k <= 5; k++) {
		ok = ok && cpt_add_overload(set, "cpt_binseg", params, k, list_int, BinsegFunction);
	}
	ok = ok && duckdb_register_scalar_function_set(connection, set) == DuckDBSuccess;

	duckdb_destroy_scalar_function_set(&set);
	duckdb_destroy_logical_type(&list_double);
	duckdb_destroy_logical_type(&list_int);
	duckdb_destroy_logical_type(&varchar);
	duckdb_destroy_logical_type(&dbl);
	duckdb_destroy_logical_type(&bigint);
	return ok;
}
