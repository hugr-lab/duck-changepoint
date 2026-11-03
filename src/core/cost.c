#include "cost.h"

#include "cpt_alloc.h"
#include "cpt_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===--------------------------------------------------------------------===//
// Prefix sums shared by l2 / normal / poisson
//===--------------------------------------------------------------------===//

// The series is centred on its global mean before accumulating x and x^2. This keeps
// S2 - S1^2/len well conditioned for series with a large offset (e.g. Nile ~ 900 +/- 170).
typedef struct {
	double *s1; // s1[i] = sum_{j<i} (x_j - shift)
	double *s2; // s2[i] = sum_{j<i} (x_j - shift)^2
	int64_t n;
} prefix_ctx;

static void *prefix_alloc(void) {
	return cpt_calloc(1, sizeof(prefix_ctx));
}

static void prefix_free(void *ctx) {
	prefix_ctx *c = (prefix_ctx *)ctx;
	if (!c) {
		return;
	}
	free(c->s1);
	free(c->s2);
	free(c);
}

static double mean_of(const double *x, int64_t n) {
	double sum = 0;
	for (int64_t i = 0; i < n; i++) {
		sum += x[i];
	}
	return n > 0 ? sum / (double)n : 0;
}

static int prefix_init(prefix_ctx *c, const double *x, int64_t n, int centre, char *err) {
	c->n = n;
	c->s1 = (double *)cpt_malloc(sizeof(double) * (size_t)(n + 1));
	c->s2 = (double *)cpt_malloc(sizeof(double) * (size_t)(n + 1));
	if (!c->s1 || !c->s2) {
		snprintf(err, CPT_ERR_LEN, "out of memory allocating prefix sums for n = %lld", (long long)n);
		return 1; // the caller frees the context, and with it whichever array was allocated
	}
	double shift = centre ? mean_of(x, n) : 0;
	c->s1[0] = 0;
	c->s2[0] = 0;
	for (int64_t i = 0; i < n; i++) {
		double d = x[i] - shift;
		c->s1[i + 1] = c->s1[i] + d;
		c->s2[i + 1] = c->s2[i] + d * d;
	}
	return 0;
}

// Sum of squared deviations from the segment mean, clamped at 0 against rounding.
static inline double prefix_ss(const prefix_ctx *c, int64_t a, int64_t b) {
	double s = c->s1[b] - c->s1[a];
	double ss = (c->s2[b] - c->s2[a]) - s * s / (double)(b - a);
	return ss > 0 ? ss : 0;
}

//===--------------------------------------------------------------------===//
// l2: sum of squared deviations from the segment mean
//===--------------------------------------------------------------------===//

static int l2_init(void *ctx, const double *x, int64_t n, char *err) {
	return prefix_init((prefix_ctx *)ctx, x, n, 1, err);
}

static double l2_eval(void *ctx, int64_t a, int64_t b) {
	return prefix_ss((const prefix_ctx *)ctx, a, b);
}

//===--------------------------------------------------------------------===//
// normal: changing mean and variance, len * ln(sigma_hat^2) (MLE variance)
//===--------------------------------------------------------------------===//

// Absolute floor for sigma_hat^2, the value R's changepoint uses (mll.meanvar). A segment with
// zero variance (a run of equal values) would otherwise have cost -inf. Being absolute, it assumes
// the data is not on a tiny scale: rescale series whose variance is below ~1e-9.
#define CPT_NORMAL_VAR_FLOOR 1e-11

static int normal_init(void *ctx, const double *x, int64_t n, char *err) {
	return prefix_init((prefix_ctx *)ctx, x, n, 1, err);
}

static double normal_eval(void *ctx, int64_t a, int64_t b) {
	const prefix_ctx *c = (const prefix_ctx *)ctx;
	double len = (double)(b - a);
	double var = prefix_ss(c, a, b) / len;
	return len * log(var < CPT_NORMAL_VAR_FLOOR ? CPT_NORMAL_VAR_FLOOR : var);
}

//===--------------------------------------------------------------------===//
// poisson: -2 * (S ln(S/len) - S), S = segment sum
//===--------------------------------------------------------------------===//

static int is_count(double v) {
	return v >= 0 && v == floor(v) && !isinf(v);
}

static int poisson_init(void *ctx, const double *x, int64_t n, char *err) {
	for (int64_t i = 0; i < n; i++) {
		if (!is_count(x[i])) {
			snprintf(err, CPT_ERR_LEN,
			         "cost 'poisson' requires non-negative integer values; element %lld is %g", (long long)i + 1,
			         x[i]);
			return 1;
		}
	}
	// Counts are summed exactly (no centring): S must be the true segment sum.
	return prefix_init((prefix_ctx *)ctx, x, n, 0, err);
}

static double poisson_eval(void *ctx, int64_t a, int64_t b) {
	const prefix_ctx *c = (const prefix_ctx *)ctx;
	double s = c->s1[b] - c->s1[a];
	if (s <= 0) {
		return 0; // lim_{S->0} S ln S = 0
	}
	return -2.0 * (s * log(s / (double)(b - a)) - s);
}

//===--------------------------------------------------------------------===//
// Factory: one descriptor per cost
//===--------------------------------------------------------------------===//

typedef struct {
	const char *name;
	void *(*alloc)(void);
	int (*init)(void *ctx, const double *x, int64_t n, char *err);
	double (*eval)(void *ctx, int64_t a, int64_t b);
	void (*free)(void *ctx);
	int n_params;
	int64_t min_len;
	int64_t eval_weight; // 0: log2(n), one O(1) step per level of the l1 index
} cost_spec;

// K = 0 for every cost: each is a minimum over segment parameters of a sum of per-point terms, so
// splitting a segment cannot raise the cost, C(a,s) + C(s,b) <= C(a,b) (Killick, Fearnhead & Eckley
// 2012, Theorem 3.1). For l1 each half picks its own median.
static const cost_spec COST_SPECS[] = {
    [CPT_COST_L2] = {"l2", prefix_alloc, l2_init, l2_eval, prefix_free, 1, 1, 1},
    // sigma_hat^2 of one point is 0, and log() makes an evaluation about twice as expensive as l2
    [CPT_COST_NORMAL] = {"normal", prefix_alloc, normal_init, normal_eval, prefix_free, 2, 2, 2},
    [CPT_COST_POISSON] = {"poisson", prefix_alloc, poisson_init, poisson_eval, prefix_free, 1, 1, 1},
    [CPT_COST_L1] = {"l1", cpt_cost_l1_alloc, cpt_cost_l1_init, cpt_cost_l1_eval, cpt_cost_l1_free, 1, 1, 0},
};

#define N_COSTS ((int)(sizeof(COST_SPECS) / sizeof(COST_SPECS[0])))

static int64_t log2_ceil(int64_t n) {
	int64_t w = 1;
	while (((int64_t)1 << w) < n) {
		w++;
	}
	return w;
}

static const cost_spec *cost_spec_of(cpt_cost_kind kind) {
	return ((int)kind >= 0 && (int)kind < N_COSTS) ? &COST_SPECS[kind] : NULL;
}

static cpt_cost cost_from_spec(const cost_spec *spec, int64_t n) {
	cpt_cost cost = {0};
	cost.name = spec->name;
	cost.ctx = spec->alloc();
	cost.init = spec->init;
	cost.eval = spec->eval;
	cost.n_params = spec->n_params;
	cost.K = 0;
	cost.min_len = spec->min_len;
	cost.eval_weight = spec->eval_weight ? spec->eval_weight : log2_ceil(n);
	cost.free = spec->free;
	return cost;
}

int cpt_cost_parse(const char *name, size_t len) {
	for (int i = 0; i < N_COSTS; i++) {
		if (cpt_ascii_ieq(name, len, COST_SPECS[i].name)) {
			return i;
		}
	}
	return -1;
}

int cpt_cost_create(cpt_cost_kind kind, const double *x, int64_t n, cpt_cost *out, char *err) {
	const cost_spec *spec = cost_spec_of(kind);
	memset(out, 0, sizeof(*out));
	if (!spec) {
		snprintf(err, CPT_ERR_LEN, "unknown cost");
		return 1;
	}
	*out = cost_from_spec(spec, n);
	if (!out->ctx) {
		snprintf(err, CPT_ERR_LEN, "out of memory");
		return 1;
	}
	if (out->init(out->ctx, x, n, err)) {
		cpt_cost_destroy(out);
		return 1;
	}
	return 0;
}

void cpt_cost_destroy(cpt_cost *cost) {
	if (cost->ctx && cost->free) {
		cost->free(cost->ctx);
	}
	cost->ctx = NULL;
}
