# Changelog

## 0.1.0 (unreleased)

First release.

- `cpt_pelt`, `cpt_binseg`, `cpt_dynp`, `cpt_crops` and `cpt_segment_stats` on `DOUBLE[]` series.
- Costs `l2`, `normal` (mean and variance), `poisson` and `l1` (median, O(log n) evaluation through a
  wavelet matrix).
- Penalties `bic`, `mbic` and `aic` with R changepoint's values, or any value ≥ 0.
- PELT pruning stays exact with `min_size > 1`, where ruptures and changepoint prune heuristically.
- Stable C API, minimum DuckDB v1.5.6; tested on 1.5.6 and 2.0.0.
