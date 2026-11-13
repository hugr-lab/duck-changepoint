# The extension, built in-tree by DuckDB's build (make release, the distribution pipeline).
duckdb_extension_load(cpt
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
