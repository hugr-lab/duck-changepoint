#pragma once

// The row loop shared by the SQL functions. Each function supplies only what is specific to it: where
// its cost and min_size arguments are, how to read its other arguments, and which algorithm to run.

#include "duckdb.h"

#include "algo.h"
#include "cost.h"

#include <stdint.h>

// The arguments every series function takes, read from one row.
typedef struct {
	const double *x;
	int64_t n;
	cpt_cost_kind kind;
	int64_t min_size;
} cpt_series_args;

// Columns of the cost and min_size arguments (an absent column means the default).
typedef struct {
	idx_t cost_col;
	idx_t min_size_col;
} cpt_arg_layout;

// Read series, cost and min_size of one row and build the cost (the caller destroys it).
int cpt_prepare(duckdb_data_chunk input, idx_t row, cpt_arg_layout layout, cpt_series_args *args, cpt_cost *cost,
                char *err);

// The function-specific part of a call that returns segment ends: read its own arguments from the
// row and run its algorithm on the prepared cost.
typedef int (*cpt_bkps_algorithm)(duckdb_data_chunk input, idx_t row, const cpt_cost *cost,
                                  const cpt_series_args *args, cpt_budget *budget, cpt_bkps *out, char *err);

// Run a segment-ends function over a chunk: a NULL series gives NULL, any error stops the query.
void cpt_execute_bkps(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output, const char *name,
                      cpt_arg_layout layout, cpt_bkps_algorithm algorithm);

// A penalty must be a finite number >= 0.
int cpt_check_penalty(double pen, const char *what, char *err);
