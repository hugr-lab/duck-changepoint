#pragma once

// Vector I/O helpers shared by the SQL functions.
//
// The C API flattens every input chunk before calling a scalar function (constant and dictionary
// vectors arrive as flat vectors), so rows can be addressed directly.

#include "duckdb.h"

#include <stdint.h>

#include "algo.h"
#include "cost.h"

// Row-level NULL check on an input vector.
bool cpt_row_is_valid(duckdb_vector vec, idx_t row);
void cpt_set_row_null(duckdb_vector vec, idx_t row);

// Borrow the elements of a LIST(DOUBLE) row (no copy). Fails if an element is NULL or the list is
// longer than INT32_MAX. The pointer is valid until the function returns.
int cpt_read_series(duckdb_vector list_vec, idx_t row, const double **x, int64_t *n, char *err);

// Index of the first NULL among child[offset .. offset + length), or -1.
int64_t cpt_first_null(duckdb_vector child, uint64_t offset, uint64_t length);

// Borrow a VARCHAR row. Not NUL-terminated.
void cpt_read_varchar(duckdb_vector vec, idx_t row, const char **data, size_t *len);

// Whether an optional argument was given: its column exists and the row is not NULL.
bool cpt_arg_present(duckdb_data_chunk input, idx_t col, idx_t row);

// Optional arguments: return `def` when the argument column does not exist or the row is NULL.
int64_t cpt_arg_bigint(duckdb_data_chunk input, idx_t col, idx_t row, int64_t def);
double cpt_arg_double(duckdb_data_chunk input, idx_t col, idx_t row, double def, bool *is_null);

// Resolve the cost argument (NULL or missing -> 'l2').
int cpt_arg_cost(duckdb_data_chunk input, idx_t col, idx_t row, cpt_cost_kind *kind, char *err);

// Validate min_size (NULL -> 1) and return it.
int cpt_arg_min_size(duckdb_data_chunk input, idx_t col, idx_t row, int64_t *min_size, char *err);

// Append a LIST(INTEGER) row to `out` at *offset (advanced by the list length).
int cpt_write_int_list(duckdb_vector out, idx_t row, idx_t *offset, const int64_t *vals, int64_t count, char *err);
void cpt_write_null_list(duckdb_vector out, idx_t row, idx_t offset);

// Logical type helpers (caller destroys).
duckdb_logical_type cpt_type_list_double(void);
duckdb_logical_type cpt_type_list_integer(void);
