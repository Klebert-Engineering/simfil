"""Check measurement orchestration without native builds or performance assertions."""

import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("runtime_runner", Path(__file__).with_name("schema-runtime-benchmark.py"))
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class MatrixRunnerTest(unittest.TestCase):
    """Use deterministic executables to verify raw evidence, ordering, and failure handling."""

    def setUp(self):
        """Create independent fake binaries with valid role-specific manifests."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cpu = min(os.sched_getaffinity(0))
        self.binary = """#!/usr/bin/env python3
import json, sys
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
result = {key[2:]: value for key, value in args.items()}
for key in ['objects', 'branches', 'queries', 'warmups']:
    result[key] = int(result[key])
result['cpus'] = list(map(int, result['cpus'].split(',')))
latency = 100 if result['representation'] == 'callback' else 102
result['worker_us'] = [latency * (i + 1) for i in range(len(result['cpus']))]
result['max_worker_us'] = max(result['worker_us'])
print(json.dumps(result))
"""
        for role in ("reference", "candidate"):
            binary = self.root / role
            binary.write_text(self.binary)
            binary.chmod(0o755)
            data = {"binary_sha256": RUNNER.digest(binary), "revision": RUNNER.REFERENCE_COMMIT,
                    "source_status": "", "compiler": "test compiler", "harness_sha256": "same-source",
                    "compiler_options": {name: ["-O3", "-DNDEBUG", "-std=gnu++20"] for name in
                        ("src/model/model.cpp", "src/model/schema.cpp", "src/expressions.cpp")},
                    "configuration": {"CMAKE_BUILD_TYPE": "Release", "CMAKE_CXX_FLAGS": "",
                        "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG", "SIMFIL_SHARED": "OFF",
                        "SIMFIL_WITH_MODEL_JSON": "ON", "SIMFIL_PERF_REFERENCE": "ON" if role == "reference" else "OFF"}}
            if role == "candidate":
                options = data["compiler_options"].pop("src/model/schema.cpp")
                for name in ("schema-pool", "schema-index", "schema-query"):
                    data["compiler_options"][f"src/model/{name}.cpp"] = options.copy()
            Path(str(binary) + ".json").write_text(json.dumps(data))
        self.args = argparse.Namespace(reference=self.root / "reference", candidate=self.root / "candidate",
            output=self.root / "samples.jsonl", layout=[f"single:main:{self.cpu}"], rounds=2,
            objects=20, branches=4, queries=2, warmups=1, smoke=False, timeout=10,
            representation=["pool"], query=["basic"], limit=1.0, note="unit test")

    def run_matrix(self):
        """Read the durable journal without exposing fake timings as real benchmark evidence."""
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            RUNNER.MatrixRunner(self.args).run()
        return [json.loads(line) for line in self.args.output.read_text().splitlines()]

    def test_interleaved_order_and_regression(self):
        """Each candidate retains its own reference samples; odd rounds reverse ordering."""
        records = self.run_matrix()
        samples = [record for record in records if record["event"] == "sample"]
        self.assertEqual([record["role"] for record in samples], ["reference", "candidate", "candidate", "reference"])
        self.assertEqual([record["round"] for record in samples], [0, 0, 1, 1])
        row = records[-1]["rows"][0]
        self.assertEqual(row["reference_median_us"], 100)
        self.assertEqual(row["candidate_median_us"], 102)
        self.assertAlmostEqual(row["latency_change_percent"], 2)
        self.assertFalse(row["within_budget"])
        self.assertFalse(records[-1]["all_rows_within_budget"])

    def test_smoke_has_no_acceptance_claim(self):
        """A tiny correctness run cannot silently appear as passing performance evidence."""
        self.args.smoke = True
        records = self.run_matrix()
        self.assertEqual(records[0]["measurement_kind"], "smoke")
        self.assertEqual(records[0]["rounds"], 1)
        self.assertIsNone(records[-1]["all_rows_within_budget"])
        self.assertIsNone(records[-1]["rows"][0]["within_budget"])

    def test_existing_output_is_preserved(self):
        """Published evidence is never overwritten by reusing an output path."""
        self.args.output.write_text("existing evidence\n")
        with self.assertRaises(FileExistsError):
            self.run_matrix()
        self.assertEqual(self.args.output.read_text(), "existing evidence\n")

    def test_changed_binary_is_rejected(self):
        """A manifest does not identify an executable modified after linking."""
        self.args.candidate.write_text(self.binary + "# modified\n")
        with self.assertRaisesRegex(ValueError, "changed since"):
            RUNNER.MatrixRunner(self.args)

    def test_reference_revision_and_dirty_state_are_rejected(self):
        """An intermediate or edited baseline cannot be mistaken for the original revision."""
        path = Path(str(self.args.reference) + ".json")
        original = json.loads(path.read_text())
        for key, value in (("revision", "intermediate"), ("source_status", " M src/model/model.cpp")):
            with self.subTest(key=key):
                path.write_text(json.dumps({**original, key: value}))
                with self.assertRaisesRegex(ValueError, "unmodified original"):
                    RUNNER.MatrixRunner(self.args)

    def test_mismatched_flags_are_rejected(self):
        """Different optimization settings are not a paired implementation comparison."""
        path = Path(str(self.args.candidate) + ".json")
        data = json.loads(path.read_text())
        data["configuration"]["CMAKE_CXX_FLAGS_RELEASE"] = "-O0"
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "CMAKE_CXX_FLAGS_RELEASE"):
            RUNNER.MatrixRunner(self.args)

    def test_mismatched_ipo_and_target_options_are_rejected(self):
        """Global and target-local optimization differences cannot hide behind Release mode."""
        path = Path(str(self.args.candidate) + ".json")
        original = json.loads(path.read_text())
        for key in ("CMAKE_INTERPROCEDURAL_OPTIMIZATION", "CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE",
                    "CMAKE_CXX_STANDARD", "CMAKE_EXE_LINKER_FLAGS_RELEASE"):
            with self.subTest(key=key):
                data = json.loads(json.dumps(original))
                data["configuration"][key] = "different"
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(ValueError, key):
                    RUNNER.MatrixRunner(self.args)
        original["compiler_options"]["src/model/model.cpp"].append("-flto")
        path.write_text(json.dumps(original))
        with self.assertRaisesRegex(ValueError, "different options"):
            RUNNER.MatrixRunner(self.args)

    def test_compile_normalization_preserves_semantic_options(self):
        """Paths may differ, but macros, standards, tuning and optimization must still match."""
        entry = {"file": "/source/src/test.cpp", "command":
            'c++ -I/source/include -isystem "/some path/include" -DFLAG=1 -O3 -std=gnu++20 '
            '-flto -o object.o -c /source/src/test.cpp -MD -MF object.d'}
        self.assertEqual(RUNNER.compiler_options(entry), ["-DFLAG=1", "-O3", "-std=gnu++20", "-flto", "-c"])

    def test_moved_schema_sources_require_equivalent_options(self):
        """Retiring the old schema file must not bypass graph implementation flag checks."""
        path = Path(str(self.args.candidate) + ".json")
        original = json.loads(path.read_text())
        for name in ("schema-pool", "schema-index", "schema-query"):
            with self.subTest(source=name):
                data = json.loads(json.dumps(original))
                data["compiler_options"][f"src/model/{name}.cpp"].append("-flto")
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(ValueError, "schema sources compile with different options"):
                    RUNNER.MatrixRunner(self.args)
                del data["compiler_options"][f"src/model/{name}.cpp"]
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(ValueError, "Missing schema compilation commands"):
                    RUNNER.MatrixRunner(self.args)

    def test_manifest_records_deleted_sources(self):
        """A tracked deletion stays in the source digest rather than crashing or disappearing."""
        source, build = self.root / "source", self.root / "build"
        (source / "include").mkdir(parents=True)
        build.mkdir()
        present = source / "include/current.h"
        present.write_text("current header\n")
        (build / "CMakeCache.txt").write_text("CMAKE_CXX_COMPILER:FILEPATH=c++\n")
        (build / "compile_commands.json").write_text("[]")
        def command(args, cwd=None):
            if args[1:2] == ["ls-files"]:
                return "include/current.h\0include/retired.h\0"
            if args[1:2] == ["status"]:
                return " D include/retired.h"
            return "test compiler" if args[0] == "c++" else "test revision"
        with patch.object(RUNNER, "command", side_effect=command):
            RUNNER.manifest(argparse.Namespace(source=source, build=build, binary=self.args.candidate))
        data = json.loads(Path(str(self.args.candidate) + ".json").read_text())
        self.assertEqual(data["source_files"]["include/current.h"], RUNNER.digest(present))
        self.assertIn("include/retired.h", data["source_files"])
        self.assertIsNone(data["source_files"]["include/retired.h"])

    def test_incorrect_workload_keeps_partial_journal(self):
        """A failed subprocess aborts instead of producing a misleading success summary."""
        bad = self.binary.replace("result['cpus'] =", "result['queries'] += 1\nresult['cpus'] =")
        self.args.candidate.write_text(bad)
        manifest = Path(str(self.args.candidate) + ".json")
        data = json.loads(manifest.read_text())
        data["binary_sha256"] = RUNNER.digest(self.args.candidate)
        manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "requested queries"):
            self.run_matrix()
        records = [json.loads(line) for line in self.args.output.read_text().splitlines()]
        self.assertEqual([record["event"] for record in records], ["metadata", "sample"])


if __name__ == "__main__":
    unittest.main()
