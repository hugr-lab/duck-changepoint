#!/usr/bin/env python3
"""Run bench/bench.sql with the given extension and print the runtime of each cpt_pelt query.

    make bench
    python bench/run_bench.py build/release/cpt.duckdb_extension
"""

import pathlib
import sys
import time

import duckdb


def main() -> int:
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{sys.argv[1]}'")
    sql = pathlib.Path(__file__).with_name("bench.sql").read_text()
    statements = [s.strip() for s in sql.split(";")]
    for stmt in statements:
        body = "\n".join(line for line in stmt.splitlines() if not line.startswith(("--", ".")))
        if not body.strip():
            continue
        if "cpt_pelt" not in body:
            con.execute(body)
            continue
        table = body.split("FROM")[-1].strip()
        start = time.perf_counter()
        n_segments = con.execute(body).fetchone()[0]
        elapsed = time.perf_counter() - start
        print(f"{table:10s} l2 PELT: {elapsed:8.3f} s  ({n_segments} segments)")
    print(f"duckdb {duckdb.__version__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
