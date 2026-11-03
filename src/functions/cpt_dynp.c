// cpt_dynp(series LIST(DOUBLE), n_bkps BIGINT [, cost VARCHAR [, min_size BIGINT]]) -> LIST(INTEGER)

#include "cpt_call.h"
#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static int run_dynp(duckdb_data_chunk input, idx_t row, const cpt_cost *cost, const cpt_series_args *args,
                    cpt_budget *budget, cpt_bkps *out, char *err) {
	if (!cpt_arg_present(input, 1, row)) {
		snprintf(err, CPT_ERR_LEN, "cpt_dynp: n_bkps must not be NULL");
		return 1;
	}
	// a negative n_bkps is rejected by cpt_dynp
	return cpt_dynp(cost, args->n, cpt_arg_bigint(input, 1, row, 0), args->min_size, budget, out, err);
}

static void DynpFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	cpt_execute_bkps(info, input, output, "cpt_dynp", (cpt_arg_layout) {2, 3}, run_dynp);
}

bool cpt_register_dynp(duckdb_connection connection) {
	duckdb_logical_type list_double = cpt_type_list_double();
	duckdb_logical_type list_int = cpt_type_list_integer();
	duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);

	duckdb_scalar_function_set set = duckdb_create_scalar_function_set("cpt_dynp");
	duckdb_logical_type params[] = {list_double, bigint, varchar, bigint};
	bool ok = true;
	for (idx_t k = 2; k <= 4; k++) {
		ok = ok && cpt_add_overload(set, "cpt_dynp", params, k, list_int, DynpFunction);
	}
	ok = ok && duckdb_register_scalar_function_set(connection, set) == DuckDBSuccess;

	duckdb_destroy_scalar_function_set(&set);
	duckdb_destroy_logical_type(&list_double);
	duckdb_destroy_logical_type(&list_int);
	duckdb_destroy_logical_type(&varchar);
	duckdb_destroy_logical_type(&bigint);
	return ok;
}
