# Testing

| Layer | Command | Gating | What it checks |
|---|---|---|---|
| SQL logic tests | `make test_release` (`make test_debug`) | yes | `test/sql/*.test` in DuckDB's unittest: results, errors, NULL/empty handling, overloads, GROUP BY, synthetic series |
| DuckDB 1.5.x | `make test_compat` on the `v1.5.x` branch | there | the same SQL tests with DuckDB 1.5.6, on the stable C API build |
| Core C tests | `make test_core` | yes (CI) | `test/c/test_core.c` under ASan + UBSan: costs vs naive formulas, PELT vs Optimal Partitioning, Dynp vs exhaustive search, valid segmentations |
| Concurrency (core) | `make test_threads` | yes (CI) | `test/c/test_threads.c` under ThreadSanitizer: 8 threads run every algorithm on series from one shared buffer and match a single-threaded reference |
| ruptures cross-check | `make compare_ruptures` | manual | 200 random series: PELT (l2, l1), Binseg (pen, n_bkps), Dynp |
| Optimality | `make check_optimality` | manual | PELT vs unpruned O(n²) Optimal Partitioning, all costs, min_size 1–5, value penalties and MBIC |
| Leaks (dynamic) | `make leak_check` | yes (CI, macOS) | every SQL function incl. error paths, and the core tests, under `leaks --atExit` |
| Leaks (static) | `make joern_leaks` | manual | Joern audit: every allocation and `duckdb_create_*` is released, returned or struct-owned |
| CRAP | `make crap` | yes (CI) | every C function in `src/` has CRAP < 10: complexity from lizard, line coverage from one LCOV report over all the tests above, scored by `tools/crap/crap_cpp.py` (shared with mssql-extension and duck-emeans) |
| CRAP tooling | `uvx pytest tools/crap` | manual | `crap_cpp.py --selftest` (the formula, the lizard and LCOV parsing, the joins) and `crap.py`'s helpers |
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
| `cpt_online.test` | online detector: backtest, chained updates (including one point at a time) equal the backtest, a state table, given baseline, NULLs and errors, work limit |
| `cpt_limits.test` | `cpt_max_evaluations`: default, error messages per function, l1 weighting, 0 = no limit, RESET |
| `cpt_threads.test` | 1,000,000 series in 9 row groups: every function and cost on 8 threads gives the same result as on 1 thread |
| `cpt_talk_examples.test` | the examples in docs/examples-from-talks.md (FOCuS Figure 1, detection delay, Hunter-style benchmark and latency) |

The synthetic series are generated in SQL right after `SELECT setseed(0.42)`. They are smaller than
one vector, so a single thread generates them, and every DuckDB release produces the same values.

Results are in the 1-based convention. Expected values were checked by reasoning about the costs,
recorded in the comments next to the less obvious ones, and not copied from the output.

## Reference data against R changepoint

The datasets and expected results from R's changepoint package live in the companion repository
`duck-changepoint-reference`. Three of the datasets ship with changepoint under the GPL, and keeping
them out of this repository keeps it MIT-only. That repository's `make test` runs
`test/sql/cpt_reference.test` against a cpt build (by default `../duck-changepoint/build/release/`)
and summarises what it checks: exact matches on Nile and Lai2005fig4, and on wave.c44137 and ftse100
a penalised cost no higher than changepoint's (whose pruning ignores minseglen).

## Online detector

`test_core` checks the online detector (`src/core/online.c`) in five ways:
- **Exactness:** after every point, the statistic, and every alarm's change position and shift, equal
  a naive recomputation over all candidates (80 random streams, 1,402 alarms).
- **Pruning:** no more than 60 candidates over 200,000 points of noise.
- **Training:** the learned baseline matches a two-pass mean and standard deviation.
- **Split invariance:** feeding in random chunks, with encode/decode after every chunk, gives
  bit-identical alarms.
- **State decoding:** every truncation of a valid state is rejected, and 3,000 randomly corrupted
  states are either rejected or safe to run on (under ASan/UBSan).

The detector also takes part in the fault injection and in the work-limit tests.

## Out-of-memory paths

Allocation in `src/core` goes through `cpt_malloc`, `cpt_calloc` and `cpt_realloc`
(`src/include/cpt_alloc.h`), which are plain libc calls in normal builds. `test_core` is built with
`-DCPT_FAULT_INJECTION`. For every cost, it makes the 1st, 2nd, 3rd, … allocation of a run through all
algorithms fail, until a run completes without a failure. Every injected failure must give an "out of
memory" error. ASan and LSan (Linux CI) and macOS `leaks` (`make leak_check`) check that each of
these paths frees what it had allocated.

## CRAP

`make crap` (`tools/crap/crap.py`) scores every C function with CRAP = CC² × (1 − coverage)³ + CC.
Complexity comes from lizard (via `uvx`). Coverage is clang source-based region coverage from
`test_core` (with fault injection), `test_threads`, and the SQL tests on a coverage build of the
extension. The run fails with exit code 2 when a function scores 10 or more; CI runs it on macOS. A
function with complexity 10 or more can never pass, however well tested, so keep functions small: see
DEVELOPMENT.md.

## Concurrency

DuckDB runs the `cpt_*` calls for different rows on different threads, and they all read from the same
input vector. The code is safe because it has no mutable global or static state: every call allocates
its own workspace and cost index, and the input and the bind data are only read. Two tests guard that:

- **`cpt_threads.test` (SQL).** DuckDB parallelises a table scan by row group (122,880 rows), so the
  small tables elsewhere run on one thread. This test uses 1,000,000 short series (9 row groups) and
  runs `cpt_pelt` (l2, l1, normal), `cpt_binseg`, `cpt_crops` and `cpt_segment_stats` with
  `SET threads = 1` and `SET threads = 8`. Every row must be identical. With 8 threads the query is
  about 7× faster on an 8-core laptop, which confirms the calls really run concurrently.
- **`make test_threads` (ThreadSanitizer).** A C program runs every algorithm and cost on 48 series from
  one shared buffer, with a single-threaded reference first, then 8 pthreads that each process all
  series, starting at different offsets so several threads work on the same series at once (6,912
  runs). ThreadSanitizer reports any data race. To confirm the check works, a race was planted once
  (a global counter incremented in `l2_eval`): TSan reported it with the stack `l2_eval` → `cpt_pelt`
  and the run failed. Running DuckDB itself under TSan would need a TSan build of DuckDB.

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
build/release/test/unittest test/sql/cpt_pelt.test
```

`unittest` loads the extension from the build's local repository, as `make test_release` does.
