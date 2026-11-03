#pragma once

// Online change detection in the mean of a stream: FOCuS0 (Romano, Eckley, Fearnhead & Rigaill, JMLR
// 2023), the exact Page-CUSUM statistic maximised over every change size and every change time,
// in O(log n) expected time per point.
//
// The data is standardised with a baseline mean and standard deviation, either given or estimated
// from the first `train` points. With S_k the sum of the first k standardised points since the
// detector (re)started, the statistic at time k is
//
//   T_k = max over tau < k of  (S_k - S_tau)^2 / (2 (k - tau))   for both signs of S_k - S_tau,
//
// the log likelihood ratio of "a change in mean after tau" for the best tau and size. An alarm is
// raised when T_k >= threshold.
//
// Functional pruning: the cost of a candidate tau as a function of the change size mu,
// mu (S_k - S_tau) - (k - tau) mu^2 / 2, changes by the same amount for every tau as k grows. So
// whether one candidate dominates others never changes, and the candidates that can still be optimal
// for an upward change are the vertices of the lower convex hull of the points (tau, S_tau) to the right
// of its minimum (for downward changes, the same with -S). About log k of them remain.

#include <stddef.h>
#include <stdint.h>

#include "algo.h" // cpt_budget, CPT_ERR_LEN

// Candidates of one sign: points (tau, v) of a lower convex hull, kept as a deque.
typedef struct {
	int64_t *tau;
	double *v;
	int64_t head; // index of the first live point
	int64_t len;  // number of live points
	int64_t cap;
} cpt_hull;

typedef struct {
	// parameters
	double threshold;
	int64_t train; // > 0: estimate the baseline from this many points, also after every alarm
	// baseline
	double mean;
	double sd;
	int training;       // 1 while the baseline is being estimated
	int64_t n_train;    // points seen in the current training phase
	double train_mean;  // Welford running mean ...
	double train_m2;    // ... and sum of squared deviations
	// detection
	int64_t n;         // points seen since the detector was created
	int64_t t0;        // points seen before the current detection phase started
	int64_t k;         // points in the current detection phase
	double s;          // sum of the standardised points in the current detection phase
	double statistic;  // T_k after the last point (0 while training)
	cpt_hull up;       // candidates for an increase: hull of (tau, S_tau)
	cpt_hull down;     // candidates for a decrease: hull of (tau, -S_tau)
} cpt_online;

typedef struct {
	int64_t change_at;   // 1-based position of the last point before the change
	int64_t detected_at; // 1-based position of the point that raised the alarm
	double shift;        // estimated change in mean, in the units of the data
	double statistic;    // T_k at the alarm
} cpt_online_alarm;

typedef struct {
	cpt_online_alarm *items;
	int64_t count, cap;
} cpt_online_alarms;

// A new detector. With train > 0 (>= 2) the baseline is estimated from the first train points and
// again after every alarm; with train == 0 the given mean and sd are the baseline throughout.
int cpt_online_init(cpt_online *d, double threshold, int64_t train, double mean, double sd, char *err);
void cpt_online_free(cpt_online *d);

// Feed n points; alarms raised on the way are appended to *alarms. Each point charges the number of
// candidates it examines to budget (NULL: no limit), which bounds the run time on long drifts.
int cpt_online_feed(cpt_online *d, const double *x, int64_t n, cpt_budget *budget, cpt_online_alarms *alarms,
                    char *err);
void cpt_online_alarms_free(cpt_online_alarms *alarms);

// The detector as bytes, and back. decode validates everything: the input may come from anywhere.
int cpt_online_encode(const cpt_online *d, uint8_t **out, size_t *len, char *err);
int cpt_online_decode(const uint8_t *buf, size_t len, cpt_online *d, char *err);
