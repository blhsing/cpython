#!/usr/bin/env python3
"""Benchmark prototype synchronous timeout overhead with pyperf."""

from __future__ import annotations

from contextlib import timeout
import pyperf


_sink = 0
_scheduler_warmed = False


def _consume(value: int) -> None:
    global _sink
    _sink ^= value


def _warm_timeout_scheduler(timeout_seconds: float) -> None:
    global _scheduler_warmed
    if not _scheduler_warmed:
        with timeout(timeout_seconds):
            pass
        _scheduler_warmed = True


def _loop_body(inner_loops: int) -> int:
    total = 0
    for value in range(inner_loops):
        total += value
    return total


def bench_loop_baseline(loops: int, inner_loops: int) -> float:
    t0 = pyperf.perf_counter()
    total = 0
    for _ in range(loops):
        total += _loop_body(inner_loops)
    dt = pyperf.perf_counter() - t0
    _consume(total)
    return dt


def bench_loop_timeout_active(
    loops: int,
    inner_loops: int,
    timeout_seconds: float,
) -> float:
    _warm_timeout_scheduler(timeout_seconds)
    t0 = pyperf.perf_counter()
    total = 0
    with timeout(timeout_seconds):
        for _ in range(loops):
            total += _loop_body(inner_loops)
    dt = pyperf.perf_counter() - t0
    _consume(total)
    return dt


def bench_timeout_enter_exit(loops: int, timeout_seconds: float) -> float:
    _warm_timeout_scheduler(timeout_seconds)
    t0 = pyperf.perf_counter()
    for _ in range(loops):
        with timeout(timeout_seconds):
            pass
    return pyperf.perf_counter() - t0


def bench_timeout_expiry_latency(loops: int, timeout_seconds: float) -> float:
    _warm_timeout_scheduler(3600.0)
    t0 = pyperf.perf_counter()
    for _ in range(loops):
        try:
            with timeout(timeout_seconds):
                while True:
                    pass
        except TimeoutError:
            pass
    return pyperf.perf_counter() - t0


def add_cmdline_args(cmd, args) -> None:
    cmd.extend(("--inner-loops", str(args.inner_loops)))
    cmd.extend(("--timeout-seconds", str(args.timeout_seconds)))
    cmd.extend(("--expiry-seconds", str(args.expiry_seconds)))
    if args.expiry:
        cmd.append("--expiry")


def main() -> None:
    runner = pyperf.Runner(add_cmdline_args=add_cmdline_args)
    runner.argparser.add_argument(
        "--inner-loops",
        type=int,
        default=1000,
        help="work per calibrated pyperf loop for loop benchmarks",
    )
    runner.argparser.add_argument(
        "--timeout-seconds",
        type=float,
        default=3600.0,
        help="non-expiring timeout duration used by overhead benchmarks",
    )
    runner.argparser.add_argument(
        "--expiry",
        action="store_true",
        help="also benchmark actual expiry latency",
    )
    runner.argparser.add_argument(
        "--expiry-seconds",
        type=float,
        default=1e-6,
        help="timeout duration used by --expiry",
    )
    args = runner.parse_args()

    runner.metadata["description"] = "CPython synchronous timeout prototype"
    runner.metadata["timeout_inner_loops"] = str(args.inner_loops)
    runner.metadata["timeout_seconds"] = str(args.timeout_seconds)

    runner.bench_time_func(
        "loop_baseline",
        bench_loop_baseline,
        args.inner_loops,
    )
    runner.bench_time_func(
        "loop_timeout_active",
        bench_loop_timeout_active,
        args.inner_loops,
        args.timeout_seconds,
    )
    runner.bench_time_func(
        "timeout_enter_exit",
        bench_timeout_enter_exit,
        args.timeout_seconds,
    )
    if args.expiry:
        runner.bench_time_func(
            "timeout_expiry_latency",
            bench_timeout_expiry_latency,
            args.expiry_seconds,
        )


if __name__ == "__main__":
    main()
