// Standalone tests for the numerical core (src/core), built with ASan/UBSan by `make test_core`.
// No DuckDB involved: costs and algorithms are exercised directly on random series.
//
// Checks:
//   - every cost's eval() against a naive O(len) evaluation
//   - PELT equals unpruned Optimal Partitioning (all costs, min_size 1..6, value penalty and MBIC)
//   - Dynp equals an exhaustive search for small n
//   - every algorithm returns valid segmentations (increasing, ends at n, segments >= min_size)
//   - n_bkps and min_size at the int64 limit neither overflow nor crash
//   - every allocation failing in turn gives an "out of memory" error and no leak (needs
//     -DCPT_FAULT_INJECTION, see src/include/cpt_alloc.h; leaks are caught by ASan/LSan and `leaks`)
//   - work limits hit at every stage stop cleanly; invalid arguments are rejected
//   - the online detector: its statistic, alarms and change estimates equal a naive recomputation over
//     every candidate after every point; few candidates survive; the trained baseline is right; feeding in
//     any chunking with encode/decode in between gives identical results; corrupt states are rejected

#include "algo.h"
#include "cost.h"
#include "online.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

#ifdef CPT_FAULT_INJECTION
long cpt_fail_countdown = 0;
#endif

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			failures++;                                                                                                \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
			fprintf(stderr, __VA_ARGS__);                                                                              \
			fprintf(stderr, "\n");                                                                                     \
		}                                                                                                              \
	} while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static double uniform(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return ((rng_state >> 11) + 0.5) / 9007199254740992.0;
}

static double normal(void) {
	return sqrt(-2 * log(uniform())) * cos(2 * M_PI * uniform());
}

static void make_series(double *x, int64_t n, cpt_cost_kind kind) {
	double level = 0, scale = 1;
	for (int64_t i = 0; i < n; i++) {
		if (uniform() < 0.05) {
			level = uniform() * 10 - 5;
			scale = 0.3 + uniform() * 2;
		}
		if (kind == CPT_COST_POISSON) {
			x[i] = floor(fabs(level) * uniform() * 3); // non-negative integers, with ties and zeros
		} else if (uniform() < 0.05) {
			x[i] = round(level); // repeated values exercise ties in l1 and the variance floor
		} else {
			x[i] = level + scale * normal();
		}
	}
}

static int cmp_double(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double naive_cost(cpt_cost_kind kind, const double *x, int64_t a, int64_t b) {
	int64_t len = b - a;
	double mean = 0;
	for (int64_t i = a; i < b; i++) {
		mean += x[i];
	}
	double sum = mean;
	mean /= (double)len;
	double ss = 0;
	for (int64_t i = a; i < b; i++) {
		ss += (x[i] - mean) * (x[i] - mean);
	}
	switch (kind) {
	case CPT_COST_L2:
		return ss;
	case CPT_COST_NORMAL: {
		double var = ss / (double)len;
		return (double)len * log(var < 1e-11 ? 1e-11 : var);
	}
	case CPT_COST_POISSON:
		return sum <= 0 ? 0 : -2.0 * (sum * log(sum / (double)len) - sum);
	case CPT_COST_L1: {
		double *tmp = malloc(sizeof(double) * (size_t)len);
		memcpy(tmp, x + a, sizeof(double) * (size_t)len);
		qsort(tmp, (size_t)len, sizeof(double), cmp_double);
		double med = tmp[(len - 1) / 2], c = 0;
		for (int64_t i = 0; i < len; i++) {
			c += fabs(tmp[i] - med);
		}
		free(tmp);
		return c;
	}
	}
	return NAN;
}

static int close_enough(double a, double b, double tol) {
	return fabs(a - b) <= tol * (1 + fabs(a) + fabs(b));
}

static double penalised(const cpt_cost *cost, const cpt_bkps *b, cpt_penalty pen) {
	double total = -pen.beta;
	int64_t a = 0;
	for (int64_t i = 0; i < b->count; i++) {
		total += cost->eval(cost->ctx, a, b->pos[i]) + pen.beta + (pen.mbic ? log((double)(b->pos[i] - a)) : 0);
		a = b->pos[i];
	}
	return total;
}

// Optimal Partitioning without pruning.
static double optimal_partitioning(const cpt_cost *cost, int64_t n, cpt_penalty pen, int64_t m) {
	double *F = malloc(sizeof(double) * (size_t)(n + 1));
	F[0] = -pen.beta;
	for (int64_t t = 1; t <= n; t++) {
		F[t] = INFINITY;
		for (int64_t s = 0; s + m <= t; s++) {
			if (!isfinite(F[s])) {
				continue;
			}
			double v = F[s] + cost->eval(cost->ctx, s, t) + pen.beta + (pen.mbic ? log((double)(t - s)) : 0);
			if (v < F[t]) {
				F[t] = v;
			}
		}
	}
	double r = F[n];
	free(F);
	return r;
}

static void check_valid(const cpt_bkps *b, int64_t n, int64_t m, const char *what) {
	CHECK(n == 0 ? b->count == 0 : (b->count >= 1 && b->pos[b->count - 1] == n), "%s: must end at n=%lld", what,
	      (long long)n);
	int64_t prev = 0;
	for (int64_t i = 0; i < b->count; i++) {
		CHECK(b->pos[i] - prev >= m || n < 2 * m, "%s: segment shorter than min_size %lld", what, (long long)m);
		prev = b->pos[i];
	}
}

// Exhaustive minimum cost over all placements of k change points (small n only).
static double exhaustive(const cpt_cost *cost, int64_t start, int64_t n, int64_t k, int64_t m) {
	if (k == 0) {
		return n - start >= m ? cost->eval(cost->ctx, start, n) : INFINITY;
	}
	double best = INFINITY;
	for (int64_t s = start + m; s + m * k <= n; s++) {
		double v = cost->eval(cost->ctx, start, s) + exhaustive(cost, s, n, k - 1, m);
		if (v < best) {
			best = v;
		}
	}
	return best;
}

// Arguments at the int64 limit must neither overflow nor crash.
static int test_extreme_arguments(void) {
	double x[8] = {0, 0, 0, 0, 9, 9, 9, 9};
	char err[CPT_ERR_LEN];
	int checks = 0;
	cpt_cost cost;
	if (cpt_cost_create(CPT_COST_L2, x, 8, &cost, err)) {
		CHECK(0, "extreme: create failed: %s", err);
		return checks;
	}
	cpt_bkps b;
	CHECK(cpt_dynp(&cost, 8, INT64_MAX, 1, NULL, &b, err) != 0, "dynp accepted n_bkps = INT64_MAX");
	checks++;
	cpt_penalty pen = {1.0, 0};
	CHECK(cpt_pelt(&cost, 8, pen, INT64_MAX, NULL, &b, err) == 0 && b.count == 1 && b.pos[0] == 8,
	      "pelt with min_size = INT64_MAX: %s", err);
	cpt_bkps_free(&b);
	checks++;
	CHECK(cpt_binseg(&cost, 8, -1, 1.0, INT64_MAX, NULL, &b, err) == 0 && b.count == 1 && b.pos[0] == 8,
	      "binseg with min_size = INT64_MAX: %s", err);
	cpt_bkps_free(&b);
	checks++;
	cpt_cost_destroy(&cost);
	return checks;
}

// Every algorithm on one cost, stopping at the first error. Results are freed; returns the error code.
static int run_all(cpt_cost_kind kind, const double *x, int64_t n, cpt_budget *budget, char *err) {
	cpt_cost cost;
	int rc = cpt_cost_create(kind, x, n, &cost, err);
	if (rc) {
		return rc;
	}
	cpt_penalty bic = cpt_penalty_named(CPT_PEN_BIC, n, cost.n_params);
	cpt_penalty mbic = cpt_penalty_named(CPT_PEN_MBIC, n, cost.n_params);
	cpt_bkps b = {NULL, 0};
	rc = cpt_pelt(&cost, n, bic, 2, budget, &b, err);
	cpt_bkps_free(&b);
	if (rc == 0) {
		rc = cpt_pelt(&cost, n, mbic, 1, budget, &b, err);
		cpt_bkps_free(&b);
	}
	if (rc == 0) {
		rc = cpt_binseg(&cost, n, 3, 0, 2, budget, &b, err);
		cpt_bkps_free(&b);
	}
	if (rc == 0) {
		rc = cpt_binseg(&cost, n, -1, bic.beta, 1, budget, &b, err);
		cpt_bkps_free(&b);
	}
	if (rc == 0) {
		rc = cpt_dynp(&cost, n, 2, 2, budget, &b, err);
		cpt_bkps_free(&b);
	}
	if (rc == 0) {
		cpt_crops_entry *entries = NULL;
		int64_t count = 0;
		rc = cpt_crops(&cost, n, 0.1, 500, 1, budget, &entries, &count, err);
		cpt_crops_free(entries, count);
	}
	cpt_cost_destroy(&cost);
	return rc;
}

static void make_counts(double *x, int64_t n) {
	for (int64_t i = 0; i < n; i++) {
		x[i] = floor(2.0 + (i / 15 % 3) * 4.0 + 2.0 * (1.0 + sin((double)i * 1.7)));
	}
}

#ifdef CPT_FAULT_INJECTION
// The k-th allocation fails, for every k until a run completes without hitting the failure.
static int test_out_of_memory(void) {
	double x[60];
	make_counts(x, 60);
	char err[CPT_ERR_LEN];
	int checks = 0;
	for (int kind = 0; kind < 4; kind++) {
		for (long k = 1;; k++) {
			cpt_fail_countdown = k;
			err[0] = 0;
			int rc = run_all((cpt_cost_kind)kind, x, 60, NULL, err);
			long left = cpt_fail_countdown;
			cpt_fail_countdown = 0;
			if (left > 0) {
				CHECK(rc == 0, "cost %d: run without injected failure failed: %s", kind, err);
				break; // every allocation of the run has been failed once
			}
			CHECK(rc == 1 && strstr(err, "out of memory"), "cost %d, allocation %ld: rc=%d err=%s", kind, k, rc,
			      err);
			checks++;
		}
	}
	return checks;
}
#endif

// Work limits of every size: each run either finishes or stops with CPT_BUDGET_EXCEEDED.
static int test_budgets(void) {
	double x[60];
	make_counts(x, 60);
	char err[CPT_ERR_LEN];
	int checks = 0;
	for (int kind = 0; kind < 4; kind++) {
		for (int64_t limit = 1; limit < 1000000; limit = limit * 3 + 1) {
			cpt_budget budget = {limit, 0};
			int rc = run_all((cpt_cost_kind)kind, x, 60, &budget, err);
			CHECK(rc == 0 || (rc == CPT_BUDGET_EXCEEDED && strstr(err, "work limit")), "cost %d, limit %lld: rc=%d %s",
			      kind, (long long)limit, rc, err);
			checks++;
		}
	}
	return checks;
}

static int test_invalid_arguments(void) {
	char err[CPT_ERR_LEN];
	cpt_cost cost;
	int checks = 0;
	CHECK(cpt_cost_create((cpt_cost_kind)99, NULL, 0, &cost, err) == 1 && strstr(err, "unknown cost"),
	      "unknown cost kind accepted");
	checks++;
	// rejected before the series is read, so no data is needed
	CHECK(cpt_cost_create(CPT_COST_L1, NULL, (int64_t)UINT32_MAX, &cost, err) == 1 && strstr(err, "at most"),
	      "l1 accepted a series longer than its index supports");
	checks++;
	double x[8] = {0, 0, 0, 0, 9, 9, 9, 9};
	cpt_bkps b;
	if (cpt_cost_create(CPT_COST_L2, x, 8, &cost, err) == 0) {
		CHECK(cpt_dynp(&cost, 8, -1, 1, NULL, &b, err) == 1 && strstr(err, "n_bkps must be >= 0"),
		      "dynp accepted n_bkps = -1");
		checks++;
		cpt_cost_destroy(&cost);
	}
	CHECK(cpt_penalty_parse("SIC", 3) == CPT_PEN_BIC && cpt_penalty_parse("hqc", 3) == -1, "penalty names");
	checks++;
	return checks;
}

//===--------------------------------------------------------------------===//
// Online detector
//===--------------------------------------------------------------------===//

// Points with level changes every ~60 steps; ramp > 0 adds a drift that makes the sums convex.
static void make_stream(double *x, int64_t n, double ramp) {
	double level = 0;
	for (int64_t i = 0; i < n; i++) {
		if (uniform() < 1.0 / 60) {
			level = uniform() * 6 - 3;
		}
		x[i] = level + normal() + ramp * (double)i;
	}
}

// The statistic recomputed over every candidate tau < k: max of (S_k - S_tau)^2 / (2 (k - tau)) over
// both signs, with the first best tau; *up tells the sign, *slope the estimated shift in sd units.
static double naive_statistic(const double *S, int64_t k, int64_t *tau, double *slope) {
	double best = 0;
	*tau = k;
	*slope = 0;
	for (int64_t t = 0; t < k; t++) {
		double rise = S[k] - S[t];
		double stat = rise * rise / (2.0 * (double)(k - t));
		if (stat > best) {
			best = stat;
			*tau = t;
			*slope = rise / (double)(k - t);
		}
	}
	return best;
}

static int test_online_exact(void) {
	char err[CPT_ERR_LEN];
	int checks = 0;
	int64_t total_alarms = 0;
	for (int iter = 0; iter < 80; iter++) {
		int64_t n = 50 + (int64_t)(uniform() * 400);
		double threshold = 4 + uniform() * 30, mean = uniform() * 4 - 2, sd = 0.5 + uniform() * 2;
		double *x = malloc(sizeof(double) * (size_t)n), *S = malloc(sizeof(double) * (size_t)(n + 1));
		make_stream(x, n, 0);
		cpt_online d;
		cpt_online_alarms alarms = {NULL, 0, 0};
		CHECK(cpt_online_init(&d, threshold, 0, mean, sd, err) == 0, "online init: %s", err);
		int64_t k = 0, t0 = 0;
		S[0] = 0;
		for (int64_t i = 0; i < n; i++) {
			int64_t before = alarms.count;
			CHECK(cpt_online_feed(&d, &x[i], 1, NULL, &alarms, err) == 0, "online feed: %s", err);
			k++;
			S[k] = S[k - 1] + (x[i] - mean) / sd;
			int64_t tau;
			double slope, stat = naive_statistic(S, k, &tau, &slope);
			if (alarms.count > before) {
				cpt_online_alarm a = alarms.items[before];
				CHECK(stat >= threshold && close_enough(a.statistic, stat, 1e-9) && a.change_at == t0 + tau &&
				          a.detected_at == i + 1 && close_enough(a.shift, slope * sd, 1e-9),
				      "alarm at %lld: got (%lld, %g, %g) want (%lld, %g, %g)", (long long)i + 1,
				      (long long)a.change_at, a.statistic, a.shift, (long long)(t0 + tau), stat, slope * sd);
				k = 0;
				t0 = i + 1;
				total_alarms++;
			} else {
				CHECK(stat < threshold && close_enough(d.statistic, stat, 1e-9), "statistic at %lld: %.12g vs %.12g",
				      (long long)i + 1, d.statistic, stat);
			}
			checks++;
		}
		cpt_online_alarms_free(&alarms);
		cpt_online_free(&d);
		free(x);
		free(S);
	}
	CHECK(total_alarms > 50, "the exactness test raised only %lld alarms", (long long)total_alarms);
	printf("online exactness: %d points, %lld alarms\n", checks, (long long)total_alarms);
	return checks;
}

// Functional pruning keeps about log(n) candidates of each sign on noise.
static int test_online_pruning(void) {
	char err[CPT_ERR_LEN];
	cpt_online d;
	cpt_online_alarms alarms = {NULL, 0, 0};
	cpt_online_init(&d, 1e12, 0, 0, 1, err);
	int64_t most = 0;
	for (int64_t i = 0; i < 200000; i++) {
		double x = normal();
		cpt_online_feed(&d, &x, 1, NULL, &alarms, err);
		most = d.up.len > most ? d.up.len : most;
		most = d.down.len > most ? d.down.len : most;
	}
	CHECK(most < 60, "online detector kept up to %lld candidates over 200000 points", (long long)most);
	cpt_online_alarms_free(&alarms);
	cpt_online_free(&d);
	return 1;
}

static int test_online_training(void) {
	char err[CPT_ERR_LEN];
	double x[50], sum = 0, ss = 0;
	for (int i = 0; i < 50; i++) {
		x[i] = 100 + 7 * normal();
		sum += x[i];
	}
	for (int i = 0; i < 50; i++) {
		ss += (x[i] - sum / 50) * (x[i] - sum / 50);
	}
	cpt_online d;
	cpt_online_alarms alarms = {NULL, 0, 0};
	cpt_online_init(&d, 20, 50, 0, 0, err);
	cpt_online_feed(&d, x, 49, NULL, &alarms, err);
	CHECK(d.training == 1, "online detector stopped training early");
	cpt_online_feed(&d, &x[49], 1, NULL, &alarms, err);
	CHECK(d.training == 0 && close_enough(d.mean, sum / 50, 1e-12) && close_enough(d.sd, sqrt(ss / 49), 1e-9),
	      "trained baseline %g, %g vs %g, %g", d.mean, d.sd, sum / 50, sqrt(ss / 49));
	cpt_online_free(&d);
	return 2;
}

static int same_alarms(const cpt_online_alarms *a, const cpt_online_alarms *b) {
	// empty lists have NULL items, and memcmp(NULL, NULL, 0) is undefined
	return a->count == b->count &&
	       (a->count == 0 || memcmp(a->items, b->items, sizeof(cpt_online_alarm) * (size_t)a->count) == 0);
}

// The state after encode/decode continues exactly where it left off, in any chunking.
static int test_online_split(void) {
	char err[CPT_ERR_LEN];
	int checks = 0;
	for (int iter = 0; iter < 40; iter++) {
		int64_t n = 200 + (int64_t)(uniform() * 800);
		double *x = malloc(sizeof(double) * (size_t)n);
		make_stream(x, n, iter % 4 == 0 ? 0.01 : 0);
		cpt_online whole, part;
		cpt_online_alarms a1 = {NULL, 0, 0}, a2 = {NULL, 0, 0};
		cpt_online_init(&whole, 8 + uniform() * 10, 2 + iter % 40, 0, 0, err);
		cpt_online_feed(&whole, x, n, NULL, &a1, err);
		cpt_online_init(&part, whole.threshold, whole.train, 0, 0, err);
		for (int64_t i = 0; i < n;) {
			int64_t chunk = 1 + (int64_t)(uniform() * 60);
			chunk = i + chunk > n ? n - i : chunk;
			cpt_online_feed(&part, x + i, chunk, NULL, &a2, err);
			uint8_t *buf;
			size_t len;
			CHECK(cpt_online_encode(&part, &buf, &len, err) == 0, "encode: %s", err);
			cpt_online_free(&part);
			CHECK(cpt_online_decode(buf, len, &part, err) == 0, "decode: %s", err);
			free(buf);
			i += chunk;
		}
		CHECK(same_alarms(&a1, &a2) && whole.statistic == part.statistic && whole.n == part.n,
		      "chunked feeding differs: %lld vs %lld alarms", (long long)a1.count, (long long)a2.count);
		checks++;
		cpt_online_alarms_free(&a1);
		cpt_online_alarms_free(&a2);
		cpt_online_free(&whole);
		cpt_online_free(&part);
		free(x);
	}
	return checks;
}

// Truncated states are rejected; corrupted ones are rejected or, if accepted, safe to run on.
static int test_online_decode(void) {
	char err[CPT_ERR_LEN];
	double x[300];
	make_stream(x, 300, 0.02);
	cpt_online d;
	cpt_online_alarms alarms = {NULL, 0, 0};
	cpt_online_init(&d, 1e9, 20, 0, 0, err);
	cpt_online_feed(&d, x, 300, NULL, &alarms, err);
	uint8_t *buf;
	size_t len;
	cpt_online_encode(&d, &buf, &len, err);
	cpt_online_free(&d);
	int checks = 0;
	for (size_t cut = 0; cut < len; cut++) {
		CHECK(cpt_online_decode(buf, cut, &d, err) == 1, "decode accepted a state cut to %zu of %zu bytes", cut, len);
		checks++;
	}
	for (int trial = 0; trial < 3000; trial++) {
		uint8_t *bad = malloc(len);
		memcpy(bad, buf, len);
		bad[(size_t)(uniform() * (double)len)] ^= (uint8_t)(1 + uniform() * 255);
		if (cpt_online_decode(bad, len, &d, err) == 0) {
			cpt_online_feed(&d, x, 100, NULL, &alarms, err); // must not crash (ASan/UBSan)
			cpt_online_free(&d);
		}
		free(bad);
		checks++;
	}
	CHECK(cpt_online_decode((const uint8_t *)"not a detector", 14, &d, err) == 1 && strstr(err, "cpt_online_new"),
	      "decode accepted garbage");
	free(buf);
	cpt_online_alarms_free(&alarms);
	return checks + 1;
}

static int test_online_params(void) {
	char err[CPT_ERR_LEN];
	cpt_online d;
	struct {
		double threshold;
		int64_t train;
		double mean, sd;
		const char *msg;
	} bad[] = {{0, 10, 0, 0, "threshold"},  {NAN, 10, 0, 0, "threshold"}, {10, 1, 0, 0, "train"},
	           {10, -3, 0, 0, "train"},     {10, 0, 0, 0, "baseline"},    {10, 0, INFINITY, 1, "baseline"}};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(cpt_online_init(&d, bad[i].threshold, bad[i].train, bad[i].mean, bad[i].sd, err) == 1 &&
		          strstr(err, bad[i].msg),
		      "online init accepted case %zu", i);
	}
	// a drift keeps every candidate, so the work per point grows; the budget stops it
	double x[2000];
	make_stream(x, 2000, 0.05);
	cpt_online_alarms alarms = {NULL, 0, 0};
	cpt_budget budget = {20000, 0};
	cpt_online_init(&d, 1e12, 0, 0, 1, err);
	CHECK(cpt_online_feed(&d, x, 2000, &budget, &alarms, err) == CPT_BUDGET_EXCEEDED && strstr(err, "work limit"),
	      "online feed ignored the work limit");
	cpt_online_free(&d);
	cpt_online_alarms_free(&alarms);
	return (int)(sizeof(bad) / sizeof(bad[0])) + 1;
}

#ifdef CPT_FAULT_INJECTION
// init, feed (hull growth, alarms), encode and decode with the k-th allocation failing.
static int run_online(const double *x, int64_t n, char *err) {
	cpt_online d;
	cpt_online_alarms alarms = {NULL, 0, 0};
	int rc = cpt_online_init(&d, 6, 10, 0, 0, err);
	if (rc == 0) {
		rc = cpt_online_feed(&d, x, n, NULL, &alarms, err);
	}
	uint8_t *buf = NULL;
	size_t len = 0;
	if (rc == 0) {
		rc = cpt_online_encode(&d, &buf, &len, err);
	}
	cpt_online_free(&d);
	if (rc == 0) {
		rc = cpt_online_decode(buf, len, &d, err);
		cpt_online_free(&d);
	}
	free(buf);
	cpt_online_alarms_free(&alarms);
	return rc;
}

static int test_online_out_of_memory(void) {
	double x[400];
	make_stream(x, 200, 0);
	make_stream(x + 200, 200, 0.05); // a drift: long candidate lists, so the hull grows
	char err[CPT_ERR_LEN];
	int checks = 0;
	for (long k = 1;; k++) {
		cpt_fail_countdown = k;
		err[0] = 0;
		int rc = run_online(x, 400, err);
		long left = cpt_fail_countdown;
		cpt_fail_countdown = 0;
		if (left > 0) {
			CHECK(rc == 0, "online run without injected failure failed: %s", err);
			break;
		}
		CHECK(rc == 1 && strstr(err, "out of memory"), "online, allocation %ld: rc=%d err=%s", k, rc, err);
		checks++;
	}
	return checks;
}
#endif

int main(void) {
	const char *names[] = {"l2", "normal", "poisson", "l1"};
	char err[CPT_ERR_LEN];
	int checks = 0;
	for (int iter = 0; iter < 400; iter++) {
		cpt_cost_kind kind = (cpt_cost_kind)(iter % 4);
		int64_t n = 1 + (int64_t)(uniform() * (iter < 40 ? 12 : 180));
		double *x = malloc(sizeof(double) * (size_t)n);
		make_series(x, n, kind);
		cpt_cost cost;
		if (cpt_cost_create(kind, x, n, &cost, err)) {
			CHECK(0, "%s: create failed: %s", names[kind], err);
			free(x);
			continue;
		}
		for (int k = 0; k < 50; k++) {
			int64_t a = (int64_t)(uniform() * n), b = (int64_t)(uniform() * n);
			if (a > b) {
				int64_t t = a;
				a = b;
				b = t;
			}
			b++;
			double fast = cost.eval(cost.ctx, a, b), slow = naive_cost(kind, x, a, b);
			CHECK(close_enough(fast, slow, 1e-8), "%s: eval(%lld,%lld)=%.12g naive=%.12g", names[kind], (long long)a,
			      (long long)b, fast, slow);
			checks++;
		}
		for (int64_t min_size = 1; min_size <= 6; min_size++) {
			int64_t m = cpt_effective_min_size(&cost, min_size);
			cpt_penalty pens[2] = {{uniform() * 3 * log((double)n + 1), 0}, cpt_penalty_named(CPT_PEN_MBIC, n, 1)};
			for (int p = 0; p < 2; p++) {
				cpt_bkps b;
				CHECK(cpt_pelt(&cost, n, pens[p], min_size, NULL, &b, err) == 0, "pelt: %s", err);
				check_valid(&b, n, m, "pelt");
				if (n >= 2 * m) {
					double got = penalised(&cost, &b, pens[p]), best = optimal_partitioning(&cost, n, pens[p], m);
					CHECK(got <= best + 1e-9 * (1 + fabs(best)),
					      "%s n=%lld min_size=%lld beta=%g mbic=%d: PELT %.12g > OP %.12g", names[kind], (long long)n,
					      (long long)min_size, pens[p].beta, pens[p].mbic, got, best);
				}
				cpt_bkps_free(&b);
				checks++;
			}
			cpt_bkps b;
			CHECK(cpt_binseg(&cost, n, -1, pens[0].beta, min_size, NULL, &b, err) == 0, "binseg: %s", err);
			check_valid(&b, n, m, "binseg");
			cpt_bkps_free(&b);
			if (n > 0 && n <= 40) {
				for (int64_t k = 0; (k + 1) * m <= n && k <= 3; k++) {
					CHECK(cpt_dynp(&cost, n, k, min_size, NULL, &b, err) == 0, "dynp: %s", err);
					check_valid(&b, n, m, "dynp");
					CHECK(b.count == k + 1, "dynp: %lld segments, wanted %lld", (long long)b.count, (long long)k + 1);
					double got = cpt_segmentation_cost(&cost, &b), best = exhaustive(&cost, 0, n, k, m);
					CHECK(close_enough(got, best, 1e-9), "%s dynp k=%lld: %.12g vs exhaustive %.12g", names[kind],
					      (long long)k, got, best);
					cpt_bkps_free(&b);
					checks++;
				}
			}
			cpt_crops_entry *entries;
			int64_t count;
			CHECK(cpt_crops(&cost, n, 0.5, 50, min_size, NULL, &entries, &count, err) == 0, "crops: %s", err);
			for (int64_t i = 0; i < count; i++) {
				check_valid(&entries[i].bkps, n, m, "crops");
			}
			cpt_crops_free(entries, count);
		}
		cpt_cost_destroy(&cost);
		free(x);
	}
	checks += test_extreme_arguments();
	checks += test_budgets();
	checks += test_invalid_arguments();
	checks += test_online_exact();
	checks += test_online_pruning();
	checks += test_online_training();
	checks += test_online_split();
	checks += test_online_decode();
	checks += test_online_params();
#ifdef CPT_FAULT_INJECTION
	checks += test_out_of_memory();
	checks += test_online_out_of_memory();
#endif
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
