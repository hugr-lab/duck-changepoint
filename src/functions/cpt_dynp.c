// cpt_dynp(series LIST(DOUBLE), n_bkps BIGINT [, cost VARCHAR [, min_size BIGINT]]) -> LIST(INTEGER)

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static void DynpFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t size = duckdb_data_chunk_get_size(input);
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	duckdb_vector n_bkps_vec = duckdb_data_chunk_get_vector(input, 1);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < size; row++) {
		if (!cpt_row_is_valid(series, row)) {
			cpt_write_null_list(output, row, offset);
			continue;
		}
		if (!cpt_row_is_valid(n_bkps_vec, row)) {
			duckdb_scalar_function_set_error(info, "cpt_dynp: n_bkps must not be NULL");
			return;
		}
		int64_t n_bkps = ((int64_t *)duckdb_vector_get_data(n_bkps_vec))[row];
		if (n_bkps < 0) {
			snprintf(err, CPT_ERR_LEN, "n_bkps must be >= 0, got %lld", (long long)n_bkps);
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		const double *x;
		int64_t n, min_size;
		cpt_cost_kind kind;
		if (cpt_read_series(series, row, &x, &n, err) || cpt_arg_cost(input, 2, row, &kind, err) ||
		    cpt_arg_min_size(input, 3, row, &min_size, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_cost cost;
		if (cpt_cost_create(kind, x, n, &cost, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_bkps bkps;
		int rc = cpt_dynp(&cost, n, n_bkps, min_size, &bkps, err);
		cpt_cost_destroy(&cost);
		if (rc) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		rc = cpt_write_int_list(output, row, &offset, bkps.pos, bkps.count, err);
		cpt_bkps_free(&bkps);
		if (rc) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
	}
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
