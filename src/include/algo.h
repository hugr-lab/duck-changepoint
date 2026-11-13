#pragma once

// Change point search algorithms. All of them work through the cpt_cost interface only.
//
// Output convention: a segmentation is the list of segment end positions in the half-open
// convention, i.e. 1-based inclusive ends, strictly increasing, last element == n. An empty
// series yields an empty list.

#include "cost.h"

#include <stdint.h>

typedef struct {
	int64_t *pos;
	int64_t count;
} cpt_bkps;

void cpt_bkps_free(cpt_bkps *b);

// Limit on cost evaluations for one SQL call, in units of one O(1) evaluation (an l1 evaluation
// counts log2 n units, see cpt_cost.eval_weight), and the place a running call learns that its query was
// interrupted: every charge asks interrupted(interrupt_ctx), so an algorithm stops within one outer step.
// Algorithms return CPT_BUDGET_EXCEEDED when n_evals passes max_evals and CPT_INTERRUPTED when the
// callback says so. max_evals <= 0 means unlimited; a NULL callback is never interrupted. A NULL budget
// is unlimited and never interrupted.
typedef struct {
	int64_t max_evals;
	int64_t n_evals;
	int (*interrupted)(void *ctx);
	void *interrupt_ctx;
} cpt_budget;

#define CPT_BUDGET_EXCEEDED 2
#define CPT_INTERRUPTED 3

// Charge units of work; returns CPT_BUDGET_EXCEEDED (and writes err) once the limit is passed.
int cpt_budget_charge(cpt_budget *budget, int64_t units, char *err);

typedef enum { CPT_PEN_VALUE = 0, CPT_PEN_BIC, CPT_PEN_MBIC, CPT_PEN_AIC } cpt_penalty_kind;

typedef struct {
	double beta; // penalty per change point (for MBIC: the linear part, (p + 2) ln n)
	int mbic;    // 1: add ln(segment length) to every segment cost (Zhang & Siegmund 2007)
} cpt_penalty;

// Parse 'bic' / 'mbic' / 'aic' (case-insensitive). Returns -1 if unknown.
int cpt_penalty_parse(const char *name, size_t len);
// Resolve a named penalty for a series of length n and a cost with p parameters per segment:
//   bic = (p + 1) ln n,  mbic = (p + 2) ln n + sum ln(n_i),  aic = 2 (p + 1).
cpt_penalty cpt_penalty_named(cpt_penalty_kind kind, int64_t n, int p);

// Effective minimum segment length: max(min_size, cost->min_len).
int64_t cpt_effective_min_size(const cpt_cost *cost, int64_t min_size);

// Sum of segment costs (no penalty) for a segmentation.
double cpt_segmentation_cost(const cpt_cost *cost, const cpt_bkps *bkps);

// PELT, Killick, Fearnhead & Eckley (2012). Exact minimiser of sum C + beta * (#segments).
int cpt_pelt(const cpt_cost *cost, int64_t n, cpt_penalty pen, int64_t min_size, cpt_budget *budget,
             cpt_bkps *out, char *err);

// Binary segmentation. n_bkps >= 0 selects "stop after n_bkps splits"; otherwise splitting stops
// once the best gain is not larger than pen.
int cpt_binseg(const cpt_cost *cost, int64_t n, int64_t n_bkps, double pen, int64_t min_size, cpt_budget *budget,
               cpt_bkps *out, char *err);

// Optimal partition into exactly n_bkps + 1 segments (segment neighbourhood), O(n_bkps n^2).
int cpt_dynp(const cpt_cost *cost, int64_t n, int64_t n_bkps, int64_t min_size, cpt_budget *budget,
             cpt_bkps *out, char *err);

typedef struct {
	double penalty; // smallest penalty in the sweep at which PELT returned this segmentation
	double cost;    // unpenalised cost of the segmentation
	cpt_bkps bkps;
} cpt_crops_entry;

// CROPS, Haynes, Eckley & Fearnhead (2017): every distinct optimal segmentation for penalties in
// [pen_min, pen_max]. Entries are sorted by penalty ascending (n_bkps descending).
int cpt_crops(const cpt_cost *cost, int64_t n, double pen_min, double pen_max, int64_t min_size,
              cpt_budget *budget, cpt_crops_entry **out, int64_t *out_count, char *err);
void cpt_crops_free(cpt_crops_entry *entries, int64_t count);
