# Proposal: let C API scalar functions see that their query was interrupted

Status: draft, for an issue (then a PR) at duckdb/duckdb. Context: hugr-lab/duck-changepoint#2.
Source references are pinned to duckdb/duckdb@a5e99a4 (main, 2026-11-03).

## Summary

Add one function to the C API:

```c
// Returns true if the query that is executing this scalar function has been interrupted or has passed
// its deadline. A function that does a lot of work per call can poll it and return early.
DUCKDB_C_API bool duckdb_scalar_function_is_interrupted(duckdb_function_info info);
```

When a function sees `true`, it returns without finishing its output. The wrapper then throws the
same `InterruptException` as everywhere else, so the user gets the usual "Interrupted!" error and the
partial output is never used.

## Motivation

A C API scalar function is called once per data chunk of up to 2,048 rows, and nothing interrupts it
until it returns. DuckDB checks for interrupts between chunks, so a query can only be cancelled once the
whole chunk is done. That is fine for cheap functions, but some do a lot of work per row:

- **Change point detection** (the cpt extension): one row is a whole time series in a `LIST`. PELT on
  200,000 points with a single change takes about 25 s. A query over 32 such series, each well
  within a per-series limit, took 18.8 s, and `duckdb_interrupt` after 1 s returned only at the end.
  In the worst case cancelling waits for 2,048 rows' worth of work.
- The same applies to anything compute-heavy that maps one row to one value: model inference on a
  row's features, geometry operations on large shapes, solvers, simulations, cryptographic work.

Without an interrupt check, extensions can only cap their own work, as cpt does with a
`cpt_max_evaluations` setting. A cap applies per row, so it cannot bound the time to cancel a whole
chunk. Capping per chunk instead would make whether a query succeeds depend on how DuckDB splits
rows into chunks.

A C++ extension can already do this through `ExpressionState::GetContext().InterruptCheck()`. The
proposal gives the same ability to C API extensions, whose binaries load across DuckDB versions.

## What exists today

- `ClientContext::IsInterrupted()` (`src/main/client_context.cpp:1144`): a relaxed atomic load. It
  respects `INTERRUPTS_SUPPRESSED` and follows `base_context` for internal connections.
- `ClientContext::InterruptCheck()` (`src/main/client_context.cpp:1161`): also checks the query
  deadline, reading the clock only every 256th call, and throws `InterruptException`.
- `CAPIScalarFunction` (`src/main/capi/v1/scalar_function-c.cpp:198`) receives the `ExpressionState`
  and could therefore reach the `ClientContext` through `state.GetContext()`. But the
  `CScalarFunctionInternalFunctionInfo` it passes to the callback (line 105) holds only the bind data
  and the local state.
- `duckdb.h` has `duckdb_interrupt(duckdb_connection)` to *send* an interrupt from the client side,
  and nothing to *observe* one inside a function.

## Proposed change

### API (`api_spec/v1/scalar_function/scalar_function.yaml`)

```yaml
  scalar_function_is_interrupted:
    offset: 546   # next free offset at a5e99a4
    lifecycle:
      - ["unstable", "<next release>", "<date>"]
    parameters:
      info:
        type: function_info
        description: The function info object.
    return_type: bool
    description: |
      Returns whether the query executing this scalar function has been interrupted (for example by
      `duckdb_interrupt`) or has passed its deadline. A function that does a lot of work per call can
      poll this and return early. After it returns, DuckDB raises the usual interrupt error and
      discards the function's output.

      The check is cheap (an atomic load; the clock is read only occasionally), but it is meant for
      polling between units of work, not for every row of a tight loop.
    return_description: true if the function should stop, false otherwise.
```

### Implementation (`src/main/capi/v1/scalar_function-c.cpp`)

```cpp
struct CScalarFunctionInternalFunctionInfo {
	explicit CScalarFunctionInternalFunctionInfo(const CScalarFunctionBindData &bind_data,
	                                             CScalarFunctionLocalState &local_state, ClientContext &context)
	    : bind_data(bind_data), local_state(local_state), context(context), success(true) {};

	const CScalarFunctionBindData &bind_data;
	CScalarFunctionLocalState &local_state;
	ClientContext &context;
	bool interrupted = false;

	bool success;
	string error = "";
};

void CAPIScalarFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	...
	CScalarFunctionInternalFunctionInfo function_info(c_bind_info, c_local_state, state.GetContext());
	auto c_function_info = ToCScalarFunctionInfo(function_info);
	c_bind_info.info.function(c_function_info, c_input, c_result);
	if (function_info.interrupted) {
		throw InterruptException(); // the function stopped early; its output is incomplete
	}
	if (!function_info.success) {
		throw InvalidInputException(function_info.error);
	}
}

bool duckdb_scalar_function_is_interrupted(duckdb_function_info info) {
	if (!info) {
		return false;
	}
	auto &function_info = duckdb::GetCScalarFunctionInfo(info);
	try {
		function_info.context.InterruptCheck(); // interrupt flag, and the deadline every 256th call
	} catch (duckdb::InterruptException &) {
		function_info.interrupted = true;
	}
	return function_info.interrupted;
}
```

Catching the exception inside the C function keeps the semantics of `InterruptCheck` (interrupts and
deadlines, including the throttled clock read) without adding a second code path. A non-throwing
`ClientContext::ShouldInterrupt()` would avoid the try/catch, if that is preferred.

The wrapper throws only if the function actually observed the interrupt. A function that never
polls, or that finishes its chunk, behaves exactly as before.

### Tests (`test/api/capi/v1/capi_scalar_functions.cpp`)

1. A scalar function that loops until `duckdb_scalar_function_is_interrupted` returns true (sleeping
   briefly in between). Run it on one row and call `duckdb_interrupt` from another thread after
   100 ms. The query must fail with an interrupt error, well before any natural end.
2. The same function with a query timeout set and no `duckdb_interrupt`: the deadline must stop it.
3. A function that polls but is never interrupted returns normal results, and its query succeeds.
4. Interrupts suppressed after `COMMIT`: `is_interrupted` stays false.

## Compatibility

- Adding a function at a new offset in the extension API struct is backwards compatible. Extensions
  that do not use it are unaffected.
- An extension can call it only when it targets a DuckDB version that has it. Extensions that must
  load in older versions keep their own work limits and use the new function where it is available.
  cpt would poll it from the place where it already charges its work budget, once per step of each
  algorithm.

## Alternatives considered

- **Per-chunk work limits in the extension.** The outcome would then depend on how DuckDB splits rows
  into chunks. Rejected for that reason.
- **Smaller chunks for expensive functions.** The vector size is a compile-time constant, not a
  per-function property.
- **Table functions instead of scalar functions.** DuckDB checks for interrupts between their output
  chunks, but one long computation still runs inside one call, and using a table function over a
  grouped column is awkward in SQL.
- **A general `duckdb_function_info_is_interrupted`.** `duckdb_function_info` is shared by scalar,
  aggregate and table function callbacks, but each wraps a different internal struct. Separate
  functions per kind are clearer. Aggregates (`duckdb_aggregate_function_is_interrupted`) would be a
  natural follow-up, since an aggregate's update or finalize can also do a lot of work per call.

## Open questions for the DuckDB maintainers

1. Is `InterruptException` the right error after an early return? It matches what the user sees
   when a C++ function calls `InterruptCheck()`.
2. Should the clock be read on every call, like `BlockingInterruptCheck`, since a function polling this
   is usually doing long work between polls?
3. Should the new function start in the unstable C API and become stable a release later, as other
   recent additions did?
