// l1 cost: sum of absolute deviations from the segment median.
//
// There is no O(1) formula, so segments are answered in O(log n) with a wavelet matrix over
// the ranks of x (ties broken by position, so ranks are a permutation of 0..n-1). Each level
// keeps the zero-count prefix (rank0) and the prefix sums of x in that level's order, which
// gives, for any [a, b) and k, the k-th smallest value and the sum of the k smallest values.
//
// With len = b - a, k = (len - 1) / 2, median m = k-th smallest, below = sum of the k smallest:
//   C(a, b) = (m * k - below) + ((total - below - m) - m * (len - k - 1))
//
// Memory: ceil(log2 n) levels of (uint32 + double) per element, about 240 MB at n = 1e6.

#include "cost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int64_t n;
	int levels;
	double *sorted;   // sorted[r] = value with rank r
	double *total;    // total[i] = sum_{j<i} x_j (original order)
	uint32_t **rank0; // rank0[l][i] = zeros among the first i elements of level l
	uint32_t *zeros;  // zeros[l] = total zeros at level l
	double **psum;    // psum[l][i] = prefix sum of values in the order entering level l (l >= 1)
} l1_ctx;

typedef struct {
	double v;
	int64_t i;
} value_index;

static int cmp_value_index(const void *pa, const void *pb) {
	const value_index *a = (const value_index *)pa;
	const value_index *b = (const value_index *)pb;
	if (a->v < b->v) {
		return -1;
	}
	if (a->v > b->v) {
		return 1;
	}
	return (a->i > b->i) - (a->i < b->i);
}

void *cpt_cost_l1_alloc(void) {
	return calloc(1, sizeof(l1_ctx));
}

void cpt_cost_l1_free(void *ctx) {
	l1_ctx *c = (l1_ctx *)ctx;
	if (!c) {
		return;
	}
	if (c->rank0) {
		for (int l = 0; l < c->levels; l++) {
			free(c->rank0[l]);
		}
	}
	if (c->psum) {
		for (int l = 0; l <= c->levels; l++) {
			free(c->psum[l]);
		}
	}
	free(c->rank0);
	free(c->psum);
	free(c->zeros);
	free(c->sorted);
	free(c->total);
	free(c);
}

int cpt_cost_l1_init(void *ctx, const double *x, int64_t n, char *err) {
	l1_ctx *c = (l1_ctx *)ctx;
	c->n = n;
	if (n > (int64_t)UINT32_MAX - 1) {
		snprintf(err, CPT_ERR_LEN, "cost 'l1' supports at most %u elements", UINT32_MAX - 1);
		return 1;
	}
	int levels = 1;
	while (((int64_t)1 << levels) < n) {
		levels++;
	}
	c->levels = levels;

	size_t n1 = (size_t)n + 1;
	value_index *vi = (value_index *)malloc(sizeof(value_index) * (size_t)(n > 0 ? n : 1));
	uint32_t *cur = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));
	uint32_t *next = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));
	c->sorted = (double *)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
	c->total = (double *)malloc(sizeof(double) * n1);
	c->rank0 = (uint32_t **)calloc((size_t)levels, sizeof(uint32_t *));
	c->psum = (double **)calloc((size_t)levels + 1, sizeof(double *));
	c->zeros = (uint32_t *)calloc((size_t)levels, sizeof(uint32_t));
	int ok = vi && cur && next && c->sorted && c->total && c->rank0 && c->psum && c->zeros;
	for (int l = 0; ok && l < levels; l++) {
		c->rank0[l] = (uint32_t *)malloc(sizeof(uint32_t) * n1);
		c->psum[l + 1] = (double *)malloc(sizeof(double) * n1);
		ok = c->rank0[l] && c->psum[l + 1];
	}
	if (!ok) {
		free(vi);
		free(cur);
		free(next);
		snprintf(err, CPT_ERR_LEN, "out of memory building l1 index for n = %lld", (long long)n);
		return 1;
	}

	c->total[0] = 0;
	for (int64_t i = 0; i < n; i++) {
		vi[i].v = x[i];
		vi[i].i = i;
		c->total[i + 1] = c->total[i] + x[i];
	}
	qsort(vi, (size_t)n, sizeof(value_index), cmp_value_index);
	for (int64_t r = 0; r < n; r++) {
		c->sorted[r] = vi[r].v;
		cur[vi[r].i] = (uint32_t)r;
	}
	free(vi);

	// Level l splits on bit (levels - 1 - l): zeros first, ones after, both stable.
	for (int l = 0; l < levels; l++) {
		int bit = levels - 1 - l;
		uint32_t *r0 = c->rank0[l];
		r0[0] = 0;
		for (int64_t i = 0; i < n; i++) {
			r0[i + 1] = r0[i] + (((cur[i] >> bit) & 1u) == 0);
		}
		uint32_t z = r0[n];
		c->zeros[l] = z;
		int64_t iz = 0, io = z;
		for (int64_t i = 0; i < n; i++) {
			if (((cur[i] >> bit) & 1u) == 0) {
				next[iz++] = cur[i];
			} else {
				next[io++] = cur[i];
			}
		}
		double *ps = c->psum[l + 1];
		ps[0] = 0;
		for (int64_t i = 0; i < n; i++) {
			ps[i + 1] = ps[i] + c->sorted[next[i]];
		}
		uint32_t *tmp = cur;
		cur = next;
		next = tmp;
	}
	free(cur);
	free(next);
	return 0;
}

double cpt_cost_l1_eval(void *ctx, int64_t a, int64_t b) {
	const l1_ctx *c = (const l1_ctx *)ctx;
	int64_t len = b - a;
	int64_t k = (len - 1) / 2;
	int64_t kk = k;
	uint32_t lo = (uint32_t)a, hi = (uint32_t)b;
	uint32_t rank = 0;
	double below = 0;
	for (int l = 0; l < c->levels; l++) {
		uint32_t za = c->rank0[l][lo];
		uint32_t zb = c->rank0[l][hi];
		uint32_t zeros = zb - za;
		rank <<= 1;
		if ((uint32_t)kk < zeros) {
			lo = za;
			hi = zb;
		} else {
			// Every element of [lo, hi) with a 0 at this bit is smaller than the target.
			below += c->psum[l + 1][zb] - c->psum[l + 1][za];
			kk -= zeros;
			lo = c->zeros[l] + (lo - za);
			hi = c->zeros[l] + (hi - zb);
			rank |= 1u;
		}
	}
	double m = c->sorted[rank];
	double total = c->total[b] - c->total[a];
	double above = total - below - m;
	double cost = (m * (double)k - below) + (above - m * (double)(len - k - 1));
	return cost > 0 ? cost : 0;
}
