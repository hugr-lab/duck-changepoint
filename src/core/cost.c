#include "cost.h"

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
	return calloc(1, sizeof(prefix_ctx));
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

static int prefix_init(prefix_ctx *c, const double *x, int64_t n, int centre, char *err) {
	c->n = n;
	c->s1 = (double *)malloc(sizeof(double) * (size_t)(n + 1));
	c->s2 = (double *)malloc(sizeof(double) * (size_t)(n + 1));
	if (!c->s1 || !c->s2) {
		snprintf(err, CPT_ERR_LEN, "out of memory allocating prefix sums for n = %lld", (long long)n);
		return 1;
	}
	double shift = 0;
	if (centre && n > 0) {
		for (int64_t i = 0; i < n; i++) {
			shift += x[i];
		}
		shift /= (double)n;
	}
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
// Factory
//===--------------------------------------------------------------------===//

int cpt_cost_parse(const char *name, size_t len) {
	static const struct {
		const char *name;
		cpt_cost_kind kind;
	} names[] = {{"l2", CPT_COST_L2}};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char *candidate = names[i].name;
		if (strlen(candidate) != len) {
			continue;
		}
		size_t j = 0;
		while (j < len) {
			char ch = name[j];
			if (ch >= 'A' && ch <= 'Z') {
				ch = (char)(ch - 'A' + 'a');
			}
			if (ch != candidate[j]) {
				break;
			}
			j++;
		}
		if (j == len) {
			return (int)names[i].kind;
		}
	}
	return -1;
}

int cpt_cost_create(cpt_cost_kind kind, const double *x, int64_t n, cpt_cost *out, char *err) {
	memset(out, 0, sizeof(*out));
	out->K = 0;
	out->min_len = 1;
	switch (kind) {
	case CPT_COST_L2:
		out->name = "l2";
		out->ctx = prefix_alloc();
		out->init = l2_init;
		out->eval = l2_eval;
		out->n_params = 1;
		out->free = prefix_free;
		break;
	default:
		snprintf(err, CPT_ERR_LEN, "unknown cost");
		return 1;
	}
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
