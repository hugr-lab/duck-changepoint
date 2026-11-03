# Proposal: let C API extensions check whether their query was interrupted

Status: draft, revision 2, for an issue (then a PR) at duckdb/duckdb. Context: hugr-lab/duck-changepoint#2.
Source references are pinned to duckdb/duckdb@a5e99a4 (main, 2026-11-03).

Revision 2 follows VGSML's review on #2. Instead of a check tied to scalar functions
(`duckdb_scalar_function_is_interrupted(duckdb_function_info)`, revision 1), it proposes one check on
the client context. That check works for every kind of callback that can reach a context, and the
remaining callbacks only need a way to get one.

## Summary

Add one function to the C API:

```c
// Returns true if the query running in this client context has been interrupted (for example by
// duckdb_interrupt) or has passed its deadline. Long-running callbacks can poll it and return early.
DUCKDB_C_API bool duckdb_client_context_is_interrupted(duckdb_client_context context);
```

Scalar functions can use it right away, with no other change: they already get the executing query's
context in their init callback. Aggregate and cast callbacks cannot reach a context today. Giving them
one is the natural follow-up.

## Motivation

A C API scalar function is called once per data chunk of up to 2,048 rows, and nothing interrupts it
until it returns. DuckDB checks for interrupts between operators, so a query can only be cancelled
once the whole chunk is done. That is fine for cheap functions, but some do a lot of work per row:

- **Change point detection** (the cpt extension): one row is a whole time series in a `LIST`. PELT on
  200,000 points with a single change takes about 25 s. A query over 32 such series, each within a
  per-series limit, took 18.8 s, and `duckdb_interrupt` after 1 s returned only at the end. In the worst
  case cancelling waits for 2,048 rows' worth of work.
- The same applies to anything compute-heavy per row: model inference, geometry on large shapes,
  solvers, simulations, cryptographic work. It also applies to aggregates whose finalize does the
  heavy work, and to COPY functions.

Without an interrupt check, extensions can only cap their own work, as cpt does with
`cpt_max_evaluations`. A cap applies per row, so it cannot bound the time to cancel a chunk. Capping per
chunk instead would make whether a query succeeds depend on how DuckDB splits rows into chunks.

A C++ extension can already do this through `ExpressionState::GetContext().InterruptCheck()`. The
proposal gives the same ability to C API extensions, whose binaries load across DuckDB versions.

## What exists today

- `ClientContext::IsInterrupted()` (`src/main/client_context.cpp:1144`): a relaxed atomic load. It
  respects `INTERRUPTS_SUPPRESSED` and follows `base_context` for internal connections.
- `ClientContext::InterruptCheck()` (`src/main/client_context.cpp:1161`): also checks the query
  deadline, reading the clock only every 256th call, and throws `InterruptException`.
- A `duckdb_client_context` is a `CClientContextWrapper` holding a `ClientContext &`
  (`src/include/duckdb/main/capi/capi_internal.hpp:43`). Callbacks can get one at these points:

| Callback | Client context available | Getter |
|---|---|---|
| Scalar function bind | yes | `duckdb_scalar_function_get_client_context(duckdb_bind_info, ...)` |
| Scalar function init (per thread, at execution start) | yes | `duckdb_scalar_function_init_get_client_context(duckdb_init_info, ...)` |
| Table function bind | yes | `duckdb_table_function_get_client_context(duckdb_bind_info, ...)` |
| Table function init / local init / execute | no | |
| COPY function bind, global init, sink, finalize | yes | `duckdb_copy_function_*_get_client_context` |
| Aggregate update / combine / finalize | no | |
| Cast | no | |
| Connection (client side) | yes | `duckdb_connection_get_client_context` |

- After a function returns, DuckDB checks for interrupts before the next operator runs
  (`PipelineExecutor::StartOperator`, `src/parallel/pipeline_executor.cpp:903`) and in its execute
  loops (lines 336, 635, 760). So a function that returns early is followed by the usual interrupt
  error before its output is used.
- `duckdb.h` has `duckdb_interrupt(duckdb_connection)` to *send* an interrupt, and nothing to
  *observe* one.

## Proposed change

### API (`api_spec/v1/connection/connection.yaml`, next to `client_context_get_connection_id`)

```yaml
  client_context_is_interrupted:
    offset: 546   # next free offset at a5e99a4
    lifecycle:
      - ["unstable", "<next release>", "<date>"]
    parameters:
      context:
        type: client_context
        description: The client context.
    return_type: bool
    description: |
      Returns whether the query running in this client context has been interrupted (for example by
      `duckdb_interrupt`) or has passed its deadline. Long-running callbacks can poll this and return
      early: DuckDB then raises the usual interrupt error before their output is used.

      The check is cheap (an atomic load; the clock is read only occasionally), but it is meant for
      polling between units of work, not for every row of a tight loop.
    return_description: true if the query should stop, false otherwise.
```

### Implementation (`src/main/capi/v1/...`, wherever the other client context functions live)

```cpp
bool duckdb_client_context_is_interrupted(duckdb_client_context context) {
	if (!context) {
		return false;
	}
	auto &wrapper = *reinterpret_cast<duckdb::CClientContextWrapper *>(context);
	try {
		wrapper.context.InterruptCheck(); // interrupt flag, and the deadline every 256th call
		return false;
	} catch (duckdb::InterruptException &) {
		return true;
	}
}
```

Catching the exception keeps the semantics of `InterruptCheck` (interrupts and deadlines, including
the throttled clock read) without a second code path. A non-throwing `ClientContext::ShouldInterrupt()`
would avoid the try/catch, if preferred.

### Use from a scalar function, with the existing API

The context from the init callback is the executing query's, and the function keeps it in its local
state for the duration of the query:

```c
static void destroy_context(void *state) {
	duckdb_client_context context = (duckdb_client_context)state;
	duckdb_destroy_client_context(&context);
}

static void Init(duckdb_init_info info) {
	duckdb_client_context context;
	duckdb_scalar_function_init_get_client_context(info, &context);
	duckdb_scalar_function_init_set_state(info, context, destroy_context);
}

static void Execute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	duckdb_client_context context = (duckdb_client_context)duckdb_scalar_function_get_state(info);
	for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); row++) {
		while (work_left(row)) {
			if (duckdb_client_context_is_interrupted(context)) {
				return; // DuckDB raises the interrupt before this output is used
			}
			do_some_work(row);
		}
	}
}
```

A context obtained at bind time should not be kept in the bind data for use at execution. Bind data
can outlive one execution, for example in a prepared statement, and the wrapper holds a reference.

### Follow-ups: contexts for callbacks that have none

These are separate, smaller additions:

- **Aggregates:** `duckdb_aggregate_function_get_client_context(duckdb_function_info, duckdb_client_context *)`
  for update, combine and finalize. An aggregate whose finalize does the heavy work, such as change
  point detection over a group, has the same problem as a scalar function.
- **Table functions:** the client context in init and local init, so execute can keep it. Less urgent:
  DuckDB already checks between the output chunks of a table function.
- **Casts:** the client context in the cast function info.

### Tests (`test/api/capi/`)

1. A scalar function with an init that stores the context, looping until
   `duckdb_client_context_is_interrupted` returns true. Run it on one row, and call `duckdb_interrupt`
   from another thread after 100 ms. The query must fail with an interrupt error well before any
   natural end, and must not return the function's partial output.
2. The same with a query timeout and no `duckdb_interrupt`: the deadline must stop it.
3. A function that polls but is never interrupted returns normal results.
4. Interrupts suppressed after `COMMIT`: `is_interrupted` stays false.
5. A NULL context returns false.

## Compatibility

- Adding a function at a new offset in the extension API struct is backwards compatible.
- An extension can call it only when it targets a DuckDB version that has it. Extensions that must load
  in older versions keep their own work limits and use the check where it is available. cpt would
  poll it from the place where it already charges its work budget, once per step of each algorithm.

## Alternatives considered

- **`duckdb_scalar_function_is_interrupted(duckdb_function_info)`** (revision 1). It needs the wrapper
  to pass the context into the function info, and every other kind of callback would need its own
  variant. The context-based check covers all of them, and for scalar functions it needs no new getter.
- **Per-chunk work limits in the extension.** The outcome would depend on how DuckDB splits rows into
  chunks.
- **Smaller chunks for expensive functions.** The vector size is a compile-time constant.
- **Table functions instead of scalar functions.** DuckDB checks between their output chunks, but one
  long computation still runs inside one call, and a table function over a grouped column is awkward
  in SQL.

## Open questions for the DuckDB maintainers

1. Should the clock be read on every call, as `BlockingInterruptCheck` does, since a caller polling
   this is usually doing long work between polls?
2. Should the function start in the unstable C API and become stable a release later, as other recent
   additions did?
3. Is the init-callback pattern above the intended way for a scalar function to keep its context for
   the duration of a query, or would you prefer a getter on `duckdb_function_info`?
