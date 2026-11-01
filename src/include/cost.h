#pragma once

// Segment cost functions shared by every search algorithm.
//
// Conventions: the series is x[0..n), segments are half-open [a, b) with 0 <= a < b <= n.
// Algorithms only talk to a cost through cpt_cost; they never look inside ctx.

#include <stddef.h>
#include <stdint.h>

#define CPT_ERR_LEN 256

typedef struct cpt_cost {
	const char *name;
	void *ctx;
	// O(n) (O(n log n) for l1) precomputation. Returns 0 on success, otherwise writes err.
	int (*init)(void *ctx, const double *x, int64_t n, char *err);
	// Cost of segment [a, b).
	double (*eval)(void *ctx, int64_t a, int64_t b);
	// Number of parameters estimated per segment (used by BIC/MBIC/AIC).
	int n_params;
	// Pruning constant of Killick et al. (2012) Theorem 3.1: C(a,s) + C(s,b) + K <= C(a,b).
	double K;
	// Smallest segment length for which the cost is well defined (e.g. 2 for 'normal').
	int64_t min_len;
	void (*free)(void *ctx);
} cpt_cost;

typedef enum { CPT_COST_L2 = 0, CPT_COST_NORMAL, CPT_COST_POISSON, CPT_COST_L1 } cpt_cost_kind;

// Parse a cost name ('l2', 'normal', 'poisson', 'l1'; case-insensitive). Returns -1 if unknown.
int cpt_cost_parse(const char *name, size_t len);

// Allocate the context for `kind` and run init(). On failure returns non-zero, writes err and
// leaves *out freed.
int cpt_cost_create(cpt_cost_kind kind, const double *x, int64_t n, cpt_cost *out, char *err);
void cpt_cost_destroy(cpt_cost *cost);

// l1 lives in its own translation unit (cost_l1.c); these are its hooks for cost.c.
void *cpt_cost_l1_alloc(void);
int cpt_cost_l1_init(void *ctx, const double *x, int64_t n, char *err);
double cpt_cost_l1_eval(void *ctx, int64_t a, int64_t b);
void cpt_cost_l1_free(void *ctx);
