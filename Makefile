.PHONY: clean clean_all compare_ruptures check_optimality help

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

# Not part of `make test`: needs ruptures + numpy (installed into configure/venv on demand).
compare_ruptures: release
	$(PYTHON_VENV_BIN) -m pip install -q ruptures numpy
	$(PYTHON_VENV_BIN) test/python/compare_ruptures.py build/release/$(EXTENSION_NAME).duckdb_extension

check_optimality: release
	$(PYTHON_VENV_BIN) -m pip install -q numpy
	$(PYTHON_VENV_BIN) test/python/check_optimality.py build/release/$(EXTENSION_NAME).duckdb_extension

clean: clean_build clean_cmake
clean_all: clean clean_configure

help:
	@echo "make configure        - create configure/venv (test runner) and detect platform"
	@echo "make release | debug  - build build/{release,debug}/cpt.duckdb_extension"
	@echo "make test             - SQL logic tests against the debug build"
	@echo "make test_release     - SQL logic tests against the release build"
	@echo "make compare_ruptures - cross-check against ruptures (manual, not CI gating)"
	@echo "make check_optimality - PELT vs unpruned Optimal Partitioning (manual, not CI gating)"

