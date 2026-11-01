# Development

## Toolchain

- CMake ≥ 3.5, a C11 compiler (clang or gcc; MSVC through the CI toolchain), Python 3, git.
- No external libraries: libc, libm and the DuckDB C API headers in `duckdb_capi/`.
- `extension-ci-tools` is a git submodule (`git submodule update --init`).

## Build

```sh
make configure      # configure/venv (DuckDB + sqllogictest runner), configure/platform.txt, version
make release        # cmake_build/release -> build/release/cpt.duckdb_extension (with metadata footer)
make debug          # same, debug
```

The version in the footer comes from `git describe` (`configure/extension_version.txt`). In a checkout
without commits, pass `EXTENSION_VERSION=v0.1.0-dev` to `make configure`. Otherwise the footer gets
git's error text and DuckDB refuses the file with "Unknown ABI type".

`compile_commands.json` is written to `cmake_build/release/`. `.clangd` points there.

## Targeting DuckDB versions

`TARGET_DUCKDB_VERSION=v1.5.6` in the Makefile is the oldest supported release, and
`duckdb_capi/*.h` must be the headers from exactly that tag:

```sh
make update_duckdb_headers   # downloads duckdb.h / duckdb_extension.h for TARGET_DUCKDB_VERSION
```

Raising the minimum version means changing both, then checking with `make test_compat
COMPAT_DUCKDB_VERSION=<new>`. Never set `USE_UNSTABLE_C_API=1`: CMake refuses, because the binary
would then load only in that exact DuckDB build.

## Tests

See [docs/TESTING.md](docs/TESTING.md). Before a commit:

```sh
make release test_release test_core
```

Before a release, also run `make test_compat compare_ruptures check_optimality bench`.

## Code conventions

- C11, tabs, 120 columns (`.clang-format`). Functions and types are `snake_case` with a `cpt_`
  prefix. SQL callbacks are `CamelCaseFunction` as in the DuckDB template.
- `src/core` must not include DuckDB headers.
- Errors go through `char err[CPT_ERR_LEN]` and a non-zero return, and the SQL layer turns them into
  `duckdb_scalar_function_set_error`. Allocation failures are reported, not ignored.
- No per-iteration allocation in algorithm loops; workspaces are allocated once per call.
- Every new cost or algorithm needs a brute-force counterpart in `test/c/test_core.c`.

## Layout

The layout follows hugr-lab/mssql-extension, adapted to the C API template:

```
src/            extension sources (core/, functions/, include/)
test/sql/       SQLLogicTests (gating)
test/c/         standalone C tests with sanitizers
test/python/    manual cross-checks (ruptures, optimality)
test/data/      reference datasets + expected results from R
tools/          export_reference.R
bench/          bench.sql + runner
docs/           architecture, algorithms, costs and penalties, testing
```
