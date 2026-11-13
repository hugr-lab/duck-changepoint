cmake_minimum_required(VERSION 3.5...3.29)
option(DUCKDB_WASM_EXTENSION "Whether compiling for Wasm target" OFF)

# The cpt extension, built inside a DuckDB build (extension_config.cmake, `make release`), as the C++
# extension template builds. src/cpp/cpt_extension.cpp is the entry point; the C sources in
# src/functions register through DuckDB's C API (duckdb.h) inside the host. The stable C API build,
# which loads in DuckDB 1.5.x, lives on the v1.5.x branch.
if(NOT COMMAND build_loadable_extension)
    message(FATAL_ERROR "cpt builds as part of DuckDB: run `make release` (see DEVELOPMENT.md)")
endif()

set(EXTENSION_NAME cpt)
project(${EXTENSION_NAME} LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Explicit source list (no globbing), grouped by layer.
set(EXTENSION_SOURCES
        # Numerical core: no DuckDB dependency
        src/core/cost.c
        src/core/cost_l1.c
        src/core/algo.c
        src/core/online.c
        # SQL surface: vector I/O and function registration
        src/functions/list_io.c
        src/functions/cpt_call.c
        src/functions/cpt_pelt.c
        src/functions/cpt_binseg.c
        src/functions/cpt_dynp.c
        src/functions/cpt_crops.c
        src/functions/cpt_segment_stats.c
        src/functions/cpt_online.c
        src/cpt_extension.c
        # C++: entry point and the query (interrupts)
        src/cpp/cpt_extension.cpp
        src/cpp/cpt_query.cpp
)

build_static_extension(${EXTENSION_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${EXTENSION_NAME} " " ${EXTENSION_SOURCES})

foreach(_target ${EXTENSION_NAME}_extension ${EXTENSION_NAME}_loadable_extension)
    # DUCKDB_C_API empty: duckdb.h would declare the C API dllimport on Windows, but this extension links
    # DuckDB statically, and mingw's linker (unlike MSVC's) refuses __imp_ references to static symbols.
    target_compile_definitions(${_target} PRIVATE "DUCKDB_C_API=")
    target_include_directories(${_target} PRIVATE src/include)
    set_property(TARGET ${_target} PROPERTY C_STANDARD 11)
    # tools/crap/crap.py instruments only this extension's code, not DuckDB.
    if(CPT_COVERAGE)
        target_compile_options(${_target} PRIVATE -fprofile-instr-generate -fcoverage-mapping -O0 -g)
    endif()
endforeach()

install(TARGETS ${EXTENSION_NAME}_extension EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}" ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
