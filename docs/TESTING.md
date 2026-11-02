# Testing

| Layer | Command | Gating | What it checks |
|---|---|---|---|
| SQL logic tests | `make test` (debug), `make test_release` | yes | `test/sql/*.test`: results, errors, NULL/empty handling, overloads, GROUP BY, synthetic series |
| Oldest DuckDB | `make test_compat` | recommended before release | the same SQL tests with DuckDB 1.5.6 |
| Core C tests | `make test_core` | yes (CI) | `test/c/test_core.c` under ASan + UBSan: costs vs naive formulas, PELT vs Optimal Partitioning, Dynp vs exhaustive search, valid segmentations |
| ruptures cross-check | `make compare_ruptures` | manual | 200 random series: PELT (l2, l1), Binseg (pen, n_bkps), Dynp |
| Optimality | `make check_optimality` | manual | PELT vs unpruned O(n²) Optimal Partitioning, all costs, min_size 1–5, value penalties and MBIC |
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

## Running a single test file

```sh
configure/venv/bin/python3 -m duckdb_sqllogictest --test-dir test/sql \
    --external-extension build/release/cpt.duckdb_extension
```

The runner stops at the first failing file.
