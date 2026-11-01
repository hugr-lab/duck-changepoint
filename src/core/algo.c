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
