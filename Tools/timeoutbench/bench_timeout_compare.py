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
        help="implementation under test",
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
