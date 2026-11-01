#!/usr/bin/env python3
"""Cross-check cpt_pelt / cpt_binseg / cpt_dynp against ruptures (manual, not CI gating).

    make compare_ruptures
    # or: python test/python/compare_ruptures.py build/release/cpt.duckdb_extension [--n-series 200] [--seed 1]

Conventions: ruptures returns 0-based exclusive segment ends including n_samples, e.g. [250, 500, 1000].
Read as 1-based inclusive ends these are the same numbers, so the lists compare element-wise without
shifting. ruptures defaults (min_size=2, jump=5) differ from ours; we pass min_size explicitly and jump=1.
Penalties match exactly: ruptures adds `pen` per segment, cpt adds it per change point, which only shifts
the objective by a constant.
"""

import argparse
import sys

import duckdb
import numpy as np
import ruptures as rpt


def make_series(rng: np.random.Generator) -> np.ndarray:
    n = int(rng.integers(30, 300))
    n_bkps = int(rng.integers(0, 6))
    ends = sorted(set(rng.integers(5, n - 5, size=n_bkps).tolist())) + [n]
    x = np.empty(n)
    start = 0
    for end in ends:
        x[start:end] = rng.normal(rng.uniform(-5, 5), rng.uniform(0.5, 2.0), size=end - start)
        start = end
    return x


def objective(x: np.ndarray, bkps: list[int], model: str, pen: float) -> float:
    """Penalised cost of a segmentation, computed directly with numpy."""
    total, start = 0.0, 0
    for end in bkps:
        seg = x[start:end]
        if model == "l1":
            total += float(np.abs(seg - np.median(seg)).sum())
        else:
            total += float(((seg - seg.mean()) ** 2).sum())
        start = end
    return total + pen * len(bkps)


def cpt(con: duckdb.DuckDBPyConnection, sql: str, x: np.ndarray, *params) -> list[int]:
    return con.execute(sql, [x.tolist(), *params]).fetchone()[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("extension")
    parser.add_argument("--n-series", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{args.extension}'")
    rng = np.random.default_rng(args.seed)

    failures = 0
    checks = 0
    ties = 0
    for i in range(args.n_series):
        x = make_series(rng)
        n = len(x)
        min_size = int(rng.integers(1, 6))
        pen = float(rng.uniform(1.0, 4.0) * np.log(n))
        k = int(rng.integers(1, 4))

        cases = []
        for model in ("l2", "l1"):
            expected = rpt.Pelt(model=model, min_size=min_size, jump=1).fit(x).predict(pen=pen)
            got = cpt(con, "SELECT cpt_pelt($1::DOUBLE[], $2, $3::DOUBLE, $4)", x, model, pen, min_size)
            cases.append((f"pelt/{model}", expected, got, model))

        expected = rpt.Binseg(model="l2", min_size=min_size, jump=1).fit(x).predict(pen=pen)
        got = cpt(con, "SELECT cpt_binseg($1::DOUBLE[], 'l2', NULL, $2::DOUBLE, $3)", x, pen, min_size)
        cases.append(("binseg/pen", expected, got, None))

        if (k + 1) * min_size <= n:
            expected = rpt.Binseg(model="l2", min_size=min_size, jump=1).fit(x).predict(n_bkps=k)
            got = cpt(con, "SELECT cpt_binseg($1::DOUBLE[], 'l2', $2, NULL, $3)", x, k, min_size)
            cases.append(("binseg/n_bkps", expected, got, None))

        # ruptures' Dynp is pure Python and O(K n^2) with an lru_cache; keep it to short series.
        if n <= 150 and (k + 1) * min_size <= n:
            expected = rpt.Dynp(model="l2", min_size=min_size, jump=1).fit(x).predict(n_bkps=k)
            got = cpt(con, "SELECT cpt_dynp($1::DOUBLE[], $2, 'l2', $3)", x, k, min_size)
            cases.append(("dynp", expected, got, None))

        for name, expected, got, model in cases:
            checks += 1
            if list(expected) == list(got):
                continue
            # PELT is exact: a different answer is only acceptable if it is an equally good optimum
            # (common for l1, whose cost is piecewise linear in the boundary position).
            if model is not None:
                a, b = objective(x, list(expected), model, pen), objective(x, got, model, pen)
                if abs(a - b) <= 1e-9 * max(1.0, abs(a)):
                    ties += 1
                    continue
            failures += 1
            print(f"series {i} (n={n}, min_size={min_size}, pen={pen:.4f}, k={k}) {name}: "
                  f"ruptures={expected} cpt={got}")

    print(f"{checks - failures}/{checks} checks agree over {args.n_series} series "
          f"({ties} of them equal-cost ties with a different optimum)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
