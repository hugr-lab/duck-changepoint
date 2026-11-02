// cpt_crops(series LIST(DOUBLE), cost VARCHAR, pen_min DOUBLE, pen_max DOUBLE [, min_size BIGINT])
//   -> LIST(STRUCT(penalty DOUBLE, n_bkps INTEGER, bkps LIST(INTEGER)))

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static int write_entries(duckdb_vector output, idx_t row, idx_t *offset, idx_t *inner_offset,
                         const cpt_crops_entry *entries, int64_t count, char *err) {
	idx_t new_size = *offset + (idx_t)count;
	if (duckdb_list_vector_reserve(output, new_size) != DuckDBSuccess) {
		snprintf(err, CPT_ERR_LEN, "failed to reserve %llu list elements", (unsigned long long)new_size);
		return 1;
	}
	duckdb_vector structs = duckdb_list_vector_get_child(output);
	double *penalty = (double *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 0));
	int32_t *n_bkps = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 1));
	duckdb_vector bkps = duckdb_struct_vector_get_child(structs, 2);
	for (int64_t i = 0; i < count; i++) {
		penalty[*offset + (idx_t)i] = entries[i].penalty;
		n_bkps[*offset + (idx_t)i] = (int32_t)(entries[i].bkps.count > 0 ? entries[i].bkps.count - 1 : 0);
		if (cpt_write_int_list(bkps, *offset + (idx_t)i, inner_offset, entries[i].bkps.pos, entries[i].bkps.count,
		                       err)) {
			return 1;
		}
	}
	duckdb_list_entry *list_entries = (duckdb_list_entry *)duckdb_vector_get_data(output);
	list_entries[row].offset = *offset;
	list_entries[row].length = (uint64_t)count;
	*offset = new_size;
	duckdb_list_vector_set_size(output, new_size);
	return 0;
}

static int read_pen(duckdb_data_chunk input, idx_t col, idx_t row, const char *what, double *out, char *err) {
	bool is_null;
	*out = cpt_arg_double(input, col, row, 0, &is_null);
	if (is_null || !(*out >= 0) || *out - *out != 0) {
		snprintf(err, CPT_ERR_LEN, "cpt_crops: %s must be a finite number >= 0", what);
		return 1;
	}
	return 0;
}

static void CropsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t size = duckdb_data_chunk_get_size(input);
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	idx_t offset = 0, inner_offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < size; row++) {
		if (!cpt_row_is_valid(series, row)) {
			cpt_write_null_list(output, row, offset);
			continue;
		}
		const double *x;
		int64_t n, min_size;
		double pen_min, pen_max;
		cpt_cost_kind kind;
		if (cpt_read_series(series, row, &x, &n, err) || cpt_arg_cost(input, 1, row, &kind, err) ||
		    read_pen(input, 2, row, "pen_min", &pen_min, err) || read_pen(input, 3, row, "pen_max", &pen_max, err) ||
		    cpt_arg_min_size(input, 4, row, &min_size, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		if (pen_min > pen_max) {
			snprintf(err, CPT_ERR_LEN, "cpt_crops: pen_min (%g) must be <= pen_max (%g)", pen_min, pen_max);
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_cost cost;
		if (cpt_cost_create(kind, x, n, &cost, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		cpt_crops_entry *entries;
		int64_t count;
		cpt_budget budget = cpt_function_budget(info);
		int rc = cpt_crops(&cost, n, pen_min, pen_max, min_size, &budget, &entries, &count, err);
		cpt_cost_destroy(&cost);
		if (rc) {
			cpt_report_error(info, "cpt_crops", rc, err);
			return;
		}
		rc = write_entries(output, row, &offset, &inner_offset, entries, count, err);
		cpt_crops_free(entries, count);
		if (rc) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
	}
}

bool cpt_register_crops(duckdb_connection connection) {
	duckdb_logical_type list_double = cpt_type_list_double();
	duckdb_logical_type list_int = cpt_type_list_integer();
	duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type integer = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);

	duckdb_logical_type member_types[] = {dbl, integer, list_int};
	const char *member_names[] = {"penalty", "n_bkps", "bkps"};
	duckdb_logical_type entry = duckdb_create_struct_type(member_types, member_names, 3);
	duckdb_logical_type ret = duckdb_create_list_type(entry);

	duckdb_scalar_function_set set = duckdb_create_scalar_function_set("cpt_crops");
	duckdb_logical_type params[] = {list_double, varchar, dbl, dbl, bigint};
	bool ok = true;
	for (idx_t k = 4; k <= 5; k++) {
		ok = ok && cpt_add_overload(set, "cpt_crops", params, k, ret, CropsFunction);
	}
	ok = ok && duckdb_register_scalar_function_set(connection, set) == DuckDBSuccess;

	duckdb_destroy_scalar_function_set(&set);
	duckdb_destroy_logical_type(&ret);
	duckdb_destroy_logical_type(&entry);
	duckdb_destroy_logical_type(&list_double);
	duckdb_destroy_logical_type(&list_int);
	duckdb_destroy_logical_type(&varchar);
	duckdb_destroy_logical_type(&dbl);
	duckdb_destroy_logical_type(&integer);
	duckdb_destroy_logical_type(&bigint);
	return ok;
}
