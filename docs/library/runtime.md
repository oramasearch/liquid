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
| `yield` | let other work run between steps of a long loop | posts the continuation to the caller executor |

`spawn` reports its `result<void>` to a completion callback. `offload` moves the blocking
callable into a coroutine frame and launches it on the compute pool. Completion posts a
runtime-side wake to the caller executor; that wake queues the caller's continuation directly.
The blocking pool never resumes the caller and never touches an executor-affine Capy event.
The wake resumes the caller only after the pool thread has left its post into the caller's
executor. That post publishes the wake, then still unlocks Corosio's scheduler and wakes a runner;
a caller resumed before then could finish a `block_on` root, and `block_on` would destroy the
context the pool thread is still inside (liquid #350, SIGBUS on a pool thread in
`reactor_scheduler::post`). The pool thread marks the offload's state once its post has returned,
and the wake waits for that mark, which costs only the few instructions left in the post. A
poster that posts `detail::wake_offload` itself without recording itself in the state (lake's
blocking pool, which guards its server's context with its own lock) is not waited for.
`yield` is the way to give up the executor between steps of a long loop. It posts the caller's
continuation and touches nothing afterwards; a zero `delay` is not a substitute, because Capy's
`delay_awaitable::await_suspend` arms its timer before it registers its stop callback, and a
timer that fires in that window resumes the coroutine before the callback exists, leaving the
callback registered in memory the frame has already released (liquid #208).

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
- Keep large buffers out of coroutine bodies. A local array lives in the heap frame, but at -O0
  the coroutine ramp also reserves stack for it, so a 256 KiB table overflows the 512 KiB stack
  of a macOS secondary thread. Hold buffers of more than a few KiB in a `std::vector` (liquid #323).
- `block_on` and `default_context()` run one Corosio `io_context` on several runner threads, so a
  socket may be closed on one runner while another runner's reactor pass still holds its
  descriptor. The Corosio pin must include the descriptor-lifetime fix for
  [corosio#5](https://github.com/oramasearch/corosio/issues/5) (merged as `5505fd7`) and the
  outstanding-work fix for [corosio#7](https://github.com/oramasearch/corosio/issues/7) (merged as
  `1d56cba`); without the second, a descriptor registered before its scheduler was set drives the
  scheduler's work count negative, `io_context::run()` never returns, and `block_on` hangs. It must
  also include the connect-after-open fix for
  [corosio#10](https://github.com/oramasearch/corosio/issues/10) (merged as `0def6d3`):
  `tcp_socket::connect()` opens and registers the socket when the awaitable is made, Linux epoll
  reports that open, unconnected socket as `EPOLLOUT|EPOLLHUP`, and without the fix a connect that
  parked after the reactor recorded it failed with `EIO` ("Input/output error"). Every liquid HTTP
  client dials that way, so sync push and pull failed intermittently on Linux (liquid #345, lake#547).
  Do not move the pin to a Corosio that lacks any of the three fixes.
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

`tests/test_runtime.cpp` verifies value and error return, laziness, executor return across a series
of immediate offloads, completion, concurrent spawned tasks, the singleton shared context, a
spawned store round trip, and two liquid tasks composed with `when_all`. The immediate operations
exercise completion racing with caller suspension.
`Runtime.AnOffloadsCompletionPostReturnsBeforeBlockOnEnds` offloads through an executor that keeps
the pool thread inside its post until `block_on` returns (or 200 ms pass), and fails if `block_on`
returned while the pool thread was still there; it fails every time without the liquid #350 change.
`Runtime.AWakeWhosePosterDidNotRecordItselfDoesNotWait` posts the wake the way lake's blocking pool
does and requires the caller to resume. `Runtime.ManyContextsOffloadAndEndConcurrently` runs 1,200 `block_on` contexts from 8 threads, each
ending right after an offload completes. `tests/test_runtime_sockets.cpp` (POSIX) opens
and closes Unix-domain socket pairs from 256 concurrent lanes inside `block_on` for three one-second
rounds; each lane parks a read on one end, writes twice to the other and closes it, and the reader
destroys its socket on whichever runner resumed it. It crashes with `mutex lock failed` on the
Corosio pin before the multi-runner close fix, hangs intermittently on a pin with only that fix
(the corosio#7 work-count underflow), and passes with both (liquid #329).
`RuntimeSockets.TcpConnectsStartedAfterTheOpenSucceedInsideBlockOn` starts 512 loopback TCP
connects from 32 lanes, each 0 to 3 yields after its socket was opened, and fails on Linux on a
pin without the corosio#10 fix (liquid #345). The install-consumer proof in
[`libliquid.md`](libliquid.md) verifies that an installed consumer receives Capy transitively.

## How to change

Public coroutine aliases and adapters belong in `include/liquid/runtime.hpp`. Context, pool, and
thread ownership belong in `src/runtime/runtime.cpp`. Add a private Corosio alias to
`src/runtime/net.hpp` only when an adapter starts using that type. A new waiting operation must
return `runtime::task<T>`, take frame-owned inputs by value, and have both its result and error
path exercised through `runtime::block_on`.
