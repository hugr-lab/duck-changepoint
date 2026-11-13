// The entry point. The functions are the C implementations in src/functions, registered through
// DuckDB's C API on a connection to the loader's database: inside DuckDB a duckdb_connection is a
// Connection * (duckdb_connect in src/main/capi/v1/duckdb-c.cpp), so the registrations run unchanged.

#define DUCKDB_EXTENSION_MAIN

#include "cpt_extension.hpp"

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

extern "C" {
#include "cpt_functions.h"
}

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	Connection connection(loader.GetDatabaseInstance());
	if (!cpt_register_all(reinterpret_cast<duckdb_connection>(&connection))) {
		throw InternalException("cpt: failed to register functions");
	}
}

void CptExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string CptExtension::Name() {
	return "cpt";
}

std::string CptExtension::Version() const {
#ifdef EXT_VERSION_CPT
	return EXT_VERSION_CPT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cpt, loader) {
	duckdb::LoadInternal(loader);
}
}
