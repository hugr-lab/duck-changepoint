#include "online.h"

#include "cpt_alloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===--------------------------------------------------------------------===//
// Candidate hull (one sign)
//===--------------------------------------------------------------------===//

static void hull_free(cpt_hull *h) {
	free(h->tau);
	free(h->v);
	*h = (cpt_hull) {NULL, NULL, 0, 0, 0};
}

// Room for one more point at the back: compact a drained front first, then grow.
static int hull_reserve(cpt_hull *h, char *err) {
	if (h->head > 0 && h->head + h->len >= h->cap) {
		memmove(h->tau, h->tau + h->head, sizeof(int64_t) * (size_t)h->len);
		memmove(h->v, h->v + h->head, sizeof(double) * (size_t)h->len);
		h->head = 0;
	}
	if (h->len < h->cap) {
		return 0;
	}
	int64_t cap = h->cap ? h->cap * 2 : 8;
	int64_t *tau = (int64_t *)cpt_realloc(h->tau, sizeof(int64_t) * (size_t)cap);
	if (tau) {
		h->tau = tau;
	}
	double *v = tau ? (double *)cpt_realloc(h->v, sizeof(double) * (size_t)cap) : NULL;
	if (!v) {
		snprintf(err, CPT_ERR_LEN, "out of memory growing the online detector");
		return 1;
	}
	h->v = v;
	h->cap = cap;
	return 0;
}

// Whether the last point stays a vertex of the lower hull when (tau, v) is added after it.
static int last_is_convex(const cpt_hull *h, int64_t tau, double v) {
	int64_t a = h->head + h->len - 2, b = a + 1;
	return (h->v[b] - h->v[a]) * (double)(tau - h->tau[b]) < (v - h->v[b]) * (double)(h->tau[b] - h->tau[a]);
}

// Add the newest point (tau, v). Points that are no longer hull vertices, and front points with a
// later point at least as low, are dominated for every change size from now on and are dropped.
static int hull_push(cpt_hull *h, int64_t tau, double v, char *err) {
	while (h->len >= 2 && !last_is_convex(h, tau, v)) {
		h->len--;
	}
	if (hull_reserve(h, err)) {
		return 1;
	}
	h->tau[h->head + h->len] = tau;
	h->v[h->head + h->len] = v;
	h->len++;
	while (h->len >= 2 && h->v[h->head + 1] <= h->v[h->head]) {
		h->head++;
		h->len--;
	}
	return 0;
}

static int hull_reset(cpt_hull *h, char *err) {
	h->head = 0;
	h->len = 0;
	return hull_push(h, 0, 0.0, err);
}

// The best candidate for the newest point (the last one): max of (v_k - v_tau)^2 / (2 (k - tau)) over
// the earlier points with v_tau < v_k. Returns 0 when there is none.
static double hull_best(const cpt_hull *h, int64_t *best_tau, double *slope) {
	int64_t last = h->head + h->len - 1;
	double best = 0;
	*best_tau = h->tau[last];
	*slope = 0;
	for (int64_t j = h->head; j < last; j++) {
		double rise = h->v[last] - h->v[j];
		double run = (double)(h->tau[last] - h->tau[j]);
		double stat = rise > 0 ? rise * rise / (2 * run) : 0;
		if (stat > best) {
			best = stat;
			*best_tau = h->tau[j];
			*slope = rise / run;
		}
	}
	return best;
}

//===--------------------------------------------------------------------===//
// Detector
//===--------------------------------------------------------------------===//

static int finite(double v) {
	return v - v == 0;
}

static int positive(double v) {
	return v > 0 && finite(v);
}

static int check_params(double threshold, int64_t train, double mean, double sd, char *err) {
	if (!positive(threshold)) {
		snprintf(err, CPT_ERR_LEN, "threshold must be a finite number > 0, got %g", threshold);
		return 1;
	}
	if (train == 1 || train < 0) {
		snprintf(err, CPT_ERR_LEN, "train must be 0 (baseline given) or >= 2, got %lld", (long long)train);
		return 1;
	}
	if (train == 0 && !(finite(mean) && positive(sd))) {
		snprintf(err, CPT_ERR_LEN, "a given baseline needs a finite mean and a finite sd > 0");
		return 1;
	}
	return 0;
}

static int start_detection(cpt_online *d, char *err) {
	d->training = 0;
	d->t0 = d->n;
	d->k = 0;
	d->s = 0;
	d->statistic = 0;
	return hull_reset(&d->up, err) || hull_reset(&d->down, err);
}

static void start_training(cpt_online *d) {
	d->training = 1;
	d->n_train = 0;
	d->train_mean = 0;
	d->train_m2 = 0;
	d->statistic = 0;
	d->up.len = 0;
	d->down.len = 0;
}

static int restart(cpt_online *d, char *err) {
	if (d->train > 0) {
		start_training(d);
		return 0;
	}
	return start_detection(d, err);
}

int cpt_online_init(cpt_online *d, double threshold, int64_t train, double mean, double sd, char *err) {
	memset(d, 0, sizeof(*d));
	if (check_params(threshold, train, mean, sd, err)) {
		return 1;
	}
	d->threshold = threshold;
	d->train = train;
	d->mean = train > 0 ? 0 : mean;
	d->sd = train > 0 ? 1 : sd;
	if (restart(d, err)) {
		cpt_online_free(d);
		return 1;
	}
	return 0;
}

void cpt_online_free(cpt_online *d) {
	hull_free(&d->up);
	hull_free(&d->down);
}

// A standard deviation of 0 (a constant training window) is floored, so any later movement is a change.
static double sd_floor(double sd, double mean) {
	double floor_sd = 1e-12 * fmax(1.0, fabs(mean));
	return sd > floor_sd ? sd : floor_sd;
}

// Welford update; once train points are in, they become the baseline and detection starts.
static int train_step(cpt_online *d, double x, char *err) {
	d->n_train++;
	double delta = x - d->train_mean;
	d->train_mean += delta / (double)d->n_train;
	d->train_m2 += delta * (x - d->train_mean);
	if (d->n_train < d->train) {
		return 0;
	}
	d->mean = d->train_mean;
	d->sd = sd_floor(sqrt(d->train_m2 / (double)(d->train - 1)), d->mean);
	return start_detection(d, err);
}

static int alarms_push(cpt_online_alarms *alarms, cpt_online_alarm a, char *err) {
	if (alarms->count == alarms->cap) {
		int64_t cap = alarms->cap ? alarms->cap * 2 : 4;
		cpt_online_alarm *items =
		    (cpt_online_alarm *)cpt_realloc(alarms->items, sizeof(cpt_online_alarm) * (size_t)cap);
		if (!items) {
			snprintf(err, CPT_ERR_LEN, "out of memory recording an alarm");
			return 1;
		}
		alarms->items = items;
		alarms->cap = cap;
	}
	alarms->items[alarms->count++] = a;
	return 0;
}

void cpt_online_alarms_free(cpt_online_alarms *alarms) {
	free(alarms->items);
	*alarms = (cpt_online_alarms) {NULL, 0, 0};
}

// The alarm for the current point: the larger of the two signs decides change time and direction.
static cpt_online_alarm current_alarm(const cpt_online *d) {
	int64_t tau_up, tau_down;
	double slope_up, slope_down;
	double up = hull_best(&d->up, &tau_up, &slope_up);
	double down = hull_best(&d->down, &tau_down, &slope_down);
	int is_up = up >= down;
	return (cpt_online_alarm) {d->t0 + (is_up ? tau_up : tau_down), d->n,
	                           (is_up ? slope_up : -slope_down) * d->sd, is_up ? up : down};
}

static int detect_step(cpt_online *d, double x, cpt_budget *budget, cpt_online_alarms *alarms, char *err) {
	d->k++;
	d->s += (x - d->mean) / d->sd;
	if (hull_push(&d->up, d->k, d->s, err) || hull_push(&d->down, d->k, -d->s, err)) {
		return 1;
	}
	int rc = cpt_budget_charge(budget, d->up.len + d->down.len, err);
	if (rc) {
		return rc;
	}
	cpt_online_alarm a = current_alarm(d);
	d->statistic = a.statistic;
	if (d->statistic < d->threshold) {
		return 0;
	}
	return alarms_push(alarms, a, err) || restart(d, err);
}

int cpt_online_feed(cpt_online *d, const double *x, int64_t n, cpt_budget *budget, cpt_online_alarms *alarms,
                    char *err) {
	for (int64_t i = 0; i < n; i++) {
		d->n++;
		int rc = d->training ? train_step(d, x[i], err) : detect_step(d, x[i], budget, alarms, err);
		if (rc) {
			return rc;
		}
	}
	return 0;
}

//===--------------------------------------------------------------------===//
// Encoding: a fixed header followed by the two hulls, native little-endian
//===--------------------------------------------------------------------===//

#define CPT_ONLINE_MAGIC "CPTO"
#define CPT_ONLINE_VERSION 1
#define CPT_ONLINE_HEADER (8 + 15 * 8) // magic, version, 8 integers, 7 doubles

typedef struct {
	uint8_t *p;
	size_t left;
} cursor;

static void put(cursor *c, const void *v, size_t size) {
	if (size == 0) {
		return; // an empty hull has NULL arrays, and memcpy(dst, NULL, 0) is undefined
	}
	memcpy(c->p, v, size);
	c->p += size;
	c->left -= size;
}

static int take(cursor *c, void *v, size_t size) {
	if (c->left < size) {
		return 1;
	}
	if (size == 0) {
		return 0;
	}
	memcpy(v, c->p, size);
	c->p += size;
	c->left -= size;
	return 0;
}

static void put_hull(cursor *c, const cpt_hull *h) {
	if (h->len == 0) {
		return; // also avoids NULL + 0 for a hull that was never allocated
	}
	put(c, h->tau + h->head, sizeof(int64_t) * (size_t)h->len);
	put(c, h->v + h->head, sizeof(double) * (size_t)h->len);
}

int cpt_online_encode(const cpt_online *d, uint8_t **out, size_t *len, char *err) {
	*len = CPT_ONLINE_HEADER + 16 * (size_t)(d->up.len + d->down.len);
	*out = (uint8_t *)cpt_malloc(*len);
	if (!*out) {
		snprintf(err, CPT_ERR_LEN, "out of memory encoding the online detector");
		return 1;
	}
	cursor c = {*out, *len};
	uint32_t version = CPT_ONLINE_VERSION;
	int64_t ints[] = {d->train, d->training, d->n_train, d->n, d->t0, d->k, d->up.len, d->down.len};
	double reals[] = {d->threshold, d->mean, d->sd, d->train_mean, d->train_m2, d->s, d->statistic};
	put(&c, CPT_ONLINE_MAGIC, 4);
	put(&c, &version, 4);
	put(&c, ints, sizeof(ints) - 2 * sizeof(int64_t)); // the hull lengths go just before the hulls
	put(&c, reals, sizeof(reals));
	put(&c, &ints[6], 2 * sizeof(int64_t));
	put_hull(&c, &d->up);
	put_hull(&c, &d->down);
	return 0;
}

// A decoded hull must be a valid candidate set: times strictly increasing within [0, k], ending at k,
// values finite.
static int hull_valid(const cpt_hull *h, int64_t k) {
	for (int64_t i = 0; i < h->len; i++) {
		int64_t prev = i == 0 ? -1 : h->tau[i - 1];
		if (h->tau[i] <= prev || h->tau[i] > k || !finite(h->v[i])) {
			return 0;
		}
	}
	return h->len > 0 && h->tau[h->len - 1] == k;
}

static int take_hull(cursor *c, cpt_hull *h, int64_t len, char *err) {
	if (len < 0 || (uint64_t)len > c->left / 16) {
		snprintf(err, CPT_ERR_LEN, "invalid online detector state: bad candidate count");
		return 1;
	}
	size_t cap = (size_t)(len > 0 ? len : 1);
	h->tau = (int64_t *)cpt_malloc(sizeof(int64_t) * cap);
	h->v = h->tau ? (double *)cpt_malloc(sizeof(double) * cap) : NULL;
	if (!h->v) {
		snprintf(err, CPT_ERR_LEN, "out of memory decoding the online detector");
		return 1;
	}
	h->cap = (int64_t)cap;
	h->len = len;
	take(c, h->tau, sizeof(int64_t) * (size_t)len);
	take(c, h->v, sizeof(double) * (size_t)len);
	return 0;
}

// Positions stay far from overflow, and the detection phase lies within the points seen.
static int counts_valid(const cpt_online *d) {
	int64_t limit = INT64_MAX / 4;
	return d->n >= 0 && d->n < limit && d->t0 >= 0 && d->k >= 0 && d->t0 + d->k <= d->n && d->n_train >= 0;
}

static int values_valid(const cpt_online *d) {
	double values[] = {d->mean, d->sd, d->s, d->train_mean, d->train_m2, d->statistic};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		if (!finite(values[i])) {
			return 0;
		}
	}
	return d->sd > 0;
}

// Training: no candidates yet. Detection: both candidate sets valid for the current k.
static int phase_valid(const cpt_online *d) {
	if (d->training) {
		return d->n_train < d->train && d->up.len == 0 && d->down.len == 0;
	}
	return hull_valid(&d->up, d->k) && hull_valid(&d->down, d->k);
}

static int state_valid(const cpt_online *d) {
	char ignored[CPT_ERR_LEN];
	return check_params(d->threshold, d->train, d->mean, d->sd, ignored) == 0 && counts_valid(d) &&
	       values_valid(d) && phase_valid(d);
}

// Magic, version, then 6 integers, 7 doubles and the 2 hull lengths.
static int take_header(cursor *c, int64_t ints[8], double reals[7]) {
	char magic[4];
	uint32_t version;
	if (take(c, magic, 4) || take(c, &version, 4)) {
		return 1;
	}
	if (memcmp(magic, CPT_ONLINE_MAGIC, 4) != 0 || version != CPT_ONLINE_VERSION) {
		return 1;
	}
	return take(c, ints, 6 * sizeof(int64_t)) || take(c, reals, 7 * sizeof(double)) ||
	       take(c, &ints[6], 2 * sizeof(int64_t));
}

static int invalid_state(cpt_online *d, char *err) {
	cpt_online_free(d);
	snprintf(err, CPT_ERR_LEN, "invalid online detector state: create one with cpt_online_new()");
	return 1;
}

int cpt_online_decode(const uint8_t *buf, size_t len, cpt_online *d, char *err) {
	memset(d, 0, sizeof(*d));
	cursor c = {(uint8_t *)buf, len};
	int64_t ints[8];
	double reals[7];
	if (take_header(&c, ints, reals) || ints[1] < 0 || ints[1] > 1) {
		return invalid_state(d, err);
	}
	*d = (cpt_online) {.threshold = reals[0], .train = ints[0], .mean = reals[1], .sd = reals[2],
	                   .training = (int)ints[1], .n_train = ints[2], .train_mean = reals[3], .train_m2 = reals[4],
	                   .n = ints[3], .t0 = ints[4], .k = ints[5], .s = reals[5], .statistic = reals[6]};
	if (take_hull(&c, &d->up, ints[6], err) || take_hull(&c, &d->down, ints[7], err)) {
		cpt_online_free(d);
		return 1;
	}
	return (c.left != 0 || !state_valid(d)) ? invalid_state(d, err) : 0;
}
