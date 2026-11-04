# Schema Types And Query Views

Simfil uses one typed schema graph for query optimization and completion.
`SchemaModel` exposes that same graph as lazy metadata objects for ordinary
Simfil queries. Completion does not evaluate those descriptors or construct
sample feature objects.

## Type Contract

`TypeId` is a 16-bit graph-local identity, not an artifact identity or a feature
class name. `NoTypeId` is the unknown sentinel. `SchemaReference` combines that
local ID with its exact owning `SchemaBinding`; its identity token still denotes
the complete immutable graph. `SchemaPool::typeCount()` and
`schemaStatistics().types` count allocated definitions, excluding the sentinel.

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

- `Unknown` means unavailable information; `Any` means an unrestricted type.
  Neither proves a descendant field absent. `Never` means no possible value.
- An empty `ObjectSchema` is a known closed object. Use `setOpen(true)` for
  undeclared members. A field with no child schema, `NoTypeId`, or an unresolved
  schema reference is unknown, **not** an implicitly scalar leaf. Openness does
  not weaken explicitly declared fields; an undeclared open alternative still
  prevents guessing one exact type for an ambiguous union.
- An array's element alternatives describe possible types, not length or tuple
  positions. An unspecified element type is unknown.
- `CombinedSchema` retains `AnyOf`, `OneOf`, or `AllOf` at the same value position.
  Empty AnyOf/OneOf is impossible; empty AllOf is unrestricted. Affinity summarizes
  alternatives but does not replace their logical operator.
- `fieldRequired` belongs to the parent-field edge: true, false, or unknown.
  `nullable` belongs to the value type and independently allows/excludes null.
- All string enums/constants use `addEnumSymbol` / `directEnumSymbols` in the
  binding's pool. `enumValues` / `addEnumValue` are for non-string scalar literals;
  the concrete builder rejects both string and string_view values there. This
  keeps descriptor output, completion, and symbol rewrites on one string index.
  Named numeric constants and flags use the separate metadata interface below;
  a few named sentinels do not make a numeric range an exhaustive enum.

This is not a JSON Schema validator. In particular, following an intersection's
fields returns a conservative set of candidate types, not the complete logical
intersection of every scalar constraint.

## Ownership And Indexes

The producer owns schema definitions; `Environment::querySchemaCallback` supplies
the binding. There is no implicit global registry in Simfil. Use the same namespace
for a schema binding, its query environment, and its descriptor model. For
completion/schema inspection, construct a request-local namespace and return
owning candidate strings. Do not modify a datasource's authoritative pool.

After building a graph, finalize its schemas. Reachability and wildcard plans are
derived indexes. `reachabilityComplete()` is a proof, not a synonym for
`finalized()`: open, dirty, or incomplete graphs must remain conservative.
Custom adapters expose fields/elements through the public enumeration methods;
an adapter with an incomplete cached index must not claim completeness.
Sparse wildcard plans use this generic interface, not concrete builder RTTI.

Runtime scalar extraction uses the allocation-free `fieldSchema(fieldId)` and
`itemSchema()` point lookups, not the allocating `fieldSchemas` / `itemSchemas`
analysis helpers. Objects and arrays read their existing member edges directly.
Combiners prepare an effective member index during finalization: a named union
member has a declared type just like an object member, regardless of which
member is currently present. This does not flatten `OneOf` descriptors or infer
a narrower type from the scalar's raw value. A field declared as one union
type retains that type; unresolved or conflicting declarations return
`NoTypeId`.

Custom adapters can override these point lookups over their own edge storage,
or call `Schema::finalize(lookup)` to prepare the default member index from their
enumeration interface. An overridden finalizer must explicitly invoke the base
implementation if it uses that index. Preparation follows logical alternatives,
not child-field recursion; repeated reads do not traverse the graph or enumerate
fields. Lookup-only aliases are resolved by `canonicalField` on the parent.

Treat a published graph as immutable. Mutations update a concrete schema's local
revision, but cannot automatically invalidate caches on all referring schemas.
Owners must rebuild/invalidate dependent bindings and indexes after graph edits.

### Compact Owning Pools

`SchemaPool : ModelPool` is the reusable owning implementation. It stores fields,
elements, alternatives, named values and numeric metadata in compact canonical
columns. `type(id)` borrows a `Schema` view over those columns; `typeNode(id)`
creates a lazy descriptor view, not another type graph. Applications such as
mapget derive their artifact owner to add format, layer and feature metadata.
The independent `ObjectSchema`/`ArraySchema` builders and custom adapters remain
supported; a consumer need not use `SchemaPool` to participate in queries.

```cpp
auto schema = std::make_shared<simfil::SchemaPool>();
auto record = schema->addSchema(simfil::Schema::Kind::Object);
auto count = schema->addSchema(simfil::Schema::Kind::Int);
schema->addFieldSchema(record, "count", count);
schema->setFieldRequired(record, "count", true);
schema->finalize();
auto binding = schema->binding("application-owned-exact-artifact-id");
```

Construction copies borrowed metadata. Finalization discards mutable drafts,
builds cycle-aware reachability and member indexes, then freezes the pool and
dictionary. Failure during packing makes that unpublished builder unusable;
retry with a new owner. Published field/element lookups allocate nothing and do
not follow descendant graphs. Direct name lists preserve declaration order;
recursive string views are lexical, independently of dictionary ID ordering.
Empty field names and empty string enum symbols remain valid.

`setBaseSchema` inherits object members without storing a second copy; local
members take precedence. Inheritance is bounded and acyclic, unlike ordinary
field/element graph edges, which may be recursive. Lookup aliases and `Context`
edge roles remain explicit; a combiner can expose both its alternatives and its
own context fields in the descriptor model.

`importGraph(source, reuse)` copies native declarations while preserving sharing
and cycles. Its temporary source-ID table translates only graph-local type
IDs. Nonzero entries in an optional reuse table refer to existing destination
types; the application must establish their semantic compatibility. Generic
import does not guess whether same-named application types may be merged.

The bounded native codec includes the private dictionary and ModelPool payload.
Derived owners embed this payload in their own versioned, content-identified
envelopes. The generic codec deliberately bypasses application dictionary stream
prefixes. Bindings pin the exact owner; the application supplies and validates
their artifact identity. No ingestion remap table, per-leaf tag or second JSON
schema tree is retained by this generic graph.

`schemaStatistics()` is prepared once. `schemaMemoryUsage()` reports retained
generic graph storage, excluding the dictionary and ModelPool payload so derived
owners can account for those exactly once. These are allocation lower bounds,
not allocator/RSS estimates. The extraction does not by itself establish a
performance improvement; the unresolved baseline gate below still applies.

Extraction validation on 2026-11-03 rebuilt the standalone Release library and
ran all 210 test cases successfully, including every benchmark body with one
sample (3,377,177 assertions; calibration affects the assertion count). The focused
pool/context/copy/type selection passed 57 cases and 7,970 assertions. Full
Memcheck and Helgrind runs each passed 205 cases with the five benchmark cases
intentionally skipped; both reported zero errors, and Memcheck retained zero
bytes at exit. A separate JSON-disabled Release library also compiled. These checks include
unknown alternatives, inherited overrides, context combiners, numeric setter
replacement and invalid flag declarations. They establish correctness/memory
checks for this cut, not the runtime performance gate or downstream validation.
The final cut also fixes generic validation of raw shared-arena storage owned by
custom columns: synthetic arena sweeps validate members without requiring a core
schema sidecar, while real core root/child references still require one. Tests
cover ordinary/singleton objects and arrays, native roundtrips, forged core
references and a malformed core self-cycle rejected before recursion.

`SchemaBinding` retains the immutable graph owner, exact content identity, lookup
and schema dictionary. `SchemaReference` combines that binding with a local
16-bit `TypeId`. Different graphs may reuse the same local IDs. Runtime field
dictionaries may differ from the schema dictionary: scalar extraction and bound
wildcard plans translate names rather than interpreting foreign IDs as local ones.
Wildcard plan caches retain each graph independently.
Dictionary translation for a wildcard plan happens only when building the plan,
not on a cache hit. Parent-field scalar extraction still translates the requested
name when the runtime and schema dictionaries are independent.

Each `ModelPool` installs a model-local shared-ownership group for each bound
graph. The actual `SchemaBinding` pointer, definitions, identity and dictionary
remain shared; no graph is copied. This prevents independent tile workers from
contending on one graph-wide reference counter during scalar extraction and
pruning. Imports and readers flatten existing local groups to the original
owner, preserving alias pointers rather than retaining chains of source models'
groups. The final strong reference releases the graph even when weak observers
remain. Both the stored handle and the new control block are cache-line isolated
to avoid allocator-dependent false sharing with another model's hot counter.
This adds storage/allocation once per installed graph, not per lookup or Value.
Memory statistics include known padded handle/deleter storage, but exclude
implementation-specific shared-pointer bookkeeping and allocator overhead.

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
have aliases. A named value includes its runtime boolean/integer/unsigned/float/string
literal, role (`constant`, `special`, `flag`), meaning (`ordinary`, `unknown`,
`variable`, `unbounded`, `saturated`), and deprecated/removed status. Numeric
metadata separately describes encoding, bounds, ordinary ranges, unit, scale,
offset and completeness. Unknown numeric codes remain ordinary raw numbers.

`NamedValue::rawValue` optionally preserves the original signed or unsigned
64-bit ordinal separately from that runtime literal. Missing metadata is distinct
from zero; aliases can share an ordinal. This does not change query evaluation,
enum completion, or the runtime label. Descriptors omit `rawValue` when absent
and otherwise expose it through the normal node protocol. As with other integer
metadata, unsigned values above `INT64_MAX` use exact decimal strings in descriptor
JSON rather than floating-point conversion.

The generic native `SchemaPool` codec is version 2. Named-value rows append the
ordinal bits and a presence/signedness tag; numeric bounds and range rows retain
their previous layout. Readers reject all other codec versions and malformed
ordinal tags rather than silently dropping metadata. `importGraph` preserves
ordinals while remapping graph-local type IDs.

Schema mode rewrites standalone named values to predicates on declared paths.
Numeric names use equality; flags use all-bits membership. Contextual comparisons
such as `speed = SPEED_UNLIMITED` resolve the name against the **selected value's
actual type**, not a global symbol table or a union of possible raw codes.
Missing or ambiguous type provenance is an error rather than a guessed profile.
Quoted strings remain strings; fields and environment constants retain precedence.
Incomplete/cyclic path sets use bounded runtime schema traversal, retaining the
owning dictionary even through descendants with no unique type. Completion uses
the same metadata and scopes named suggestions to the other comparison operand.

Values extracted from a concrete parent field or array-element edge retain an
optional `SchemaReference`. Int/Float/String categories do not change. Identity
and selection operations, traces, and the first min/max winner on a tie retain
the reference. Arithmetic, casts, comparisons, new literals and ordinary numeric
sum/count construct unannotated values, including `+ 0`. A custom selecting fold
preserves whichever Value it returns. Null and undefined never acquire a type
from their parent. Ordinary stored leaves remain compact and unannotated.

`ValueType::Bitmask` instead carries intrinsic uint64 bits and exact type
identity. Persistent mask rows contain only uint64 bits and a local TypeId;
the common binding table owns the graph. `hasFlag(mask, 'NAME')` tests all
declared bits; a named zero matches exactly zero. Unknown flags, incompatible
types, negative/float coercions and invalid shifts are errors. Unknown active
bits are retained. Complement and shifts respect the declared width. Use
`bitmask(existingMask, '0x8000000000000000')` for lossless full-width construction;
ordinary signed integer literals reject overflow.

The exact JSON tag is
`{"_bitmask":true,"bits":"0x8000000000000000","schema":"exact-graph-token","typeId":4}`.
Import requires that exact graph through the model binding/resolver. Objects with
additional keys remain application objects. Mask JSON never uses floating-point
numbers or labels as a substitute for raw bits.

Exact path enumeration accepts an optional `bool* complete` output. Active-path
TypeIds detect cycles; reused definitions in sibling branches and repeated
field names are valid. Enumeration also has depth/work caps. Incomplete enum
path sets use a schema-guided predicate over actual data instead of an incorrect
finite OR that misses deeper matches. That predicate matches only declared enum
positions, not unrelated strings, and static path analysis reports unresolved
access. Its compiled root TypeId must refer to the same logical graph when
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

Descriptors are ordinary objects, even for represented scalar/array types:

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

Explicit combiners use `alternatives`, not `elements`. Multiple type IDs on a
field edge are an implicit union. Unknown optional metadata is omitted. Field
names are under `fields`, so they cannot collide with descriptor metadata names.
`get`, `at`, `keyAt`, `iterate`, and `toJson` agree; `kind` is a string through all
of them. Descriptor `schema()` is `NoTypeId`, not the represented type ID.
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

Field edges distinguish enumerable `Schema::FieldRole::Data` from lookup-only
`Context`. `forEachDirectField` and `directFields` still describe every declared
member; `forEachEnumerableField` omits context edges. Explicit lookup, including
completion after `context.`, follows either role. Recursive discovery, enum
shorthand and wildcard child plans do not descend through context links.
Arrays may also declare named lookup edges, such as a coordinate's `lon` member.
Wildcard plans preserve those direct matches, including context terminals, while
recursing only through array elements. Ordinary arrays still skip named lookup.
Classification is independent of spelling: a `$`-prefixed data field is still
enumerable, and an ordinary-named context field is not. Lazy field descriptors
mark lookup-only links with `role: "context"` without hiding their target `$ref`.

Unknown context targets do not make the data reachability index incomplete.
Repeated/alternative declarations with conflicting roles stay conservative;
they cannot establish a context-only absence proof. The runtime model must
implement the same enumeration contract, as `OverlayNode` does for injected
members; schema metadata does not remove real children from arbitrary models.

```cpp
auto candidates = simfil::complete(env, "items[17].na", 12, rootSchemaId, options);
```

This overload takes a `TypeId` instead of a `ModelNode`. It reuses tokenization,
the relaxed parser, completion AST, smart-case filtering, and replacement ranges.
It does not execute constant-folding or registered functions. Unfinished string
literals are accepted only for schema completion; compilation remains strict.

Field access follows member types, arbitrary positive/negative array indices
follow element types, and logical alternatives do not insert a fictional array
level. Filtering preserves the input type. AND/OR inspect cursor-local branches
independently of sample truth values. Comparisons offer the left operand's enum
choices; unknown functions/accesses stay unknown instead of inventing fields.
Root-owned operand alias hooks have the same precedence/scope as compilation;
explicit paths do not reinterpret member names as aliases.

Model lookup aliases are separate: an adapter can override
`Schema::canonicalField` to resolve a member name as its runtime model does,
without duplicating enumerated field paths. Member-type resolution, field path
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

`CopyOptions::schemaRemap` optionally translates each known source type while
copying into another exact graph. Unqualified leaves remain unqualified and do
not invoke the callback. Missing or physically incompatible target definitions
are errors, not permission to drop provenance.

`ModelPool::remapSchemas(target, callback)` performs the equivalent operation on
stored core schema columns without copying payloads or changing node addresses.
It validates all replacements before committing one direct target namespace;
failed validation leaves the model unchanged. Derived models must prevalidate
their own custom schema columns and update them after the core operation.
Use this only on unpublished, exclusively owned models: existing node handles
observe the new binding, while already detached Values retain their old exact
type. Translation tables are temporary ingestion work, not query-path state.

`ModelPool::copyNode(node, options)` copies a query-visible subtree through its
native node protocol. `copySequence(span<Value const>, options)` returns an outer
array and shares one budget across every result and descendant. Neither operation
calls `toJson`, copies bare foreign schema IDs, or inserts dictionary names. A source
field name must already exist in the destination pool; IDs are remapped by name.
Strings and bytes become destination-owned payloads. The source owner is retained
during copying, but not by the result. Exact schema graph owners are retained in
the destination's common binding table and schema IDs are remapped. A direct
single-graph pool rejects foreign types; an unbound result pool can import
several graphs. Unbound nonzero source schema IDs are errors, not silently erased.

Copying an existing compound preserves its schema and ordinary scalar leaves.
An independently selected scalar Value with provenance becomes a qualified node
holding a same-pool primitive address and local type ID. Unannotated values
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
`test/schema-lookup.cpp` covers direct members, named unions, aliases, duplicate
declarations, incomplete/cyclic metadata and prepared adapter indexes. Its
publication guard rejects schema enumeration during runtime scalar extraction.
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
benchmarks compare real-model/schema root suggestions with direct root-type
suggestions and a nested array/union index. Setup and finalization are outside
timed loops. They are diagnostic measurements, not flaky wall-time assertions.

### Reference Measurements

Paired local Release runs on 2026-09-29 (GCC 13, 40 samples, 2000 resamples),
without concurrent builds, compare the preserved `626984e` executable with this
implementation. Times are sample means, not performance guarantees. The baseline
used the previous implicit-scalar schema leaves; the current fixture makes those
same scalar types explicit without changing the model data or query topology.

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
real model/schema root, 53.6 us directly through the root type, and 48.2 us
through a nested array/union index. Those short samples have appreciable timing
noise; they establish comparable latency, not a statistically significant ordering.
The direct path needs no synthetic model construction. Removing duplicate field
enumeration reduced its earlier nested-type measurement from about 83 us to
about 50 us on this machine.

### Earlier Bound Type Measurements

On 2026-10-30, a quiet Release run (GCC 13, 40 samples, 2000 resamples)
measured the owning-binding/scalar-provenance implementation on the same
sparse-wide fixture, before merging the subsequent `size-checks` optimizations
through `77fe792`. In particular, that merge makes diagnostics collection
optional on the evaluation hot path; the following numbers do not measure the
merged implementation. The earlier same-day `e324358` baseline used 20 samples
and 1000 resamples, with appreciable noise in its field-plan result. These are
not freshly interleaved before/after runs or a statistical regression threshold.

| Query | Earlier Baseline | Callback At That Revision | Bound Graph At That Revision |
| --- | ---: | ---: | ---: |
| No schema | 6.814 ms | 7.070 ms | - |
| Basic pruning | 3.222 ms | 3.549 ms | 5.532 ms |
| Field plans | 1.560 ms | 1.606 ms | 2.098 ms |
| Exact path | 0.786 ms | 0.927 ms | 1.419 ms |

Bound field plans are 2.64x faster than bound basic pruning, and take 1.48x
the bound exact-path time. Binding/provenance is not free: relative to the
callback cases at that revision, bound plans cost about 31% more and bound exact access
about 53% more. Persisted scalar leaves remain compact in both variants; the
bound queries derive and retain their type while emitting runtime values.
This benchmark does not isolate binding lookup, dictionary translation and
qualification costs, so attributing the difference requires profiling.

Nor is the legacy path uniformly unchanged: its no-schema/basic/plan/exact
means increased by approximately 4%/10%/3%/18% over the earlier baseline.
Retain these controls when optimizing instead of reporting only the relative
pruning speedup. In the same quiet run, model-root, direct-root and nested
array/union completion measured 47.9, 50.0 and 50.6 us respectively, comparable
to the earlier roughly 50 us measurements.

### Allocation-Free Member Lookup

On 2026-11-02, paired Release measurements compared `a67e654` with direct
`fieldSchema` / `itemSchema` lookup and cached wildcard dictionary translation.
Both executables used GCC 13, `-O3 -DNDEBUG`, the same sparse-wide fixture
(2,000 objects, 32 branches), and CPU affinity to one core of a Ryzen 7 PRO
7840U. The following are medians of seven interleaved rounds, each evaluating
150 queries after three warmups, with a retained `BoundExpression`. Setup,
schema finalization and compilation are outside the measurements.

| Query | Before | Point Lookup |
| --- | ---: | ---: |
| No schema | 6.948 ms | 7.042 ms |
| Callback basic pruning | 3.628 ms | 3.490 ms |
| Callback field plans | 1.624 ms | 1.594 ms |
| Unbound exact path | 0.918 ms | 0.897 ms |
| Bound basic pruning | 5.552 ms | 5.216 ms |
| Bound field plans | 2.079 ms | 1.781 ms |
| Bound exact path | 1.404 ms | 1.167 ms |

Bound field-plan latency falls about 14%, and bound exact-path latency about
17%. The unbound controls vary by -3.8% to +1.3%; these local timings are not
regression thresholds. Separate Callgrind runs show approximately 11% fewer
instructions for both bound cases, unchanged no-schema/exact-path instruction
counts, and about 1% fewer callback field-plan instructions.

Counting `operator new` calls only around warmed evaluation gives **8,001 to 1
allocations per bound query**, for all three bound variants. The remaining
allocation holds the final result vector; deriving the 2,000 numeric leaf
types no longer allocates. This is not a promise that arbitrary queries or
owning string results allocate nothing. Bound field plans also reduce successful
schema-dictionary `get` calls from 8,001 to 2,000 per query. Exact graph ownership,
scalar qualification and independent-dictionary translation still have a cost.

The existing Catch2 schema/completion benchmarks were also rerun three times
with 40 samples and 2,000 resamples. Median completion means were 47.5 us for
model-root, 53.3 us for direct-root and 50.3 us for nested array/union completion.
One run had substantial timing outliers; neither these measurements nor the
earlier ones establish an end-to-end application speedup.

### Runtime Cost Recovery Against The Original Baseline

The allocation-free lookup result above compared against an already regressed
implementation. It did **not** meet the performance budget against `77fe792`.
The subsequent runtime pass removes redundant work rather than dropping types:

- `ModelPool::type` and `ModelPool::schema` read core column metadata directly;
  custom columns still use virtual resolution, and typed nodes retain their
  overrides. Generic nodes no longer construct owning wrappers just to read
  these two properties.
- Scalar extraction constructs the qualified `Value` once. Compound children
  need no inferred scalar type, while intrinsic scalar provenance takes
  precedence over the parent edge.
- `ExpressionRuntime` binds static field names to each exact schema dictionary.
  Successful translations and wildcard plans share a graph-owned cache, with a
  last-graph fast path. Missing names remain retryable; definitions are looked
  up again, so schema replacement does not leave dangling definition pointers.
  Dynamic subscripts still resolve the actual key for each access.
- Resolution reuses the node's existing strong owner instead of acquiring an
  extra dispatch reference and locking `enable_shared_from_this` for each
  temporary wrapper. Wrappers and independently retained results still own
  their model/graph; no borrowed-value lifetime protocol was introduced.
- Address-only node construction initializes its default null payload directly,
  rather than moving and destroying an otherwise unused scalar variant.

The final comparison uses the same explicit-leaf sparse-wide fixture and
compiler settings as above, but both executables first start and join a thread.
This activates the ordinary threaded-process shared-pointer synchronization
path before timing, rather than benefiting from a single-threaded-process
library shortcut. Evaluation itself remains single-worker, pinned to CPU 4.
Seven interleaved rounds evaluate 200 queries each after three warmups, retaining
the `BoundExpression`; setup, dictionary migration and compilation are excluded.
The original `77fe792` executable is the reference, not `a67e654`.

| Query | Original Median | Final Median | Latency Change |
| --- | ---: | ---: | ---: |
| No schema | 7.483 ms | 5.646 ms | -24.5% |
| Callback basic pruning | 3.569 ms | 2.751 ms | -22.9% |
| Callback field plans | 1.604 ms | 1.245 ms | -22.4% |
| Unbound exact path | 0.814 ms | 0.564 ms | -30.7% |
| Bound basic pruning | 3.559 ms | 3.282 ms | -7.8% |
| Bound field plans | 1.607 ms | 1.305 ms | -18.8% |
| Bound exact path | 0.826 ms | 0.641 ms | -22.4% |
| Separate dictionaries, bound basic | 3.495 ms | 3.272 ms | -6.4% |
| Separate dictionaries, bound plans | 1.624 ms | 1.306 ms | -19.6% |
| Separate dictionaries, bound exact | 0.820 ms | 0.636 ms | -22.4% |

Each bound variant is compared with the original corresponding callback/exact
query, which did not preserve scalar provenance. The separate-dictionary
candidate additionally pays translation into an independent runtime namespace;
its reference still uses the original shared dictionary. Repeated reference
medians differ because each variant has its own interleaved reference samples.

All candidate sample ranges were below their respective reference ranges. For
the narrowest margin, separate-dictionary basic pruning, the seven candidate
means span 3.181--3.350 ms versus 3.460--3.661 ms for the reference. This fixture
meets the at-most-1% regression budget without using a 5% exception; it is not
an end-to-end MapViewer or cross-platform performance guarantee. Exact-type
handling still has a measurable cost relative to the faster unbound candidate.
Separate Callgrind runs count 3.8% more instructions for bound basic pruning,
but 5.8% fewer for bound plans and 8.2% fewer for bound exact paths versus the
original corresponding query. Instruction counts are not latency: the basic
path still performs extra type bookkeeping while avoiding costlier ownership
operations and wrapper construction.

Warmed evaluation in every mode performs one 88-byte allocation for the result
vector, with no successful dictionary `get` calls. `Value`, `ModelNode` and
`ScalarValueType` remain 88, 72 and 40 bytes on this platform, respectively.
`test/schema.cpp` now retains bindings, activates threaded-process refcounts,
and benchmarks all three independent-dictionary variants. Focused tests cover
core/custom/foreign metadata dispatch, graph-local ID collisions, dictionary
translation, late declarations, replacement definitions, dynamic subscripts,
and result ownership after both the environment and source models are gone.


### Parallel Ownership Follow-Up

The single-worker result above did not establish parallel scalability. A
four-worker diagnostic with distinct models sharing one binding exposed a 52%
regression in basic pruning against `77fe792`: each transient schema reference
contended on the same graph-wide atomic counter. Splitting ownership counters
without copying the graph removed most of it. That alone was insufficient:
`perf c2c` then identified false sharing between a model's stored binding handle
and another model's reference counter. Isolating both the handle and the new
counter fixes that allocator-placement effect; padding unrelated model nodes
or Values is not part of the change.

The following complete matrix uses GCC 13.3 Release on an AMD Ryzen 7 PRO 7840U.
Each worker has its own model, environment and retained `BoundExpression`, but
shares the same schema graph and AST. Seven interleaved rounds each time 200
queries per worker after three warmups. The statistic is the median of the
slowest worker's mean query latency in each round, not aggregate throughput.
Construction and compilation are excluded. Models are either built on the main
thread or by their querying worker to check allocator-placement sensitivity.
CPUs 4/5 and 6/7 share cores; CPUs 4/6/8/10 are distinct physical cores. The
reference is always the original corresponding callback/exact query at
`77fe792`, not the regressed Schema 2 implementation. `separate-*` additionally
pays for an independent runtime dictionary; its reference still shares the
schema dictionary. Negative change means faster.

| Workers / Placement | Construction | Query | Original | Local Owners | Change |
| --- | --- | --- | ---: | ---: | ---: |
| 1 | main | bound-basic | 3.654 ms | 3.255 ms | -10.90% |
| 1 | main | bound-plans | 1.646 ms | 1.327 ms | -19.40% |
| 1 | main | bound-exact | 0.871 ms | 0.681 ms | -21.80% |
| 1 | main | separate-basic | 3.622 ms | 3.298 ms | -8.95% |
| 1 | main | separate-plans | 1.628 ms | 1.321 ms | -18.86% |
| 1 | main | separate-exact | 0.861 ms | 0.688 ms | -20.13% |
| 4 / 2 cores (SMT) | main | bound-basic | 5.573 ms | 5.831 ms | +4.63% |
| 4 / 2 cores (SMT) | main | bound-plans | 2.570 ms | 2.477 ms | -3.61% |
| 4 / 2 cores (SMT) | main | bound-exact | 1.211 ms | 1.151 ms | -4.97% |
| 4 / 2 cores (SMT) | main | separate-basic | 5.592 ms | 5.792 ms | +3.57% |
| 4 / 2 cores (SMT) | main | separate-plans | 2.581 ms | 2.473 ms | -4.20% |
| 4 / 2 cores (SMT) | main | separate-exact | 1.204 ms | 1.155 ms | -4.05% |
| 4 / 4 cores | main | bound-basic | 3.863 ms | 3.594 ms | -6.98% |
| 4 / 4 cores | main | bound-plans | 1.711 ms | 1.382 ms | -19.22% |
| 4 / 4 cores | main | bound-exact | 0.912 ms | 0.721 ms | -20.97% |
| 4 / 4 cores | main | separate-basic | 3.869 ms | 3.515 ms | -9.15% |
| 4 / 4 cores | main | separate-plans | 1.723 ms | 1.412 ms | -18.06% |
| 4 / 4 cores | main | separate-exact | 0.948 ms | 0.713 ms | -24.83% |
| 4 / 2 cores (SMT) | worker | bound-basic | 5.635 ms | 5.920 ms | +5.05% |
| 4 / 2 cores (SMT) | worker | bound-plans | 2.590 ms | 2.518 ms | -2.76% |
| 4 / 2 cores (SMT) | worker | bound-exact | 1.218 ms | 1.182 ms | -2.97% |
| 4 / 2 cores (SMT) | worker | separate-basic | 5.612 ms | 5.933 ms | +5.72% |
| 4 / 2 cores (SMT) | worker | separate-plans | 2.593 ms | 2.485 ms | -4.17% |
| 4 / 2 cores (SMT) | worker | separate-exact | 1.205 ms | 1.197 ms | -0.66% |
| 4 / 4 cores | worker | bound-basic | 3.891 ms | 3.562 ms | -8.45% |
| 4 / 4 cores | worker | bound-plans | 1.779 ms | 1.414 ms | -20.50% |
| 4 / 4 cores | worker | bound-exact | 0.927 ms | 0.763 ms | -17.70% |
| 4 / 4 cores | worker | separate-basic | 3.912 ms | 3.616 ms | -7.57% |
| 4 / 4 cores | worker | separate-plans | 1.735 ms | 1.435 ms | -17.31% |
| 4 / 4 cores | worker | separate-exact | 0.939 ms | 0.763 ms | -18.68% |

All single-worker and four-distinct-core medians improve. The strict <=1%
regression target is nevertheless **not fully met**: basic pruning with four
workers sharing two physical cores remains 3.57--5.72% slower. That is not an
approved 5% exception, and the remaining target stays open. Field plans and
exact paths improve in those SMT runs too, but do not erase the basic-pruning
result. This is a bounded fixture comparison, not an end-to-end MapViewer,
same-model concurrent-query, or cross-platform performance guarantee.

All per-worker samples, CPU placement and source identifiers are retained in
[`performance/schema-runtime-2026-11-02.json`](performance/schema-runtime-2026-11-02.json).
The repository's sparse-wide benchmark also runs four-model batches for basic
pruning, plans and exact paths. Its batch timings include thread/binding setup
amortized over 16 queries per worker; they are not the pinned steady-state
measurements reported above and should not become noisy CI timing assertions.

The same fixture now compares compact `SchemaPool` against the independent
`ObjectSchema` registry for basic pruning, field plans and exact paths, with
both shared and separate dictionaries. Payloads and container IDs are unchanged;
native schema rebinding is outside timing. These additional benchmark variants
are coverage for the extraction, not yet a new accepted baseline measurement.

The persistent [paired runtime harness](performance/README.md) compiles the same
fixture against the original revision and the candidate, records build identities
and all worker samples, and includes compact-pool variants. Its smoke mode is
strictly correctness coverage; it never reports performance acceptance. Retain
the historical raw evidence rather than replacing it with an intermediate-base
comparison or an unpinned one-sample benchmark.
Both harness versions compile, and a 60-row tiny smoke matrix passes all 120
reference/candidate subprocesses, including untimed exact-binding/leaf-type
checks. Nine runner tests cover evidence preservation, paired ordering and
rejection of mismatched builds/workloads. These are harness-correctness results,
not a new latency comparison.

Untimed capacity checks on the full 2,000-object/32-branch fixture report
1,967,654 bytes of model-column backing capacity at `77fe792`, versus 1,973,457
bytes with the candidate's registry binding (+5,803 bytes, +0.295%) and 1,973,456
bytes with compact-pool binding. These exclude dictionaries and shared graph
storage. The candidate schema dictionary reports 8,752 bytes; each separate
runtime dictionary adds 8,832 bytes. Compact graph type/index storage reports
25,746 bytes plus 1,088,538 bytes of primitive ModelPool capacity, shared across
workers. These are existing API lower bounds, not allocator/RSS totals. The
independent registry lacks a complete retained-size API, so **whole-schema memory
comparison against the pre-cutover representation remains open**. Raw capacity
evidence is retained in
[`performance/schema-memory-2026-11-03.json`](performance/schema-memory-2026-11-03.json).
Timings from this loaded-host capacity probe were discarded. The prepared paired
runtime matrix has not been run; the historical SMT regression gate remains open.

CTest in coverage builds runs every benchmark with one measured sample and no
statistical analysis. Fixture setup, calibration, benchmark bodies and their assertions
still execute; ordinary builds retain full sampling. Instrumented timings, especially
with shared atomic gcov counters, are not performance evidence.

A separate nine-round lifecycle probe installed one binding in each of 10,000
empty models, then destroyed the models. Relative to `c217e0d` (which already
supports bindings), median installation rises from 117 to 265 ns/model, and
whole-model destruction from 7.713 to 8.031 us/model. Installation adds one heap
allocation and 240 requested bytes per model/graph on this libstdc++ build
(3 allocations / 68 bytes before, 4 / 308 after). The retained-capacity lower
bound increases by 112 bytes; it deliberately does not claim the omitted
control-block bookkeeping/padding. These are installation/teardown costs, not
per-query allocations, and other standard libraries may use different layouts.

Regression tests cover independent counters with unchanged graph pointers,
direct/imported/read bindings, alias pointers differing from their owner,
weak-reference release, detached scalar lifetimes, and concurrent workers
using one schema with independent model dictionaries. No borrowed Value API,
public node-layout change, or stream-format change is introduced.
