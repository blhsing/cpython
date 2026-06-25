I tried a different implementation strategy that avoids keeping the eval breaker hot for the whole duration of an active timeout block. I am calling it the deadline-scheduler approach below, because the main difference is that a scheduler thread watches deadlines and only notifies the eval loop once a deadline has actually expired.

The current PR/prototype sets a timeout bit while inside `with timeout(...)`, so the eval loop enters pending-handling frequently while the timeout is active. The actual clock read can be amortized with a skip counter, but `_Py_HandlePending()` is still reached repeatedly. That appears to be the source of the active-timeout overhead discussed above.

The deadline-scheduler prototype keeps the eval loop on its normal fast path until the timeout actually expires:

* `with timeout(seconds)` pushes a deadline block onto the current `PyThreadState`.
* A runtime scheduler thread tracks active deadline blocks.
* While the timeout has not expired, no timeout eval-breaker bit is set.
* When a deadline expires, the scheduler requests cancellation on the target thread state with the timeout reason.
* The target thread raises `TimeoutError` at the next normal eval-breaker check.

So pure Python code is still interrupted cooperatively at bytecode/eval-breaker boundaries, but an active, non-expired timeout does not force pending handling on every bytecode/check point.

Prototype branch: https://github.com/blhsing/cpython/tree/feature/cooperative-timeout

CI passed here: https://github.com/blhsing/cpython/actions/runs/28073335858

Benchmark environment:

* Linux 6.14, x86_64
* Intel Xeon Silver 4410Y
* GCC 13.3.0, `-O3`
* CPython 3.16.0a0
* `pyperf`
* `timeout_seconds=3600.0`, so the timeout never expires during the benchmark
* `inner_loops=1000`

Mean ± std dev:

| Benchmark | Baseline | OP approach | Deadline-scheduler approach |
| --- | ---: | ---: | ---: |
| `pass_loop` | 22.9 us ± 0.1 us | 33.3 us ± 0.2 us, 1.45x slower | 22.2 us ± 0.1 us, not meaningfully different |
| `arithmetic_loop` | 28.5 us ± 0.2 us | 37.8 us ± 0.2 us, 1.33x slower | 25.5 us ± 0.2 us, not meaningfully different |
| `listcomp_work` | 35.5 us ± 0.7 us | 46.5 us ± 0.4 us, 1.31x slower | 35.8 us ± 0.7 us, not significant |
| empty timeout context enter/exit | 342 ns ± 2 ns, `nullcontext` baseline | 1.06 us ± 0.01 us | 1.73 us ± 0.05 us |

The small "faster than baseline" results for the deadline-scheduler approach are just build/run noise, not a claimed speedup. The same-binary no-timeout control for the deadline-scheduler build measured `pass_loop` at 22.1 us ± 0.1 us, `arithmetic_loop` at 25.4 us ± 0.1 us, and `listcomp_work` at 35.9 us ± 0.7 us, so the active timeout overhead is effectively lost in noise for these workloads.

The tradeoff is visible in the empty enter/exit benchmark: the scheduler-based approach pays more to register and unregister a timeout block. For very tiny timeout scopes, the OP approach is cheaper. For timeout scopes around non-trivial Python work, avoiding repeated pending handling while the timeout is active seems to dominate.

For `_sre`, I made the engine cooperative by reusing its existing periodic signal-check hook. `_sre` already has a `MAYBE_CHECK_SIGNALS` path that runs every 4096 iterations of the matching engine and calls `PyErr_CheckSignals()`. The prototype adds:

```c
if (_PyThreadState_CheckCancellation(_PyThreadState_GET())) {
    RETURN_ERROR(SRE_ERROR_INTERRUPTED);
}
```

at the same point. `_PyThreadState_CheckCancellation()` drains the cooperative cancellation request bit, checks whether an expired timeout is the reason, and sets `TimeoutError` when the current thread has an expired timeout block. If no cancellation is pending and the active timeout has not expired, it returns immediately. If it has expired, `_sre` returns through its existing interrupted-error path, and the Python-level regex call propagates the timeout exception.

That means catastrophic backtracking can be interrupted without adding a public `re`-specific timeout parameter and without adding a separate polling mechanism to the regex engine. It also keeps the polling cadence tied to `_sre`'s existing signal-check cadence instead of checking on every regex VM transition.

This still has the usual async-exception caveat: a timeout in pure Python is delivered as a normal Python exception at an eval-breaker boundary, so `finally` blocks and context-manager exits run, but arbitrary Python code can still be interrupted between bytecodes just like with `KeyboardInterrupt`.

<details>
<summary>Benchmark script</summary>

```python
#!/usr/bin/env python3
"""Compare timeout prototype overhead against a clean baseline."""

from __future__ import annotations

from contextlib import nullcontext
import pyperf


_sink = 0
_timeout_warmed = False


def _consume(value: int) -> None:
    global _sink
    _sink ^= value


def _has_timeout(implementation: str) -> bool:
    return implementation != "baseline"


def _get_timeout(seconds: float):
    if seconds <= 0:
        raise ValueError("timeout must be positive for the overhead benchmark")
    from contextlib import timeout
    return timeout(seconds)


def _warm_timeout(implementation: str, timeout_seconds: float) -> None:
    global _timeout_warmed
    if _has_timeout(implementation) and not _timeout_warmed:
        with _get_timeout(timeout_seconds):
            pass
        _timeout_warmed = True


def _pass_loop(inner_loops: int) -> int:
    total = 0
    for value in range(inner_loops):
        total ^= value
    return total


def _arithmetic_loop(inner_loops: int) -> int:
    total = 0
    for value in range(inner_loops):
        total += value
    return total


def _listcomp_work(inner_loops: int) -> int:
    values = [value * value for value in range(inner_loops)]
    return values[-1] if values else 0


def bench_pass_loop(
    loops: int,
    implementation: str,
    inner_loops: int,
    timeout_seconds: float,
) -> float:
    _warm_timeout(implementation, timeout_seconds)
    total = 0
    t0 = pyperf.perf_counter()
    if _has_timeout(implementation):
        with _get_timeout(timeout_seconds):
            for _ in range(loops):
                total ^= _pass_loop(inner_loops)
    else:
        for _ in range(loops):
            total ^= _pass_loop(inner_loops)
    dt = pyperf.perf_counter() - t0
    _consume(total)
    return dt


def bench_arithmetic_loop(
    loops: int,
    implementation: str,
    inner_loops: int,
    timeout_seconds: float,
) -> float:
    _warm_timeout(implementation, timeout_seconds)
    total = 0
    t0 = pyperf.perf_counter()
    if _has_timeout(implementation):
        with _get_timeout(timeout_seconds):
            for _ in range(loops):
                total += _arithmetic_loop(inner_loops)
    else:
        for _ in range(loops):
            total += _arithmetic_loop(inner_loops)
    dt = pyperf.perf_counter() - t0
    _consume(total)
    return dt


def bench_listcomp_work(
    loops: int,
    implementation: str,
    inner_loops: int,
    timeout_seconds: float,
) -> float:
    _warm_timeout(implementation, timeout_seconds)
    total = 0
    t0 = pyperf.perf_counter()
    if _has_timeout(implementation):
        with _get_timeout(timeout_seconds):
            for _ in range(loops):
                total ^= _listcomp_work(inner_loops)
    else:
        for _ in range(loops):
            total ^= _listcomp_work(inner_loops)
    dt = pyperf.perf_counter() - t0
    _consume(total)
    return dt


def bench_context_enter_exit(
    loops: int,
    implementation: str,
    timeout_seconds: float,
) -> float:
    _warm_timeout(implementation, timeout_seconds)
    if _has_timeout(implementation):
        t0 = pyperf.perf_counter()
        for _ in range(loops):
            with _get_timeout(timeout_seconds):
                pass
    else:
        t0 = pyperf.perf_counter()
        for _ in range(loops):
            with nullcontext():
                pass
    return pyperf.perf_counter() - t0


def add_cmdline_args(cmd, args) -> None:
    cmd.extend(("--implementation", args.implementation))
    cmd.extend(("--inner-loops", str(args.inner_loops)))
    cmd.extend(("--timeout-seconds", str(args.timeout_seconds)))


def main() -> None:
    runner = pyperf.Runner(add_cmdline_args=add_cmdline_args)
    runner.argparser.add_argument(
        "--implementation",
        choices=("baseline", "op", "ours"),
        required=True,
        help="'ours' is the deadline-scheduler prototype",
    )
    runner.argparser.add_argument(
        "--inner-loops",
        type=int,
        default=1000,
        help="work per calibrated pyperf loop",
    )
    runner.argparser.add_argument(
        "--timeout-seconds",
        type=float,
        default=3600.0,
        help="non-expiring timeout duration",
    )
    args = runner.parse_args()

    runner.metadata["timeout_implementation"] = args.implementation
    runner.metadata["timeout_inner_loops"] = str(args.inner_loops)
    runner.metadata["timeout_seconds"] = str(args.timeout_seconds)

    runner.bench_time_func(
        "pass_loop",
        bench_pass_loop,
        args.implementation,
        args.inner_loops,
        args.timeout_seconds,
    )
    runner.bench_time_func(
        "arithmetic_loop",
        bench_arithmetic_loop,
        args.implementation,
        args.inner_loops,
        args.timeout_seconds,
    )
    runner.bench_time_func(
        "listcomp_work",
        bench_listcomp_work,
        args.implementation,
        args.inner_loops,
        args.timeout_seconds,
    )
    runner.bench_time_func(
        "context_enter_exit",
        bench_context_enter_exit,
        args.implementation,
        args.timeout_seconds,
    )


if __name__ == "__main__":
    main()
```

</details>
