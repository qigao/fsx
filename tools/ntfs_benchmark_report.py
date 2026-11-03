"""Summarize one controlled-replay run; keep hosted runs separate.

Usage: python tools/ntfs_benchmark_report.py REPLAY.csv QUERIES.csv
Outputs JSON with exact sample counts and nearest-rank P95. No performance
thresholds or statistical significance are inferred.
"""

import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        yield from csv.DictReader(line for line in stream if not line.startswith("#"))


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return {"n": 0}
    return {
        "n": len(ordered),
        "p50_ms": statistics.median(ordered) / 1e6,
        "p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1] / 1e6,
        "min_ms": ordered[0] / 1e6,
        "max_ms": ordered[-1] / 1e6,
    }


def summarize(measurement_path, query_path):
    accepted = [line for line in Path(measurement_path).read_text(encoding="utf-8").splitlines()
                if line.startswith("# acceptance=passed operations=")]
    if len(accepted) != 1:
        raise ValueError("expected one completed replay acceptance")
    samples = list(rows(measurement_path))
    if int(accepted[0].split("operations=")[1]) != len(samples):
        raise ValueError("acceptance operation count does not match CSV")
    inputs = defaultdict(set)
    positions = defaultdict(list)
    groups = defaultdict(list)
    operations = set()
    for row in samples:
        key = row["mode"], row["phase"]
        groups[key].append(row)
        inputs[row["phase"], row["sample"]].add(row["input_hash"])
        operation = row["mode"], row["round"], row["phase"], row["sample"]
        if operation in operations:
            raise ValueError(f"duplicate operation: {operation}")
        operations.add(operation)
        if row["phase"] == "build":
            positions[row["mode"]].append(int(row["position"]))
    if any(len(value) != 1 for value in inputs.values()):
        raise ValueError("input fingerprints differ between configurations/rounds")
    if len(positions) != 4 or any(sorted(value) != [0, 1, 2, 3] for value in positions.values()):
        raise ValueError("expected four modes, each occupying every position once")

    queries = defaultdict(list)
    crossing = defaultdict(int)
    outside = defaultdict(int)
    for row in rows(query_path):
        mode, round_number, phase = row["phase"].split("/")
        operation = mode, round_number, phase, row["sample"]
        if operation not in operations:
            raise ValueError(f"query has no measured index operation: {operation}")
        if int(row["status"]) != 0:
            raise ValueError(f"failed query: {row}")
        start, end = int(row["query_start_ns"]), int(row["query_end_ns"])
        index_start, index_end = int(row["index_start_ns"]), int(row["index_end_ns"])
        if start <= end and index_start <= start and end <= index_end:
            queries[mode, phase].append(end - start)
        elif start < index_end and end > index_start:
            crossing[mode, phase] += 1
        else:
            outside[mode, phase] += 1

    stages = defaultdict(lambda: defaultdict(list))
    saturated = 0
    with Path(measurement_path).open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("# stages "):
                fields = dict(item.split("=", 1) for item in line.split()[2:])
                for name, value in fields.items():
                    if name.endswith("_ns") or name.endswith("_waits"):
                        stages[fields["mode"], fields["phase"]][name].append(int(value))
            if line.startswith("# concurrent "):
                fields = dict(item.split("=", 1) for item in line.split()[2:])
                saturated += int(fields["samples"]) == int(fields["capacity"])

    result = {"operations": len(samples), "matched_inputs": True,
              "balanced_positions": True, "saturated_operations": saturated, "modes": {}}
    for (mode, phase), batch in groups.items():
        result["modes"].setdefault(mode, {})[phase] = {
            "wall": distribution([int(row["wall_ns"]) for row in batch]),
            "caller": distribution([int(row["submit_ns"]) for row in batch]),
            "work_write_calls": sum(int(row["work_write_calls"]) for row in batch),
            "work_write_bytes": sum(int(row["work_write_bytes"]) for row in batch),
            "work_pages_range": [min(int(row["work_pages"]) for row in batch),
                                 max(int(row["work_pages"]) for row in batch)],
            "stage_median_ns": {name: statistics.median(values)
                                for name, values in stages[mode, phase].items()},
            "contained_queries": distribution(queries[mode, phase]),
            "crossing_queries": crossing[mode, phase],
            "outside_queries": outside[mode, phase],
        }
    return result


if __name__ == "__main__":
    print(json.dumps(summarize(*sys.argv[1:]), indent=2))
