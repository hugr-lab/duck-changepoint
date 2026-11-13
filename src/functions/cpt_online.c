// Online change detection (FOCuS0, see online.h):
//
//   cpt_online_new([threshold DOUBLE [, train BIGINT | , mean DOUBLE, sd DOUBLE]]) -> BLOB
//   cpt_online_update(state BLOB, values DOUBLE[])
//     -> STRUCT(state BLOB, n BIGINT, statistic DOUBLE, training BOOLEAN, alarms ALARM[])
//   cpt_online(series DOUBLE[] [, threshold [, train | , mean, sd]]) -> ALARM[]
//
//   ALARM = STRUCT(change_at BIGINT, detected_at BIGINT, shift DOUBLE, statistic DOUBLE)
//
// The state is a value: update takes the old state and new points and returns the new state, so it can
// be kept in a table and advanced by any query (or a trigger). cpt_online runs a fresh detector over a
// whole series, for backtesting.

#include "cpt_functions.h"
#include "list_io.h"
#include "online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_THRESHOLD 15.0
#define DEFAULT_TRAIN 100

typedef struct {
	double threshold;
	int64_t train;
	double mean, sd;
} online_params;

// Parameters starting at column first: (threshold [, train]) or (threshold, mean, sd). NULL means default.
static int read_params(duckdb_data_chunk input, idx_t row, idx_t first, online_params *p, char *err) {
	bool unused, mean_null, sd_null;
	p->threshold = cpt_arg_double(input, first, row, DEFAULT_THRESHOLD, &unused);
	if (duckdb_data_chunk_get_column_count(input) < first + 3) {
		p->train = cpt_arg_bigint(input, first + 1, row, DEFAULT_TRAIN);
		p->mean = 0;
		p->sd = 0;
		return 0;
	}
	p->train = 0;
	p->mean = cpt_arg_double(input, first + 1, row, 0, &mean_null);
	p->sd = cpt_arg_double(input, first + 2, row, 0, &sd_null);
	if (mean_null || sd_null) {
		snprintf(err, CPT_ERR_LEN, "a given baseline needs both mean and sd");
		return 1;
	}
	return 0;
}

static int init_from_params(duckdb_data_chunk input, idx_t row, idx_t first, cpt_online *d, char *err) {
	online_params p;
	return read_params(input, row, first, &p, err) || cpt_online_init(d, p.threshold, p.train, p.mean, p.sd, err);
}

static int write_state(duckdb_vector vec, idx_t row, const cpt_online *d, char *err) {
	uint8_t *buf;
	size_t len;
	if (cpt_online_encode(d, &buf, &len, err)) {
		return 1;
	}
	duckdb_vector_assign_string_element_len(vec, row, (const char *)buf, len);
	free(buf);
	return 0;
}

// Append the alarms as row `row` of a LIST(ALARM) vector.
static int write_alarms(duckdb_vector list, idx_t row, idx_t *offset, const cpt_online_alarms *alarms, char *err) {
	idx_t new_size = *offset + (idx_t)alarms->count;
	if (duckdb_list_vector_reserve(list, new_size) != DuckDBSuccess) {
		snprintf(err, CPT_ERR_LEN, "failed to reserve %llu alarms", (unsigned long long)new_size);
		return 1;
	}
	duckdb_vector structs = duckdb_list_vector_get_child(list);
	int64_t *change_at = (int64_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 0));
	int64_t *detected_at = (int64_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 1));
	double *shift = (double *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 2));
	double *statistic = (double *)duckdb_vector_get_data(duckdb_struct_vector_get_child(structs, 3));
	for (int64_t i = 0; i < alarms->count; i++) {
		idx_t k = *offset + (idx_t)i;
		change_at[k] = alarms->items[i].change_at;
		detected_at[k] = alarms->items[i].detected_at;
		shift[k] = alarms->items[i].shift;
		statistic[k] = alarms->items[i].statistic;
	}
	((duckdb_list_entry *)duckdb_vector_get_data(list))[row] = (duckdb_list_entry) {*offset, (uint64_t)alarms->count};
	*offset = new_size;
	duckdb_list_vector_set_size(list, new_size);
	return 0;
}

//===--------------------------------------------------------------------===//
// cpt_online_new
//===--------------------------------------------------------------------===//

static void OnlineNewFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); row++) {
		cpt_online d;
		int rc = init_from_params(input, row, 0, &d, err);
		if (rc == 0) {
			rc = write_state(output, row, &d, err);
			cpt_online_free(&d);
		}
		if (rc) {
			cpt_report_error(info, "cpt_online_new", rc, err);
			return;
		}
	}
}

//===--------------------------------------------------------------------===//
// cpt_online_update
//===--------------------------------------------------------------------===//

static int read_state(duckdb_data_chunk input, idx_t row, cpt_online *d, char *err) {
	duckdb_vector vec = duckdb_data_chunk_get_vector(input, 0);
	if (!cpt_row_is_valid(vec, row)) {
		snprintf(err, CPT_ERR_LEN, "cpt_online_update: state must not be NULL; create one with cpt_online_new()");
		return 1;
	}
	const char *data;
	size_t len;
	cpt_read_varchar(vec, row, &data, &len);
	return cpt_online_decode((const uint8_t *)data, len, d, err);
}

// The output struct's fields for one row: state, n, statistic, training (alarms are written separately).
static int write_update(duckdb_vector output, idx_t row, const cpt_online *d, char *err) {
	if (write_state(duckdb_struct_vector_get_child(output, 0), row, d, err)) {
		return 1;
	}
	((int64_t *)duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 1)))[row] = d->n;
	((double *)duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 2)))[row] = d->statistic;
	((bool *)duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 3)))[row] = d->training != 0;
	return 0;
}

static int update_row(duckdb_function_info info, duckdb_data_chunk input, idx_t row, duckdb_vector output,
                      idx_t *offset, char *err) {
	const double *x;
	int64_t n;
	cpt_online d;
	if (cpt_read_series(duckdb_data_chunk_get_vector(input, 1), row, &x, &n, err) || read_state(input, row, &d, err)) {
		return 1;
	}
	cpt_online_alarms alarms = {NULL, 0, 0};
	cpt_budget budget = cpt_function_budget(info);
	int rc = cpt_online_feed(&d, x, n, &budget, &alarms, err);
	if (rc == 0) {
		rc = write_update(output, row, &d, err) ||
		     write_alarms(duckdb_struct_vector_get_child(output, 4), row, offset, &alarms, err);
	}
	cpt_online_alarms_free(&alarms);
	cpt_online_free(&d);
	return rc;
}

static void OnlineUpdateFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	duckdb_vector values = duckdb_data_chunk_get_vector(input, 1);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); row++) {
		if (!cpt_row_is_valid(values, row)) {
			cpt_write_null_list(duckdb_struct_vector_get_child(output, 4), row, offset);
			cpt_set_row_null(output, row);
			continue;
		}
		int rc = update_row(info, input, row, output, &offset, err);
		if (rc) {
			cpt_report_error(info, "cpt_online_update", rc, err);
			return;
		}
	}
}

//===--------------------------------------------------------------------===//
// cpt_online (backtest over a whole series)
//===--------------------------------------------------------------------===//

static int backtest_row(duckdb_function_info info, duckdb_data_chunk input, idx_t row, duckdb_vector output,
                        idx_t *offset, char *err) {
	const double *x;
	int64_t n;
	cpt_online d;
	if (cpt_read_series(duckdb_data_chunk_get_vector(input, 0), row, &x, &n, err) ||
	    init_from_params(input, row, 1, &d, err)) {
		return 1;
	}
	cpt_online_alarms alarms = {NULL, 0, 0};
	cpt_budget budget = cpt_function_budget(info);
	int rc = cpt_online_feed(&d, x, n, &budget, &alarms, err);
	if (rc == 0) {
		rc = write_alarms(output, row, offset, &alarms, err);
	}
	cpt_online_alarms_free(&alarms);
	cpt_online_free(&d);
	return rc;
}

static void OnlineFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	duckdb_vector series = duckdb_data_chunk_get_vector(input, 0);
	idx_t offset = 0;
	char err[CPT_ERR_LEN];
	for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); row++) {
		if (!cpt_row_is_valid(series, row)) {
			cpt_write_null_list(output, row, offset);
			continue;
		}
		int rc = backtest_row(info, input, row, output, &offset, err);
		if (rc) {
			cpt_report_error(info, "cpt_online", rc, err);
			return;
		}
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

typedef struct {
	duckdb_logical_type list_double, blob, dbl, bigint, boolean, alarm_list, update;
} online_types;

static online_types online_types_create(void) {
	online_types t;
	t.list_double = cpt_type_list_double();
	t.blob = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
	t.dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	t.bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	t.boolean = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
	duckdb_logical_type alarm_types[] = {t.bigint, t.bigint, t.dbl, t.dbl};
	const char *alarm_names[] = {"change_at", "detected_at", "shift", "statistic"};
	duckdb_logical_type alarm = duckdb_create_struct_type(alarm_types, alarm_names, 4);
	t.alarm_list = duckdb_create_list_type(alarm);
	duckdb_destroy_logical_type(&alarm);
	duckdb_logical_type update_types[] = {t.blob, t.bigint, t.dbl, t.boolean, t.alarm_list};
	const char *update_names[] = {"state", "n", "statistic", "training", "alarms"};
	t.update = duckdb_create_struct_type(update_types, update_names, 5);
	return t;
}

static void online_types_destroy(online_types *t) {
	duckdb_destroy_logical_type(&t->list_double);
	duckdb_destroy_logical_type(&t->blob);
	duckdb_destroy_logical_type(&t->dbl);
	duckdb_destroy_logical_type(&t->bigint);
	duckdb_destroy_logical_type(&t->boolean);
	duckdb_destroy_logical_type(&t->alarm_list);
	duckdb_destroy_logical_type(&t->update);
}

// One function set: params are appended after `lead` leading arguments, in the shapes (), (threshold),
// (threshold, train) and (threshold, mean, sd).
static bool register_with_params(duckdb_connection connection, const char *name, duckdb_logical_type *lead,
                                 idx_t n_lead, duckdb_logical_type ret, duckdb_scalar_function_t fn,
                                 const online_types *t) {
	duckdb_scalar_function_set set = duckdb_create_scalar_function_set(name);
	duckdb_logical_type params[5];
	for (idx_t i = 0; i < n_lead; i++) {
		params[i] = lead[i];
	}
	duckdb_logical_type tails[][3] = {{t->dbl, t->bigint, NULL}, {t->dbl, t->dbl, t->dbl}};
	bool ok = cpt_add_overload(set, name, params, n_lead, ret, fn);
	for (idx_t k = 1; k <= 2; k++) {
		memcpy(params + n_lead, tails[0], sizeof(duckdb_logical_type) * k);
		ok = ok && cpt_add_overload(set, name, params, n_lead + k, ret, fn);
	}
	memcpy(params + n_lead, tails[1], sizeof(duckdb_logical_type) * 3);
	ok = ok && cpt_add_overload(set, name, params, n_lead + 3, ret, fn);
	ok = ok && duckdb_register_scalar_function_set(connection, set) == DuckDBSuccess;
	duckdb_destroy_scalar_function_set(&set);
	return ok;
}

bool cpt_register_online(duckdb_connection connection) {
	online_types t = online_types_create();
	duckdb_scalar_function_set update = duckdb_create_scalar_function_set("cpt_online_update");
	duckdb_logical_type update_params[] = {t.blob, t.list_double};
	bool ok = register_with_params(connection, "cpt_online_new", NULL, 0, t.blob, OnlineNewFunction, &t) &&
	          register_with_params(connection, "cpt_online", &t.list_double, 1, t.alarm_list, OnlineFunction, &t) &&
	          cpt_add_overload(update, "cpt_online_update", update_params, 2, t.update, OnlineUpdateFunction) &&
	          duckdb_register_scalar_function_set(connection, update) == DuckDBSuccess;
	duckdb_destroy_scalar_function_set(&update);
	online_types_destroy(&t);
	return ok;
}
