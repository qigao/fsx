"""Validate and summarize one native content benchmark run.

No timing thresholds. Never pool runs from different hosts or sanitizer profiles.
Tool capture is explicit: --capture-tools hashes and queries the recorded native
executables; omit it when reanalyzing archived CSV on another machine.
"""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import statistics
import subprocess


def read_measurements(path):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    metadata = dict(line[2:].split("=", 1) for line in lines
                    if line.startswith("# ") and not line.startswith("# acceptance="))
    accepted = [line for line in lines if line.startswith("# acceptance=passed operations=")]
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    if len(accepted) != 1 or int(accepted[0].split("operations=")[1]) != len(rows):
        raise ValueError("incomplete native acceptance or incorrect operation count")
    if metadata["format"] != "fsx-content-benchmark-v3":
        raise ValueError("unsupported measurement format")
    text = {"corpus", "source", "pattern", "phase", "input_hash"}
    return metadata, [{k: v if k in text else int(v) for k, v in row.items()} for row in rows]


def schedule(sources):
    expected = []
    for corpus in ("small", "large"):
        expected.append((corpus, "none", "none", "baseline", 0, 0, 0))
        for source in sources:
            variants = [(source, 1), (source, 2)]
            if source == "scope" and "live" in sources:
                variants += [("direct", 1), ("direct", 2), ("profile", 1), ("profile", 2)]
            for pattern in (("anchor", "needle", "absent") if corpus == "small"
                            else ("anchor", "absent")):
                expected.extend((corpus, origin, pattern, "warmup", 0, pos, mode)
                                for pos, (origin, mode) in enumerate(variants))
                expected.extend((corpus, variants[(rnd - 1 + pos) % len(variants)][0],
                                 pattern, "measure", rnd, pos,
                                 variants[(rnd - 1 + pos) % len(variants)][1])
                                for rnd in range(1, 7 if len(variants) == 6 else 5) for pos in range(len(variants)))
            expected.extend((corpus, source, "anchor", "cancel", 0, mode - 1, mode)
                            for mode in (1, 2))
    return Counter(expected)


def distribution(values, scale=1):
    values = sorted(values)
    if not values:
        return {"n": 0}
    return {"n": len(values), "median": statistics.median(values) / scale,
            **{f"p{p}": values[math.ceil(p / 100 * len(values)) - 1] / scale
               for p in (50, 95, 99)},
            "min": values[0] / scale, "max": values[-1] / scale}


def summarize(measurement_path, query_path):
    metadata, rows = read_measurements(measurement_path)
    sources = ("scope",) if metadata["platform"].startswith("Windows/") else ("scope", "live")
    fields = ("corpus", "source", "pattern", "phase", "round", "position", "parallel_files")
    if Counter(tuple(row[k] for k in fields) for row in rows) != schedule(sources):
        raise ValueError("missing/duplicate/unbalanced benchmark operations")
    operations = {row["operation"]: row for row in rows}
    if sorted(operations) != list(range(1, len(rows) + 1)) or len(operations) != len(rows):
        raise ValueError("operation identifiers must be unique and contiguous")
    inputs = defaultdict(set)
    for row in rows:
        inputs[row["corpus"]].add((row["input_hash"], row["files"], row["bytes"],
                                   row["metadata_epoch"], row["scan_ns"]))
        expected_files, expected_bytes = (64, 65536) if row["corpus"] == "small" else (4, 16777216)
        if row["files"] != expected_files or row["bytes"] != expected_bytes:
            raise ValueError("unexpected corpus size")
        baseline, cancel = row["phase"] == "baseline", row["phase"] == "cancel"
        if row["status"] != (-6 if cancel else 0) or row["end_ns"] <= row["start_ns"]:
            raise ValueError("failed operation or nonpositive observation window")
        if row["owned_sampled_peak"] < row["owned_base"]:
            raise ValueError("invalid sampled allocation peak")
        if row["queued_sampled_peak"] > 128:
            raise ValueError("candidate queue exceeded its bound")
        if row["running_sampled_peak"] > row["parallel_files"] + (row["source"] == "live"):
            raise ValueError("native process count exceeded its bound")
        if row["source"] == "direct":
            if (row["queued_sampled_peak"] or row["spawn_call_ns"] <= 0
                    or row["child_lifetime_ns"] < row["spawn_call_ns"]
                    or row["collect_decode_ns"] <= 0
                    or row["child_lifetime_ns"] + row["collect_decode_ns"] >
                       row["parallel_files"] * (row["end_ns"] - row["start_ns"])):
                raise ValueError("invalid direct-process stages or concurrency bounds")
        elif any(row[k] for k in ("spawn_call_ns", "child_lifetime_ns", "collect_decode_ns")):
            raise ValueError("direct-process stages attached to a different backend")
        if row["source"] == "profile":
            stages = ("open_ns", "spawn_ns", "read_ns", "text_ns", "poll_ns", "write_ns",
                      "output_ns", "finish_ns", "postcheck_ns")
            if (row["fsx_files"] != row["files"] or row["fsx_read_bytes"] != row["bytes"]
                    or row["fsx_write_bytes"] != row["bytes"]
                    or any(row["fsx_" + k] < row["files"] for k in ("read_calls", "write_calls", "poll_calls"))
                    or sum(row["fsx_" + k] for k in stages) > row["fsx_file_ns"]
                    or row["fsx_file_ns"] > row["parallel_files"] * (row["end_ns"] - row["start_ns"])
                    or row["fsx_worker_cpu_ns"] <= 0):
                raise ValueError("inconsistent profiled file stages or byte totals")
        elif any(v for k, v in row.items() if k.startswith("fsx_")):
            raise ValueError("profiling counters attached to an unprofiled operation")
        if baseline:
            if any(row[k] for k in ("records", "processes", "cancel_ns", "first_batch_ns")):
                raise ValueError("baseline unexpectedly ran a content process")
            continue
        if row["submit_ns"] <= 0:
            raise ValueError("missing submission measurement")
        if cancel:
            if not 0 < row["cancel_ns"] <= row["end_ns"] - row["start_ns"] or row["records"] <= 0:
                raise ValueError("missing cancellation observation")
        else:
            expected = (0 if row["pattern"] == "absent" else
                        1024 if row["pattern"] == "needle" else
                        64 if row["corpus"] == "small" else 256)
            if (row["records"] != expected or row["files_scanned"] != row["files"]
                    or row["candidates"] != row["files"]
                    or row["processes"] != row["files"] + (row["source"] == "live")):
                raise ValueError("inconsistent native result/process totals")
            if bool(row["first_batch_ns"]) != bool(expected) or row["cancel_ns"]:
                raise ValueError("invalid first-result/cancellation timing")
        if row["first_batch_ns"] > row["end_ns"] - row["start_ns"]:
            raise ValueError("first batch lies outside its operation")
    if any(len(values) != 1 for values in inputs.values()):
        raise ValueError("corpus fingerprints or metadata changed between modes")

    queries, crossing, outside = defaultdict(list), Counter(), Counter()
    with Path(query_path).open(encoding="utf-8") as stream:
        for raw in csv.DictReader(stream):
            sample = {k: int(v) for k, v in raw.items()}
            row = operations[sample["operation"]]
            if (sample["status"] != 0 or sample["metadata_epoch"] != row["metadata_epoch"]
                    or sample["matches"] != row["files"] or sample["start_ns"] > sample["end_ns"]):
                raise ValueError("failed/inconsistent concurrent metadata query")
            start, end = sample["start_ns"], sample["end_ns"]
            if row["start_ns"] <= start <= end <= row["end_ns"]:
                queries[row["operation"]].append(end - start)
            elif start < row["end_ns"] and end > row["start_ns"]:
                crossing[row["operation"]] += 1
            else:
                outside[row["operation"]] += 1
    grouped = defaultdict(list)
    for row in rows:
        if row["phase"] != "warmup":
            grouped[row["corpus"], row["source"], row["pattern"],
                    row["phase"], row["parallel_files"]].append(row)
    groups = []
    for key, batch in sorted(grouped.items()):
        corpus, source, pattern, phase, parallel = key
        durations = [row["end_ns"] - row["start_ns"] for row in batch]
        result = dict(zip(("corpus", "source", "pattern", "phase", "parallel_files"), key))
        result.update(wall_ms=distribution(durations, 1e6),
                      submit_ms=distribution([r["submit_ns"] for r in batch if r["submit_ns"]], 1e6),
                      first_batch_ms=distribution([r["first_batch_ns"] for r in batch if r["first_batch_ns"]], 1e6),
                      cancel_ms=distribution([r["cancel_ns"] for r in batch if r["cancel_ns"]], 1e6),
                      contained_query_ms=distribution([v for r in batch for v in queries[r["operation"]]], 1e6),
                      crossing_queries=sum(crossing[r["operation"]] for r in batch),
                      outside_queries=sum(outside[r["operation"]] for r in batch),
                      sampled_owned_peak_bytes=max(r["owned_sampled_peak"] for r in batch),
                      sampled_owned_increase_bytes=max(r["owned_sampled_peak"] - r["owned_base"] for r in batch),
                      sampled_running_peak=max(r["running_sampled_peak"] for r in batch),
                      sampled_queued_peak=max(r["queued_sampled_peak"] for r in batch))
        if source == "direct":
            result["spawn_call_per_process_ms"] = distribution([r["spawn_call_ns"] / r["files"] for r in batch], 1e6)
            result["child_lifetime_per_process_ms"] = distribution([r["child_lifetime_ns"] / r["files"] for r in batch], 1e6)
            result["collect_decode_per_process_ms"] = distribution([r["collect_decode_ns"] / r["files"] for r in batch], 1e6)
            result["first_checked_output_ms"] = result.pop("first_batch_ms")
            result["process_peak_semantics"] = "exact admitted/unreaped roots, not sampled"
        if source == "profile":
            result["fsx_stage_sum_ms"] = {k[4:]: distribution([r[k] for r in batch], 1e6)
                                           for k in batch[0] if k.startswith("fsx_") and k.endswith("_ns")}
            result["fsx_counters"] = {k[4:]: sorted({r[k] for r in batch})
                                      for k in batch[0] if k.startswith("fsx_") and not k.endswith("_ns")}
        if phase == "measure":
            result["files_per_second"] = statistics.median(r["files"] * 1e9 / duration
                                                            for r, duration in zip(batch, durations))
            result["mib_per_second"] = statistics.median(r["bytes"] * 1e9 / duration / 1048576
                                                          for r, duration in zip(batch, durations))
        groups.append(result)
    pairs = defaultdict(dict)
    for group in groups:
        if group["phase"] == "measure":
            pairs[group["corpus"], group["source"], group["pattern"]][group["parallel_files"]] = group
    ratios = [{"corpus": key[0], "source": key[1], "pattern": key[2],
               "serial_median_wall_div_parallel": modes[1]["wall_ms"]["median"] / modes[2]["wall_ms"]["median"]}
              for key, modes in sorted(pairs.items())]
    direct_comparisons = []
    indexed = {(g["corpus"], g["source"], g["pattern"], g["parallel_files"]): g
               for g in groups if g["phase"] == "measure"}
    for (corpus, source, pattern, parallel), direct in indexed.items():
        if source == "direct":
            scoped = indexed[corpus, "scope", pattern, parallel]
            direct_comparisons.append({"corpus": corpus, "pattern": pattern,
                                       "parallel_files": parallel,
                                       "fsx_scope_wall_div_direct": scoped["wall_ms"]["median"] / direct["wall_ms"]["median"]})
    profiling_comparisons = []
    for (corpus, source, pattern, parallel), profiled in indexed.items():
        if source == "profile":
            scoped = indexed[corpus, "scope", pattern, parallel]
            profiling_comparisons.append({"corpus": corpus, "pattern": pattern,
                                          "parallel_files": parallel,
                                          "profiled_wall_div_unprofiled": profiled["wall_ms"]["median"] / scoped["wall_ms"]["median"]})
    return {"metadata": metadata, "operations": len(rows), "matched_inputs": True,
            "balanced_positions": True, "groups": groups, "observed_ratios": ratios,
            "direct_comparisons": direct_comparisons, "profiling_comparisons": profiling_comparisons,
            "limitations": ["Warm uncontrolled OS cache; fixed small corpus; six measured scope/direct/profile samples and four live samples per mode/case.",
                            "Native validation and observer overhead are included in content wall time.",
                            "Query quantiles use only wholly contained observations; P50/P95/P99 are nearest-rank.",
                            "Allocation/process/queue peaks are sampled lower bounds, not RSS or child memory limits.",
                            "No cold-cache, 100k-file, batch-ugrep, CPU/RSS or production-tail-latency acceptance.",
                            "Cancellation starts at the first acquired confirmed file; these are not hung-child timings.",
                            "Hosted profiles and sanitizer timings must be analyzed separately; no speed threshold.",
                            "Private POSIX profiling is opt-in; stage spans overlap across file workers. File CPU excludes decode, producer, metadata-query and child CPU. File wall includes profiling calls; decode is separate.",
                            "POSIX direct controls reopen immutable fixture paths, use regular temporary output files and blocking waitpid; omit FSX identity/text/post-read checks, streaming backpressure and cooperative cancellation.",
                            "Direct spawn-call time is not a ready-to-match metric; child lifetime includes spawn, matching and reaping. Stage sums overlap across parallel workers and are not wall-time components. Per-process stage quantiles describe four per-run averages, not individual child samples."]}


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("measurements", type=Path)
    parser.add_argument("queries", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True, help="exact checkout revision; mark local edits explicitly")
    parser.add_argument("--capture-tools", action="store_true")
    args = parser.parse_args()
    result = summarize(args.measurements, args.queries)
    result["revision"] = args.revision
    result["host"] = {"system": platform.system(), "release": platform.release(),
                      "machine": platform.machine(), "logical_cpus_visible": os.cpu_count()}
    result["raw_sha256"] = {path.name: sha256(path) for path in (args.measurements, args.queries)}
    repo = Path(__file__).resolve().parent.parent
    result["source_sha256"] = {path: sha256(repo / path) for path in (
        "tests/content_benchmark_test.c", "tests/CMakeLists.txt", "tools/content_benchmark_report.py",
        "src/content/text.c", "src/content/content.h", "src/internal.h", "tests/content_text_test.c",
        "src/content/api.c", "src/content/parallel.c", "src/content/process.c",
        "src/content/process_windows.c", "src/content/discovery_posix.c")}
    if result["source_sha256"]["tests/content_benchmark_test.c"] != result["metadata"]["benchmark_source_sha256"]:
        raise ValueError("native benchmark was built from different source")
    result["tools_captured"] = args.capture_tools
    if args.capture_tools:
        result["tools"] = {}
        for tool in ("ugrep", "bfs"):
            if tool not in result["metadata"]:
                continue  # Windows has no BFS_LIVE benchmark.
            path = Path(result["metadata"][tool])
            completed = subprocess.run([str(path), "--version"], check=True,
                                       capture_output=True, text=True, timeout=15)
            result["tools"][tool] = {"path": str(path), "sha256": sha256(path),
                                     "version": completed.stdout.strip()}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"accepted {result['operations']} operations; balanced modes; summary: {args.output}")


if __name__ == "__main__":
    main()
