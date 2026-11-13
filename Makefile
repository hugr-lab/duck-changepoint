PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# The C++ extension, built inside DuckDB v2.0.0 (the duckdb submodule) as the extension template does.
# The stable C API build, which loads in DuckDB 1.5.x, lives on the v1.5.x branch.
EXT_NAME=cpt
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# DuckDB 2.0 takes its version from this, not from git; without it a build stamps the commit hash and
# only loads into a DuckDB built from the same tree. The distribution CI sets it the same way.
OVERRIDE_GIT_DESCRIBE ?= v2.0.0

# Compatible with DuckDB Community Extensions CI
# (provides: release, debug, reldebug, test_release, test_debug, wasm_*, format, clean, ...)
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: test_core test_threads compare_ruptures check_optimality leak_check joern_leaks crap bench help

EXTENSION_BINARY := build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension
CORE_SOURCES := src/core/cost.c src/core/cost_l1.c src/core/algo.c src/core/online.c

# Python checks against the DuckDB 2.0.0 package, in a pinned uv environment.
PYTHON := uv run --quiet --no-project --python 3.12 --with duckdb==2.0.0 --with numpy==2.3.4

# Not part of `make test`: cross-checks against ruptures and unpruned Optimal Partitioning.
compare_ruptures: release
	$(PYTHON) --with ruptures==1.1.10 python -I test/python/compare_ruptures.py $(EXTENSION_BINARY)

check_optimality: release
	$(PYTHON) python -I test/python/check_optimality.py $(EXTENSION_BINARY)

# Standalone C tests of src/core under AddressSanitizer + UndefinedBehaviorSanitizer (no DuckDB needed).
# CPT_FAULT_INJECTION lets the test fail every allocation in turn (src/include/cpt_alloc.h).
CORE_TEST_BIN := build/test_core
test_core:
	@mkdir -p build
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
		-Wall -Wextra -DCPT_FAULT_INJECTION -Isrc/include test/c/test_core.c $(CORE_SOURCES) -lm -o $(CORE_TEST_BIN)
	$(CORE_TEST_BIN)

# Concurrency test of src/core under ThreadSanitizer: 8 threads run every algorithm on series from one
# shared buffer and compare with a single-threaded reference (no DuckDB needed).
THREADS_TEST_BIN := build/test_threads
test_threads:
	@mkdir -p build
	$(CC) -std=c11 -g -O1 -fsanitize=thread -fno-omit-frame-pointer -Wall -Wextra -pthread \
		-Isrc/include test/c/test_threads.c $(CORE_SOURCES) -lm -o $(THREADS_TEST_BIN)
	TSAN_OPTIONS=halt_on_error=1 $(THREADS_TEST_BIN)

# Leak checks (macOS). Dynamic: a host program links the libduckdb of this tree's reldebug build (a C++
# extension loads only into the DuckDB it was built with), loads that build's extension and runs every
# function incl. error paths under `leaks --atExit` (exit 1 on any leak); the core tests run under
# `leaks` too. Not the debug build: DuckDB compiles it with AddressSanitizer, whose heap `leaks` cannot
# inspect.
LEAK_ITERATIONS ?= 50
LEAK_LIB_DIR := build/reldebug/src
leak_check: reldebug
	@mkdir -p build
	$(CC) -std=c11 -g -O1 -Iduckdb/src/include test/leak/leak_host.c -L$(LEAK_LIB_DIR) -lduckdb \
		-Wl,-rpath,$(PROJ_DIR)$(LEAK_LIB_DIR) -o build/leak_host
	$(CC) -std=c11 -g -O1 -DCPT_FAULT_INJECTION -Isrc/include test/c/test_core.c $(CORE_SOURCES) -lm \
		-o build/test_core_plain
	MallocStackLogging=1 leaks --atExit -- build/leak_host build/reldebug/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension \
		$(LEAK_ITERATIONS) > build/leaks_extension.txt 2>&1 || (tail -60 build/leaks_extension.txt; exit 1)
	@grep -E "iterations|leaks for" build/leaks_extension.txt
	MallocStackLogging=1 leaks --atExit -- build/test_core_plain > build/leaks_core.txt 2>&1 \
		|| (tail -60 build/leaks_core.txt; exit 1)
	@grep -E "checks|leaks for" build/leaks_core.txt

# Static leak audit with Joern: every malloc/calloc/realloc and duckdb_create_* must be released,
# returned, or owned by a struct that is freed (tools/joern/leaks.sc). Exit 1 on any unreleased.
joern_leaks:
	@mkdir -p build/joern
	joern-parse src --output build/joern/cpg.bin
	joern --script tools/joern/leaks.sc --param cpgFile=build/joern/cpg.bin; rc=$$?; rm -rf workspace; exit $$rc

# CRAP score of every C and C++ function in src/ (complexity from lizard, coverage from llvm-cov over
# test_core, test_threads and the SQL tests on a coverage build). Exit 2 when any function scores CRAP >= 10.
crap:
	python3 tools/crap/crap.py

bench: release
	$(PYTHON) python -I bench/run_bench.py $(EXTENSION_BINARY)

help:
	@echo "make release | debug  - build DuckDB and build/{release,debug}/extension/cpt/cpt.duckdb_extension"
	@echo "make test_release     - SQL logic tests (DuckDB's unittest over test/sql) against the release build"
	@echo "make test_core        - C tests of src/core under ASan/UBSan (no DuckDB needed)"
	@echo "make test_threads     - 8 threads on shared series under ThreadSanitizer (no DuckDB needed)"
	@echo "make compare_ruptures - cross-check against ruptures (manual, not CI gating)"
	@echo "make check_optimality - PELT vs unpruned Optimal Partitioning (manual, not CI gating)"
	@echo "make leak_check       - every SQL function and the core under macOS leaks"
	@echo "make joern_leaks      - static allocation/release audit of src/ with Joern"
	@echo "make crap             - CRAP score per C/C++ function (lizard + llvm-cov); fails at CRAP >= 10"
	@echo "make bench            - l2 PELT runtime for n = 1e4, 1e5, 1e6"
