-- l2 PELT runtime for n = 1e4, 1e5, 1e6.
--
--   make bench                  (bench/run_bench.py: loads the extension and times each query)
--   duckdb -unsigned -c ".read bench/bench.sql"   after LOAD, with .timer on
--
-- PELT is linear in expectation when the number of change points grows with n (Killick et al. 2012,
-- Theorem 3.2). The generator therefore uses segments of 600 points (n/600 change points);
-- a handful of change points in 1e6 points would leave PELT close to O(n^2) because nothing is pruned
-- inside a long homogeneous segment.
--
-- Means and noise come from sin()-based uniforms (noise via Box-Muller) rather than random()/hash(),
-- so every DuckDB version generates the same series.

.timer on

CREATE OR REPLACE MACRO cpt_bench_u(i, k) AS
    (abs(sin((i + 1) * 12.9898 + k * 78.233) * 43758.5453) % 1.0) * 0.999998 + 0.000001;

CREATE OR REPLACE MACRO cpt_bench_series(n) AS TABLE
    WITH pts AS (
        SELECT i,
               round(10 * cpt_bench_u(i // 600, 3)) - 5 AS mean,
               sqrt(-2 * ln(cpt_bench_u(i, 1))) * cos(2 * pi() * cpt_bench_u(i, 2)) AS noise
        FROM range(n) t(i)
    )
    SELECT list(mean + noise ORDER BY i) AS xs FROM pts;

CREATE OR REPLACE TABLE bench_1e4 AS FROM cpt_bench_series(10000);
CREATE OR REPLACE TABLE bench_1e5 AS FROM cpt_bench_series(100000);
CREATE OR REPLACE TABLE bench_1e6 AS FROM cpt_bench_series(1000000);

SELECT len(cpt_pelt(xs, 'l2')) AS n_segments FROM bench_1e4;
SELECT len(cpt_pelt(xs, 'l2')) AS n_segments FROM bench_1e5;
SELECT len(cpt_pelt(xs, 'l2')) AS n_segments FROM bench_1e6;
