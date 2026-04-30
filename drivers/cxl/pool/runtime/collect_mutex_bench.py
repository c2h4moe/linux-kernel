#!/usr/bin/env python3
import argparse
import csv
import os
import pathlib
import re
import statistics
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent
RESULTS_DIR = ROOT / "bench_results"
LINE_RE = re.compile(r"([A-Za-z_]+)=([^\s]+)")


def run_cmd(cmd):
    print(f"RUN: {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed: {' '.join(cmd)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout


def parse_result(stdout):
    for line in stdout.splitlines():
        if line.startswith("mode="):
            data = dict(LINE_RE.findall(line))
            return {
                "mode": data["mode"],
                "threads": int(data["threads"]),
                "machines": int(data["machines"]),
                "total_ops": int(data["total_ops"]),
                "elapsed_ns": int(data["elapsed_ns"]),
                "avg_ns_per_inc": float(data["avg_ns_per_inc"]),
                "counter": int(data["counter"]),
            }
    raise RuntimeError(f"no result line found in output:\n{stdout}")


def build_targets():
    run_cmd(["make", "pthread_mutex_bench", "cxl_mutex_bench"])


def collect_rows(max_threads, total_ops, repetitions, cxl_max_machines):
    rows = []
    for threads in range(1, max_threads + 1):
        machines = min(threads, cxl_max_machines)
        for run_idx in range(1, repetitions + 1):
            out = run_cmd(["./pthread_mutex_bench", str(threads), str(total_ops)])
            row = parse_result(out)
            row["run"] = run_idx
            rows.append(row)

            out = run_cmd(
                ["./cxl_mutex_bench", str(machines), str(threads), str(total_ops)]
            )
            row = parse_result(out)
            row["run"] = run_idx
            rows.append(row)
    return rows


def write_csv(rows, path):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "mode",
                "threads",
                "machines",
                "total_ops",
                "run",
                "elapsed_ns",
                "avg_ns_per_inc",
                "counter",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def summarize(rows):
    groups = {}
    for row in rows:
        key = (row["mode"], row["threads"], row["machines"], row["total_ops"])
        groups.setdefault(key, []).append(row["avg_ns_per_inc"])

    summary = []
    for (mode, threads, machines, total_ops), values in sorted(groups.items()):
        summary.append(
            {
                "mode": mode,
                "threads": threads,
                "machines": machines,
                "total_ops": total_ops,
                "avg_ns_per_inc_mean": statistics.fmean(values),
                "avg_ns_per_inc_stdev": statistics.pstdev(values),
            }
        )
    return summary


def write_summary_csv(rows, path):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "mode",
                "threads",
                "machines",
                "total_ops",
                "avg_ns_per_inc_mean",
                "avg_ns_per_inc_stdev",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def maybe_plot(summary_rows, png_path):
    os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-cxl-bench")
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:
        print(f"matplotlib unavailable, skip plot: {exc}")
        return False

    series = {"pthread": {"x": [], "y": []}, "cxl": {"x": [], "y": []}}
    for row in summary_rows:
        series[row["mode"]]["x"].append(row["threads"])
        series[row["mode"]]["y"].append(row["avg_ns_per_inc_mean"])

    plt.figure(figsize=(8, 5))
    plt.plot(series["pthread"]["x"], series["pthread"]["y"], marker="o",
             label="pthread_mutex")
    plt.plot(series["cxl"]["x"], series["cxl"]["y"], marker="o",
             label="cxl_mutex")
    plt.xlabel("Threads")
    plt.ylabel("Average ns per counter++")
    plt.title("Mutex Counter Increment Cost")
    plt.grid(True, linestyle="--", linewidth=0.5, alpha=0.6)
    plt.legend()
    plt.tight_layout()
    plt.savefig(png_path, dpi=150)
    plt.close()
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-threads", type=int, default=32)
    parser.add_argument("--total-ops", type=int, default=5000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--cxl-max-machines", type=int, default=8)
    args = parser.parse_args()

    RESULTS_DIR.mkdir(exist_ok=True)
    build_targets()
    rows = collect_rows(
        max_threads=args.max_threads,
        total_ops=args.total_ops,
        repetitions=args.repetitions,
        cxl_max_machines=args.cxl_max_machines,
    )
    raw_csv = RESULTS_DIR / "mutex_bench_raw.csv"
    summary_csv = RESULTS_DIR / "mutex_bench_summary.csv"
    plot_png = RESULTS_DIR / "mutex_bench.png"
    write_csv(rows, raw_csv)
    summary_rows = summarize(rows)
    write_summary_csv(summary_rows, summary_csv)
    plotted = maybe_plot(summary_rows, plot_png)

    print(f"raw_csv={raw_csv}")
    print(f"summary_csv={summary_csv}")
    if plotted:
        print(f"plot_png={plot_png}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
