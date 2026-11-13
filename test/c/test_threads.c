// Concurrency test for the numerical core, built with ThreadSanitizer by `make test_threads`.
//
// DuckDB runs cpt_* calls for different rows on different threads, all reading series from the same
// shared input vector. This test does the same with pthreads: one shared, read-only buffer of series;
// a single-threaded reference result for every series and algorithm; then 8 threads that each run
// every algorithm on every series (starting at different offsets so the same series is processed by
// several threads at once) and compare with the reference. TSan reports any data race, e.g. from
// shared mutable state added to a cost or algorithm later.

#define _POSIX_C_SOURCE 200809L

#include "algo.h"
#include "cost.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_SERIES 48
#define N_THREADS 8
#define N_ALGOS 6

typedef struct {
	const double *x;
	int64_t n;
} series;

static double *buffer;
static series all[N_SERIES];
static cpt_bkps reference[N_SERIES][N_ALGOS];

// The work a cpt_* call does on one series; algo selects function and cost.
static int run(int algo, const series *s, cpt_bkps *out, char *err) {
	static const cpt_cost_kind kinds[N_ALGOS] = {CPT_COST_L2,     CPT_COST_NORMAL, CPT_COST_L1,
	                                             CPT_COST_POISSON, CPT_COST_L2,     CPT_COST_L2};
	cpt_cost cost;
	if (cpt_cost_create(kinds[algo], s->x, s->n, &cost, err)) {
		return 1;
	}
	cpt_budget budget = {1000000000LL, 0, NULL, NULL};
	int rc = 0;
	switch (algo) {
	case 0:
	case 1:
	case 2:
	case 3:
		rc = cpt_pelt(&cost, s->n, cpt_penalty_named(CPT_PEN_BIC, s->n, cost.n_params), 2, &budget, out, err);
		break;
	case 4:
		rc = cpt_binseg(&cost, s->n, 3, 0, 2, &budget, out, err);
		break;
	case 5: {
		// CROPS: flatten to the concatenated segment ends of all entries
		cpt_crops_entry *entries;
		int64_t count;
		rc = cpt_crops(&cost, s->n, 1.0, 200.0, 2, &budget, &entries, &count, err);
		if (rc == 0) {
			int64_t total = 0;
			for (int64_t i = 0; i < count; i++) {
				total += entries[i].bkps.count;
			}
			out->pos = (int64_t *)malloc(sizeof(int64_t) * (size_t)(total > 0 ? total : 1));
			out->count = 0;
			for (int64_t i = 0; i < count; i++) {
				memcpy(out->pos + out->count, entries[i].bkps.pos, sizeof(int64_t) * (size_t)entries[i].bkps.count);
				out->count += entries[i].bkps.count;
			}
			cpt_crops_free(entries, count);
		}
		break;
	}
	}
	cpt_cost_destroy(&cost);
	return rc;
}

static int same(const cpt_bkps *a, const cpt_bkps *b) {
	return a->count == b->count && memcmp(a->pos, b->pos, sizeof(int64_t) * (size_t)a->count) == 0;
}

typedef struct {
	int id;
	int64_t checks;
	int64_t mismatches;
} worker;

static void *work(void *arg) {
	worker *w = (worker *)arg;
	char err[CPT_ERR_LEN];
	for (int round = 0; round < 3; round++) {
		for (int k = 0; k < N_SERIES; k++) {
			int i = (k + w->id * 5) % N_SERIES; // threads overlap on the same series
			for (int algo = 0; algo < N_ALGOS; algo++) {
				cpt_bkps b = {NULL, 0};
				if (run(algo, &all[i], &b, err) != 0 || !same(&b, &reference[i][algo])) {
					w->mismatches++;
				}
				cpt_bkps_free(&b);
				w->checks++;
			}
		}
	}
	return NULL;
}

int main(void) {
	// One shared buffer, like a DuckDB list child vector. Counts so 'poisson' accepts every series.
	int64_t total = 0;
	int64_t lengths[N_SERIES];
	for (int i = 0; i < N_SERIES; i++) {
		lengths[i] = 40 + (i * 37) % 260;
		total += lengths[i];
	}
	buffer = (double *)malloc(sizeof(double) * (size_t)total);
	int64_t off = 0;
	for (int i = 0; i < N_SERIES; i++) {
		all[i].x = buffer + off;
		all[i].n = lengths[i];
		for (int64_t j = 0; j < lengths[i]; j++) {
			double level = (j * 3 / lengths[i]) % 2 == 0 ? 4.0 : 9.0 + i % 3;
			buffer[off + j] = floor(level + 2.0 * (1.0 + sin((double)(j * 7 + i * 13) * 0.37)));
		}
		off += lengths[i];
	}

	char err[CPT_ERR_LEN];
	for (int i = 0; i < N_SERIES; i++) {
		for (int algo = 0; algo < N_ALGOS; algo++) {
			if (run(algo, &all[i], &reference[i][algo], err) != 0) {
				fprintf(stderr, "reference run failed: series %d algo %d: %s\n", i, algo, err);
				return 1;
			}
		}
	}

	pthread_t threads[N_THREADS];
	worker workers[N_THREADS];
	for (int t = 0; t < N_THREADS; t++) {
		workers[t] = (worker) {t, 0, 0};
		pthread_create(&threads[t], NULL, work, &workers[t]);
	}
	int64_t checks = 0, mismatches = 0;
	for (int t = 0; t < N_THREADS; t++) {
		pthread_join(threads[t], NULL);
		checks += workers[t].checks;
		mismatches += workers[t].mismatches;
	}

	for (int i = 0; i < N_SERIES; i++) {
		for (int algo = 0; algo < N_ALGOS; algo++) {
			cpt_bkps_free(&reference[i][algo]);
		}
	}
	free(buffer);
	printf("%d threads, %lld concurrent runs, %lld mismatches\n", N_THREADS, (long long)checks, (long long)mismatches);
	return mismatches ? 1 : 0;
}
