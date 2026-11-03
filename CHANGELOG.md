# Changelog

## Unreleased

- Online change detection: `cpt_online_new`, `cpt_online_update` and `cpt_online` (FOCuS0, exact
  Page-CUSUM over every change size, O(log n) per point). The detector state is a BLOB value that can
  live in a table; feeding in any batches gives the same alarms as one backtest. See docs/online.md.
- Code restructured into small, single-purpose functions: every C function now has CRAP < 10
  (`make crap`, checked in CI; previously 11 functions were at or above 10, the worst at 35).
- Out-of-memory paths are tested by fault injection in `test_core`, and checked for leaks.
- Clearer error when `cpt_crops` gets a NULL `pen_min` or `pen_max`.

## 0.1.0 (2026-11-02)

First release.

- `cpt_pelt`, `cpt_binseg`, `cpt_dynp`, `cpt_crops` and `cpt_segment_stats` on `DOUBLE[]` series.
- Costs `l2`, `normal` (mean and variance), `poisson` and `l1` (median, O(log n) evaluation through a
  wavelet matrix).
- Penalties `bic`, `mbic` and `aic` with R changepoint's values, or any value ≥ 0.
- PELT pruning stays exact with `min_size > 1`, where ruptures and changepoint prune heuristically.
- Stable C API, minimum DuckDB v1.5.6; tested on 1.5.6 and 2.0.0.
- `cpt_max_evaluations` setting (default 1e9, 0 = no limit) bounds the work of one call, since a
  running C API scalar function cannot be interrupted.
- MIT license. The comparison data from R changepoint (GPL) is kept in the separate
  `duck-changepoint-reference` repository.
