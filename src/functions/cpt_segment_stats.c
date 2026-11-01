// cpt_segment_stats(series LIST(DOUBLE), bkps LIST(BIGINT))
//   -> LIST(STRUCT(start INTEGER, "end" INTEGER, mean DOUBLE, var DOUBLE, n INTEGER))
//
// start/end are 1-based and inclusive; var is the sample variance (NULL for one-element segments).
// bkps is declared BIGINT[] so both INTEGER[] (what the cpt_* functions return) and BIGINT[] bind.

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static int check_bkps(const int64_t *b, uint64_t count, int64_t n, char *err) {
	if (n == 0) {
		if (count != 0) {
			snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: bkps must be empty for an empty series");
			return 1;
		}
		return 0;
	}
	if (count == 0 || b[count - 1] != n) {
		snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: the last break point must equal the series length %lld",
		         (long long)n);
		return 1;
	}
	int64_t prev = 0;
	for (uint64_t i = 0; i < count; i++) {
		if (b[i] <= prev) {
			snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: break points must be strictly increasing and >= 1");
			return 1;
		}
		prev = b[i];
	}
	return 0;
}

static void SegmentStatsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t size = duckdb_data_chunk_get_size(input);
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	duckdb_vector bkps_vec = duckdb_data_chunk_get_vector(input, 1);
	duckdb_list_entry *bkps_entries = (duckdb_list_entry *)duckdb_vector_get_data(bkps_vec);
	duckdb_vector bkps_child = duckdb_list_vector_get_child(bkps_vec);
	const int64_t *bkps_data = (const int64_t *)duckdb_vector_get_data(bkps_child);
	uint64_t *bkps_validity = duckdb_vector_get_validity(bkps_child);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < size; row++) {
		if (!cpt_row_is_valid(series, row) || !cpt_row_is_valid(bkps_vec, row)) {
			cpt_write_null_list(output, row, offset);
			continue;
		}
		const double *x;
		int64_t n;
		if (cpt_read_series(series, row, &x, &n, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
		duckdb_list_entry be = bkps_entries[row];
		for (uint64_t i = 0; bkps_validity && i < be.length; i++) {
			if (!duckdb_validity_row_is_valid(bkps_validity, be.offset + i)) {
				duckdb_scalar_function_set_error(info, "cpt_segment_stats: bkps contains a NULL");
				return;
			}
		}
		const int64_t *b = bkps_data + be.offset;
		if (check_bkps(b, be.length, n, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}

		idx_t new_size = offset + be.length;
		if (duckdb_list_vector_reserve(output, new_size) != DuckDBSuccess) {
			duckdb_scalar_function_set_error(info, "cpt_segment_stats: failed to reserve output");
			return;
		}
		duckdb_vector structs = duckdb_list_vector_get_child(output);
		int32_t *start = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 0));
		int32_t *end = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 1));
		double *mean = (double *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 2));
		duckdb_vector var_vec = duckdb_struct_vector_get_child(structs, 3);
		double *var = (double *)duckdb_vector_get_data(var_vec);
		int32_t *count = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 4));

		int64_t a = 0;
		for (uint64_t i = 0; i < be.length; i++) {
			int64_t e = b[i];
			int64_t len = e - a;
			double sum = 0;
			for (int64_t j = a; j < e; j++) {
				sum += x[j];
			}
			double mu = sum / (double)len;
			double ss = 0; // second pass for accuracy
			for (int64_t j = a; j < e; j++) {
				double d = x[j] - mu;
				ss += d * d;
			}
			idx_t k = offset + i;
			start[k] = (int32_t)(a + 1);
			end[k] = (int32_t)e;
			mean[k] = mu;
			count[k] = (int32_t)len;
			if (len > 1) {
				var[k] = ss / (double)(len - 1);
			} else {
				var[k] = 0;
				cpt_set_row_null(var_vec, k);
			}
			a = e;
		}
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(output);
		entries[row].offset = offset;
		entries[row].length = be.length;
		offset = new_size;
		duckdb_list_vector_set_size(output, new_size);
	}
}

bool cpt_register_segment_stats(duckdb_connection connection) {
	duckdb_logical_type list_double = cpt_type_list_double();
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_logical_type list_bigint = duckdb_create_list_type(bigint);
	duckdb_logical_type dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type integer = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);

	duckdb_logical_type member_types[] = {integer, integer, dbl, dbl, integer};
	const char *member_names[] = {"start", "end", "mean", "var", "n"};
	duckdb_logical_type entry = duckdb_create_struct_type(member_types, member_names, 5);
	duckdb_logical_type ret = duckdb_create_list_type(entry);

	duckdb_scalar_function_set set = duckdb_create_scalar_function_set("cpt_segment_stats");
	duckdb_logical_type params[] = {list_double, list_bigint};
	bool ok = cpt_add_overload(set, "cpt_segment_stats", params, 2, ret, SegmentStatsFunction);
	ok = ok && duckdb_register_scalar_function_set(connection, set) == DuckDBSuccess;

	duckdb_destroy_scalar_function_set(&set);
	duckdb_destroy_logical_type(&ret);
	duckdb_destroy_logical_type(&entry);
	duckdb_destroy_logical_type(&list_double);
	duckdb_destroy_logical_type(&list_bigint);
	duckdb_destroy_logical_type(&bigint);
	duckdb_destroy_logical_type(&dbl);
	duckdb_destroy_logical_type(&integer);
	return ok;
}
