#pragma once

#include "duckdb_extension.h"

// The C API has no DEFAULT parameters, so every function is registered as a scalar function set
// with one overload per trailing-argument count. All overloads of a function share one callback,
// which checks the column count of the input chunk.

// Add one overload `name(params...) -> ret` to `set`. Returns false on failure.
bool cpt_add_overload(duckdb_scalar_function_set set, const char *name, duckdb_logical_type *params, idx_t n_params,
                      duckdb_logical_type ret, duckdb_scalar_function_t fn);

bool cpt_register_pelt(duckdb_connection connection);
bool cpt_register_binseg(duckdb_connection connection);
bool cpt_register_dynp(duckdb_connection connection);
bool cpt_register_crops(duckdb_connection connection);
bool cpt_register_segment_stats(duckdb_connection connection);
