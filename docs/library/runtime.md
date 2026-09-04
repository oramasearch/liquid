# Coroutine runtime

**What.** libliquid's waiting and I/O operations return lazy `liquid::runtime::task<T>` values.
The public type is a Boost.Capy task whose value is `liquid::result<T>`. Corosio provides the
event loop and future file, socket, and timer implementations behind `src/runtime/net.hpp`.
Capy is a public link dependency; Corosio is private to libliquid.

**Why.** Lake and liquify already run coroutine event loops, while the command-line program and
tests begin in synchronous code. A coroutine-native library lets event-loop clients await work
without blocking and gives synchronous clients one explicit entry point, `runtime::block_on`.
Errors remain values at both entry points.

## Executor model

`context::executor()` exposes a Capy executor without exposing Corosio. The process-wide
`default_context()` is created on first use, keeps its Corosio `io_context` alive with a work
guard, and runs it on `max(2, hardware_concurrency / 2)` threads. It configures four blocking-I/O
workers. At process exit it stops the context, joins those threads, then destroys the context.

| Entry point | Intended use | Executor / ownership |
|---|---|---|
| `block_on` | CLI and tests | a new private context, run until its task completes |
| `spawn` | long-lived I/O work | the process-wide shared runtime context |
| `offload` | blocking CPU work | the compute thread pool; resumes on the caller executor |

`spawn` reports its `result<void>` to a completion callback. `offload` moves the blocking
callable into a coroutine frame and launches it on the compute pool. Completion posts a
runtime-side wake to the caller executor; only that executor sets the Capy async event that
resumes the caller.

Use `runtime::as_io` when a liquid task must participate in Capy's `when_all` or `timeout`.
The adapter preserves the liquid `result<T>` as the I/O payload; Capy's outer error code remains
available for cancellation and scheduling failures.

## Rules and pitfalls

- A task is lazy and `[[nodiscard]]`. Creating it does no work; await it or pass it to
  `block_on`, `spawn`, or a composition primitive.
- Never call `block_on` from inside a coroutine. It creates a nested context and blocks the
  coroutine's executor thread; await the task instead.
- Coroutine functions take parameters by value because execution begins after the call returns.
- Do not write a capturing-lambda coroutine. Use a named coroutine function whose frame owns its
  inputs. Ordinary non-coroutine callbacks and blocking callables may capture values.
- Use `offload` only for blocking work. After it completes, touch executor-affine Corosio objects
  only from the resumed caller coroutine.
- Do not create raw threads outside `src/runtime/runtime.cpp`.
- OS callback threads may enqueue work under a mutex and use
  `run_async(default_context().executor())` to schedule a runtime-side wake. They never call
  `async_event::set()` and never `co_await`.
- Corosio types stay under `src/runtime/`; public headers other than `liquid/runtime.hpp` do not
  name Boost.Capy.

## How to verify

Run the static and shared proof from the repository root:

```sh
git submodule update --init modules/capy modules/corosio
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
```

`tests/test_runtime.cpp` verifies value and error return, laziness, executor return after
offloading, completion, concurrent spawned tasks, the singleton shared context, a spawned store
round trip, and two liquid tasks composed with `when_all`. The install-consumer proof in
[`libliquid.md`](libliquid.md) verifies that an installed consumer receives Capy transitively.

## How to change

Public coroutine aliases and adapters belong in `include/liquid/runtime.hpp`. Context, pool, and
thread ownership belong in `src/runtime/runtime.cpp`. Add a private Corosio alias to
`src/runtime/net.hpp` only when an adapter starts using that type. A new waiting operation must
return `runtime::task<T>`, take frame-owned inputs by value, and have both its result and error
path exercised through `runtime::block_on`.
