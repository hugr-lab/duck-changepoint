// Leak-check host: links libduckdb, loads the cpt extension and runs every SQL function, including
// error paths, many times. Run it under a leak detector:
//
//   make leak_check                     (macOS: leaks --atExit; Linux: valgrind --leak-check=full)
//
// Anything cpt allocates and does not free (cost contexts, PELT/DP workspaces, CROPS entries,
// logical types, function sets) shows up as a leak with a stack inside libcpt / cpt.duckdb_extension.

#include "duckdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *setup[] = {
    "SELECT setseed(0.42)",
    "CREATE TABLE s AS SELECT g, list((CASE WHEN i < 100 THEN 0 WHEN i < 200 THEN 5 ELSE -2 END) + g "
    "  + sqrt(-2 * ln(random())) * cos(2 * pi() * random()) ORDER BY i) AS xs "
    "FROM range(300) t(i), range(8) r(g) GROUP BY g",
    "CREATE TABLE c AS SELECT list((i // 50 % 3) * 3 + (i % 4) ORDER BY i)::DOUBLE[] AS xs FROM range(300) t(i)",
};

// Queries that must succeed. Results are fetched and destroyed every time.
static const char *ok_queries[] = {
    "SELECT cpt_pelt(xs) FROM s",
    "SELECT cpt_pelt(xs, 'l2', 'mbic', 3) FROM s",
    "SELECT cpt_pelt(xs, 'normal', NULL, 2) FROM s",
    "SELECT cpt_pelt(xs, 'l1', 10.0) FROM s",
    "SELECT cpt_pelt(xs, 'poisson', 'aic') FROM c",
    "SELECT cpt_binseg(xs, 'l2', 3) FROM s",
    "SELECT cpt_binseg(xs, 'l1', NULL, 20.0, 5) FROM s",
    "SELECT cpt_dynp(xs, 2, 'normal', 4) FROM s",
    "SELECT cpt_crops(xs, 'l2', 1, 500) FROM s",
    "SELECT cpt_crops(xs, 'l1', 1, 500, 3) FROM s",
    "SELECT cpt_segment_stats(xs, cpt_pelt(xs, 'l2')) FROM s",
    "SELECT cpt_segment_stats([4, 1, 2, 3]::DOUBLE[], [1, 4])",
    "SELECT cpt_pelt([]::DOUBLE[]), cpt_pelt(NULL::DOUBLE[]), cpt_crops([]::DOUBLE[], 'l2', 1, 2)",
    "SELECT count(*) FROM (SELECT cpt_pelt([0, 0, 0, 5, 5, 5]::DOUBLE[], 'l2', 1.0) FROM range(5000))",
};

// Queries that must fail: the error path must free everything allocated before the failure.
static const char *error_queries[] = {
    "SELECT cpt_pelt([1.0, NULL, 3.0])",
    "SELECT cpt_pelt(xs, 'l3') FROM s",
    "SELECT cpt_pelt(xs, 'l2', 'hqc') FROM s",
    "SELECT cpt_pelt(xs, 'l2', -1.0) FROM s",
    "SELECT cpt_pelt([1.0, 2.5], 'poisson')",
    "SELECT cpt_binseg(xs, 'l2', 1, 5.0) FROM s",
    "SELECT cpt_dynp(xs, 1000) FROM s",
    "SELECT cpt_crops(xs, 'l2', 5, 1) FROM s",
    "SELECT cpt_segment_stats([1.0, 2.0, 3.0], [1, 2])",
    // the error is raised on a later row, after earlier rows wrote output
    "SELECT cpt_pelt(CASE WHEN g = 7 THEN [1.0, 'nan'::DOUBLE] ELSE xs END) FROM s",
};

static int run(duckdb_connection con, const char *sql, int expect_ok) {
	duckdb_result res;
	duckdb_state st = duckdb_query(con, sql, &res);
	int ok = (st == DuckDBSuccess) == expect_ok;
	if (!ok) {
		fprintf(stderr, "unexpected %s: %s\n  %s\n", expect_ok ? "error" : "success", sql,
		        st == DuckDBSuccess ? "" : duckdb_result_error(&res));
	}
	duckdb_destroy_result(&res);
	return ok;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <cpt.duckdb_extension> [iterations]\n", argv[0]);
		return 2;
	}
	int iterations = argc > 2 ? atoi(argv[2]) : 50;
	int failures = 0;

	duckdb_config config;
	duckdb_create_config(&config);
	duckdb_set_config(config, "allow_unsigned_extensions", "true");
	duckdb_set_config(config, "threads", "4");
	duckdb_database db;
	char *open_error = NULL;
	if (duckdb_open_ext(NULL, &db, config, &open_error) != DuckDBSuccess) {
		fprintf(stderr, "open failed: %s\n", open_error ? open_error : "?");
		duckdb_free(open_error);
		return 1;
	}
	duckdb_destroy_config(&config);
	duckdb_connection con;
	duckdb_connect(db, &con);

	char load[4096];
	snprintf(load, sizeof(load), "LOAD '%s'", argv[1]);
	failures += !run(con, load, 1);
	for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
		failures += !run(con, setup[i], 1);
	}
	for (int it = 0; it < iterations && failures == 0; it++) {
		for (size_t i = 0; i < sizeof(ok_queries) / sizeof(ok_queries[0]); i++) {
			failures += !run(con, ok_queries[i], 1);
		}
		for (size_t i = 0; i < sizeof(error_queries) / sizeof(error_queries[0]); i++) {
			failures += !run(con, error_queries[i], 0);
		}
	}

	duckdb_disconnect(&con);
	duckdb_close(&db);
	printf("%d iterations, %d unexpected results\n", iterations, failures);
	return failures ? 1 : 0;
}
