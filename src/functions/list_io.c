#include "list_io.h"

#include <stdio.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

bool cpt_row_is_valid(duckdb_vector vec, idx_t row) {
	uint64_t *validity = duckdb_vector_get_validity(vec);
	return duckdb_validity_row_is_valid(validity, row);
}

void cpt_set_row_null(duckdb_vector vec, idx_t row) {
	duckdb_vector_ensure_validity_writable(vec);
	duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vec), row);
}

int64_t cpt_first_null(duckdb_vector child, uint64_t offset, uint64_t length) {
	uint64_t *validity = duckdb_vector_get_validity(child);
	for (uint64_t i = 0; validity && i < length; i++) {
		if (!duckdb_validity_row_is_valid(validity, offset + i)) {
			return (int64_t)i;
		}
	}
	return -1;
}

// Index of the first NaN or +-inf, or -1 (written without isfinite to avoid libm in this unit).
static int64_t first_non_finite(const double *x, int64_t n) {
	for (int64_t i = 0; i < n; i++) {
		if (x[i] != x[i] || x[i] - x[i] != 0) {
			return i;
		}
	}
	return -1;
}

int cpt_read_series(duckdb_vector list_vec, idx_t row, const double **x, int64_t *n, char *err) {
	duckdb_list_entry entry = ((duckdb_list_entry *)duckdb_vector_get_data(list_vec))[row];
	if (entry.length > (uint64_t)INT32_MAX) {
		snprintf(err, CPT_ERR_LEN, "series has %llu elements; at most %d are supported",
		         (unsigned long long)entry.length, INT32_MAX);
		return 1;
	}
	duckdb_vector child = duckdb_list_vector_get_child(list_vec);
	int64_t null_at = cpt_first_null(child, entry.offset, entry.length);
	if (null_at >= 0) {
		snprintf(err, CPT_ERR_LEN, "series contains a NULL at position %lld", (long long)null_at + 1);
		return 1;
	}
	*x = entry.length ? (const double *)duckdb_vector_get_data(child) + entry.offset : NULL;
	*n = (int64_t)entry.length;
	int64_t bad = first_non_finite(*x, *n);
	if (bad >= 0) {
		snprintf(err, CPT_ERR_LEN, "series contains a non-finite value at position %lld", (long long)bad + 1);
		return 1;
	}
	return 0;
}

void cpt_read_varchar(duckdb_vector vec, idx_t row, const char **data, size_t *len) {
	duckdb_string_t *strings = (duckdb_string_t *)duckdb_vector_get_data(vec);
	*len = duckdb_string_t_length(strings[row]);
	*data = duckdb_string_t_data(&strings[row]);
}

bool cpt_arg_present(duckdb_data_chunk input, idx_t col, idx_t row) {
	return col < duckdb_data_chunk_get_column_count(input) &&
	       cpt_row_is_valid(duckdb_data_chunk_get_vector(input, col), row);
}

static void *arg_data(duckdb_data_chunk input, idx_t col) {
	return duckdb_vector_get_data(duckdb_data_chunk_get_vector(input, col));
}

int64_t cpt_arg_bigint(duckdb_data_chunk input, idx_t col, idx_t row, int64_t def) {
	return cpt_arg_present(input, col, row) ? ((int64_t *)arg_data(input, col))[row] : def;
}

double cpt_arg_double(duckdb_data_chunk input, idx_t col, idx_t row, double def, bool *is_null) {
	*is_null = !cpt_arg_present(input, col, row);
	return *is_null ? def : ((double *)arg_data(input, col))[row];
}

int cpt_arg_cost(duckdb_data_chunk input, idx_t col, idx_t row, cpt_cost_kind *kind, char *err) {
	*kind = CPT_COST_L2;
	if (!cpt_arg_present(input, col, row)) {
		return 0;
	}
	const char *name;
	size_t len;
	cpt_read_varchar(duckdb_data_chunk_get_vector(input, col), row, &name, &len);
	int parsed = cpt_cost_parse(name, len);
	if (parsed < 0) {
		snprintf(err, CPT_ERR_LEN, "unknown cost '%.*s'; expected one of 'l2', 'normal', 'poisson', 'l1'",
		         (int)(len > 64 ? 64 : len), name);
		return 1;
	}
	*kind = (cpt_cost_kind)parsed;
	return 0;
}

int cpt_arg_min_size(duckdb_data_chunk input, idx_t col, idx_t row, int64_t *min_size, char *err) {
	*min_size = cpt_arg_bigint(input, col, row, 1);
	if (*min_size < 1) {
		snprintf(err, CPT_ERR_LEN, "min_size must be >= 1, got %lld", (long long)*min_size);
		return 1;
	}
	return 0;
}

int cpt_write_int_list(duckdb_vector out, idx_t row, idx_t *offset, const int64_t *vals, int64_t count, char *err) {
	idx_t new_size = *offset + (idx_t)count;
	if (duckdb_list_vector_reserve(out, new_size) != DuckDBSuccess) {
		snprintf(err, CPT_ERR_LEN, "failed to reserve %llu list elements", (unsigned long long)new_size);
		return 1;
	}
	// The child buffer may move on reserve: fetch the pointer afterwards.
	int32_t *child = (int32_t *)duckdb_vector_get_data(duckdb_list_vector_get_child(out));
	for (int64_t i = 0; i < count; i++) {
		child[*offset + (idx_t)i] = (int32_t)vals[i];
	}
	duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(out);
	entries[row].offset = *offset;
	entries[row].length = (uint64_t)count;
	*offset = new_size;
	duckdb_list_vector_set_size(out, new_size);
	return 0;
}

void cpt_write_null_list(duckdb_vector out, idx_t row, idx_t offset) {
	duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(out);
	entries[row].offset = offset;
	entries[row].length = 0;
	cpt_set_row_null(out, row);
}

static duckdb_logical_type list_of(duckdb_type id) {
	duckdb_logical_type child = duckdb_create_logical_type(id);
	duckdb_logical_type list = duckdb_create_list_type(child);
	duckdb_destroy_logical_type(&child);
	return list;
}

duckdb_logical_type cpt_type_list_double(void) {
	return list_of(DUCKDB_TYPE_DOUBLE);
}

duckdb_logical_type cpt_type_list_integer(void) {
	return list_of(DUCKDB_TYPE_INTEGER);
}
