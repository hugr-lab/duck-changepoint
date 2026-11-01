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
int cpt_pelt(const cpt_cost *cost, int64_t n, cpt_penalty pen, int64_t min_size, cpt_bkps *out, char *err);

