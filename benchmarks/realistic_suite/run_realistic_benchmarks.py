#!/usr/bin/env python3
"""
Vortex v1.3.0 Realistic 9-Language Benchmark Runner & Checksum Verifier.
Zero artificial patterns, zero -ffast-math, zero custom allocator tricks.
Every language runs the exact same 5 algorithms and must pass checksum parity.
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
        parts = line.strip().split(None, 1)
        if len(parts) == 2:
            metrics[parts[0]] = parts[1].strip()
    return wall_ms, metrics


def detect_vortex_bin():
    for c in [REPO_ROOT / ("vortex.exe" if os.name == "nt" else "vortex"), REPO_ROOT / "vortex"]:
        if c.exists():
            return str(c)
    return shutil.which("vortex")


def verify_checksums(name, metrics, ref):
    if ref is None:
        return
    for k in ["T2_CHK", "T4_CHK", "T5_CHK"]:
        if int(metrics[k]) != int(ref[k]):
            raise RuntimeError(f"Checksum mismatch for {name} on {k}: got {metrics[k]}, expected {ref[k]}")
    for k, tol in [("T1_CHK", 1e-3), ("T3_CHK", 1e-2)]:
        diff = abs(float(metrics[k]) - float(ref[k]))
        if diff > tol:
            raise RuntimeError(f"Float checksum mismatch for {name} on {k}: got {metrics[k]}, expected {ref[k]} (diff={diff})")


def compile_and_bench(name, sublabel, compile_cmd, run_cmd, out_bin, runs, results, ref_chk):
    compile_ms = 0.0
    if compile_cmd is not None:
        t0 = time.perf_counter()
        subprocess.run(compile_cmd, cwd=str(SUITE_DIR), capture_output=True, text=True, check=True)
        compile_ms = (time.perf_counter() - t0) * 1000.0

    size_kb = (Path(out_bin).stat().st_size / 1024.0) if out_bin and Path(out_bin).exists() else 0.0
    samples = [run_measured(run_cmd, SUITE_DIR) for _ in range(runs)]
    for _, m in samples:
        verify_checksums(name, m, ref_chk)

    t1 = round(median([float(m["T1_US"]) / 1000.0 for _, m in samples]), 2)
    t2 = round(median([float(m["T2_US"]) / 1000.0 for _, m in samples]), 2)
    t3 = round(median([float(m["T3_US"]) / 1000.0 for _, m in samples]), 2)
    t4 = round(median([float(m["T4_US"]) / 1000.0 for _, m in samples]), 2)
    t5 = round(median([float(m["T5_US"]) / 1000.0 for _, m in samples]), 2)
    tot = round(median([float(m["TOTAL_US"]) / 1000.0 for _, m in samples]), 2)
    wall = round(median([w for w, _ in samples]), 2)
    startup_ms = round(max(0.0, wall - tot), 2)

    entry = {
        "name": name,
        "sublabel": sublabel,
        "runs": runs,
        "t1_ms": t1,
        "t2_ms": t2,
        "t3_ms": t3,
        "t4_ms": t4,
        "t5_ms": t5,
        "total_ms": tot,
        "wall_ms": wall,
        "startup_ms": startup_ms,
        "compile_ms": round(compile_ms, 1),
        "binary_kb": round(size_kb, 1),
        "checksums": {
            "T1_CHK": samples[0][1]["T1_CHK"],
            "T2_CHK": samples[0][1]["T2_CHK"],
            "T3_CHK": samples[0][1]["T3_CHK"],
            "T4_CHK": samples[0][1]["T4_CHK"],
            "T5_CHK": samples[0][1]["T5_CHK"],
        },
    }
    results.append(entry)
    return samples[0][1]


def run_silicon_suite(vortex_bin, cc):
    hw_out = {}
    if vortex_bin:
        _, m = run_measured([vortex_bin, "run", "--release", "bench_silicon_hw.vx"], SUITE_DIR)
        hw_out["cpu_name"] = m.get("CPU_NAME", "Unknown CPU")
        hw_out["gpu_name"] = m.get("GPU_NAME", "None")
        hw_out["vx_scalar_f64_512_ms"] = round(float(m["VX_SCALAR_F64_512_US"]) / 1000.0, 2)
        hw_out["vx_hw_f64_512_ms"] = round(float(m["VX_HW_F64_512_US"]) / 1000.0, 2)
        hw_out["vx_hw_f64_gflops"] = round(0.268435456 / (float(m["VX_HW_F64_512_US"]) / 1e6), 2)
        hw_out["vx_hw_f64_route"] = int(m["VX_HW_F64_ROUTE"])
        hw_out["vx_hw_f64_err"] = float(m["VX_HW_F64_ERR"])
        hw_out["vx_hw_f32_cpu_1024_ms"] = round(float(m["VX_HW_F32_CPU_1024_US"]) / 1000.0, 2)
        hw_out["vx_hw_f32_cpu_gflops"] = round(2.147483648 / (float(m["VX_HW_F32_CPU_1024_US"]) / 1e6), 2)
        hw_out["vx_hw_f32_gpu_1024_ms"] = round(float(m["VX_HW_F32_GPU_1024_US"]) / 1000.0, 2)
        hw_out["vx_hw_f32_gpu_gflops"] = round(2.147483648 / (float(m["VX_HW_F32_GPU_1024_US"]) / 1e6), 2)
        hw_out["vx_hw_f32_gpu_route"] = int(m["VX_HW_F32_GPU_ROUTE"])
        hw_out["vx_hw_f32_err"] = float(m["VX_HW_F32_ERR"])

    if cc:
        ext = ".exe" if os.name == "nt" else ""
        c_bin = SUITE_DIR / f"_tmp_sil_c{ext}"
        subprocess.run([cc, "-O3", "-march=native", "bench_silicon_c.c", "-o", str(c_bin)],
                       cwd=str(SUITE_DIR), capture_output=True, check=True)
        c_samples = [run_measured([str(c_bin)], SUITE_DIR)[1] for _ in range(3)]
        c_f64 = median([float(s["C_F64_512_US"]) for s in c_samples])
        c_f32 = median([float(s["C_F32_1024_US"]) for s in c_samples])
        hw_out["c_f64_512_ms"] = round(c_f64 / 1000.0, 2)
        hw_out["c_f64_512_gflops"] = round(0.268435456 / (c_f64 / 1e6), 2)
        hw_out["c_f32_1024_ms"] = round(c_f32 / 1000.0, 2)
        hw_out["c_f32_1024_gflops"] = round(2.147483648 / (c_f32 / 1e6), 2)
        c_bin.unlink(missing_ok=True)
    return hw_out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--skip-python", action="store_true")
    ap.add_argument("--json", dest="json_out", default="")
    args = ap.parse_args()

    print(f"Host: {platform.system()} {platform.release()} | Arch: {platform.machine()} | Cores: {os.cpu_count()}")
    print(f"Runs per target: {args.runs} (1 exact run for CPython)\n")

    results = []
    temps = []
    ext = ".exe" if os.name == "nt" else ""
    ref_chk = None

    cc = shutil.which("clang") or shutil.which("gcc")
    if cc:
        c_libs = [] if os.name == "nt" else ["-lm", "-lpthread"]
        out_c = SUITE_DIR / f"_tmp_c{ext}"
        temps.append(out_c)
        ref_chk = compile_and_bench(
            "C (Clang -O3)", f"{Path(cc).stem} 23 (-O3 -march=native)",
            [cc, "-O3", "-march=native", "-ffp-contract=off", "bench_c.c", "-o", str(out_c), *c_libs],
            [str(out_c)], out_c, args.runs, results, None,
        )

    cxx = shutil.which("clang++") or shutil.which("g++")
    if cxx:
        c_libs = [] if os.name == "nt" else ["-lm", "-lpthread"]
        out_cpp = SUITE_DIR / f"_tmp_cpp{ext}"
        temps.append(out_cpp)
        compile_and_bench(
            "C++ (Clang++ -O3)", f"{Path(cxx).stem} 23 (-std=c++20 -O3 -march=native)",
            [cxx, "-std=c++20", "-O3", "-march=native", "-ffp-contract=off", "bench_cpp.cpp", "-o", str(out_cpp), *c_libs],
            [str(out_cpp)], out_cpp, args.runs, results, ref_chk,
        )

    rustc = shutil.which("rustc")
    if rustc:
        out_rs = SUITE_DIR / f"_tmp_rs{ext}"
        temps.append(out_rs)
        compile_and_bench(
            "Rust (-O3)", "rustc 1.99 (-C opt-level=3 -C target-cpu=native)",
            [rustc, "-C", "opt-level=3", "-C", "target-cpu=native", "bench_rust.rs", "-o", str(out_rs)],
            [str(out_rs)], out_rs, args.runs, results, ref_chk,
        )

    go_bin = shutil.which("go")
    if go_bin:
        out_go = SUITE_DIR / f"_tmp_go{ext}"
        temps.append(out_go)
        compile_and_bench(
            "Go (1.27)", "go build (goroutines T5)",
            [go_bin, "build", "-o", str(out_go), "bench_go.go"],
            [str(out_go)], out_go, args.runs, results, ref_chk,
        )

    vortex_bin = detect_vortex_bin()
    if vortex_bin:
        compile_and_bench(
            "Vortex (--release)", "JIT x86-64 v1.3.0 (f64 SSE2 + 12T spin-pool)",
            None, [vortex_bin, "run", "--release", "bench_vortex.vx"],
            vortex_bin, args.runs, results, ref_chk,
        )
        compile_and_bench(
            "Vortex (--safe)", "JIT x86-64 v1.3.0 (bounds/arena checks ON)",
            None, [vortex_bin, "run", "--safe", "bench_vortex.vx"],
            vortex_bin, args.runs, results, ref_chk,
        )

    javac = shutil.which("javac")
    java = shutil.which("java")
    if javac and java:
        cls = SUITE_DIR / "BenchJava.class"
        temps.append(cls)
        compile_and_bench(
            "Java (OpenJDK 21)", "HotSpot 21 C2 JIT (parallel IntStream T5)",
            [javac, "BenchJava.java"],
            [java, "-cp", str(SUITE_DIR), "BenchJava"], cls, args.runs, results, ref_chk,
        )

    csc = r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
    if os.name == "nt" and os.path.exists(csc):
        out_cs = SUITE_DIR / "_tmp_cs.exe"
        temps.append(out_cs)
        compile_and_bench(
            "C# (.NET v4.8)", "csc 64-bit /optimize+ (Parallel.For T5)",
            [csc, "/nologo", "/optimize+", "/platform:x64", f"/out:{out_cs}", "BenchCSharp.cs"],
            [str(out_cs)], out_cs, args.runs, results, ref_chk,
        )

    node = shutil.which("node")
    if node:
        compile_and_bench(
            "Node.js (v26 V8)", "V8 JIT + worker_threads T5",
            None, [node, "bench_js.js"], node, args.runs, results, ref_chk,
        )

    if not args.skip_python:
        compile_and_bench(
            "Python 3.12", "CPython 3.12 (single-thread T5 GIL)",
            None, [sys.executable, "bench_python.py"], sys.executable, 1, results, ref_chk,
        )

    for p in temps:
        if p.exists():
            p.unlink(missing_ok=True)

    silicon = run_silicon_suite(vortex_bin, cc)

    print(f"| {'Profil':<22} | {'Total(ms)':>9} | {'T1(NBody)':>9} | {'T2(QSort)':>9} | {'T3(MatMul)':>10} | {'T4(Sieve)':>9} | {'T5(Mandel)':>10} | {'AOT(ms)':>7} | {'Start(ms)':>9} |")
    print("|" + "-" * 24 + "|" + "|".join(["-" * 11] * 7 + ["-" * 9, "-" * 11]) + "|")
    for r in sorted(results, key=lambda x: x["total_ms"]):
        print(f"| {r['name']:<22} | {r['total_ms']:>9.2f} | {r['t1_ms']:>9.2f} | {r['t2_ms']:>9.2f} | {r['t3_ms']:>10.2f} | {r['t4_ms']:>9.2f} | {r['t5_ms']:>10.2f} | {r['compile_ms']:>7.1f} | {r['startup_ms']:>9.2f} |")

    print("\n=== SILICON HARDWARE SUITE (std/hw vs Pure Scalar) ===")
    print(json.dumps(silicon, indent=2))

    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps({
                "host": {
                    "system": platform.system(),
                    "release": platform.release(),
                    "machine": platform.machine(),
                    "cores": os.cpu_count(),
                    "cpu": silicon.get("cpu_name", ""),
                    "gpu": silicon.get("gpu_name", ""),
                },
                "results": results,
                "silicon": silicon,
            }, indent=2),
            encoding="utf-8",
        )


if __name__ == "__main__":
    main()
