#include "algo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

void cpt_bkps_free(cpt_bkps *b) {
	free(b->pos);
	b->pos = NULL;
	b->count = 0;
}

static int bkps_single(int64_t n, cpt_bkps *out, char *err) {
	out->count = 0;
	out->pos = NULL;
	if (n == 0) {
		return 0;
	}
	out->pos = (int64_t *)malloc(sizeof(int64_t));
	if (!out->pos) {
		snprintf(err, CPT_ERR_LEN, "out of memory");
		return 1;
	}
	out->pos[0] = n;
	out->count = 1;
	return 0;
}

// Walk prev[] back from n and store the ends in increasing order.
static int bkps_backtrack(const int64_t *prev, int64_t n, cpt_bkps *out, char *err) {
	int64_t count = 0;
	for (int64_t t = n; t > 0; t = prev[t]) {
		count++;
	}
	out->pos = (int64_t *)malloc(sizeof(int64_t) * (size_t)count);
	if (!out->pos) {
		snprintf(err, CPT_ERR_LEN, "out of memory");
		return 1;
	}
	out->count = count;
	int64_t i = count;
	for (int64_t t = n; t > 0; t = prev[t]) {
		out->pos[--i] = t;
	}
	return 0;
}

static int ascii_ieq(const char *s, size_t len, const char *lit) {
	if (strlen(lit) != len) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		char ch = s[i];
		if (ch >= 'A' && ch <= 'Z') {
			ch = (char)(ch - 'A' + 'a');
		}
		if (ch != lit[i]) {
			return 0;
		}
	}
	return 1;
}

int cpt_penalty_parse(const char *name, size_t len) {
	if (ascii_ieq(name, len, "bic") || ascii_ieq(name, len, "sic")) {
		return CPT_PEN_BIC;
	}
	if (ascii_ieq(name, len, "mbic")) {
		return CPT_PEN_MBIC;
	}
	if (ascii_ieq(name, len, "aic")) {
		return CPT_PEN_AIC;
	}
	return -1;
}

cpt_penalty cpt_penalty_named(cpt_penalty_kind kind, int64_t n, int p) {
	cpt_penalty pen = {0, 0};
	double ln_n = n > 1 ? log((double)n) : 0;
	// Same values as R's changepoint (penalty_decision): the change location counts as one more
	// parameter, so a change costs (p + 1) parameters for BIC/AIC.
	switch (kind) {
	case CPT_PEN_MBIC:
		// changepoint's MBIC: (p + 2) ln n per change plus ln(n_i) for every segment.
		pen.beta = (double)(p + 2) * ln_n;
		pen.mbic = 1;
		break;
	case CPT_PEN_AIC:
		pen.beta = 2.0 * (double)(p + 1);
		break;
	case CPT_PEN_BIC:
	default:
		pen.beta = (double)(p + 1) * ln_n;
		break;
	}
	return pen;
}

int64_t cpt_effective_min_size(const cpt_cost *cost, int64_t min_size) {
	if (min_size < 1) {
		min_size = 1;
	}
	return min_size > cost->min_len ? min_size : cost->min_len;
}

double cpt_segmentation_cost(const cpt_cost *cost, const cpt_bkps *bkps) {
	double q = 0;
	int64_t a = 0;
	for (int64_t i = 0; i < bkps->count; i++) {
		q += cost->eval(cost->ctx, a, bkps->pos[i]);
		a = bkps->pos[i];
	}
	return q;
}

//===--------------------------------------------------------------------===//
// PELT
//===--------------------------------------------------------------------===//

int cpt_pelt(const cpt_cost *cost, int64_t n, cpt_penalty pen, int64_t min_size, cpt_bkps *out, char *err) {
	out->pos = NULL;
	out->count = 0;
	int64_t m = cpt_effective_min_size(cost, min_size);
	if (n < 2 * m) {
		return bkps_single(n, out, err);
	}
	const double beta = pen.beta;
	// With MBIC every segment also pays ln(len). ln(x) + ln(y) - ln(x + y) <= ln(n), so
	// C'(a,s) + C'(s,b) - ln(n) <= C'(a,b): the Theorem 3.1 constant drops by ln(n).
	const double K = cost->K - (pen.mbic ? log((double)n) : 0);

	size_t n1 = (size_t)n + 1;
	double *F = (double *)malloc(sizeof(double) * n1);
	int64_t *prev = (int64_t *)malloc(sizeof(int64_t) * n1);
	int64_t *R = (int64_t *)malloc(sizeof(int64_t) * n1);
	int64_t *dead_from = (int64_t *)malloc(sizeof(int64_t) * n1); // parallel to R
	double *vals = (double *)malloc(sizeof(double) * n1);
	if (!F || !prev || !R || !dead_from || !vals) {
		free(F);
		free(prev);
		free(R);
		free(dead_from);
		free(vals);
		snprintf(err, CPT_ERR_LEN, "out of memory allocating PELT workspace for n = %lld", (long long)n);
		return 1;
	}

	F[0] = -beta;
	prev[0] = 0;
	for (int64_t t = 1; t <= n; t++) {
		F[t] = INFINITY;
		prev[t] = 0;
	}
	int64_t n_r = 0;
	for (int64_t t = m; t <= n; t++) {
		// t - m is the newest admissible last change; earlier candidates are already in R.
		int64_t s_new = t - m;
		if (isfinite(F[s_new])) {
			R[n_r] = s_new;
			dead_from[n_r] = INT64_MAX;
			n_r++;
		}
		double best = INFINITY;
		int64_t arg = 0;
		for (int64_t j = 0; j < n_r; j++) {
			int64_t s = R[j];
			if (t >= dead_from[j]) {
				vals[j] = INFINITY; // dominated by an earlier change point; dropped below
				continue;
			}
			double v = F[s] + cost->eval(cost->ctx, s, t);
			if (pen.mbic) {
				v += log((double)(t - s));
			}
			vals[j] = v;
			if (v + beta < best) {
				best = v + beta;
				arg = s;
			}
		}
		F[t] = best;
		prev[t] = arg;
		// Pruning (Killick et al. 2012, Theorem 3.1): if F[s] + C(s,t) + K > F[t], then for every
		// t' at which t can be the last change, F[t] + C(t,t') beats F[s] + C(s,t'). With a minimum
		// segment length that holds only for t' >= t + m, so s stays a candidate until then. Pruning
		// it immediately (as ruptures and changepoint do) can lose the optimum when m > 1.
		int64_t keep = 0;
		for (int64_t j = 0; j < n_r; j++) {
			if (t >= dead_from[j]) {
				continue;
			}
			if (dead_from[j] == INT64_MAX && vals[j] + K > F[t]) {
				dead_from[j] = t + m;
			}
			if (m == 1 && dead_from[j] != INT64_MAX) {
				continue; // t + 1 is the next step: drop now
			}
			R[keep] = R[j];
			dead_from[keep] = dead_from[j];
			keep++;
		}
		n_r = keep;
	}

	int rc = bkps_backtrack(prev, n, out, err);
	free(F);
	free(prev);
	free(R);
	free(dead_from);
	free(vals);
	return rc;
}

//===--------------------------------------------------------------------===//
// Binary segmentation
//===--------------------------------------------------------------------===//

typedef struct {
	int64_t a, b;
	int64_t split; // -1: no admissible split
	double gain;
} binseg_segment;

static void binseg_best_split(const cpt_cost *cost, int64_t m, binseg_segment *seg) {
	seg->split = -1;
	seg->gain = -INFINITY;
	if (seg->b - seg->a < 2 * m) {
		return;
	}
	double whole = cost->eval(cost->ctx, seg->a, seg->b);
	double best = INFINITY;
	for (int64_t s = seg->a + m; s <= seg->b - m; s++) {
		double v = cost->eval(cost->ctx, seg->a, s) + cost->eval(cost->ctx, s, seg->b);
		if (v < best) {
			best = v;
			seg->split = s;
		}
	}
	seg->gain = whole - best;
}

int cpt_binseg(const cpt_cost *cost, int64_t n, int64_t n_bkps, double pen, int64_t min_size, cpt_bkps *out,
               char *err) {
	out->pos = NULL;
	out->count = 0;
	int64_t m = cpt_effective_min_size(cost, min_size);
	if (n < 2 * m || n_bkps == 0) {
		return bkps_single(n, out, err);
	}
	// Segments are kept in position order; at most n / m of them can exist.
	int64_t cap = n / m + 1;
	binseg_segment *segs = (binseg_segment *)malloc(sizeof(binseg_segment) * (size_t)cap);
	if (!segs) {
		snprintf(err, CPT_ERR_LEN, "out of memory");
		return 1;
	}
	int64_t n_segs = 1;
	segs[0].a = 0;
	segs[0].b = n;
	binseg_best_split(cost, m, &segs[0]);

	for (;;) {
		int64_t best = -1;
		for (int64_t i = 0; i < n_segs; i++) {
			if (segs[i].split >= 0 && (best < 0 || segs[i].gain > segs[best].gain)) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		if (n_bkps >= 0) {
			if (n_segs - 1 >= n_bkps) {
				break;
			}
		} else if (!(segs[best].gain > pen)) {
			break;
		}
		memmove(&segs[best + 2], &segs[best + 1], sizeof(binseg_segment) * (size_t)(n_segs - best - 1));
		int64_t s = segs[best].split;
		segs[best + 1].a = s;
		segs[best + 1].b = segs[best].b;
		segs[best].b = s;
		binseg_best_split(cost, m, &segs[best]);
		binseg_best_split(cost, m, &segs[best + 1]);
		n_segs++;
	}

	out->pos = (int64_t *)malloc(sizeof(int64_t) * (size_t)n_segs);
	if (!out->pos) {
		free(segs);
		snprintf(err, CPT_ERR_LEN, "out of memory");
		return 1;
	}
	for (int64_t i = 0; i < n_segs; i++) {
		out->pos[i] = segs[i].b;
	}
	out->count = n_segs;
	free(segs);
	return 0;
}

//===--------------------------------------------------------------------===//
// Dynamic programming with a known number of change points
//===--------------------------------------------------------------------===//

int cpt_dynp(const cpt_cost *cost, int64_t n, int64_t n_bkps, int64_t min_size, cpt_bkps *out, char *err) {
	out->pos = NULL;
	out->count = 0;
	int64_t m = cpt_effective_min_size(cost, min_size);
	if (n == 0) {
		return 0;
	}
	int64_t n_seg = n_bkps + 1;
	if (n_bkps < 0 || n_seg > n / m) {
		snprintf(err, CPT_ERR_LEN,
		         "cannot split a series of length %lld into %lld segments of at least %lld elements",
		         (long long)n, (long long)n_seg, (long long)m);
		return 1;
	}
	size_t n1 = (size_t)n + 1;
	double *prev_row = (double *)malloc(sizeof(double) * n1);
	double *cur_row = (double *)malloc(sizeof(double) * n1);
	// arg[k * (n + 1) + t]: start of the last segment in the best k+1-segment split of [0, t)
	int64_t *arg = (int64_t *)malloc(sizeof(int64_t) * n1 * (size_t)n_seg);
	if (!prev_row || !cur_row || !arg) {
		free(prev_row);
		free(cur_row);
		free(arg);
		snprintf(err, CPT_ERR_LEN, "out of memory allocating DP table (%lld x %lld)", (long long)n_seg,
		         (long long)n1);
		return 1;
	}
	for (int64_t t = 0; t <= n; t++) {
		prev_row[t] = t >= m ? cost->eval(cost->ctx, 0, t) : INFINITY;
		arg[t] = 0;
	}
	for (int64_t k = 1; k < n_seg; k++) {
		int64_t *arg_k = arg + (size_t)k * n1;
		// [0, t) as k+1 segments: t needs (k+1)*m points and the remaining segments need theirs.
		int64_t t_lo = (k + 1) * m;
		int64_t t_hi = n - (n_seg - k - 1) * m;
		for (int64_t t = 0; t <= n; t++) {
			cur_row[t] = INFINITY;
			arg_k[t] = 0;
		}
		for (int64_t t = t_lo; t <= t_hi; t++) {
			double best = INFINITY;
			int64_t best_s = 0;
			for (int64_t s = k * m; s <= t - m; s++) {
				double v = prev_row[s] + cost->eval(cost->ctx, s, t);
				if (v < best) {
					best = v;
					best_s = s;
				}
			}
			cur_row[t] = best;
			arg_k[t] = best_s;
		}
		double *tmp = prev_row;
		prev_row = cur_row;
		cur_row = tmp;
	}

	out->pos = (int64_t *)malloc(sizeof(int64_t) * (size_t)n_seg);
	int rc = 0;
	if (!out->pos) {
		snprintf(err, CPT_ERR_LEN, "out of memory");
		rc = 1;
	} else {
		out->count = n_seg;
		int64_t t = n;
		for (int64_t k = n_seg - 1; k >= 0; k--) {
			out->pos[k] = t;
			t = arg[(size_t)k * n1 + t];
		}
	}
	free(prev_row);
	free(cur_row);
	free(arg);
	return rc;
}

//===--------------------------------------------------------------------===//
// CROPS
//===--------------------------------------------------------------------===//

typedef struct {
	cpt_crops_entry *items;
	int64_t count, cap;
} crops_set;

// Run PELT at beta and return the index of its segmentation in the set (merging duplicates).
static int crops_run(const cpt_cost *cost, int64_t n, double beta, int64_t min_size, crops_set *set, int64_t *idx,
                     char *err) {
	cpt_penalty pen = {beta, 0};
	cpt_bkps b;
	if (cpt_pelt(cost, n, pen, min_size, &b, err)) {
		return 1;
	}
	// Optimal segmentations with the same number of change points have the same cost, so the
	// number of change points identifies a point on the lower envelope.
	for (int64_t i = 0; i < set->count; i++) {
		if (set->items[i].bkps.count == b.count) {
			if (beta < set->items[i].penalty) {
				set->items[i].penalty = beta;
			}
			cpt_bkps_free(&b);
			*idx = i;
			return 0;
		}
	}
	if (set->count == set->cap) {
		int64_t cap = set->cap ? set->cap * 2 : 16;
		cpt_crops_entry *items = (cpt_crops_entry *)realloc(set->items, sizeof(cpt_crops_entry) * (size_t)cap);
		if (!items) {
			cpt_bkps_free(&b);
			snprintf(err, CPT_ERR_LEN, "out of memory");
			return 1;
		}
		set->items = items;
		set->cap = cap;
	}
	cpt_crops_entry *e = &set->items[set->count];
	e->penalty = beta;
	e->bkps = b;
	e->cost = cpt_segmentation_cost(cost, &b);
	*idx = set->count++;
	return 0;
}

static int cmp_entry_penalty(const void *pa, const void *pb) {
	const cpt_crops_entry *a = (const cpt_crops_entry *)pa;
	const cpt_crops_entry *b = (const cpt_crops_entry *)pb;
	if (a->penalty != b->penalty) {
		return a->penalty < b->penalty ? -1 : 1;
	}
	return (a->bkps.count < b->bkps.count) - (a->bkps.count > b->bkps.count);
}

void cpt_crops_free(cpt_crops_entry *entries, int64_t count) {
	if (!entries) {
		return;
	}
	for (int64_t i = 0; i < count; i++) {
		cpt_bkps_free(&entries[i].bkps);
	}
	free(entries);
}

int cpt_crops(const cpt_cost *cost, int64_t n, double pen_min, double pen_max, int64_t min_size,
              cpt_crops_entry **out, int64_t *out_count, char *err) {
	*out = NULL;
	*out_count = 0;
	crops_set set = {NULL, 0, 0};
	typedef struct {
		double b0, b1;
		int64_t i0, i1;
	} interval;
	interval *stack = NULL;
	int64_t n_stack = 0, cap_stack = 0;
	int rc = 0;

	int64_t i_min, i_max;
	if (crops_run(cost, n, pen_min, min_size, &set, &i_min, err) ||
	    (pen_max > pen_min && crops_run(cost, n, pen_max, min_size, &set, &i_max, err))) {
		rc = 1;
		goto done;
	}
	if (pen_max > pen_min) {
		cap_stack = 16;
		stack = (interval *)malloc(sizeof(interval) * (size_t)cap_stack);
		if (!stack) {
			snprintf(err, CPT_ERR_LEN, "out of memory");
			rc = 1;
			goto done;
		}
		stack[n_stack++] = (interval) {pen_min, pen_max, i_min, i_max};
	}
	while (n_stack > 0) {
		interval iv = stack[--n_stack];
		int64_t m0 = set.items[iv.i0].bkps.count - 1;
		int64_t m1 = set.items[iv.i1].bkps.count - 1;
		if (m0 <= m1 + 1) {
			continue; // no segmentation with a change point count strictly between them
		}
		// Penalty at which the two segmentations have equal penalised cost.
		double b_int = (set.items[iv.i1].cost - set.items[iv.i0].cost) / (double)(m0 - m1);
		if (!(b_int > iv.b0 && b_int < iv.b1)) {
			continue; // rounding put the intersection outside the interval
		}
		int64_t i_int;
		if (crops_run(cost, n, b_int, min_size, &set, &i_int, err)) {
			rc = 1;
			goto done;
		}
		int64_t m_int = set.items[i_int].bkps.count - 1;
		if (m_int == m0 || m_int == m1) {
			continue; // the two envelopes meet at b_int: nothing in between
		}
		if (n_stack + 2 > cap_stack) {
			cap_stack *= 2;
			interval *s2 = (interval *)realloc(stack, sizeof(interval) * (size_t)cap_stack);
			if (!s2) {
				snprintf(err, CPT_ERR_LEN, "out of memory");
				rc = 1;
				goto done;
			}
			stack = s2;
		}
		stack[n_stack++] = (interval) {iv.b0, b_int, iv.i0, i_int};
		stack[n_stack++] = (interval) {b_int, iv.b1, i_int, iv.i1};
	}
	qsort(set.items, (size_t)set.count, sizeof(cpt_crops_entry), cmp_entry_penalty);

done:
	free(stack);
	if (rc) {
		cpt_crops_free(set.items, set.count);
		return rc;
	}
	*out = set.items;
	*out_count = set.count;
	return 0;
}
