# Schema Domains And Query Views

Simfil uses one typed schema graph for query optimization and completion.
`SchemaModel` exposes that same graph as lazy metadata objects for ordinary
Simfil queries. Completion does not evaluate those descriptors or construct
sample feature objects.

## Domain Contract

`Schema::Kind` is a 32-bit packed value. Its upper 16 bits are a **static**
kind-name `StringId`; its lower 16 bits describe possible runtime `ValueType`s.
Use `Schema::makeKind`, `kindNameId`, `affinities`, `hasAffinity`, and
`valueTypeAffinity`. Only affinity bits are OR-combinable; never OR whole kinds.
`ValueType` itself remains an ordinary enum.

Core kinds include unknown, any, never, untyped scalar (`Value`), null, boolean,
integer, float, string, bytes, bitmask, object, array, union, exclusive union, and
intersection. An external kind such as `Feature` can have object affinity and
provide fields through the same typed interface. Generic consumers do not
require inheritance from `ObjectSchema` or knowledge of that name.

Extensions allocate names between `StringPool::NextStaticId` and
`StringPool::FirstDynamicId` (now 256). Register identical static mappings in
every participating pool. This changes serialized string assignments; consumers
must coordinate their protocol version rather than mix old and new dictionaries.
Kind names must not be dynamically interned datasource-specific IDs. `typename`
is a separate optional concrete producer type name, not the kind's spelling.

The distinctions are intentional:

- `Unknown` means unavailable information; `Any` means an unrestricted domain.
  Neither proves a descendant field absent. `Never` means no possible value.
- An empty `ObjectSchema` is a known closed object. Use `setOpen(true)` for
  undeclared members. A field with no child schema, `NoSchemaId`, or an unresolved
  schema reference is unknown, **not** an implicitly scalar leaf. Openness does
  not weaken explicitly declared fields; an undeclared open alternative still
  prevents guessing one exact domain for an ambiguous union.
- An array's element alternatives describe possible types, not length or tuple
  positions. An unspecified element domain is unknown.
- `CombinedSchema` retains `AnyOf`, `OneOf`, or `AllOf` at the same value position.
  Empty AnyOf/OneOf is impossible; empty AllOf is unrestricted. Affinity summarizes
  alternatives but does not replace their logical operator.
- `fieldRequired` belongs to the parent-field edge: true, false, or unknown.
  `nullable` belongs to the value domain and independently allows/excludes null.
- All string enums/constants use `addEnumSymbol` / `directEnumSymbols` in the
  binding's pool. `enumValues` / `addEnumValue` are for non-string scalar literals;
  the concrete builder rejects both string and string_view values there. This
  keeps descriptor output, completion, and symbol rewrites on one string index.
  Named numeric constants and flags use the separate metadata interface below;
  a few named sentinels do not make a numeric range an exhaustive enum.

This is not a JSON Schema validator. In particular, following an intersection's
fields returns a conservative set of candidate domains, not the complete logical
intersection of every scalar constraint.

## Ownership And Indexes

The producer owns schema definitions; `Environment::querySchemaCallback` supplies
the binding. There is no additional registry in Simfil. Use the same namespace
for a schema binding, its query environment, and its descriptor model. For
completion/schema inspection, construct a request-local namespace and return
owning candidate strings. Do not modify a datasource's authoritative pool.

After building a graph, finalize its schemas. Reachability and wildcard plans are
derived indexes. `reachabilityComplete()` is a proof, not a synonym for
`finalized()`: open, dirty, or incomplete graphs must remain conservative.
Custom adapters expose fields/elements through the public enumeration methods;
an adapter with an incomplete cached index must not claim completeness.
Sparse wildcard plans use this generic interface, not concrete builder RTTI.

Treat a published graph as immutable. Mutations update a concrete schema's local
revision, but cannot automatically invalidate caches on all referring schemas.
Owners must rebuild/invalidate dependent bindings and indexes after graph edits.

`SchemaBinding` retains the immutable graph owner, exact content identity, lookup
and schema dictionary. `SchemaReference` combines that binding with a local
16-bit `SchemaId`. Different graphs may reuse the same local IDs. Runtime field
dictionaries may differ from the schema dictionary: scalar extraction and bound
wildcard plans translate names rather than interpreting foreign IDs as local ones.
Wildcard plan caches retain each graph independently.

`ModelPool::freeze()` seals the pool and its shared `StringPool`. Factories,
binary reads, rebinding and mutations through retained Object/Array nodes then
fail. Derived schema owners must also guard their own columns. Freeze before
publication, without concurrent writes or outstanding low-level mutable storage
borrows; it does not revoke arbitrary C++ references. Copying a frozen StringPool
creates a mutable private dictionary suitable for compilation/completion.

## Named Scalar Metadata

Adapters implement `forEachNamedValue`, `numericDomain`, and `bitWidth` directly
over their owning graph. Views borrow storage from that graph; they must not
reference callback-local strings or ranges. Names have exact spelling and may
have aliases. A named value includes its raw boolean/integer/unsigned/float/string
literal, role (`constant`, `special`, `flag`), meaning (`ordinary`, `unknown`,
`variable`, `unbounded`, `saturated`), and deprecated/removed status. Numeric
metadata separately describes encoding, bounds, ordinary ranges, unit, scale,
offset and completeness. Unknown numeric codes remain ordinary raw numbers.

Schema mode rewrites standalone named values to predicates on declared paths.
Numeric names use equality; flags use all-bits membership. Contextual comparisons
such as `speed = SPEED_UNLIMITED` resolve the name against the **selected value's
actual domain**, not a global symbol table or a union of possible raw codes.
Missing or ambiguous domain provenance is an error rather than a guessed profile.
Quoted strings remain strings; fields and environment constants retain precedence.
Incomplete/cyclic path sets use bounded runtime schema traversal, retaining the
owning dictionary even through descendants with no unique domain. Completion uses
the same metadata and scopes named suggestions to the other comparison operand.

Values extracted from a concrete parent field or array-element edge retain an
optional `SchemaReference`. Int/Float/String categories do not change. Identity
and selection operations, traces, and the first min/max winner on a tie retain
the reference. Arithmetic, casts, comparisons, new literals and ordinary numeric
sum/count construct unannotated values, including `+ 0`. A custom selecting fold
preserves whichever Value it returns. Null and undefined never acquire a domain
from their parent. Ordinary stored leaves remain compact and unannotated.

`ValueType::Bitmask` instead carries intrinsic uint64 bits and exact domain
identity. Persistent mask rows contain only uint64 bits and a local SchemaId;
the common binding table owns the graph. `hasFlag(mask, 'NAME')` tests all
declared bits; a named zero matches exactly zero. Unknown flags, incompatible
domains, negative/float coercions and invalid shifts are errors. Unknown active
bits are retained. Complement and shifts respect the declared width. Use
`bitmask(existingMask, '0x8000000000000000')` for lossless full-width construction;
ordinary signed integer literals reject overflow.

The exact JSON tag is
`{"_bitmask":true,"bits":"0x8000000000000000","schema":"exact-graph-token","domain":4}`.
Import requires that exact graph through the model binding/resolver. Objects with
additional keys remain application objects. Mask JSON never uses floating-point
numbers or labels as a substitute for raw bits.

Exact path enumeration accepts an optional `bool* complete` output. Active-path
SchemaIds detect cycles; reused definitions in sibling branches and repeated
field names are valid. Enumeration also has depth/work caps. Incomplete enum
path sets use a schema-guided predicate over actual data instead of an incorrect
finite OR that misses deeper matches. That predicate matches only declared enum
positions, not unrelated strings, and static path analysis reports unresolved
access. Its compiled root SchemaId must refer to the same logical graph when
the AST is rebound.

Field-path enumeration prunes branches only when a finalized, complete
reachability index proves that the requested field is absent. This prevents
unrelated shared graphs from consuming the traversal budget repeatedly. Adapter
`canHaveField` implementations must remain conservative for lookup aliases as
well as canonical names, including aliases reachable through descendants.
`referencedSchemaPaths` keeps any discovered paths but sets
`hasUnresolvedAccess` when enumeration is incomplete; a partial path list is
not an exhaustive ownership or scope proof.

Read-only schema bindings can override `hasDirectEnumSymbol(text, pool)` when
declared enum strings do not exist in the runtime dictionary. Recursive/open
schema enum predicates use this textual hook; they must not intern datasource
metadata or reuse compilation-pool IDs in another environment.

## Lazy Descriptors

```cpp
#include <simfil/model/schema-model.h>

// strings and lookup belong to this request's schema binding. The closure must
// capture the graph owner by shared ownership, not a temporary Environment&.
auto descriptors = std::make_shared<simfil::SchemaModel>(strings, lookup, 10000);
auto root = descriptors->root(rootSchemaId);
simfil::Environment env(descriptors->strings());
auto query = simfil::compile(env, "fields.items.elements[0].kind", false);
```

The constructor is
`SchemaModel(shared_ptr<StringPool>, Schema::Lookup, maxNodes=10000)`.
The lookup closure and pool outlive all returned nodes through the model owner.
The graph and namespace must not change during the view's lifetime. Views are
request-local and not concurrently mutable. The adapter never interns strings.

Descriptors are ordinary objects, even for represented scalar/array domains:

```json
{
  "kind": "Feature",
  "$ref": 1,
  "typename": "RoadLink",
  "fields": {
    "items": {
      "kind": "array",
      "$ref": 2,
      "required": false,
      "elements": [{"kind": "string", "$ref": 3, "enum": ["urban", "rural"]}]
    }
  },
  "open": false
}
```

Explicit combiners use `alternatives`, not `elements`. Multiple domain IDs on a
field edge are an implicit union. Unknown optional metadata is omitted. Field
names are under `fields`, so they cannot collide with descriptor metadata names.
`get`, `at`, `keyAt`, `iterate`, and `toJson` agree; `kind` is a string through all
of them. Descriptor `schema()` is `NoSchemaId`, not the represented domain ID.
This prevents feature-field pruning from hiding descriptor metadata.

Scalar descriptors additionally expose `namedValues`, `numericDomain`, and
`bitWidth` when applicable. Named entries have `name`, `value`, `role`, `meaning`,
`deprecated`, and `removed`. Numeric metadata has `encoding`, `unit`, optional
`minimum`/`maximum`, `ordinaryRanges` (`min`/`max`), `scale`, `offset`, and
`complete`. Flag values are hexadecimal strings; unsigned numbers outside the
signed scalar range are decimal strings. These are lazy views of the same graph,
not a second registry or JSON model.

Every known descriptor exposes its numeric identity as `$ref`, including
expanded definitions. An implicit union exposes an array of IDs, such as
`{"kind":"union","$ref":[2,3]}`; each ID can be used as a new descriptor root.
Cycles stop expansion with a reference such as
`{"kind":"object","$ref":1,"truncated":"cycle"}`. Allocation exhaustion
uses `{"kind":"unknown","truncated":"node-budget"}`. A stop marker is not
an empty definition. The model does not impose a schema-depth cutoff before query
evaluation. `exhausted()` reports any allocation-budget stop, even when a scalar
projection never emits its marker; callers must mark such query results incomplete.
Limits count lazy views, not serialized bytes; callers still need bounded evaluation
and output serialization with stack-safety guards. Repeated metadata
access reuses child views. `materializedNodeCount()` includes the terminal budget
marker, and `maxNodes` must be at least two.

## Direct Completion

```cpp
auto candidates = simfil::complete(env, "items[17].na", 12, rootSchemaId, options);
```

This overload takes a `SchemaId` instead of a `ModelNode`. It reuses tokenization,
the relaxed parser, completion AST, smart-case filtering, and replacement ranges.
It does not execute constant-folding or registered functions. Unfinished string
literals are accepted only for schema completion; compilation remains strict.

Field access follows member domains, arbitrary positive/negative array indices
follow element domains, and logical alternatives do not insert a fictional array
level. Filtering preserves the input domain. AND/OR inspect cursor-local branches
independently of sample truth values. Comparisons offer the left operand's enum
choices; unknown functions/accesses stay unknown instead of inventing fields.
Root-owned operand alias hooks have the same precedence/scope as compilation;
explicit paths do not reinterpret member names as aliases.

Model lookup aliases are separate: an adapter can override
`Schema::canonicalField` to resolve a member name as its runtime model does,
without duplicating enumerated field paths. Member-domain resolution, field path
discovery, wildcard direct-field plans and concrete/recursive `referencedSchemaPaths`
analysis use that hook. Adapters must also keep `canHaveField` conservative for
lookup aliases reachable in descendants; a canonical-only name index cannot
prove an alias absent. For example, mapget features
resolve `attributes` to `properties` unless a real `attributes` member is declared.

`limit`, `timeoutMs`, `maxSchemaVisits` (default 10000), and `maxSchemaDepth`
(AST inference depth, default 128) bound candidate/traversal work. Registry
callbacks must also be cheap and finite; Simfil cannot interrupt code inside
such a callback. The existing model-based overload remains for real data, not
as a schema-completion fallback. Neither overload completes operators.

## Streaming Evaluation

Both `eval(env, ast, node, consumer, options, diagnostics)` and
`BoundExpression::eval(node, consumer, options, diagnostics)` accept a `ResultFn`
and return `expected<EvaluationSummary, Error>`. They do not gather a result
vector. Compound values retain their model owner; the consumer can serialize
or copy them under its own output budget.

```cpp
simfil::EvaluationOptions limits;
limits.maxResults = 100;
limits.maxWork = 10000;
limits.maxDepth = 128;
limits.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
limits.cancel = &requestCanceled; // atomic_bool; optional, valid throughout eval
auto outcome = bound.eval(*root, simfil::LambdaResultFn(
    [&](simfil::Context, const simfil::Value& value) {
        return emitWithinOutputBudget(value) ? simfil::Result::Continue : simfil::Result::Stop;
    }), limits);
```

Reasons are `Complete`, `ConsumerStopped`, `ResultLimit`, `WorkLimit`,
`DepthLimit`, `Timeout`, and `Canceled`, with result/work counts. Reaching exactly
the result limit reports `ResultLimit` conservatively without evaluating an extra
value to prove exhaustion. Limits can be zero. A new invocation gets a fresh
budget; environment-bound expression caches remain reusable. Callback errors
propagate as errors, including unpacked values and synthesized missing-value nulls.

Work charges expression entries, intermediate emissions, and wildcard traversal,
including zero-hit searches and aggregate inputs. `split` now emits incrementally;
`keys` retains its lazy iteration and charges key visits. Custom functions must honor
`Result::Stop` and call `Context::step()` during long non-emitting work.

These are **cooperative limits, not a CPU/memory sandbox**. Compilation/constant
folding, parser input size, JSON/materialization, consumer execution, a single
value's bytes, regex execution, and non-cooperating custom functions are not
preempted or bounded by these counters. Embedders need separate request/output
budgets and policies for expensive primitives. The original vector-returning
evaluation overload remains unbounded and unchanged for existing callers.

## Native Result Materialization

`ModelPool::copyNode(node, options)` copies a query-visible subtree through its
native node protocol. `copySequence(span<Value const>, options)` returns an outer
array and shares one budget across every result and descendant. Neither operation
calls `toJson`, copies bare foreign schema IDs, or inserts dictionary names. A source
field name must already exist in the destination pool; IDs are remapped by name.
Strings and bytes become destination-owned payloads. The source owner is retained
during copying, but not by the result. Exact schema graph owners are retained in
the destination's common binding table and schema IDs are remapped. A direct
single-graph pool rejects foreign domains; an unbound result pool can import
several graphs. Unbound nonzero source schema IDs are errors, not silently erased.

Copying an existing compound preserves its schema and ordinary scalar leaves.
An independently selected scalar Value with provenance becomes a qualified node
holding a same-pool primitive address and local domain ID. Unannotated values
remain ordinary storage. There is no destination-schema inference, generated
result schema, or per-result parallel metadata table. Bitmasks always use their
dedicated column. Numeric/string JSON export remains plain and intentionally
does not preserve qualification; native binary results do.

Binary readers require every stored graph identity to resolve exactly before
returning success. Install bindings or `setSchemaResolver` first.
`hasDirectSchemaBinding(identity)` checks a payload's single-graph contract
without consulting the resolver. Dictionary restoration remains separate.
Copied containers use exact-size storage; singleton and empty results use compact
singleton handles rather than growable arena chunks. These materialized containers
are intended for read-only results, not subsequent incremental appends.

Empty results, one null, two scalars and one array-valued result are respectively
`[]`, `[null]`, `[1,2]` and `[[1,2]]`. `Model::newUndefined()` creates a native node
distinct from null; value access and binary serialization preserve it. Its JSON
output is `{"_undefined":true}`. This is an output annotation, not a JSON ingestion
tag: importing that literal object still produces an ordinary object.
Existing language absence rules are unchanged: a missing field or empty path
normally yields a null value, so its projection is `[null]`, not an empty
sequence. An explicit native undefined value survives runtime path traversal;
the compiler's internal unknown-value sentinel still follows compilation rules.

`ModelPool::CopyOptions` defaults to depth 64, 100000 nodes and 16 MiB of charged
payload/reference storage. The outer sequence consumes one node. These are
cooperative limits, not a process-memory sandbox: allocator page capacity and
custom node accessor work are outside the byte charge. Cycles, exhausted limits,
missing dictionary keys and unsupported transients return explicit errors.
Shared acyclic children are copied independently. Failed copies can leave bounded
unreachable allocations in the append-only destination; no partial root is
returned. Evaluator work/result limits remain a separate responsibility.

The mask/qualifier columns and common graph bindings change the native ModelPool
wire layout and static dictionary assignments. Dependent binary protocols must
version this break; there is no implicit legacy conversion.

## Validation

`test/schema-domain.cpp` exercises descriptors, specialized kinds, combiners,
completion, custom bindings, recursive rewrites, and namespace ownership.
`test/evaluation.cpp` covers streaming limits, interruption, compound ownership,
callback errors, and streaming builtins. `test/model-copy.cpp` covers native
undefined, sequence cardinality, ownership, dictionary remapping and copy limits.
`test/schema-value.cpp` covers exact scalar provenance, named predicates, mask
operators/roundtrips, mixed result graphs, freeze boundaries and malformed reads.
Its constructor-time reader regression checks qualified scalar storage without
requiring `shared_from_this()` before a derived model has finished construction.
Existing language/model tests continue to cover the unbounded APIs. Descriptors,
direct completion, bounded evaluation and native copying are also available with
`SIMFIL_WITH_MODEL_JSON=OFF`; JSON serialization is not their storage backend.

Run release benchmarks without other builds/tests consuming the machine:

```bash
build-release/test/test.simfil '[perf.schema],[perf.completion]' \
  --benchmark-samples 40 --benchmark-resamples 2000
```

The sparse-wide fixture compares no schema, basic pruning, generic field plans,
and an exact-path ceiling on identical data. It also repeats basic pruning,
field plans and the exact path with an owning schema binding and runtime scalar
provenance; these costs must not be hidden behind the legacy callback benchmark.
Scalar leaves now have explicit
ValueSchemas: unknown child metadata no longer silently means scalar. Completion
benchmarks compare real-model/schema root suggestions with direct root-domain
suggestions and a nested array/union index. Setup and finalization are outside
timed loops. They are diagnostic measurements, not flaky wall-time assertions.

### Reference Measurements

Paired local Release runs on 2026-09-29 (GCC 13, 40 samples, 2000 resamples),
without concurrent builds, compare the preserved `626984e` executable with this
implementation. Times are sample means, not performance guarantees. The baseline
used the previous implicit-scalar schema leaves; the current fixture makes those
same scalar domains explicit without changing the model data or query topology.

| Query | Before | After |
| --- | ---: | ---: |
| Sparse wide, no schema | 6.115 ms | 6.239 ms |
| Sparse wide, basic pruning | 3.049 ms | 3.165 ms |
| Sparse wide, field plans | 1.436 ms | 1.376 ms |
| Sparse wide, exact path | 0.750 ms | 0.776 ms |
| Nested field, no schema | 3.081 ms | 3.535 ms |
| Missing field, no schema | 2.369 ms | 2.995 ms |
| Nested field, schema pruning | 2.195 ms | 2.305 ms |
| Missing field, schema pruning | 425 ns | 438 ns |

Field plans remain about 2.30x faster than basic pruning and 1.77x the exact-path
cost. This is not a blanket speedup: the unbounded deep traversal cases increased
by roughly 15-26%. The added evaluation state/checks are potential contributors,
but experimentally specializing the budgeted/unbounded wildcard traversal did
not recover that difference and was not retained. Further profiling is needed
to attribute the remaining cost; retain these no-schema controls rather than
hiding that cost behind schema pruning.

For 256 candidate fields, completion in the same run measured 48.9 us through a
real model/schema root, 53.6 us directly through the root domain, and 48.2 us
through a nested array/union index. Those short samples have appreciable timing
noise; they establish comparable latency, not a statistically significant ordering.
The direct path needs no synthetic model construction. Removing duplicate field
enumeration reduced its earlier nested-domain measurement from about 83 us to
about 50 us on this machine.

### Bound Domain Measurements

On 2026-10-30, a quiet Release run (GCC 13, 40 samples, 2000 resamples)
measured the owning-binding/scalar-provenance implementation on the same
sparse-wide fixture. The earlier same-day `e324358` baseline used 20 samples
and 1000 resamples, with appreciable noise in its field-plan result. These are
not freshly interleaved before/after runs or a statistical regression threshold.

| Query | Earlier Baseline | Current Callback | Current Bound Graph |
| --- | ---: | ---: | ---: |
| No schema | 6.814 ms | 7.070 ms | - |
| Basic pruning | 3.222 ms | 3.549 ms | 5.532 ms |
| Field plans | 1.560 ms | 1.606 ms | 2.098 ms |
| Exact path | 0.786 ms | 0.927 ms | 1.419 ms |

Bound field plans are 2.64x faster than bound basic pruning, and take 1.48x
the bound exact-path time. Binding/provenance is not free: relative to the
current callback cases, bound plans cost about 31% more and bound exact access
about 53% more. Persisted scalar leaves remain compact in both variants; the
bound queries derive and retain their domain while emitting runtime values.
This benchmark does not isolate binding lookup, dictionary translation and
qualification costs, so attributing the difference requires profiling.

Nor is the legacy path uniformly unchanged: its no-schema/basic/plan/exact
means increased by approximately 4%/10%/3%/18% over the earlier baseline.
Retain these controls when optimizing instead of reporting only the relative
pruning speedup. In the same quiet run, model-root, direct-root and nested
array/union completion measured 47.9, 50.0 and 50.6 us respectively, comparable
to the earlier roughly 50 us measurements.
