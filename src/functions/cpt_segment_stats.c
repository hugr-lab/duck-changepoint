// cpt_segment_stats(series LIST(DOUBLE), bkps LIST(BIGINT))
//   -> LIST(STRUCT(start INTEGER, "end" INTEGER, mean DOUBLE, var DOUBLE, n INTEGER))
//
// start/end are 1-based and inclusive; var is the sample variance (NULL for one-element segments).
// bkps is declared BIGINT[] so both INTEGER[] (what the cpt_* functions return) and BIGINT[] bind.

#include "cpt_functions.h"
#include "list_io.h"

#include <stdio.h>

DUCKDB_EXTENSION_EXTERN

static int increasing_from_one(const int64_t *b, uint64_t count) {
	int64_t prev = 0;
	for (uint64_t i = 0; i < count; i++) {
		if (b[i] <= prev) {
			return 0;
		}
		prev = b[i];
	}
	return 1;
}

// bkps must be strictly increasing, >= 1, and end at n (empty for an empty series).
static int check_bkps(const int64_t *b, uint64_t count, int64_t n, char *err) {
	if (n == 0 && count != 0) {
		snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: bkps must be empty for an empty series");
		return 1;
	}
	if (n > 0 && (count == 0 || b[count - 1] != n)) {
		snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: the last break point must equal the series length %lld",
		         (long long)n);
		return 1;
	}
	if (!increasing_from_one(b, count)) {
		snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: break points must be strictly increasing and >= 1");
		return 1;
	}
	return 0;
}

// The bkps argument of one row; elements must not be NULL.
static int read_bkps(duckdb_vector vec, idx_t row, const int64_t **b, uint64_t *count, char *err) {
	duckdb_list_entry entry = ((duckdb_list_entry *)duckdb_vector_get_data(vec))[row];
	duckdb_vector child = duckdb_list_vector_get_child(vec);
	if (cpt_first_null(child, entry.offset, entry.length) >= 0) {
		snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: bkps contains a NULL");
		return 1;
	}
	*b = (const int64_t *)duckdb_vector_get_data(child) + entry.offset;
	*count = entry.length;
	return 0;
}

// Mean and sample variance of x[a..e), two-pass for accuracy.
static void segment_moments(const double *x, int64_t a, int64_t e, double *mean, double *ss) {
	double sum = 0;
	for (int64_t j = a; j < e; j++) {
		sum += x[j];
	}
	*mean = sum / (double)(e - a);
	*ss = 0;
	for (int64_t j = a; j < e; j++) {
		*ss += (x[j] - *mean) * (x[j] - *mean);
	}
}

// The struct fields of the output list's child, fetched after reserving (buffers may move).
typedef struct {
	int32_t *start, *end, *n;
	double *mean, *var;
	duckdb_vector var_vec;
} stats_columns;

static stats_columns stats_columns_of(duckdb_vector output) {
	duckdb_vector structs = duckdb_list_vector_get_child(output);
	stats_columns c;
	c.start = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 0));
	c.end = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 1));
	c.mean = (double *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 2));
	c.var_vec = duckdb_struct_vector_get_child(structs, 3);
	c.var = (double *)duckdb_vector_get_data(c.var_vec);
	c.n = (int32_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 4));
	return c;
}

// Segment x[a..e) at output position k; var is NULL for a one-element segment.
static void write_segment(const stats_columns *c, idx_t k, const double *x, int64_t a, int64_t e) {
	double mean, ss;
	segment_moments(x, a, e, &mean, &ss);
	c->start[k] = (int32_t)(a + 1);
	c->end[k] = (int32_t)e;
	c->mean[k] = mean;
	c->n[k] = (int32_t)(e - a);
	c->var[k] = e - a > 1 ? ss / (double)(e - a - 1) : 0;
	if (e - a <= 1) {
		cpt_set_row_null(c->var_vec, k);
	}
}

static int write_stats(duckdb_vector output, idx_t row, idx_t *offset, const double *x, const int64_t *b,
                       uint64_t count, char *err) {
	idx_t new_size = *offset + count;
	if (duckdb_list_vector_reserve(output, new_size) != DuckDBSuccess) {
		snprintf(err, CPT_ERR_LEN, "cpt_segment_stats: failed to reserve output");
		return 1;
	}
	stats_columns c = stats_columns_of(output);
	for (uint64_t i = 0; i < count; i++) {
		write_segment(&c, *offset + i, x, i == 0 ? 0 : b[i - 1], b[i]);
	}
	duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(output);
	entries[row] = (duckdb_list_entry) {*offset, count};
	*offset = new_size;
	duckdb_list_vector_set_size(output, new_size);
	return 0;
}

static int stats_row(duckdb_data_chunk input, idx_t row, duckdb_vector output, idx_t *offset, char *err) {
	const double *x;
	int64_t n;
	const int64_t *b;
	uint64_t count;
	if (cpt_read_series(duckdb_data_chunk_get_vector(input, 0), row, &x, &n, err) ||
	    read_bkps(duckdb_data_chunk_get_vector(input, 1), row, &b, &count, err) || check_bkps(b, count, n, err)) {
		return 1;
	}
	return write_stats(output, row, offset, x, b, count, err);
}

static void SegmentStatsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t size = duckdb_data_chunk_get_size(input);
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	duckdb_vector bkps = duckdb_data_chunk_get_vector(input, 1);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < size; row++) {
		if (!cpt_row_is_valid(series, row) || !cpt_row_is_valid(bkps, row)) {
			cpt_write_null_list(output, row, offset);
		} else if (stats_row(input, row, output, &offset, err)) {
			duckdb_scalar_function_set_error(info, err);
			return;
		}
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
