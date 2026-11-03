# DuckDB Changepoint Extension (`cpt`)

Offline change point detection inside DuckDB: PELT, binary segmentation, exact dynamic programming
and CROPS penalty sweeps over four segment costs (mean, mean + variance, Poisson rate, median).
Written in C11 against the **stable DuckDB C extension API**, so one binary loads in DuckDB 1.5.x
and 2.0.x.

```sql
LOAD cpt;

SELECT cpt_pelt(list(value ORDER BY ts), 'l2') AS bkps FROM readings;
-- [250, 500, 750, 1000]
```

## Contents

- [Installation](#installation)
- [Functions](#functions)
- [Examples](#examples)
- [Costs](#costs)
- [Penalties](#penalties)
- [Differences from ruptures and changepoint](#differences-from-ruptures-and-changepoint)
- [Performance](#performance)
- [Limits and cancellation](#limits-and-cancellation)
- [Building and testing](#building-and-testing)
- [License](#license)
- [Talks](#talks)
- [References](#references)

Further documentation: [docs/examples-from-talks.md](docs/examples-from-talks.md), [docs/architecture.md](docs/architecture.md), [docs/algorithms.md](docs/algorithms.md),
[docs/costs-and-penalties.md](docs/costs-and-penalties.md), [docs/TESTING.md](docs/TESTING.md),
[DEVELOPMENT.md](DEVELOPMENT.md).

## Installation

Build it (see [Building and testing](#building-and-testing)) and load the unsigned binary:

```sh
duckdb -unsigned -c "LOAD 'build/release/cpt.duckdb_extension'; SELECT cpt_pelt([0,0,0,9,9,9]::DOUBLE[], 'l2', 1.0);"
```

From Python: `duckdb.connect(config={"allow_unsigned_extensions": "true"})`, then `LOAD '<path>'`.

The binary carries the `C_STRUCT` ABI footer with `v1.5.6` as its minimum DuckDB version. It loads in
DuckDB v1.5.6 and later, including 2.0.x. `make test_compat` runs the SQL tests on 1.5.6 to check this.

## Functions

Every function takes one ordered series per row as `DOUBLE[]` (build it with
`list(x ORDER BY ts)`; `INTEGER[]` and `BIGINT[]` cast implicitly). The results are the **1-based
end positions of the segments**, in increasing order, with the last element equal to `n`:
`[250, 500, 1000]` means rows 1–250, 251–500 and 501–1000.

- A `NULL` series returns `NULL`.
- An empty list returns `[]`.
- A `NULL`, `NaN` or `±inf` element raises an error.

The C API has no `DEFAULT` parameters, so each function is registered as a set of overloads, one per
number of trailing arguments. Passing `NULL` for an optional argument means "use the default".

| Function | Overloads | Returns |
|---|---|---|
| `cpt_pelt` | `(series)`<br>`(series, cost)`<br>`(series, cost, penalty DOUBLE)`<br>`(series, cost, penalty DOUBLE, min_size)`<br>`(series, cost, penalty_name VARCHAR)`<br>`(series, cost, penalty_name VARCHAR, min_size)` | `INTEGER[]` |
| `cpt_binseg` | `(series)`<br>`(series, cost)`<br>`(series, cost, n_bkps)`<br>`(series, cost, n_bkps, penalty)`<br>`(series, cost, n_bkps, penalty, min_size)` | `INTEGER[]` |
| `cpt_dynp` | `(series, n_bkps)`<br>`(series, n_bkps, cost)`<br>`(series, n_bkps, cost, min_size)` | `INTEGER[]` |
| `cpt_crops` | `(series, cost, pen_min, pen_max)`<br>`(series, cost, pen_min, pen_max, min_size)` | `STRUCT(penalty DOUBLE, n_bkps INTEGER, bkps INTEGER[])[]` |
| `cpt_segment_stats` | `(series, bkps)` | `STRUCT(start INTEGER, "end" INTEGER, mean DOUBLE, var DOUBLE, n INTEGER)[]` |

Argument types: `series DOUBLE[]`, `cost VARCHAR`, `penalty`/`pen_min`/`pen_max DOUBLE`,
`n_bkps`/`min_size BIGINT` (integer literals bind directly), `bkps BIGINT[]` (the `INTEGER[]`
returned by the other functions widens implicitly).

Defaults: `cost = 'l2'`, `penalty = NULL` (BIC), `min_size = 1`.

**`cpt_pelt`**: the exact minimiser of *Σ segment cost + β · (number of change points)*, found with
PELT (Killick, Fearnhead & Eckley 2012). `penalty` is a number β ≥ 0, or one of `'bic'`, `'mbic'` or
`'aic'` (case-insensitive). `NULL` means BIC.

**`cpt_binseg`**: greedy binary segmentation. Pass `n_bkps` to stop after that many splits, or
`penalty` to stop once the best split gains no more than the penalty. Passing both is an error.
Passing neither uses BIC. If the greedy search runs out of admissible splits, it returns fewer than
`n_bkps` change points.

**`cpt_dynp`**: the optimal split into exactly `n_bkps + 1` segments (segment neighbourhood, as in
ruptures' `Dynp` and changepoint's `SegNeigh`). It is exact and takes O(n_bkps · n²) time.

**`cpt_crops`**: every distinct segmentation that PELT produces for penalties in `[pen_min, pen_max]`
(Haynes, Eckley & Fearnhead 2017). The result is ordered by `penalty` ascending, which is `n_bkps`
descending. `penalty` is the smallest penalty in the sweep at which PELT returned that segmentation,
so `cpt_pelt(series, cost, penalty)` reproduces `bkps`.

**`cpt_segment_stats`**: per-segment start, end (1-based, inclusive), mean, sample variance (`NULL`
for a one-element segment) and length. `bkps` must be strictly increasing and end at `n`.

`min_size` is the minimum number of points in a segment. The `normal` cost always uses at least 2.

## Examples

Basic use on a time series table:

```sql
SELECT cpt_pelt(list(value ORDER BY ts), 'l2') AS bkps
FROM readings;
```

Per group, with the default BIC penalty and a minimum segment length of 5:

```sql
SELECT sensor_id,
       cpt_pelt(list(value ORDER BY ts), 'normal', NULL, 5) AS bkps
FROM readings
GROUP BY sensor_id;
```

Labelling every row with its segment number:

```sql
WITH s AS (
  SELECT sensor_id,
         list(value ORDER BY ts) AS xs,
         list(ts ORDER BY ts)    AS tss
  FROM readings GROUP BY sensor_id
), b AS (
  SELECT sensor_id, xs, tss, cpt_pelt(xs, 'l2') AS bkps FROM s
)
SELECT sensor_id,
       unnest(tss) AS ts,
       unnest(xs)  AS value,
       -- segment number = 1 + number of segment ends strictly before this position
       unnest(list_transform(range(1, len(xs) + 1),
              lambda pos: len(list_filter(bkps, lambda e: e < pos)) + 1)) AS segment
FROM b;
```

DuckDB 2.0 rejects the old `x -> ...` lambda syntax by default. Use `lambda x: ...`, which works in 1.5
and 2.0. For long series, joining row positions (`generate_subscripts(xs, 1)`) to
`cpt_segment_stats` ranges avoids the O(n · k) lambda; `test/sql/cpt_segment_stats.test` shows both
forms.

Segment statistics:

```sql
SELECT sensor_id, unnest(cpt_segment_stats(xs, cpt_pelt(xs, 'l2')))
FROM s;
```

Penalty sweep with CROPS:

```sql
SELECT unnest(cpt_crops(xs, 'l2', 2 * ln(len(xs)), 50 * ln(len(xs))))
FROM s;
```

Known number of change points:

```sql
SELECT cpt_dynp(xs, 3, 'l2') FROM s;
```

Generating test series in SQL (mirrors `ruptures.pw_constant`): 4 segments of 250, noise σ = 1,
with Box-Muller normals:

```sql
SELECT setseed(0.42);
SELECT cpt_pelt(list(
         CASE WHEN i < 250 THEN 0 WHEN i < 500 THEN 5 WHEN i < 750 THEN -3 ELSE 2 END
         + sqrt(-2 * ln(random())) * cos(2 * pi() * random())
         ORDER BY i), 'l2')
FROM range(1000) t(i);
-- [250, 500, 750, 1000]
```

## Costs

C(a, b) is the cost of the segment of points a+1..b, with length *n* = b − a and segment sum *S*.
Every cost is evaluated in O(1) after an O(n) pass of prefix sums, except `l1`.

| `cost` | C(a, b) | Models | p | Evaluation |
|---|---|---|---|---|
| `'l2'` | Σ (xᵢ − x̄)² | change in mean, known variance | 1 | O(1) |
| `'normal'` | n · ln σ̂², σ̂² = Σ (xᵢ − x̄)² / n | change in mean and variance | 2 | O(1) |
| `'poisson'` | −2 (S ln(S/n) − S), 0 when S = 0 | change in rate of counts | 1 | O(1) |
| `'l1'` | Σ \|xᵢ − median\| | change in median (robust) | 1 | O(log n) |

- `normal` floors σ̂² at 1e-11, the same as changepoint, so runs of equal values do not get cost −∞.
  The floor is absolute, so rescale series whose variance is below about 1e-9. Segments have at
  least 2 points.
- `poisson` requires non-negative integer values and raises an error otherwise.
- `l1` answers median and partial-sum queries with a wavelet matrix: O(n log n) to build,
  O(log n) per segment, and about 12 · n · log₂ n bytes of memory (≈ 240 MB at n = 1e6).

All four satisfy C(a, s) + C(s, b) ≤ C(a, b), so PELT prunes with K = 0 (Killick et al. 2012,
Theorem 3.1). See [docs/costs-and-penalties.md](docs/costs-and-penalties.md) for the derivations.

## Penalties

The penalty is charged per change point, with p parameters per segment as listed in the table above:

| name | β | extra |
|---|---|---|
| `'bic'` (default) | (p + 1) · ln n | |
| `'mbic'` | (p + 2) · ln n | + Σ ln(nᵢ) over segments (Zhang & Siegmund 2007) |
| `'aic'` | 2 (p + 1) | |

These are the values used by R's changepoint, where the change location counts as one more
parameter. For `l2`, BIC is 2 ln n.

BIC assumes the cost is a −2 log-likelihood. For `l2` that means unit noise variance: divide the
series by its noise standard deviation (as changepoint's `cpt.mean` expects), or pass an explicit
penalty such as `2 * σ² * ln(n)`. `l1` has no likelihood scale, so treat its BIC as a starting point
and use `cpt_crops` to see the alternatives.

## Differences from ruptures and changepoint

| | cpt | ruptures | changepoint (R) |
|---|---|---|---|
| Output | segment ends, 1-based, incl. n: `[28, 100]` | 0-based exclusive ends incl. n: same numbers | `cpts()`: last index of each segment except the final one: `28` |
| Penalty unit | per change point | per segment (same optimum) | per change point |
| Default penalty | BIC | none (`pen` is required) | MBIC |
| PELT `min_size` / `minseglen` default | 1 (2 for `normal`) | `min_size=2`, `jump=5` | 1 for mean, 2 for meanvar |
| PELT pruning with `min_size > 1` | exact (below) | heuristic | heuristic |
| `normal` cost constant | n ln σ̂² | n ln σ̂² (+ small diagonal) | n (ln 2π + ln σ̂² + 1): same optimum |
| `l1` cost | yes | `CostL1` | no |
| Binseg stop | `n_bkps`, or gain > penalty | same | same |

**Exact pruning with a minimum segment length.** PELT drops a candidate s at time t when
F(s) + C(s, t) + K > F(t). The proof that s can never become optimal again uses t as the last change
before a later t′, and with a minimum segment length m that option only exists for t′ ≥ t + m. ruptures
and changepoint drop s immediately and can lose the optimum when m > 1. cpt keeps such candidates
until t + m. On the `wave.c44137` and `ftse100` reference series, changepoint's
`cpt.meanvar(method = "PELT")` returns segmentations with a higher penalised cost than unpruned
Optimal Partitioning finds, while cpt matches it. The companion repository
[duck-changepoint-reference](#reference-data-against-r-changepoint) checks this.

**Matching ruptures.** Use `Pelt(model="l2", min_size=m, jump=1).fit(x).predict(pen=β)` against
`cpt_pelt(x, 'l2', β, m)`. `test/python/compare_ruptures.py` does this over random series (PELT `l2`
and `l1`, Binseg, Dynp), and all results agree. The only exceptions are a few `l1` ties, where both
answers have the same cost to the last digit.

**Matching changepoint.** `cpt.mean(x, method = "PELT", penalty = "BIC")` corresponds to
`cpt_pelt(x, 'l2', 'bic')` (scale x to unit variance first), and `cpt.meanvar` corresponds to
`'normal'`. On Nile and Lai2005fig4 the results are identical (see
[Reference data against R changepoint](#reference-data-against-r-changepoint)).

## Performance

`make bench` (Apple M-series laptop, DuckDB 2.0.0, l2 PELT, segments of 600 points):

| n | time | segments |
|---|---|---|
| 1e4 | 0.01 s | 16 |
| 1e5 | 0.08 s | 152 |
| 1e6 | 0.84 s | 1516 |

PELT runs in linear expected time when the number of change points grows with n. With only a few
change points in a very long series, little can be pruned inside the long segments and the run time
approaches O(n²). Binary segmentation stays O(n log n) in that case: on 1e5 points with 3 changes it
takes 2 ms, against 0.1 s for l2 PELT and about 6 s for l1 PELT, where each evaluation costs
O(log n). `cpt_dynp` is O(n_bkps · n²) and meant for series up to roughly 1e4 points.

## Limits and cancellation

A scalar function in the C API cannot see that its query was interrupted: once a `cpt_*` call has
started on a series, it runs to the end. To keep that bounded, every call (one series) has a work limit:

```sql
SET cpt_max_evaluations = 1000000000;   -- default; 0 = no limit
```

The unit is one segment cost evaluation. An `l1` evaluation counts log₂ n units and a `normal`
evaluation counts 2, so the limit means roughly the same time for every cost. At the default, a call
stops after about 2–3 s on a laptop with an error that says why. PELT on 200,000 points with a single
change would otherwise take about 25 s (l2) or several minutes (l1). The 1e6-point benchmark above uses
about 3.8 × 10⁸ units. The limit applies to PELT, Binseg, Dynp and to a whole CROPS sweep.

**The limit is per series, not per query.** DuckDB checks for an interrupt only between calls of a
scalar function, and one call processes a whole vector of up to 2,048 rows (series). A query over many
series that each stay under the limit therefore cannot be cancelled until the current vector is done:
32 series of 40,000 points with one change each took 18.8 s, and an interrupt after 1 s returned only
at the end ([#2](https://github.com/hugr-lab/duck-changepoint/issues/2)). In the worst case that is
2,048 × the per-series limit. On servers where users send their own SQL, lower the limit accordingly.
Series in the same vector also run one after another on a single thread. A proper fix needs an
interrupt check for scalar functions in the DuckDB C API, which does not exist yet.

When the limit is hit, the error message suggests what to do. For long series with few changes,
`cpt_binseg` (O(n log n)) finds the same change in milliseconds. To raise or remove the limit,
`SET cpt_max_evaluations`. On a server where users send their own SQL, set it globally
(`SET GLOBAL cpt_max_evaluations = ...`) and, if needed, stop users from changing it with DuckDB's
`SET lock_configuration = true`.

## Building and testing

Requirements: CMake ≥ 3.5, a C11 compiler, Python 3 (for the build helpers and test runner) and git.

```sh
git clone --recurse-submodules <repo> && cd duck-changepoint
make configure        # venv with the DuckDB test runner, platform detection
make release          # build/release/cpt.duckdb_extension
make test             # SQL logic tests (debug build) on the latest DuckDB
make test_compat      # same tests on DuckDB 1.5.6
make test_core        # C tests of the numerical core under ASan/UBSan
make test_threads     # the core on 8 threads under ThreadSanitizer
```

These checks are run by hand and do not gate CI:

```sh
make compare_ruptures   # 200 random series against ruptures
make check_optimality   # PELT against unpruned Optimal Partitioning
make bench              # timings above
make leak_check         # every function under macOS `leaks` (needs `brew install duckdb`)
make joern_leaks        # static allocation/release audit with Joern
make crap               # CRAP < 10 for every C function (lizard + llvm-cov)
```

See [DEVELOPMENT.md](DEVELOPMENT.md) and [docs/TESTING.md](docs/TESTING.md).

### Reference data against R changepoint

The comparison with R's changepoint package on Nile, Lai2005fig4, wave.c44137 and ftse100 lives in a
separate repository, [hugr-lab/duck-changepoint-reference](https://github.com/hugr-lab/duck-changepoint-reference), because three of those datasets are distributed with
changepoint under the GPL. With both repositories checked out side by side:

```sh
cd ../duck-changepoint-reference
make test                         # against ../duck-changepoint/build/release/cpt.duckdb_extension
make test DUCKDB_VERSION=1.5.6
make reference-data               # regenerate the data with R changepoint in a container
```

## License

MIT, see [LICENSE](LICENSE). The bundled DuckDB C API headers in `duckdb_capi/` are MIT-licensed by the
DuckDB Foundation ([LICENSES/](LICENSES/README.md)). This repository contains no GPL code or data:
the algorithms are implemented from the published papers, and the changepoint-derived reference data is
kept in `duck-changepoint-reference` (GPL-2.0-or-later).

## Talks

The examples from both talks are rebuilt in SQL in [docs/examples-from-talks.md](docs/examples-from-talks.md).

- **Paul Fearnhead, "Fast Online Changepoint Detection via Functional Pruning CUSUM Statistics"**
  ([recording](https://media.ed.ac.uk/media/Fast+Online+Changepoint+Detection+via+Functional+Pruning+CUSUM+Statistics+-+Paul+Fearnhead/1_w941egwk),
  University of Edinburgh Media Hopper, ICMS workshop "Structural Breaks and Shape Constraints",
  May 2022).
  One of PELT's authors on functional pruning, the idea behind FPOP and the online detector FOCuS
  ([JMLR 2023](https://www.jmlr.org/beta/papers/v24/21-1230.html)). cpt implements PELT's
  inequality-based pruning, not functional pruning; the talk shows where the field went next.
- **Matt Fleming, "Using Change Point Detection to Fight Noisy Benchmark Results"**
  ([session page](https://www.p99conf.io/session/using-change-point-detection-to-fight-noisy-benchmark-results/),
  P99 CONF, 16 min, free registration required to watch;
  [slides](https://www.slideshare.net/slideshow/using-change-point-detection-to-fight-noisy-benchmark-results-by-matt-fleming/272474341)).
  Change point detection on noisy performance data in practice, with the open-source tools Hunter
  and Nyrkiö ([Hunter paper](https://arxiv.org/abs/2301.03034)).

## References

- Killick, R., Fearnhead, P., Eckley, I. A. (2012). Optimal detection of changepoints with a linear
  computational cost. *JASA* 107(500), 1590–1598.
- Truong, C., Oudre, L., Vayatis, N. (2020). Selective review of offline change point detection
  methods. *Signal Processing* 167, 107299.
- Haynes, K., Eckley, I. A., Fearnhead, P. (2017). Computationally efficient changepoint detection
  for a range of penalties. *JCGS* 26(1), 134–143.
- Zhang, N. R., Siegmund, D. O. (2007). A modified Bayes information criterion with applications to
  the analysis of comparative genomic hybridization data. *Biometrics* 63(1), 22–32.
- Jackson, B. et al. (2005). An algorithm for optimal partitioning of data on an interval.
  *IEEE Signal Processing Letters* 12(2), 105–108.
