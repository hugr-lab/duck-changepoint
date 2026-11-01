#include "duckdb_extension.h"

#include "cpt_functions.h"

DUCKDB_EXTENSION_EXTERN

bool cpt_add_overload(duckdb_scalar_function_set set, const char *name, duckdb_logical_type *params, idx_t n_params,
                      duckdb_logical_type ret, duckdb_scalar_function_t fn) {
	duckdb_scalar_function f = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(f, name);
	for (idx_t i = 0; i < n_params; i++) {
		duckdb_scalar_function_add_parameter(f, params[i]);
	}
	duckdb_scalar_function_set_return_type(f, ret);
	duckdb_scalar_function_set_function(f, fn);
	// NULL arguments mean "use the default" for optional parameters, so the callback sees them.
	duckdb_scalar_function_set_special_handling(f);
	bool ok = duckdb_add_scalar_function_to_set(set, f) == DuckDBSuccess;
	duckdb_destroy_scalar_function(&f);
	return ok;
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info,
                            struct duckdb_extension_access *access) {
	if (!cpt_register_pelt(connection) || !cpt_register_binseg(connection) || !cpt_register_dynp(connection) ||
	    !cpt_register_segment_stats(connection)) {
		access->set_error(info, "cpt: failed to register functions");
		return false;
	}
	return true;
}
