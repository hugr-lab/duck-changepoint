// cpt_pelt(series LIST(DOUBLE)
//          [, cost VARCHAR [, penalty DOUBLE | penalty_name VARCHAR [, min_size BIGINT]]]) -> LIST(INTEGER)

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

// Resolve the penalty argument at column 2: DOUBLE value, 'bic'/'mbic'/'aic', or NULL -> BIC.
static int pelt_penalty(duckdb_data_chunk input, idx_t row, int64_t n, int p, cpt_penalty *pen, char *err) {
	*pen = cpt_penalty_named(CPT_PEN_BIC, n, p);
	if (duckdb_data_chunk_get_column_count(input) < 3) {
		return 0;
	}
	duckdb_vector vec = duckdb_data_chunk_get_vector(input, 2);
	if (!cpt_row_is_valid(vec, row)) {
		return 0;
	}
	duckdb_logical_type type = duckdb_vector_get_column_type(vec);
	duckdb_type id = duckdb_get_type_id(type);
	duckdb_destroy_logical_type(&type);
	if (id == DUCKDB_TYPE_VARCHAR) {
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
	double beta = ((double *)duckdb_vector_get_data(vec))[row];
	if (!(beta >= 0) || beta - beta != 0) {
		snprintf(err, CPT_ERR_LEN, "penalty must be a finite number >= 0, got %g", beta);
		return 1;
	}
	pen->beta = beta;
	pen->mbic = 0;
	return 0;
}

static void PeltFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
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
		    cpt_arg_min_size(input, 3, row, &min_size, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_cost cost;
		if (cpt_cost_create(kind, x, n, &cost, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_penalty pen;
		cpt_bkps bkps;
		cpt_budget budget = cpt_function_budget(info);
		int rc = pelt_penalty(input, row, n, cost.n_params, &pen, err);
		if (rc == 0) {
			rc = cpt_pelt(&cost, n, pen, min_size, &budget, &bkps, err);
		}
		cpt_cost_destroy(&cost);
		if (rc) {
			cpt_report_error(info, "cpt_pelt", rc, err);
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
