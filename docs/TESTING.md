# Testing

| Layer | Command | Gating | What it checks |
|---|---|---|---|
| SQL logic tests | `make test` (debug), `make test_release` | yes | `test/sql/*.test`: results, errors, NULL/empty handling, overloads, GROUP BY, synthetic series |
| Oldest DuckDB | `make test_compat` | recommended before release | the same SQL tests with DuckDB 1.5.6 |
| Core C tests | `make test_core` | yes (CI) | `test/c/test_core.c` under ASan + UBSan: costs vs naive formulas, PELT vs Optimal Partitioning, Dynp vs exhaustive search, valid segmentations |
| ruptures cross-check | `make compare_ruptures` | manual | 200 random series: PELT (l2, l1), Binseg (pen, n_bkps), Dynp |
| Optimality | `make check_optimality` | manual | PELT vs unpruned O(n²) Optimal Partitioning, all costs, min_size 1–5, value penalties and MBIC |
| Leaks (dynamic) | `make leak_check` | manual (macOS) | every SQL function incl. error paths, and the core tests, under `leaks --atExit` |
| Leaks (static) | `make joern_leaks` | manual | Joern audit: every allocation and `duckdb_create_*` is released, returned or struct-owned |
| Benchmark | `make bench` | manual | l2 PELT at n = 1e4, 1e5, 1e6 |
| R changepoint | `make test` in `duck-changepoint-reference` | manual | Nile, Lai2005fig4, wave.c44137, ftse100 |

## SQL tests (`test/sql/`)

| File | Content |
|---|---|
| `cpt_basic.test` | load, empty/NULL, argument errors, overloads, constant vectors, 3000 groups |
| `cpt_pelt.test` | synthetic mean, variance, Poisson and outlier series; min_size; GROUP BY |
| `cpt_binseg_dynp.test` | binseg = dynp = truth on separable series; stopping rules; dynp ≤ binseg cost |
| `cpt_crops.test` | sweep covers PELT, ordering, reproducibility, min_size |
| `cpt_segment_stats.test` | statistics, README examples for labelling rows |
| `cpt_talk_examples.test` | the examples in docs/examples-from-talks.md (FOCuS Figure 1, detection delay, Hunter-style benchmark and latency) |

The synthetic series are generated in SQL right after `SELECT setseed(0.42)`. They are smaller than
one vector, so a single thread generates them, and DuckDB 1.5.6 and 2.0 produce the same values.

Results are in the 1-based convention. Expected values were checked by reasoning about the costs,
recorded in the comments next to the less obvious ones, and not copied from the output.

## Reference data against R changepoint

The datasets and expected results from R's changepoint package live in the companion repository
`duck-changepoint-reference`. Three of the datasets ship with changepoint under the GPL, and keeping
them out of this repository keeps it MIT-only. That repository's `make test` runs
`test/sql/cpt_reference.test` against a cpt build (by default `../duck-changepoint/build/release/`)
and summarises what it checks: exact matches on Nile and Lai2005fig4, and on wave.c44137 and ftse100
a penalised cost no higher than changepoint's (whose pruning ignores minseglen).

## Memory leaks

Three checks complement each other:

- **`make leak_check` (macOS).** `test/leak/leak_host.c` links libduckdb (`brew install duckdb`),
  loads the debug extension and runs 14 succeeding and 10 failing queries 50 times
  (`LEAK_ITERATIONS`). The queries cover every function and cost, NULL and empty input, 5000-row
  constant vectors, and an error raised on a later row after earlier rows already wrote output.
  `leaks --atExit` then reports anything unreachable at exit. The core tests run under `leaks` too,
  built without ASan because ASan's allocator hides allocations from `leaks`. To confirm the check
  works, a leak was planted once (PELT's output not freed): `leaks` reported 86 leaks with the stack
  `PeltFunction` → `cpt_pelt` → `bkps_backtrack`. Peak RSS is flat between 50 and 500 iterations
  (35.1 MB vs 35.3 MB), so nothing accumulates while still referenced either.
- **LeakSanitizer (Linux CI).** `make test_core` builds with `-fsanitize=address`, which enables
  LSan on Linux, so the core is leak-checked on every push. Apple's clang does not support LSan on
  macOS.
- **`make joern_leaks` (static).** `tools/joern/leaks.sc` walks the Joern code property graph of
  `src/`. Every `malloc`/`calloc`/`realloc` and `duckdb_create_*` must be released in the same
  function, returned to a caller that is checked the same way, or stored in a field that a free
  function releases. It reports 56 allocations, 9 allocating call sites and 0 unreleased. The check
  is per function, not per path; error paths are covered by `leak_check`.

`joern-scan src` (Joern's built-in queries) reports two kinds of informational findings. "Copy
loop" marks array writes in loops; these are bounds-checked by the ASan tests. "Unchecked malloc"
in `cpt_cost_l1_init` is a false positive: the allocations are checked together through `ok`, and
on failure `cpt_cost_create` releases the partial index with `cpt_cost_destroy`.

## Running a single test file

```sh
configure/venv/bin/python3 -m duckdb_sqllogictest --test-dir test/sql \
    --external-extension build/release/cpt.duckdb_extension
```

The runner stops at the first failing file.
