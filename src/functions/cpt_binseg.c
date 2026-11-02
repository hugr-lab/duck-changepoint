// cpt_binseg(series LIST(DOUBLE)
//            [, cost VARCHAR [, n_bkps BIGINT [, penalty DOUBLE [, min_size BIGINT]]]]) -> LIST(INTEGER)

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static void BinsegFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t size = duckdb_data_chunk_get_size(input);
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < size; row++) {
		if (!cpt_row_is_valid(series, row)) {
			cpt_write_null_list(output, row, offset);
			continue;
		}
		const double *x;
		int64_t n, min_size;
		cpt_cost_kind kind;
		if (cpt_read_series(series, row, &x, &n, err) || cpt_arg_cost(input, 1, row, &kind, err) ||
		    cpt_arg_min_size(input, 4, row, &min_size, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		bool has_n_bkps = duckdb_data_chunk_get_column_count(input) > 2 &&
		                  cpt_row_is_valid(duckdb_data_chunk_get_vector(input, 2), row);
		int64_t n_bkps = cpt_arg_bigint(input, 2, row, -1);
		bool pen_null;
		double pen = cpt_arg_double(input, 3, row, 0, &pen_null);
		if (has_n_bkps && !pen_null) {
			duckdb_scalar_function_set_error(info, "cpt_binseg: give either n_bkps or penalty, not both");
			return;
		}
		if (has_n_bkps && n_bkps < 0) {
			snprintf(err, CPT_ERR_LEN, "n_bkps must be >= 0, got %lld", (long long)n_bkps);
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		if (!pen_null && (!(pen >= 0) || pen - pen != 0)) {
			snprintf(err, CPT_ERR_LEN, "penalty must be a finite number >= 0, got %g", pen);
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_cost cost;
		if (cpt_cost_create(kind, x, n, &cost, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		if (!has_n_bkps && pen_null) {
			pen = cpt_penalty_named(CPT_PEN_BIC, n, cost.n_params).beta;
		}
		cpt_bkps bkps;
		cpt_budget budget = cpt_function_budget(info);
		int rc = cpt_binseg(&cost, n, has_n_bkps ? n_bkps : -1, pen, min_size, &budget, &bkps, err);
		cpt_cost_destroy(&cost);
		if (rc) {
			cpt_report_error(info, "cpt_binseg", rc, err);
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
