// Standalone tests for the numerical core (src/core), built with ASan/UBSan by `make test_core`.
// No DuckDB involved: costs and algorithms are exercised directly on random series.
//
// Checks:
//   - every cost's eval() against a naive O(len) evaluation
//   - PELT equals unpruned Optimal Partitioning (all costs, min_size 1..6, value penalty and MBIC)
//   - Dynp equals an exhaustive search for small n
//   - every algorithm returns valid segmentations (increasing, ends at n, segments >= min_size)

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
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
