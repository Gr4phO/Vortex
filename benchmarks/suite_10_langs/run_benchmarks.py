#!/usr/bin/env python3
"""Reproducible multi-OS benchmark runner for Vortex & comparison languages.
Works on Windows, Linux, and macOS using only the Python 3 standard library.
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

SUITE_DIR = Path(__file__).resolve().parent
REPO_ROOT = SUITE_DIR.parent.parent


def median(vals):
    s = sorted(vals)
    n = len(s)
    if n == 0:
        return 0.0
    if n % 2 == 1:
        return s[n // 2]
    return 0.5 * (s[n // 2 - 1] + s[n // 2])


def run_measured(cmd, cwd):
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, check=True)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    metrics = {}
    for line in proc.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) == 2:
            metrics[parts[0]] = parts[1]
    return wall_ms, metrics


def detect_vortex_bin():
    candidates = [
        REPO_ROOT / ("vortex.exe" if os.name == "nt" else "vortex"),
        REPO_ROOT / "vortex",
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return shutil.which("vortex")


def compile_and_bench(name, compile_cmd, run_cmd, out_bin, runs, results):
    compile_ms = 0.0
    if compile_cmd is not None:
        t0 = time.perf_counter()
        subprocess.run(compile_cmd, cwd=str(SUITE_DIR), capture_output=True, text=True, check=True)
        compile_ms = (time.perf_counter() - t0) * 1000.0

    size_kb = (Path(out_bin).stat().st_size / 1024.0) if out_bin and Path(out_bin).exists() else 0.0
    samples = [run_measured(run_cmd, SUITE_DIR) for _ in range(runs)]
    entry = {
        "name": name,
        "runs": runs,
        "t1_ms": round(median([float(m["TASK1_US"]) / 1000.0 for _, m in samples]), 2),
        "t2_ms": round(median([float(m["TASK2_US"]) / 1000.0 for _, m in samples]), 2),
        "t3_ms": round(median([float(m["TASK3_US"]) / 1000.0 for _, m in samples]), 2),
        "t4_ms": round(median([float(m["TASK4_US"]) / 1000.0 for _, m in samples]), 2),
        "t5_ms": round(median([float(m["TASK5_US"]) / 1000.0 for _, m in samples]), 2),
        "total_ms": round(median([float(m["TOTAL_US"]) / 1000.0 for _, m in samples]), 2),
        "wall_ms": round(median([w for w, _ in samples]), 2),
        "compile_ms": round(compile_ms, 1),
        "binary_kb": round(size_kb, 1),
    }
    results.append(entry)


def main():
    ap = argparse.ArgumentParser(description="Run reproducible multi-OS Vortex benchmarks.")
    ap.add_argument("--runs", type=int, default=3, help="Median runs per target (default: 3)")
    ap.add_argument("--skip-python", action="store_true", help="Skip slow CPython benchmark")
    ap.add_argument("--only", choices=["all", "vortex"], default="all", help="Run all detected languages or Vortex only")
    ap.add_argument("--json", dest="json_out", default="", help="Optional path to save raw JSON results")
    args = ap.parse_args()

    print(f"Host: {platform.system()} {platform.release()} | Arch: {platform.machine()} | Cores: {os.cpu_count()}")
    print(f"Runs per target: {args.runs}\n")

    results = []
    temps = []
    ext = ".exe" if os.name == "nt" else ""
    vortex_bin = detect_vortex_bin()
    if vortex_bin:
        compile_and_bench(
            "Vortex (--release)", None,
            [vortex_bin, "run", "--release", "bench_vortex.vx"],
            vortex_bin, args.runs, results,
        )
        compile_and_bench(
            "Vortex (--safe)", None,
            [vortex_bin, "run", "--safe", "bench_vortex.vx"],
            vortex_bin, args.runs, results,
        )

    if args.only == "all":
        cc = shutil.which("clang") or shutil.which("gcc")
        if cc:
            c_libs = [] if os.name == "nt" else ["-lm", "-lpthread"]
            for label, flags in [
                (f"C ({Path(cc).stem} -O3)", ["-O3", "-march=native"]),
                (f"C ({Path(cc).stem} -O3 -ffast-math)", ["-O3", "-march=native", "-ffast-math"]),
                (f"C ({Path(cc).stem} -O3 + arena)", ["-O3", "-march=native", "-DBENCH_ARENA"]),
            ]:
                out_p = SUITE_DIR / f"_tmp_{len(temps)}{ext}"
                temps.append(out_p)
                compile_and_bench(label, [cc, *flags, "bench_c.c", "-o", str(out_p), *c_libs], [str(out_p)], out_p, args.runs, results)

        cxx = shutil.which("clang++") or shutil.which("g++")
        if cxx:
            c_libs = [] if os.name == "nt" else ["-lm", "-lpthread"]
            out_p = SUITE_DIR / f"_tmp_cpp{ext}"
            temps.append(out_p)
            compile_and_bench(f"C++ ({Path(cxx).stem} -O3)", [cxx, "-std=c++20", "-O3", "-march=native", "bench_cpp.cpp", "-o", str(out_p), *c_libs], [str(out_p)], out_p, args.runs, results)

        rustc = shutil.which("rustc")
        if rustc:
            out_p = SUITE_DIR / f"_tmp_rs{ext}"
            temps.append(out_p)
            compile_and_bench("Rust (rustc -O3)", [rustc, "-C", "opt-level=3", "-C", "target-cpu=native", "bench_rust.rs", "-o", str(out_p)], [str(out_p)], out_p, args.runs, results)

        node = shutil.which("node")
        if node:
            compile_and_bench("Node.js (V8 JIT)", None, [node, "bench_js.js"], node, args.runs, results)

        if not args.skip_python:
            compile_and_bench("Python 3 (CPython)", None, [sys.executable, "bench_python.py"], sys.executable, 1, results)

    for p in temps:
        if p.exists():
            p.unlink(missing_ok=True)

    print(f"| {'Profil':<32} | {'Total(ms)':>9} | {'T1':>7} | {'T2':>7} | {'T3':>7} | {'T4':>7} | {'T5':>7} | {'Wall(ms)':>8} | {'AOT(ms)':>7} | {'Bin(KB)':>8} |")
    print("|" + "-" * 34 + "|" + "|".join(["-" * 11] + ["-" * 9] * 5 + ["-" * 10] + ["-" * 9] + ["-" * 10]) + "|")
    for r in sorted(results, key=lambda x: x["total_ms"]):
        print(f"| {r['name']:<32} | {r['total_ms']:>9.2f} | {r['t1_ms']:>7.2f} | {r['t2_ms']:>7.2f} | {r['t3_ms']:>7.2f} | {r['t4_ms']:>7.2f} | {r['t5_ms']:>7.2f} | {r['wall_ms']:>8.2f} | {r['compile_ms']:>7.1f} | {r['binary_kb']:>8.1f} |")

    if args.json_out:
        Path(args.json_out).write_text(json.dumps({"host": platform.uname()._asdict(), "results": results}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
