#!/usr/bin/env python3
"""Record reproducible, interleaved schema-runtime comparisons without hiding raw samples."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import sys
import time


REFERENCE_COMMIT = "77fe792588ce34653998984b0df967f59b84ec04"
HERE = Path(__file__).resolve().parent
BUILD_OPTIONS = ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_CXX_STANDARD",
                 "CMAKE_CXX_EXTENSIONS", "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
                 "CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE", "CMAKE_EXE_LINKER_FLAGS",
                 "CMAKE_EXE_LINKER_FLAGS_RELEASE", "CMAKE_STATIC_LINKER_FLAGS",
                 "CMAKE_STATIC_LINKER_FLAGS_RELEASE", "SIMFIL_SHARED", "SIMFIL_WITH_MODEL_JSON")


def digest(path):
    """Hash actual file bytes, including uncommitted candidate sources."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(args, cwd=None):
    """Run one checked command without a shell or inherited output in the JSON log."""
    return subprocess.check_output(args, cwd=cwd, text=True).rstrip("\n")


def compiler_options(entry):
    """Remove only path-dependent input/output/include arguments from an actual compile command."""
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    options = []
    iterator = iter(arguments[1:])
    for argument in iterator:
        if argument in ("-o", "-I", "-isystem", "-iquote", "-idirafter", "-MF", "-MT", "-MQ"):
            next(iterator)
        elif argument == entry["file"] or argument.startswith("-I") or argument in ("-MD", "-MMD"):
            continue
        else:
            options.append(argument)
    return options


def manifest(args):
    """Capture the exact linked executable and its checkout/build inputs beside the binary."""
    source, build, binary = args.source.resolve(), args.build.resolve(), args.binary.resolve()
    paths = ["include", "src", "CMakeLists.txt", "cmake/deps.cmake"]
    names = command(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *paths], source)
    files = {name: digest(source / name) for name in sorted(set(names.split("\0"))) if name}
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    keys = ["CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "SIMFIL_PERF_REFERENCE", *BUILD_OPTIONS]
    profiles = {}
    for entry in json.loads((build / "compile_commands.json").read_text()):
        path = Path(entry["file"]).resolve()
        if path.is_relative_to(source / "src"):
            profiles[str(path.relative_to(source))] = compiler_options(entry)
    result = {
        "format": 1, "binary_sha256": digest(binary), "source": str(source),
        "revision": command(["git", "rev-parse", "HEAD"], source),
        "source_status": command(["git", "status", "--porcelain", "--", *paths], source),
        "source_files": files,
        "source_sha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
        "harness_sha256": digest(HERE / "schema-runtime-benchmark.cpp"),
        "compiler": command([cache["CMAKE_CXX_COMPILER"], "--version"]).splitlines()[0],
        "configuration": {key: cache.get(key) for key in keys},
        "compiler_options": profiles,
        "compile_commands_sha256": digest(build / "compile_commands.json"),
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    destination = Path(str(binary) + ".json")
    temporary = destination.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(result, indent=2) + "\n")
    temporary.replace(destination)


class MatrixRunner:
    """Own one paired run, its immutable inputs, and its append-only measurement journal."""

    def __init__(self, args):
        """Validate identities and CPU layouts before creating any measurement output."""
        self.args = args
        self.binaries = {"reference": args.reference.resolve(), "candidate": args.candidate.resolve()}
        self.manifests = {}
        for role, binary in self.binaries.items():
            data = json.loads(Path(str(binary) + ".json").read_text())
            if data["binary_sha256"] != digest(binary):
                raise ValueError(f"{role} binary changed since its build manifest")
            if data["configuration"]["CMAKE_BUILD_TYPE"] != "Release":
                raise ValueError(f"{role} is not a Release build")
            self.manifests[role] = data
        reference, candidate = self.manifests["reference"], self.manifests["candidate"]
        if reference["revision"] != REFERENCE_COMMIT or reference["source_status"]:
            raise ValueError("Reference must be an unmodified original 77fe792 checkout")
        if reference["configuration"]["SIMFIL_PERF_REFERENCE"] != "ON" or candidate["configuration"]["SIMFIL_PERF_REFERENCE"] != "OFF":
            raise ValueError("Reference/candidate build roles are incorrect")
        for key in ("compiler", "harness_sha256"):
            if reference[key] != candidate[key]:
                raise ValueError(f"Reference and candidate differ in {key}")
        for key in BUILD_OPTIONS:
            if reference["configuration"].get(key) != candidate["configuration"].get(key):
                raise ValueError(f"Reference and candidate differ in {key}")
        common = reference["compiler_options"].keys() & candidate["compiler_options"].keys()
        if not {"src/model/model.cpp", "src/model/schema.cpp", "src/expressions.cpp"} <= common:
            raise ValueError("Missing core compilation commands in build manifest")
        for source in common:
            if reference["compiler_options"][source] != candidate["compiler_options"][source]:
                raise ValueError(f"Reference and candidate compile {source} with different options")
        self.layouts = []
        available = os.sched_getaffinity(0)
        for entry in args.layout or ["single:main:4", "smt:main:4,5,6,7", "physical:main:4,6,8,10",
                                     "smt:worker:4,5,6,7", "physical:worker:4,6,8,10"]:
            name, construction, raw_cpus = entry.split(":")
            cpus = [int(cpu) for cpu in raw_cpus.split(",")]
            if not name or construction not in ("main", "worker") or not cpus or len(cpus) > 16 or len(set(cpus)) != len(cpus) or not set(cpus) <= available:
                raise ValueError(f"Invalid/unavailable layout: {entry}")
            if any(layout["name"] == name and layout["construction"] == construction for layout in self.layouts):
                raise ValueError(f"Duplicate layout: {entry}")
            self.layouts.append({"name": name, "construction": construction, "cpus": cpus})
        self.rounds = 1 if args.smoke else args.rounds
        self.workload = {"objects": 20, "branches": 4, "queries": 2, "warmups": 1} if args.smoke else {
            "objects": args.objects, "branches": args.branches, "queries": args.queries, "warmups": args.warmups}
        if self.rounds < 1 or min(self.workload.values()) < 1:
            raise ValueError("Rounds and workload sizes must be positive")
        self.samples = {}

    def topology(self):
        """Record actual Linux topology instead of assuming layout labels prove core placement."""
        result = {}
        for cpu in sorted({cpu for layout in self.layouts for cpu in layout["cpus"]}):
            base = Path(f"/sys/devices/system/cpu/cpu{cpu}")
            result[cpu] = {}
            for field in ("topology/core_id", "topology/physical_package_id", "topology/thread_siblings_list",
                          "cpufreq/scaling_governor", "cpufreq/scaling_driver"):
                path = base / field
                result[cpu][field] = path.read_text().strip() if path.exists() else None
        return result

    @staticmethod
    def write(stream, record):
        """Make each completed subprocess durable even if a later measurement is interrupted."""
        stream.write(json.dumps(record, sort_keys=True) + "\n")
        stream.flush()
        os.fsync(stream.fileno())

    def measure(self, role, layout, representation, query):
        """Execute one pinned workload and reject malformed, incomplete, or failed results."""
        options = {**self.workload, "cpus": ",".join(map(str, layout["cpus"])),
                   "construction": layout["construction"], "query": query,
                   "representation": "callback" if role == "reference" else representation}
        args = [str(self.binaries[role])]
        for key, value in options.items():
            args.extend(["--" + key, str(value)])
        process = subprocess.run(args, check=True, text=True, capture_output=True, timeout=self.args.timeout)
        result = json.loads(process.stdout)
        for key, value in options.items():
            if result[key] != (layout["cpus"] if key == "cpus" else value):
                raise ValueError(f"Benchmark did not execute requested {key}")
        workers = result["worker_us"]
        if len(workers) != len(layout["cpus"]) or any(not math.isfinite(value) or value <= 0 for value in workers) or result["max_worker_us"] != max(workers):
            raise ValueError("Invalid worker measurements")
        return result

    def run(self):
        """Alternate ordering per round, retaining separate reference samples for every candidate."""
        # Exclusive creation protects published evidence from accidental replacement.
        with self.args.output.open("x") as stream:
            self.write(stream, {"event": "metadata", "format": 1,
                "measurement_kind": "smoke" if self.args.smoke else "paired-runtime",
                "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "platform": platform.platform(), "available_cpus": sorted(os.sched_getaffinity(0)),
                "topology": self.topology(), "builds": self.manifests,
                "runner_sha256": digest(Path(__file__)), "rounds": self.rounds,
                "workload": self.workload, "layouts": self.layouts,
                "representations": self.args.representation, "queries": self.args.query,
                "statistic": "median of maximum per-worker mean latency per round",
                "order": "reference/candidate per row, reversed in odd rounds",
                "note": self.args.note, "regression_limit_percent": self.args.limit})
            for round_index in range(self.rounds):
                for layout in self.layouts:
                    for representation in self.args.representation:
                        for query in self.args.query:
                            key = (layout["name"], layout["construction"], representation, query)
                            samples = self.samples.setdefault(key, {"reference": [], "candidate": []})
                            order = ("reference", "candidate") if round_index % 2 == 0 else ("candidate", "reference")
                            for role in order:
                                result = self.measure(role, layout, representation, query)
                                samples[role].append(result["max_worker_us"])
                                self.write(stream, {"event": "sample", "round": round_index,
                                    "layout": layout["name"], "candidate_representation": representation,
                                    "role": role, "load_average_after": os.getloadavg(), "result": result})
                            print(f"round {round_index + 1}/{self.rounds}: {'/'.join(key)}", file=sys.stderr)
            rows = []
            for key, samples in self.samples.items():
                reference = statistics.median(samples["reference"])
                candidate = statistics.median(samples["candidate"])
                change = 100 * (candidate / reference - 1)
                rows.append({"layout": key[0], "construction": key[1], "representation": key[2], "query": key[3],
                    "reference_median_us": reference, "candidate_median_us": candidate,
                    "reference_range_us": [min(samples["reference"]), max(samples["reference"])],
                    "candidate_range_us": [min(samples["candidate"]), max(samples["candidate"])],
                    "latency_change_percent": change,
                    "within_budget": None if self.args.smoke else candidate <= reference * (1 + self.args.limit / 100)})
            self.write(stream, {"event": "summary", "rows": rows,
                "all_rows_within_budget": None if self.args.smoke else all(row["within_budget"] for row in rows)})
        print(json.dumps(rows, indent=2))


def main():
    """Separate build-manifest capture from deliberately scheduled runtime measurement."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("manifest", help="Called by CMake after linking")
    for name in ("binary", "source", "build"):
        build.add_argument("--" + name, required=True, type=Path)
    run = commands.add_parser("run", help="Run an interleaved matrix on otherwise idle hardware")
    for name in ("reference", "candidate", "output"):
        run.add_argument("--" + name, required=True, type=Path)
    run.add_argument("--layout", action="append", help="Repeat NAME:main|worker:CPU[,CPU...]; defaults to original five layouts")
    run.add_argument("--representation", nargs="+", choices=["callback", "bound", "separate", "pool", "pool-separate"],
                     default=["bound", "separate", "pool", "pool-separate"])
    run.add_argument("--query", nargs="+", choices=["basic", "plans", "exact"], default=["basic", "plans", "exact"])
    for name, default in (("rounds", 7), ("objects", 2000), ("branches", 32), ("queries", 200), ("warmups", 3), ("timeout", 180)):
        run.add_argument("--" + name, type=int, default=default)
    run.add_argument("--limit", type=float, default=1.0, help="Latency regression budget in percent")
    run.add_argument("--smoke", action="store_true", help="Tiny correctness matrix; never reports performance acceptance")
    run.add_argument("--note", default="", help="Record power/thermal controls and other run conditions")
    args = parser.parse_args()
    if args.command == "manifest":
        manifest(args)
    else:
        MatrixRunner(args).run()


if __name__ == "__main__":
    main()
