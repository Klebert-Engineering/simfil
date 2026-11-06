# Schema Runtime Measurements

`schema-runtime-benchmark.cpp` is one fixture compiled against both the original
`77fe792588ce34653998984b0df967f59b84ec04` revision and the candidate. The reference
uses its callback interface; candidate modes retain exact canonical `SchemaPool`
owners through typed views. There is no second candidate registry backend. Ordinary
leaves remain untagged in every mode. The original reference has no scalar-type
provenance, so this measures the total implementation cost, not just one lookup.

Compilation, graph finalization, model construction, expression binding and
warmup are outside timing. Each worker owns its model, environment and retained
`BoundExpression`, sharing only the immutable AST and schema graph. Even the
single-worker case executes in a real thread, activating normal threaded-process
reference counting. Every evaluation checks the expected result count.
Untimed proof queries additionally check scalar provenance against the exact
pool and verify that separate dictionaries really have different field IDs.
The main thread pins to the first requested CPU before constructing the graph
or any main-thread models; early workers wait until peers finish before teardown.

## Build Both Versions

Run from the candidate simfil checkout, with an initialized `cmake/CPM.cmake`
submodule. A separate detached worktree avoids changing the current branch:

```bash
root="$PWD"
git worktree add --detach build-release/schema-reference-source \
  77fe792588ce34653998984b0df967f59b84ec04

cmake -S docs/performance -B build-release/runtime-reference -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSIMFIL_SOURCE_DIR="$root/build-release/schema-reference-source" \
  -DSIMFIL_CPM_SOURCE="$root/cmake/CPM.cmake" \
  -DSIMFIL_PERF_REFERENCE=ON
cmake --build build-release/runtime-reference --parallel 4

cmake -S docs/performance -B build-release/runtime-candidate -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSIMFIL_SOURCE_DIR="$root" \
  -DSIMFIL_CPM_SOURCE="$root/cmake/CPM.cmake"
cmake --build build-release/runtime-candidate --parallel 4
```

CPM accepts the usual `CPM_SOURCE_CACHE` or individual package-source overrides
for reusing existing dependencies. Use the same compiler and optimization flags
for both builds. Do not edit either source tree during compilation. The post-link
manifest records source hashes (including uncommitted/new candidate code), exact
revision, compiler/options, compilation-database hash and executable hash. The
runner rejects a modified reference, mismatched optimization/IPO/linker settings
or fixture, unequal normalized compiler options on common library sources, and
executables changed since their manifests were written.

## Check Correctness

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s docs/performance -p 'test_schema_runtime_runner.py'

python3 docs/performance/schema-runtime-benchmark.py run \
  --reference build-release/runtime-reference/schema-runtime-benchmark \
  --candidate build-release/runtime-candidate/schema-runtime-benchmark \
  --output build-release/runtime-smoke.jsonl --smoke
```

Smoke mode uses a tiny workload and one round. It exercises every requested
representation/query/layout but deliberately emits no performance-acceptance
claim. Neither smoke results nor the one-sample Catch2 correctness runs are
timing evidence.

## Collect Evidence

Run on otherwise idle Linux hardware, without concurrent builds, tests, profiling
or data conversion. Record power/thermal controls using `--note`. The defaults
reproduce the original machine's five placements: CPU 4 alone, CPUs 4/5/6/7 on
two SMT cores, and CPUs 4/6/8/10 on four physical cores; parallel layouts cover
both main-thread and worker-local model construction. Check the recorded Linux
topology: those CPU numbers do **not** imply the same placement on another host.
Override layouts with repeated `--layout NAME:main|worker:CPU[,CPU...]` arguments.

```bash
python3 docs/performance/schema-runtime-benchmark.py run \
  --reference build-release/runtime-reference/schema-runtime-benchmark \
  --candidate build-release/runtime-candidate/schema-runtime-benchmark \
  --output build-release/runtime-measurements.jsonl \
  --note 'Record actual idle/power/thermal conditions here'
```

Defaults are seven rounds, 2,000 objects, 32 branches, three warmups and 200 queries
per worker. The matrix contains basic pruning, field plans and exact paths for
each of `pool` and `pool-separate`. Both use the sole canonical SchemaPool;
`pool-separate` has independent runtime/schema dictionaries, while its reference
uses the original shared dictionary. The optional `environment` control attaches
only an Environment fallback root, not model-owned scalar provenance. It is not a
substitute for either owner-retaining mode. The historical `bound`/`separate`
candidate registry modes were removed with that implementation, not relabeled
as additional independent backends.

Each candidate row gets its own interleaved reference samples, reversing order
in odd rounds. All worker latencies are flushed to an append-only JSONL journal
after every subprocess, so an interrupted run retains its completed evidence.
Existing outputs are never overwritten. The summary compares the **median of
the slowest worker's mean latency per round**, retaining sample ranges and raw
worker arrays; it is not aggregate throughput. A default 1% budget is a descriptive
threshold, not a noisy CI assertion or proof about other workloads/platforms.
Review sample variance and repeat suspicious rows rather than treating one
threshold crossing as statistical certainty.

Each subprocess also records the existing retained-capacity lower bounds for
model columns and dictionaries, after timing. Shared schema dictionaries count
once; separate runtime dictionaries count per model. Candidate pool modes report
generic type/index storage and primitive columns separately. The independent
`ObjectSchema` registry has no complete retained-memory API, so its graph value
is explicitly `null`, not zero. Model-column comparisons are useful but **do not
close the schema-owner memory comparison against the pre-cutover implementation**.
Neither these counters nor a leak-free Memcheck run measure allocator/RSS totals.

The historical `schema-runtime-2026-11-02.json` predates this persistent harness
and compact-pool extraction. Its four-worker SMT basic-pruning regression remains
open until a fresh, comparable measurement establishes otherwise. Adding this
runner does not itself change that result.
