#include "cpt_call.h"

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

int cpt_check_penalty(double pen, const char *what, char *err) {
	if (!(pen >= 0) || pen - pen != 0) { // negative, NaN or infinite
		snprintf(err, CPT_ERR_LEN, "%s must be a finite number >= 0, got %g", what, pen);
		return 1;
	}
	return 0;
}

int cpt_prepare(duckdb_data_chunk input, idx_t row, cpt_arg_layout layout, cpt_series_args *args, cpt_cost *cost,
                char *err) {
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	if (cpt_read_series(series, row, &args->x, &args->n, err) ||
	    cpt_arg_cost(input, layout.cost_col, row, &args->kind, err) ||
	    cpt_arg_min_size(input, layout.min_size_col, row, &args->min_size, err)) {
		return 1;
	}
	return cpt_cost_create(args->kind, args->x, args->n, cost, err);
}

static int bkps_row(duckdb_function_info info, duckdb_data_chunk input, idx_t row, cpt_arg_layout layout,
                    cpt_bkps_algorithm algorithm, cpt_bkps *out, char *err) {
	cpt_series_args args;
	cpt_cost cost;
	if (cpt_prepare(input, row, layout, &args, &cost, err)) {
		return 1;
	}
	cpt_budget budget = cpt_function_budget(info);
	int rc = algorithm(input, row, &cost, &args, &budget, out, err);
	cpt_cost_destroy(&cost);
	return rc;
}

void cpt_execute_bkps(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output, const char *name,
                      cpt_arg_layout layout, cpt_bkps_algorithm algorithm) {
	idx_t size = duckdb_data_chunk_get_size(input);
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < size; row++) {
		if (!cpt_row_is_valid(series, row)) {
			cpt_write_null_list(output, row, offset);
			continue;
		}
		cpt_bkps bkps = {NULL, 0};
		int rc = bkps_row(info, input, row, layout, algorithm, &bkps, err);
		if (rc == 0) {
			rc = cpt_write_int_list(output, row, &offset, bkps.pos, bkps.count, err);
		}
		cpt_bkps_free(&bkps);
		if (rc) {
			cpt_report_error(info, name, rc, err);
			return;
		}
	}
}
