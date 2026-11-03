// cpt_pelt(series LIST(DOUBLE)
//          [, cost VARCHAR [, penalty DOUBLE | penalty_name VARCHAR [, min_size BIGINT]]]) -> LIST(INTEGER)

#include "cpt_call.h"
#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static int penalty_from_name(duckdb_vector vec, idx_t row, int64_t n, int p, cpt_penalty *pen, char *err) {
	const char *name;
	size_t len;
	cpt_read_varchar(vec, row, &name, &len);
	int kind = cpt_penalty_parse(name, len);
	if (kind < 0) {
		snprintf(err, CPT_ERR_LEN, "unknown penalty '%.*s'; expected 'bic', 'mbic', 'aic' or a number",
		         (int)(len > 64 ? 64 : len), name);
		return 1;
	}
	*pen = cpt_penalty_named((cpt_penalty_kind)kind, n, p);
	return 0;
}

static int is_varchar(duckdb_vector vec) {
	duckdb_logical_type type = duckdb_vector_get_column_type(vec);
	duckdb_type id = duckdb_get_type_id(type);
	duckdb_destroy_logical_type(&type);
	return id == DUCKDB_TYPE_VARCHAR;
}

// The penalty argument at column 2: a DOUBLE value, 'bic' / 'mbic' / 'aic', or NULL / absent for BIC.
static int pelt_penalty(duckdb_data_chunk input, idx_t row, int64_t n, int p, cpt_penalty *pen, char *err) {
	*pen = cpt_penalty_named(CPT_PEN_BIC, n, p);
	if (!cpt_arg_present(input, 2, row)) {
		return 0;
	}
	duckdb_vector vec = duckdb_data_chunk_get_vector(input, 2);
	if (is_varchar(vec)) {
		return penalty_from_name(vec, row, n, p, pen, err);
	}
	double beta = ((double *)duckdb_vector_get_data(vec))[row];
	*pen = (cpt_penalty) {beta, 0};
	return cpt_check_penalty(beta, "penalty", err);
}

static int run_pelt(duckdb_data_chunk input, idx_t row, const cpt_cost *cost, const cpt_series_args *args,
                    cpt_budget *budget, cpt_bkps *out, char *err) {
	cpt_penalty pen;
	if (pelt_penalty(input, row, args->n, cost->n_params, &pen, err)) {
		return 1;
	}
	return cpt_pelt(cost, args->n, pen, args->min_size, budget, out, err);
}

static void PeltFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	cpt_execute_bkps(info, input, output, "cpt_pelt", (cpt_arg_layout) {1, 3}, run_pelt);
}

bool cpt_register_pelt(duckdb_connection connection) {
	duckdb_logical_type list_double = cpt_type_list_double();
	duckdb_logical_type list_int = cpt_type_list_integer();
	duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);

	duckdb_scalar_function_set set = duckdb_create_scalar_function_set("cpt_pelt");
	duckdb_logical_type with_value[] = {list_double, varchar, dbl, bigint};
	duckdb_logical_type with_name[] = {list_double, varchar, varchar, bigint};
	bool ok = true;
	for (idx_t k = 1; k <= 4; k++) {
		ok = ok && cpt_add_overload(set, "cpt_pelt", with_value, k, list_int, PeltFunction);
	}
	for (idx_t k = 3; k <= 4; k++) {
		ok = ok && cpt_add_overload(set, "cpt_pelt", with_name, k, list_int, PeltFunction);
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
