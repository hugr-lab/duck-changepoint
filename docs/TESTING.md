# Testing

| Layer | Command | Gating | What it checks |
|---|---|---|---|
| SQL logic tests | `make test` (debug), `make test_release` | yes | `test/sql/*.test`: results, errors, NULL/empty handling, overloads, GROUP BY, reference data |
| Oldest DuckDB | `make test_compat` | recommended before release | the same SQL tests with DuckDB 1.5.6 |
| Core C tests | `make test_core` | yes (CI) | `test/c/test_core.c` under ASan + UBSan: costs vs naive formulas, PELT vs Optimal Partitioning, Dynp vs exhaustive search, valid segmentations |
| ruptures cross-check | `make compare_ruptures` | manual | 200 random series: PELT (l2, l1), Binseg (pen, n_bkps), Dynp |
| Optimality | `make check_optimality` | manual | PELT vs unpruned O(n²) Optimal Partitioning, all costs, min_size 1–5, value penalties and MBIC |
| Benchmark | `make bench` | manual | l2 PELT at n = 1e4, 1e5, 1e6 |

## SQL tests (`test/sql/`)

| File | Content |
|---|---|
| `cpt_basic.test` | load, empty/NULL, argument errors, overloads, constant vectors, 3000 groups |
| `cpt_pelt.test` | synthetic mean, variance, Poisson and outlier series; min_size; GROUP BY |
| `cpt_binseg_dynp.test` | binseg = dynp = truth on separable series; stopping rules; dynp ≤ binseg cost |
| `cpt_crops.test` | sweep covers PELT, ordering, reproducibility, min_size |
| `cpt_segment_stats.test` | statistics, README examples for labelling rows |
| `cpt_reference.test` | Nile, Lai2005fig4, wave.c44137 and ftse100 against R changepoint |

The synthetic series are generated in SQL right after `SELECT setseed(0.42)`. They are smaller than
one vector, so a single thread generates them, and DuckDB 1.5.6 and 2.0 produce the same values.

Results are in the 1-based convention. Expected values were checked by reasoning about the costs,
recorded in the comments next to the less obvious ones, and not copied from the output.

## Reference data (`test/data/`)

`tools/export_reference.R` writes the CSV files and `expected_*.json`, which hold changepoint's call,
penalty value, `cpts` and `bkps`. Re-run it with `make reference-data` (rocker/r-ver in Apple container
or Docker). The committed files come from changepoint 2.3 on R 4.4.2.

- **Nile.** `cpt.mean` on the series scaled by its sd gives `[28, 100]` for BIC and MBIC.
  `cpt.meanvar` gives BIC `[4, 6, 28, 97, 100]` and MBIC `[4, 6, 28, 100]`. All match exactly.
- **Lai2005fig4.** Column 5 is GBM29 (the vignette's column; the data has no GBM31). BIC and MBIC
  both give `[81, 85, 89, 96, 123, 133, 193]`, and both match.
- **wave.c44137, ftse100** (`cpt.meanvar`). changepoint's answers are not optimal: it prunes without
  regard to minseglen = 2. The test checks that cpt's penalised cost, computed in SQL from
  `cpt_segment_stats`, is ≤ changepoint's, and that at least 80% of changepoint's change points
  appear in cpt's answer.

## Running a single test file

```sh
configure/venv/bin/python3 -m duckdb_sqllogictest --test-dir test/sql \
    --external-extension build/release/cpt.duckdb_extension
```

The runner stops at the first failing file.
