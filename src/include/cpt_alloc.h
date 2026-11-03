#pragma once

// Allocation in src/core goes through these wrappers. In normal builds they are plain libc calls.
// Test builds define CPT_FAULT_INJECTION: the n-th allocation after cpt_fail_countdown = n then fails,
// so test/c/test_core.c can exercise every out-of-memory path and check (under ASan/LSan and macOS
// `leaks`) that each one reports an error and frees what it had allocated. The countdown is the only
// global mutable state, and it exists only in those single-threaded test builds.

#include <stddef.h>
#include <stdlib.h>

#ifdef CPT_FAULT_INJECTION
extern long cpt_fail_countdown;
static inline int cpt_alloc_fails(void) {
	return cpt_fail_countdown > 0 && --cpt_fail_countdown == 0;
}
#else
static inline int cpt_alloc_fails(void) {
	return 0;
}
#endif

static inline void *cpt_malloc(size_t size) {
	return cpt_alloc_fails() ? NULL : malloc(size);
}

static inline void *cpt_calloc(size_t count, size_t size) {
	return cpt_alloc_fails() ? NULL : calloc(count, size);
}

static inline void *cpt_realloc(void *ptr, size_t size) {
	return cpt_alloc_fails() ? NULL : realloc(ptr, size);
}
