#!/usr/bin/env python3
"""Compare real KWin capture using the opt-in capture benchmark."""

import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

project_dir = Path(__file__).resolve().parent.parent
project_version = re.search(r"project\(M4Capture VERSION (\S+)",
                            (project_dir / "CMakeLists.txt").read_text())[1]
build_dir = project_dir / "out" / f"build-release-v{project_version}"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, default=build_dir / "m4capture-benchmarks")
parser.add_argument("--baseline", type=Path, help="Preserved pre-change benchmark executable")
parser.add_argument("--runs", type=int, default=3)
parser.add_argument("--output", type=Path, default=build_dir / "capture-performance.json")
args = parser.parse_args()
if args.runs < 1:
    parser.error("--runs must be positive")
args.output.parent.mkdir(parents=True, exist_ok=True)
logs = args.output.with_suffix("")
logs.mkdir(exist_ok=True)
results = []
modes = (["before"] if args.baseline else []) + ["dmabuf", "cpu-fallback"]
for width in (1920, 3840):
    for run in range(1, args.runs + 1):
        for mode in modes:
            environment = os.environ.copy()
            environment.update(QT_QPA_PLATFORM="wayland", QT_SCALE_FACTOR="1",
                               M4CAPTURE_LIVE_TEST="1", M4CAPTURE_BENCHMARK_WIDTH=str(width))
            environment.pop("M4CAPTURE_FORCE_CPU", None)
            if mode == "cpu-fallback":
                environment["M4CAPTURE_FORCE_CPU"] = "1"
            binary = args.baseline if mode == "before" else args.binary
            process = subprocess.run([str(binary.resolve()), "capturePerformance"], env=environment,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            (logs / f"{mode}-{width}-{run}.log").write_text(process.stdout)
            match = re.search(r"BENCHMARK (\d+)x(\d+) (seconds=[^\n]+)", process.stdout)
            if process.returncode or not match:
                raise SystemExit(f"Benchmark failed: {mode}, {width}, run {run}\n{process.stdout}")
            if mode != "before":
                expected = "dmabuf" if mode == "dmabuf" else "cpu"
                if f"BENCHMARK_PATH {expected}" not in process.stdout:
                    raise SystemExit(f"Unexpected capture path in {mode} benchmark\n{process.stdout}")
            sample = dict(mode=mode, width=int(match[1]), height=int(match[2]), run=run)
            sample.update({key: float(value) for key, value in re.findall(r"(\w+)=([\d.]+)", match[3])})
            results.append(sample)
            args.output.write_text(json.dumps(results, indent=2) + "\n")
            print(json.dumps(sample), flush=True)
print("Medians (CPU percent: 100% = one logical core):", flush=True)
for width in (1920, 3840):
    for mode in modes:
        samples = [r for r in results if r["width"] == width and r["mode"] == mode]
        print(json.dumps(dict(width=width, mode=mode, **{
            key: round(statistics.median(r[key] for r in samples), 3)
            for key in ("capture_fps", "cpu_percent", "cpu_ms_per_frame")
        })), flush=True)
