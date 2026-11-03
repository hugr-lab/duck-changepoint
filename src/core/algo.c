#include "algo.h"

#include "cpt_alloc.h"
#include "cpt_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

static int out_of_memory(char *err, const char *what) {
	snprintf(err, CPT_ERR_LEN, "out of memory allocating %s", what);
	return 1;
}

void cpt_bkps_free(cpt_bkps *b) {
	free(b->pos);
	b->pos = NULL;
	b->count = 0;
}

// A segmentation from count segment ends; pos may be NULL when count is 0.
static int bkps_alloc(cpt_bkps *out, int64_t count, char *err) {
	out->count = 0;
	out->pos = NULL;
	if (count == 0) {
		return 0;
	}
	out->pos = (int64_t *)cpt_malloc(sizeof(int64_t) * (size_t)count);
	if (!out->pos) {
		return out_of_memory(err, "the result");
	}
	out->count = count;
	return 0;
}

// The whole series as one segment ([] for an empty series).
static int bkps_single(int64_t n, cpt_bkps *out, char *err) {
	if (bkps_alloc(out, n > 0 ? 1 : 0, err)) {
		return 1;
	}
	if (n > 0) {
		out->pos[0] = n;
	}
	return 0;
}

// Walk prev[] back from n and store the ends in increasing order.
static int bkps_backtrack(const int64_t *prev, int64_t n, cpt_bkps *out, char *err) {
	int64_t count = 0;
	for (int64_t t = n; t > 0; t = prev[t]) {
		count++;
	}
	if (bkps_alloc(out, count, err)) {
		return 1;
	}
	int64_t i = count;
	for (int64_t t = n; t > 0; t = prev[t]) {
		out->pos[--i] = t;
	}
	return 0;
}

int cpt_budget_charge(cpt_budget *budget, int64_t units, char *err) {
	if (!budget) {
		return 0;
	}
	budget->n_evals += units;
	if (budget->max_evals > 0 && budget->n_evals > budget->max_evals) {
		snprintf(err, CPT_ERR_LEN, "work limit reached after %lld evaluation units (limit %lld)",
		         (long long)budget->n_evals, (long long)budget->max_evals);
		return CPT_BUDGET_EXCEEDED;
	}
	return 0;
}

// Charge `evals` cost evaluations, weighted by the cost's eval_weight. Called once per outer step, not
// per evaluation, to keep inner loops tight.
static int budget_charge(cpt_budget *budget, const cpt_cost *cost, int64_t evals, char *err) {
	return cpt_budget_charge(budget, evals * cost->eval_weight, err);
}

//===--------------------------------------------------------------------===//
// Penalties and shared definitions
//===--------------------------------------------------------------------===//

int cpt_penalty_parse(const char *name, size_t len) {
	static const struct {
		const char *name;
		cpt_penalty_kind kind;
	} names[] = {{"bic", CPT_PEN_BIC}, {"sic", CPT_PEN_BIC}, {"mbic", CPT_PEN_MBIC}, {"aic", CPT_PEN_AIC}};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (cpt_ascii_ieq(name, len, names[i].name)) {
			return (int)names[i].kind;
		}
	}
	return -1;
}

// Same values as R's changepoint (penalty_decision): the change location counts as one more
// parameter, so a change costs (p + 1) parameters for BIC/AIC. changepoint's MBIC is (p + 2) ln n
// per change plus ln(n_i) for every segment.
cpt_penalty cpt_penalty_named(cpt_penalty_kind kind, int64_t n, int p) {
	double ln_n = n > 1 ? log((double)n) : 0;
	switch (kind) {
	case CPT_PEN_MBIC:
		return (cpt_penalty) {(double)(p + 2) * ln_n, 1};
	case CPT_PEN_AIC:
		return (cpt_penalty) {2.0 * (double)(p + 1), 0};
	case CPT_PEN_BIC:
	default:
		return (cpt_penalty) {(double)(p + 1) * ln_n, 0};
	}
}

int64_t cpt_effective_min_size(const cpt_cost *cost, int64_t min_size) {
	int64_t m = min_size < 1 ? 1 : min_size;
	return m > cost->min_len ? m : cost->min_len;
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

// F[t]: optimal penalised cost of x[0..t). R: candidate last changes, with dead_from[j] the step from
// which R[j] is known to be dominated (INT64_MAX: not yet). vals[j]: F[R[j]] + C(R[j], t) at step t.
typedef struct {
	double *F;
	int64_t *prev;
	int64_t *R;
	int64_t *dead_from;
	double *vals;
	int64_t n_r;
} pelt_ws;

static void pelt_ws_free(pelt_ws *w) {
	free(w->F);
	free(w->prev);
	free(w->R);
	free(w->dead_from);
	free(w->vals);
}

static int pelt_ws_alloc(pelt_ws *w, int64_t n, double beta, char *err) {
	size_t n1 = (size_t)n + 1;
	w->F = (double *)cpt_malloc(sizeof(double) * n1);
	w->prev = (int64_t *)cpt_calloc(n1, sizeof(int64_t));
	w->R = (int64_t *)cpt_malloc(sizeof(int64_t) * n1);
	w->dead_from = (int64_t *)cpt_malloc(sizeof(int64_t) * n1);
	w->vals = (double *)cpt_malloc(sizeof(double) * n1);
	w->n_r = 0;
	if (!w->F || !w->prev || !w->R || !w->dead_from || !w->vals) {
		pelt_ws_free(w);
		return out_of_memory(err, "the PELT workspace");
	}
	w->F[0] = -beta;
	for (int64_t t = 1; t <= n; t++) {
		w->F[t] = INFINITY;
	}
	return 0;
}

// x[0..s) must be segmentable (F[s] finite) for s to be a last change.
static void pelt_admit(pelt_ws *w, int64_t s) {
	if (isfinite(w->F[s])) {
		w->R[w->n_r] = s;
		w->dead_from[w->n_r] = INT64_MAX;
		w->n_r++;
	}
}

static double pelt_value(const pelt_ws *w, const cpt_cost *cost, cpt_penalty pen, int64_t s, int64_t t) {
	double v = w->F[s] + cost->eval(cost->ctx, s, t);
	return pen.mbic ? v + log((double)(t - s)) : v;
}

// F[t] and prev[t] from every live candidate; vals[] keeps the values for pruning.
static void pelt_evaluate(pelt_ws *w, const cpt_cost *cost, cpt_penalty pen, int64_t t) {
	double best = INFINITY;
	int64_t arg = 0;
	for (int64_t j = 0; j < w->n_r; j++) {
		w->vals[j] = t >= w->dead_from[j] ? INFINITY : pelt_value(w, cost, pen, w->R[j], t);
		if (w->vals[j] + pen.beta < best) {
			best = w->vals[j] + pen.beta;
			arg = w->R[j];
		}
	}
	w->F[t] = best;
	w->prev[t] = arg;
}

// Pruning (Killick et al. 2012, Theorem 3.1): if F[s] + C(s,t) + K > F[t], then for every t' at
// which t can be the last change, F[t] + C(t,t') beats F[s] + C(s,t'). With a minimum segment length
// that holds only for t' >= t + m, so s stays a candidate until then. Pruning it immediately (as
// ruptures and changepoint do) can lose the optimum when m > 1. Returns whether R[j] is kept.
static int pelt_keep(pelt_ws *w, int64_t j, int64_t t, int64_t m, double K) {
	if (t >= w->dead_from[j]) {
		return 0;
	}
	if (w->dead_from[j] == INT64_MAX && w->vals[j] + K > w->F[t]) {
		w->dead_from[j] = t + m;
	}
	// with m == 1, t + 1 is the next step: a dominated candidate goes now
	return m > 1 || w->dead_from[j] == INT64_MAX;
}

static void pelt_prune(pelt_ws *w, int64_t t, int64_t m, double K) {
	int64_t keep = 0;
	for (int64_t j = 0; j < w->n_r; j++) {
		if (pelt_keep(w, j, t, m, K)) {
			w->R[keep] = w->R[j];
			w->dead_from[keep] = w->dead_from[j];
			keep++;
		}
	}
	w->n_r = keep;
}

static int pelt_run(pelt_ws *w, const cpt_cost *cost, cpt_penalty pen, int64_t n, int64_t m, cpt_budget *budget,
                    char *err) {
	// With MBIC every segment also pays ln(len). ln(x) + ln(y) - ln(x + y) <= ln(n), so
	// C'(a,s) + C'(s,b) - ln(n) <= C'(a,b): the Theorem 3.1 constant drops by ln(n).
	const double K = cost->K - (pen.mbic ? log((double)n) : 0);
	for (int64_t t = m; t <= n; t++) {
		pelt_admit(w, t - m); // the newest admissible last change; earlier ones are already in R
		pelt_evaluate(w, cost, pen, t);
		int rc = budget_charge(budget, cost, w->n_r, err);
		if (rc) {
			return rc;
		}
		pelt_prune(w, t, m, K);
	}
	return 0;
}

int cpt_pelt(const cpt_cost *cost, int64_t n, cpt_penalty pen, int64_t min_size, cpt_budget *budget,
             cpt_bkps *out, char *err) {
	out->pos = NULL;
	out->count = 0;
	int64_t m = cpt_effective_min_size(cost, min_size);
	// n / 2 < m is n < 2 * m without overflowing for a huge min_size.
	if (n / 2 < m) {
		return bkps_single(n, out, err);
	}
	pelt_ws w;
	if (pelt_ws_alloc(&w, n, pen.beta, err)) {
		return 1;
	}
	int rc = pelt_run(&w, cost, pen, n, m, budget, err);
	if (rc == 0) {
		rc = bkps_backtrack(w.prev, n, out, err);
	}
	pelt_ws_free(&w);
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

static int binseg_best_split(const cpt_cost *cost, int64_t m, binseg_segment *seg, cpt_budget *budget, char *err) {
	seg->split = -1;
	seg->gain = -INFINITY;
	if ((seg->b - seg->a) / 2 < m) {
		return 0;
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
	return budget_charge(budget, cost, 1 + 2 * (seg->b - seg->a - 2 * m + 1), err);
}

// The splittable segment with the largest gain (the leftmost on ties), or -1.
static int64_t binseg_pick(const binseg_segment *segs, int64_t n_segs) {
	int64_t best = -1;
	for (int64_t i = 0; i < n_segs; i++) {
		if (segs[i].split >= 0 && (best < 0 || segs[i].gain > segs[best].gain)) {
			best = i;
		}
	}
	return best;
}

// Stop after n_bkps splits (n_bkps >= 0), or once the best gain is not above the penalty.
static int binseg_done(const binseg_segment *best, int64_t n_segs, int64_t n_bkps, double pen) {
	return n_bkps >= 0 ? n_segs - 1 >= n_bkps : !(best->gain > pen);
}

// Split segment i at its best split; the two halves are rescanned for their own best split.
static int binseg_split(const cpt_cost *cost, int64_t m, binseg_segment *segs, int64_t *n_segs, int64_t i,
                        cpt_budget *budget, char *err) {
	memmove(&segs[i + 2], &segs[i + 1], sizeof(binseg_segment) * (size_t)(*n_segs - i - 1));
	int64_t s = segs[i].split;
	segs[i + 1] = (binseg_segment) {s, segs[i].b, -1, -INFINITY};
	segs[i].b = s;
	(*n_segs)++;
	int rc = binseg_best_split(cost, m, &segs[i], budget, err);
	return rc ? rc : binseg_best_split(cost, m, &segs[i + 1], budget, err);
}

static int binseg_collect(const binseg_segment *segs, int64_t n_segs, cpt_bkps *out, char *err) {
	if (bkps_alloc(out, n_segs, err)) {
		return 1;
	}
	for (int64_t i = 0; i < n_segs; i++) {
		out->pos[i] = segs[i].b;
	}
	return 0;
}

int cpt_binseg(const cpt_cost *cost, int64_t n, int64_t n_bkps, double pen, int64_t min_size, cpt_budget *budget,
               cpt_bkps *out, char *err) {
	out->pos = NULL;
	out->count = 0;
	int64_t m = cpt_effective_min_size(cost, min_size);
	if (n / 2 < m || n_bkps == 0) {
		return bkps_single(n, out, err);
	}
	// Segments are kept in position order; at most n / m of them can exist.
	binseg_segment *segs = (binseg_segment *)cpt_malloc(sizeof(binseg_segment) * (size_t)(n / m + 1));
	if (!segs) {
		return out_of_memory(err, "the segment list");
	}
	int64_t n_segs = 1;
	segs[0] = (binseg_segment) {0, n, -1, -INFINITY};
	int rc = binseg_best_split(cost, m, &segs[0], budget, err);
	int64_t best;
	while (rc == 0 && (best = binseg_pick(segs, n_segs)) >= 0 && !binseg_done(&segs[best], n_segs, n_bkps, pen)) {
		rc = binseg_split(cost, m, segs, &n_segs, best, budget, err);
	}
	if (rc == 0) {
		rc = binseg_collect(segs, n_segs, out, err);
	}
	free(segs);
	return rc;
}

//===--------------------------------------------------------------------===//
// Dynamic programming with a known number of change points
//===--------------------------------------------------------------------===//

// A negative n_bkps is rejected for every series; an empty series gives [] for any other n_bkps.
static int dynp_check(int64_t n, int64_t n_bkps, int64_t m, char *err) {
	if (n_bkps < 0) {
		snprintf(err, CPT_ERR_LEN, "n_bkps must be >= 0, got %lld", (long long)n_bkps);
		return 1;
	}
	// n_bkps + 1 > n / m, checked before the addition so that it cannot overflow.
	if (n > 0 && n_bkps >= n / m) {
		snprintf(err, CPT_ERR_LEN, "cannot split a series of length %lld into %llu segments of at least %lld elements",
		         (long long)n, (unsigned long long)n_bkps + 1, (long long)m);
		return 1;
	}
	return 0;
}

// prev_row/cur_row: best cost of [0, t) as k and k + 1 segments.
// arg[k * (n + 1) + t]: start of the last segment in the best (k + 1)-segment split of [0, t).
typedef struct {
	double *prev_row;
	double *cur_row;
	int64_t *arg;
	size_t n1;
} dynp_ws;

static void dynp_ws_free(dynp_ws *w) {
	free(w->prev_row);
	free(w->cur_row);
	free(w->arg);
}

static int dynp_ws_alloc(dynp_ws *w, int64_t n, int64_t n_seg, char *err) {
	w->n1 = (size_t)n + 1;
	w->prev_row = (double *)cpt_malloc(sizeof(double) * w->n1);
	w->cur_row = (double *)cpt_malloc(sizeof(double) * w->n1);
	w->arg = (int64_t *)cpt_calloc(w->n1 * (size_t)n_seg, sizeof(int64_t));
	if (!w->prev_row || !w->cur_row || !w->arg) {
		dynp_ws_free(w);
		return out_of_memory(err, "the DP table");
	}
	return 0;
}

static void dynp_first_row(dynp_ws *w, const cpt_cost *cost, int64_t n, int64_t m) {
	for (int64_t t = 0; t <= n; t++) {
		w->prev_row[t] = t >= m ? cost->eval(cost->ctx, 0, t) : INFINITY;
	}
}

// Best start s of the last segment [s, t) of a (k + 1)-segment split, written to cur_row/arg.
static void dynp_best_start(dynp_ws *w, const cpt_cost *cost, int64_t k, int64_t m, int64_t t) {
	double best = INFINITY;
	int64_t best_s = 0;
	for (int64_t s = k * m; s <= t - m; s++) {
		double v = w->prev_row[s] + cost->eval(cost->ctx, s, t);
		if (v < best) {
			best = v;
			best_s = s;
		}
	}
	w->cur_row[t] = best;
	w->arg[(size_t)k * w->n1 + (size_t)t] = best_s;
}

// Row k: [0, t) as k + 1 segments. t needs (k + 1) * m points and the remaining segments theirs.
static int dynp_row(dynp_ws *w, const cpt_cost *cost, int64_t n, int64_t n_seg, int64_t k, int64_t m,
                    cpt_budget *budget, char *err) {
	for (int64_t t = 0; t <= n; t++) {
		w->cur_row[t] = INFINITY;
	}
	for (int64_t t = (k + 1) * m; t <= n - (n_seg - k - 1) * m; t++) {
		dynp_best_start(w, cost, k, m, t);
		int rc = budget_charge(budget, cost, t - m - k * m + 1, err);
		if (rc) {
			return rc;
		}
	}
	double *tmp = w->prev_row;
	w->prev_row = w->cur_row;
	w->cur_row = tmp;
	return 0;
}

static int dynp_backtrack(const dynp_ws *w, int64_t n, int64_t n_seg, cpt_bkps *out, char *err) {
	if (bkps_alloc(out, n_seg, err)) {
		return 1;
	}
	int64_t t = n;
	for (int64_t k = n_seg - 1; k >= 0; k--) {
		out->pos[k] = t;
		t = w->arg[(size_t)k * w->n1 + (size_t)t];
	}
	return 0;
}

int cpt_dynp(const cpt_cost *cost, int64_t n, int64_t n_bkps, int64_t min_size, cpt_budget *budget,
             cpt_bkps *out, char *err) {
	out->pos = NULL;
	out->count = 0;
	int64_t m = cpt_effective_min_size(cost, min_size);
	if (dynp_check(n, n_bkps, m, err)) {
		return 1;
	}
	if (n == 0) {
		return 0;
	}
	int64_t n_seg = n_bkps + 1;
	dynp_ws w;
	if (dynp_ws_alloc(&w, n, n_seg, err)) {
		return 1;
	}
	dynp_first_row(&w, cost, n, m);
	int rc = budget_charge(budget, cost, n, err);
	for (int64_t k = 1; k < n_seg && rc == 0; k++) {
		rc = dynp_row(&w, cost, n, n_seg, k, m, budget, err);
	}
	if (rc == 0) {
		rc = dynp_backtrack(&w, n, n_seg, out, err);
	}
	dynp_ws_free(&w);
	return rc;
}

//===--------------------------------------------------------------------===//
// CROPS
//===--------------------------------------------------------------------===//

// Grow *items to hold at least need elements of size elem, doubling from 4.
static int grow(void **items, int64_t *cap, int64_t need, size_t elem, char *err) {
	if (need <= *cap) {
		return 0;
	}
	int64_t cap2 = *cap ? *cap * 2 : 4;
	while (cap2 < need) {
		cap2 *= 2;
	}
	void *p = cpt_realloc(*items, elem * (size_t)cap2);
	if (!p) {
		return out_of_memory(err, "the CROPS state");
	}
	*items = p;
	*cap = cap2;
	return 0;
}

typedef struct {
	cpt_crops_entry *items;
	int64_t count, cap;
} crops_set;

typedef struct {
	double b0, b1;
	int64_t i0, i1;
} crops_interval;

typedef struct {
	crops_interval *items;
	int64_t count, cap;
} crops_stack;

static int stack_push(crops_stack *st, crops_interval iv, char *err) {
	if (grow((void **)&st->items, &st->cap, st->count + 1, sizeof(crops_interval), err)) {
		return 1;
	}
	st->items[st->count++] = iv;
	return 0;
}

// Optimal segmentations with the same number of change points have the same cost, so the number of
// change points identifies a point on the lower envelope.
static int64_t crops_find(const crops_set *set, int64_t count) {
	for (int64_t i = 0; i < set->count; i++) {
		if (set->items[i].bkps.count == count) {
			return i;
		}
	}
	return -1;
}

// Add b (taking ownership) at beta, or merge it into the entry with the same count.
static int crops_add(crops_set *set, const cpt_cost *cost, cpt_bkps b, double beta, int64_t *idx, char *err) {
	int64_t i = crops_find(set, b.count);
	if (i >= 0) {
		set->items[i].penalty = fmin(set->items[i].penalty, beta);
		cpt_bkps_free(&b);
		*idx = i;
		return 0;
	}
	if (grow((void **)&set->items, &set->cap, set->count + 1, sizeof(cpt_crops_entry), err)) {
		cpt_bkps_free(&b);
		return 1;
	}
	set->items[set->count] = (cpt_crops_entry) {beta, cpt_segmentation_cost(cost, &b), b};
	*idx = set->count++;
	return 0;
}

// Run PELT at beta and return the index of its segmentation in the set.
static int crops_run(const cpt_cost *cost, int64_t n, double beta, int64_t min_size, cpt_budget *budget,
                     crops_set *set, int64_t *idx, char *err) {
	cpt_bkps b;
	int rc = cpt_pelt(cost, n, (cpt_penalty) {beta, 0}, min_size, budget, &b, err);
	return rc ? rc : crops_add(set, cost, b, beta, idx, err);
}

static int64_t n_changes(const crops_set *set, int64_t i) {
	return set->items[i].bkps.count - 1;
}

// One interval: run PELT where the lines of its two segmentations meet; if that finds a segmentation
// strictly between them, both halves are pushed for further search.
static int crops_step(const cpt_cost *cost, int64_t n, int64_t min_size, cpt_budget *budget, crops_set *set,
                      crops_stack *st, crops_interval iv, char *err) {
	int64_t m0 = n_changes(set, iv.i0), m1 = n_changes(set, iv.i1);
	if (m0 <= m1 + 1) {
		return 0; // no segmentation with a change point count strictly between them
	}
	double b_int = (set->items[iv.i1].cost - set->items[iv.i0].cost) / (double)(m0 - m1);
	if (!(b_int > iv.b0 && b_int < iv.b1)) {
		return 0; // rounding put the intersection outside the interval
	}
	int64_t i_int;
	int rc = crops_run(cost, n, b_int, min_size, budget, set, &i_int, err);
	if (rc || n_changes(set, i_int) == m0 || n_changes(set, i_int) == m1) {
		return rc; // on error, or the two lines meet on the envelope: nothing in between
	}
	rc = stack_push(st, (crops_interval) {iv.b0, b_int, iv.i0, i_int}, err);
	return rc ? rc : stack_push(st, (crops_interval) {b_int, iv.b1, i_int, iv.i1}, err);
}

static int crops_search(const cpt_cost *cost, int64_t n, double pen_min, double pen_max, int64_t min_size,
                        cpt_budget *budget, crops_set *set, char *err) {
	int64_t i_min, i_max;
	int rc = crops_run(cost, n, pen_min, min_size, budget, set, &i_min, err);
	if (rc || !(pen_max > pen_min)) {
		return rc;
	}
	rc = crops_run(cost, n, pen_max, min_size, budget, set, &i_max, err);
	crops_stack st = {NULL, 0, 0};
	if (rc == 0) {
		rc = stack_push(&st, (crops_interval) {pen_min, pen_max, i_min, i_max}, err);
	}
	while (rc == 0 && st.count > 0) {
		crops_interval iv = st.items[--st.count];
		rc = crops_step(cost, n, min_size, budget, set, &st, iv, err);
	}
	free(st.items);
	return rc;
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
              cpt_budget *budget, cpt_crops_entry **out, int64_t *out_count, char *err) {
	crops_set set = {NULL, 0, 0};
	int rc = crops_search(cost, n, pen_min, pen_max, min_size, budget, &set, err);
	if (rc) {
		cpt_crops_free(set.items, set.count);
		*out = NULL;
		*out_count = 0;
		return rc;
	}
	qsort(set.items, (size_t)set.count, sizeof(cpt_crops_entry), cmp_entry_penalty);
	*out = set.items;
	*out_count = set.count;
	return 0;
}
