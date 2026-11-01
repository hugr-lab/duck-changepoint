#!/usr/bin/env python3
"""Check that cpt_pelt returns the exact optimum (manual, not CI gating).

PELT must return the same penalised cost as Optimal Partitioning (Jackson et al. 2005), the O(n^2)
dynamic programme without pruning. This is the test that catches invalid pruning, e.g. pruning that
ignores min_size, which ruptures and changepoint both do.

    python test/python/check_optimality.py build/release/cpt.duckdb_extension [--n-series 200]
"""

import argparse
import sys

import duckdb
import numpy as np

VAR_FLOOR = 1e-11  # same floor as the 'normal' cost


def segment_costs(x: np.ndarray, cost: str, t: int, s: np.ndarray) -> np.ndarray:
    """Cost of every segment [s_i, t) for a vector of starts s."""
    if cost == "l1":
        return np.array([np.abs(x[a:t] - np.median(x[a:t])).sum() for a in s])
    seg_len = t - s
    csum = np.concatenate([[0.0], np.cumsum(x - x.mean())])
    csq = np.concatenate([[0.0], np.cumsum((x - x.mean()) ** 2)])
    if cost == "poisson":
        total = np.concatenate([[0.0], np.cumsum(x)])
        tot = total[t] - total[s]
        with np.errstate(divide="ignore", invalid="ignore"):
            return np.where(tot > 0, -2.0 * (tot * np.log(tot / seg_len) - tot), 0.0)
    ss = np.maximum((csq[t] - csq[s]) - (csum[t] - csum[s]) ** 2 / seg_len, 0.0)
    if cost == "l2":
        return ss
    return seg_len * np.log(np.maximum(ss / seg_len, VAR_FLOOR))  # normal


def optimal_partitioning(x: np.ndarray, cost: str, beta: float, min_size: int, mbic: bool) -> float:
    n = len(x)
    f = np.full(n + 1, np.inf)
    f[0] = -beta
    for t in range(min_size, n + 1):
        s = np.arange(0, t - min_size + 1)
        s = s[np.isfinite(f[s])]
        c = f[s] + segment_costs(x, cost, t, s) + beta
        if mbic:
            c = c + np.log(t - s)
        f[t] = c.min()
    return float(f[n])


def objective(x: np.ndarray, cost: str, bkps: list[int], beta: float, mbic: bool) -> float:
    total, start = -beta, 0
    for end in bkps:
        total += float(segment_costs(x, cost, end, np.array([start]))[0]) + beta
        if mbic:
            total += np.log(end - start)
        start = end
    return total


def make_series(rng: np.random.Generator, cost: str) -> np.ndarray:
    n = int(rng.integers(20, 120 if cost == "l1" else 250))
    ends = sorted(set(rng.integers(3, n - 3, size=int(rng.integers(0, 6))).tolist())) + [n]
    parts, start = [], 0
    for end in ends:
        if cost == "poisson":
            parts.append(rng.poisson(rng.uniform(0.5, 15), size=end - start).astype(float))
        else:
            parts.append(rng.normal(rng.uniform(-3, 3), rng.uniform(0.3, 2.0), size=end - start))
        start = end
    return np.concatenate(parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("extension")
    parser.add_argument("--n-series", type=int, default=200)
    parser.add_argument("--seed", type=int, default=11)
    args = parser.parse_args()

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{args.extension}'")
    rng = np.random.default_rng(args.seed)
    n_params = {"l2": 1, "l1": 1, "poisson": 1, "normal": 2}

    failures = checks = 0
    for i in range(args.n_series):
        cost = ("l2", "normal", "poisson", "l1")[i % 4]
        x = make_series(rng, cost)
        n = len(x)
        min_size = int(rng.integers(1, 6))
        eff_min = max(min_size, 2) if cost == "normal" else min_size
        for penalty in ("value", "mbic"):
            if penalty == "mbic":
                beta, mbic = (n_params[cost] + 2) * np.log(n), True
                got = con.execute("SELECT cpt_pelt($1::DOUBLE[], $2, 'mbic', $3)", [x.tolist(), cost, min_size])
            else:
                beta, mbic = float(rng.uniform(0.2, 3.0) * np.log(n)), False
                got = con.execute("SELECT cpt_pelt($1::DOUBLE[], $2, $3::DOUBLE, $4)",
                                  [x.tolist(), cost, beta, min_size])
            bkps = got.fetchone()[0]
            best = optimal_partitioning(x, cost, beta, eff_min, mbic)
            mine = objective(x, cost, bkps, beta, mbic)
            seg_ok = all(b - a >= eff_min for a, b in zip([0] + bkps[:-1], bkps))
            checks += 1
            if not seg_ok or mine > best + 1e-7 * max(1.0, abs(best)):
                failures += 1
                print(f"series {i} cost={cost} n={n} min_size={min_size} penalty={penalty} beta={beta:.4f}: "
                      f"PELT {mine:.10f} vs OP {best:.10f} (segments ok: {seg_ok})")

    print(f"{checks - failures}/{checks} PELT results equal the Optimal Partitioning optimum")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
