# Architecture

```
src/
├── cpt_extension.c          entrypoint (DUCKDB_EXTENSION_ENTRYPOINT) + overload helper
├── core/                    numerical core: plain C11, no DuckDB headers
│   ├── cost.c               cost interface, factory, l2 / normal / poisson
│   ├── cost_l1.c            l1 via a wavelet matrix
│   ├── algo.c               PELT, Binseg, Dynp, CROPS, penalties
│   └── online.c             online detector (FOCuS0): hull of candidates, state encoding
├── functions/               SQL surface: one file per SQL function
│   ├── list_io.c            reading LIST/VARCHAR/optional args, writing LIST(INTEGER)
│   ├── cpt_call.c           the shared row loop: prepare args and cost, run, write, report errors
│   ├── cpt_pelt.c  cpt_binseg.c  cpt_dynp.c  cpt_crops.c  cpt_segment_stats.c
│   ├── cpt_online.c         cpt_online_new / cpt_online_update / cpt_online
└── include/                 cost.h, algo.h, list_io.h, cpt_functions.h, cpt_call.h,
                             cpt_alloc.h (allocation wrappers), cpt_text.h
duckdb_capi/                 duckdb.h + duckdb_extension.h from DuckDB v1.5.6 (TARGET_DUCKDB_VERSION)
extension-ci-tools/          submodule: Makefiles, metadata footer script, CI workflows
```

The layers depend in one direction only: `functions/` → `core/`. `core/` can be compiled and
tested without DuckDB (`make test_core`).

## The cost interface

```c
typedef struct cpt_cost {
    const char *name;
    void *ctx;
    int (*init)(void *ctx, const double *x, int64_t n, char *err);
    double (*eval)(void *ctx, int64_t a, int64_t b);   // segment [a, b)
    int n_params;                                      // for BIC / MBIC / AIC
    double K;                                          // Theorem 3.1 pruning constant
    int64_t min_len;                                   // 2 for 'normal'
    int64_t eval_weight;                               // work-limit units per eval: 1 l2/poisson, 2 normal, log2 n l1
    void (*free)(void *ctx);
} cpt_cost;
```

To add a cost:

1. Write `alloc`/`init`/`eval`/`free` (in `cost.c`, or in a new file for anything sizeable), allocating
   with `cpt_malloc` and friends.
2. Add a `cpt_cost_kind` and a row to `COST_SPECS` in `cost.c`: name, functions, `n_params`, `min_len`
   and `eval_weight`, the cost of one `eval` relative to an O(1) evaluation, which scales
   `cpt_max_evaluations`. State in a comment why K = 0 (or another K) is valid.
4. Add a naive evaluation to `test/c/test_core.c` and to `test/python/check_optimality.py`.

## DuckDB C API usage

- **Stable API only.** `USE_UNSTABLE_C_API=0`, and the headers in `duckdb_capi/` come from v1.5.6. The
  metadata footer says `C_STRUCT` with minimum version `v1.5.6`, and DuckDB hands the extension the
  v1.5.6 function-pointer struct. Any DuckDB that supports that struct version can load it.
- **Overloads instead of defaults.** Each SQL function is a `duckdb_scalar_function_set` with one
  overload per trailing-argument count. All overloads share a callback, which checks
  `duckdb_data_chunk_get_column_count`. `cpt_pelt`'s third argument is either `DOUBLE` or `VARCHAR`;
  the callback checks the vector's logical type. A literal `NULL` in that position resolves without
  ambiguity on 1.5.6 and 2.0.
- **NULL handling.** Every overload sets `duckdb_scalar_function_set_special_handling`, so NULL
  arguments reach the callback, where a NULL optional argument means "default".
- **Flat input.** The C API wrapper calls `DataChunk::Flatten()` before invoking a scalar function, so
  constant and dictionary vectors arrive flat. The tests cover a constant list argument over 5000 rows.
- **Zero-copy input.** A series is used in place: `child_data + entry.offset`. Only the cost's prefix
  arrays are allocated.
- **List output.** Each row writes into the child at a running offset:
  `duckdb_list_vector_reserve(out, offset + k)` (re-fetching the child data pointer, which can move),
  write, set the `duckdb_list_entry`, then `duckdb_list_vector_set_size`. `cpt_crops` nests this once
  more for the `bkps` list inside the struct.
- **Errors.** `duckdb_scalar_function_set_error` stops the query, with messages prefixed by the
  function or argument name.
- **Threads.** There is no global state; each call allocates and frees its own workspace.
- **No interrupt check.** A scalar function cannot see that its query was interrupted. Every overload
  therefore has a bind callback that reads the `cpt_max_evaluations` setting (registered with
  `duckdb_register_config_option`) into bind data. The algorithms count weighted cost evaluations in a
  `cpt_budget`, check it once per outer step, and return `CPT_BUDGET_EXCEEDED` when it runs out.
  The limit is per series. DuckDB checks for interrupts only between calls, and one call processes a
  whole vector (up to 2,048 series), so cancelling a query can still take up to 2,048 × the limit (#2).
