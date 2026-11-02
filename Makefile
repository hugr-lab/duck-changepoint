.PHONY: clean clean_all bench compare_ruptures check_optimality test_compat test_core leak_check joern_leaks help

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Main extension configuration
EXTENSION_NAME=cpt

# Stable C API only: the binary carries the C_STRUCT ABI footer and loads in every DuckDB release
# from TARGET_DUCKDB_VERSION onwards (1.5.x and 2.0.x). Never set this to 1.
USE_UNSTABLE_C_API=0

# Oldest DuckDB version we support. duckdb_capi/ must hold the headers from exactly this tag
# (refresh with `make update_duckdb_headers`).
TARGET_DUCKDB_VERSION=v1.5.6

all: configure release

# Compatible with DuckDB Community Extensions CI
# (provides: configure, platform, venv, test_extension_*, update_duckdb_headers, ...)
include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
include extension-ci-tools/makefiles/c_api_extensions/c_cpp.Makefile

configure: venv platform extension_version

debug: build_extension_library_debug build_extension_with_metadata_debug
release: build_extension_library_release build_extension_with_metadata_release

test: test_debug
test_debug: test_extension_debug
test_release: test_extension_release

# Run the SQL tests against the oldest supported DuckDB (in a separate venv) as well as the default one.
COMPAT_DUCKDB_VERSION ?= 1.5.6
COMPAT_VENV := configure/venv-compat
test_compat: release
	@test -d $(COMPAT_VENV) || ( $(PYTHON_BIN) -m venv $(COMPAT_VENV) && \
		$(COMPAT_VENV)/bin/python3 -m pip install -q duckdb==$(COMPAT_DUCKDB_VERSION) packaging \
		git+https://github.com/duckdb/duckdb-sqllogictest-python@2ac8dbc012ddbd96a57dca37784fd8ee3c0eb021 )
	$(COMPAT_VENV)/bin/python3 -m duckdb_sqllogictest --test-dir test/sql \
		--external-extension build/release/$(EXTENSION_NAME).duckdb_extension

# Not part of `make test`: needs ruptures + numpy (installed into configure/venv on demand).
compare_ruptures: release
	$(PYTHON_VENV_BIN) -m pip install -q ruptures numpy
	$(PYTHON_VENV_BIN) test/python/compare_ruptures.py build/release/$(EXTENSION_NAME).duckdb_extension

check_optimality: release
	$(PYTHON_VENV_BIN) -m pip install -q numpy
	$(PYTHON_VENV_BIN) test/python/check_optimality.py build/release/$(EXTENSION_NAME).duckdb_extension

# Standalone C tests of src/core under AddressSanitizer + UndefinedBehaviorSanitizer (no DuckDB needed).
CORE_TEST_BIN := build/test_core
test_core:
	@mkdir -p build
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
		-Isrc/include test/c/test_core.c src/core/cost.c src/core/cost_l1.c src/core/algo.c -lm -o $(CORE_TEST_BIN)
	$(CORE_TEST_BIN)

# Leak checks (manual). Dynamic: a host program links libduckdb, loads the debug extension and runs
# every function incl. error paths under macOS `leaks --atExit` (exit 1 on any leak); the core tests
# run under `leaks` too (ASan hides allocations from it). Needs libduckdb: brew install duckdb.
DUCKDB_LIB_DIR ?= $(shell brew --prefix duckdb 2>/dev/null)/lib
LEAK_ITERATIONS ?= 50
leak_check: debug
	@mkdir -p build
	$(CC) -std=c11 -g -O1 -Iduckdb_capi test/leak/leak_host.c -L$(DUCKDB_LIB_DIR) -lduckdb \
		-Wl,-rpath,$(DUCKDB_LIB_DIR) -o build/leak_host
	$(CC) -std=c11 -g -O1 -Isrc/include test/c/test_core.c src/core/cost.c src/core/cost_l1.c src/core/algo.c \
		-lm -o build/test_core_plain
	MallocStackLogging=1 leaks --atExit -- build/leak_host build/debug/$(EXTENSION_NAME).duckdb_extension \
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

bench: release
	$(PYTHON_VENV_BIN) bench/run_bench.py build/release/$(EXTENSION_NAME).duckdb_extension

clean: clean_build clean_cmake
clean_all: clean clean_configure

help:
	@echo "make configure        - create configure/venv (test runner) and detect platform"
	@echo "make release | debug  - build build/{release,debug}/cpt.duckdb_extension"
	@echo "make test             - SQL logic tests against the debug build"
	@echo "make test_release     - SQL logic tests against the release build"
	@echo "make test_compat      - SQL logic tests on DuckDB $(COMPAT_DUCKDB_VERSION) (oldest supported)"
	@echo "make test_core        - C tests of src/core under ASan/UBSan (no DuckDB needed)"
	@echo "make compare_ruptures - cross-check against ruptures (manual, not CI gating)"
	@echo "make check_optimality - PELT vs unpruned Optimal Partitioning (manual, not CI gating)"
	@echo "make leak_check       - every SQL function and the core under macOS leaks (needs brew duckdb)"
	@echo "make joern_leaks      - static allocation/release audit of src/ with Joern"
	@echo "make bench            - l2 PELT runtime for n = 1e4, 1e5, 1e6"

