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

#include "algo.h"
#include "cost.h"

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
#ifdef CPT_FAULT_INJECTION
	checks += test_out_of_memory();
#endif
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
