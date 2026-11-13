# CLAUDE.md

Guidance for coding agents working in this repository.

## What this is

`cpt`: a DuckDB loadable extension for offline change point detection (PELT, Binseg, Dynp, CROPS
over l2 / normal / poisson / l1 costs). For DuckDB 2.0.0 as a C++ extension built with DuckDB (the
`duckdb` submodule); the numerical core and the SQL functions are C11, registered through DuckDB's C API
from the C++ entry point in `src/cpp/`. The stable C API build for DuckDB 1.5.x lives on the `v1.5.x`
branch; fixes that matter there go to both branches.

## Hard rules

- `src/core/` and `src/functions/` are C11; C++ only in `src/cpp/`. The C files include `duckdb.h`,
  never `duckdb_extension.h`, whose calls would go through a C API extension's function table that this
  extension's entry point never fills.
- `src/core/` has no DuckDB dependency. Algorithms use costs only through `cpt_cost`.
- PELT must stay exact: any change to pruning needs `make test_core` and `make check_optimality` to
  pass. ruptures and changepoint prune heuristically when min_size > 1; do not copy them.
- Output convention: 1-based segment ends, increasing, last element = n.
- Penalties follow R changepoint: bic = (p+1) ln n, mbic = (p+2) ln n + Σ ln nᵢ, aic = 2(p+1).

## Commands

```sh
GEN=ninja make release              # DuckDB + build/release/extension/cpt/cpt.duckdb_extension
make test_release                   # SQL tests in DuckDB's unittest
make test_core                      # C core tests under ASan/UBSan
make test_threads                   # core on 8 threads under ThreadSanitizer
make compare_ruptures check_optimality bench   # manual checks
make leak_check joern_leaks         # leak checks (macOS leaks + Joern); run after touching allocation code
make crap                           # CRAP < 10 for every C function; CI fails otherwise
```

Use `rtk` as a command prefix in this environment (see the user's global instructions).

## Where things are

- `src/core/cost.c`, `cost_l1.c`, `algo.c`: the offline numerics; `online.c`: the online detector.
  Its state BLOB is a versioned format: bump `CPT_ONLINE_VERSION` on any change, and keep `decode`
  validating everything (the BLOB comes from users).
- `src/functions/*.c`: one file per SQL function, helpers in `list_io.c`.
- `docs/`: architecture, algorithms, cost/penalty derivations, testing.
- `website/`: the Docusaurus site. Its pages are generated from README.md, DEVELOPMENT.md and `docs/*.md`
  by `website/sync_pages.py`: after editing those, rerun it and commit `website/docs` too (CI checks).
- Reference data from R changepoint (GPL) lives in the separate `duck-changepoint-reference` repo.
  Do not add GPL code or data here; this repository is MIT.

## Gotchas

- DuckDB 2.0 rejects `x -> ...` lambdas by default. Use `lambda x: ...` in tests and docs.
- `generate_series` has no DOUBLE overload; build penalty grids from `range()`.
- DuckDB's unittest (`make test_release`) runs every test file and reports each failure; it loads the
  extension from the build's local repository. A test that skips on `require cpt` means the extension
  was not found, not that it passed.
- Every C and C++ function stays at CRAP < 10 (`make crap`): small, single-purpose functions; core allocation
  through `cpt_malloc`/`cpt_calloc`/`cpt_realloc` so fault injection reaches the out-of-memory paths.
- No mutable global or static state in src/: calls run concurrently on DuckDB's threads
  (`make test_threads`, `test/sql/cpt_threads.test`).
- New loops must charge `cpt_budget` (see algo.c): the charge is where `cpt_max_evaluations` bounds
  the work and where a running call learns that its query was interrupted (`cpt_query.h`).
- Without `OVERRIDE_GIT_DESCRIBE=v2.0.0` (set in the Makefile) a DuckDB 2.0 build is stamped with the
  commit hash, and the binary refuses to load into the released DuckDB.
- Expected values in tests must be derived from the model, not copied from output. Several tests
  carry the arithmetic in a comment.
