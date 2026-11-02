# CLAUDE.md

Guidance for coding agents working in this repository.

## What this is

`cpt`: a DuckDB loadable extension for offline change point detection (PELT, Binseg, Dynp, CROPS
over l2 / normal / poisson / l1 costs), written in C11 against the **stable** DuckDB C extension API.
One binary must load in DuckDB 1.5.6 and later (1.5.x and 2.0.x).

## Hard rules

- C11 only. No C++, no DuckDB internals, no `duckdb_extension_v2.h`, no `USE_UNSTABLE_C_API`.
- `duckdb_capi/` holds the v1.5.6 headers; only call API functions present there.
- `src/core/` has no DuckDB dependency. Algorithms use costs only through `cpt_cost`.
- PELT must stay exact: any change to pruning needs `make test_core` and `make check_optimality` to
  pass. ruptures and changepoint prune heuristically when min_size > 1; do not copy them.
- Output convention: 1-based segment ends, increasing, last element = n.
- Penalties follow R changepoint: bic = (p+1) ln n, mbic = (p+2) ln n + Σ ln nᵢ, aic = 2(p+1).

## Commands

```sh
make configure && make release      # build/release/cpt.duckdb_extension
make test_release                   # SQL tests, latest DuckDB
make test_compat                    # SQL tests, DuckDB 1.5.6
make test_core                      # C core tests under ASan/UBSan
make compare_ruptures check_optimality bench   # manual checks
make leak_check joern_leaks         # leak checks (macOS leaks + Joern); run after touching allocation code
```

Use `rtk` as a command prefix in this environment (see the user's global instructions).

## Where things are

- `src/core/cost.c`, `cost_l1.c`, `algo.c`: the numerics.
- `src/functions/*.c`: one file per SQL function, helpers in `list_io.c`.
- `docs/`: architecture, algorithms, cost/penalty derivations, testing.
- Reference data from R changepoint (GPL) lives in the separate `duck-changepoint-reference` repo.
  Do not add GPL code or data here; this repository is MIT.

## Gotchas

- DuckDB 2.0 rejects `x -> ...` lambdas by default. Use `lambda x: ...` in tests and docs.
- `generate_series` has no DOUBLE overload; build penalty grids from `range()`.
- The sqllogictest runner stops at the first failing file.
- A C API scalar function cannot be interrupted: new loops must charge `cpt_budget` (see algo.c)
  so `cpt_max_evaluations` keeps bounding run time.
- With no git commits, set `EXTENSION_VERSION` or the metadata footer is corrupt.
- Expected values in tests must be derived from the model, not copied from output. Several tests
  carry the arithmetic in a comment.
