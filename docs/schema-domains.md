# Schema Types And Query Views

Simfil uses one canonical `SchemaPool` for query optimization and completion.
`SchemaType` is a cheap typed view; `SchemaTypeModelNode` exposes the same records
through the ordinary node protocol for metadata queries. Completion does not
evaluate descriptors or construct sample feature objects.

## Type Contract

`TypeId` is a 16-bit graph-local identity, not an artifact identity or a feature
class name. `NoTypeId` is the unknown sentinel. `SchemaType` combines that local
ID with its exact pool and optionally retains the pool. Its pool's artifact
identity names the complete immutable graph. `SchemaPool::typeCount()` and
`schemaStatistics().types` count allocated definitions, excluding the sentinel.

`SchemaType::Kind` is a 32-bit packed value. Its upper 16 bits are a **static**
kind-name `StringId`; its lower 16 bits describe possible runtime `ValueType`s.
Use `SchemaType::makeKind`, `kindNameId`, `affinities`, `hasAffinity`, and
`valueTypeAffinity`. Only affinity bits are OR-combinable; never OR whole kinds.
`ValueType` itself remains an ordinary enum.

Core kinds include unknown, any, never, untyped scalar (`Value`), null, boolean,
integer, float, string, bytes, bitmask, object, array, union, exclusive union, and
intersection. An external kind such as `Feature` can have object affinity and
provide fields through the same typed interface. Generic consumers do not
require a separate application type implementation or knowledge of that name.

Extensions allocate names between `StringPool::NextStaticId` and
`StringPool::FirstDynamicId` (now 256). Register identical static mappings in
every participating pool. This changes serialized string assignments; consumers
must coordinate their protocol version rather than mix old and new dictionaries.
Kind names must not be dynamically interned datasource-specific IDs. `typename`
is a separate optional concrete producer type name, not the kind's spelling.

The distinctions are intentional:

- `Unknown` means unavailable information; `Any` means an unrestricted type.
  Neither proves a descendant field absent. `Never` means no possible value.
- An empty object type is a known closed object. Use `setOpen(true)` for
  undeclared members. A field with no child schema, `NoTypeId`, or an unresolved
  schema reference is unknown, **not** an implicitly scalar leaf. Openness does
  not weaken explicitly declared fields; an undeclared open alternative still
  prevents guessing one exact type for an ambiguous union.
- An array's element alternatives describe possible types, not length or tuple
  positions. An unspecified element type is unknown.
- `SchemaCompositionType` retains `AnyOf`, `OneOf`, or `AllOf` at one value position.
  Empty AnyOf/OneOf is impossible; empty AllOf is unrestricted. Affinity summarizes
  alternatives but does not replace their logical operator.
- `SchemaField::required()` belongs to the parent-field edge: true, false, or unknown.
  `nullable` belongs to the value type and independently allows/excludes null.
- String enums use `SchemaEnumType::addSymbol` / `directEnumSymbols` in the
  schema's dictionary. `addValue` is for non-string scalar literals; its value
  type excludes both string and string_view. This
  keeps descriptor output, completion, and symbol rewrites on one string index.
  Named numeric constants and flags use the separate metadata interface below;
  a few named sentinels do not make a numeric range an exhaustive enum.

This is not a JSON Schema validator. In particular, following an intersection's
fields returns a conservative set of candidate types, not the complete logical
intersection of every scalar constraint.

## Ownership And Indexes

`SchemaPool : ModelPool` is the sole schema owner and storage implementation.
Fixed records use `ModelColumn`; variable collections use `ArrayArena`.
Construction writes those records directly. There is no draft graph, packing
pass, independent virtual schema backend, or descriptor-tree owner.

`schemaType(id)` returns a borrowed `SchemaType`. A borrowed copy performs no
allocation or reference-count update. Keep the pool alive, or call `retain()` to
obtain an owning view; a stack-owned pool cannot produce an owning view. Converting
to an object/array/enum/bitmask/scalar/composition view preserves ownership.
`borrow()` removes ownership from the returned view only. Collection ranges and
strings always borrow storage, even when obtained from an owning view.

Use `Environment::setSchema(root)` for a runtime fallback and supply the root
explicitly to compilation, completion and static analysis. The environment
retains it once; `Environment::schema()` borrows it. An empty
`CompileOptions::rootSchema` intentionally disables schema interpretation rather
than substituting that fallback. Real model nodes resolve their attached types
through `Model::resolveSchemaType` / `ModelNode::schemaType`.

Schema and data dictionaries may differ. Field IDs in `SchemaPath` and schema
ranges belong to the schema dictionary. Compilation stores portable text; bound
expressions cache runtime translation per exact owner and type. Never reinterpret
foreign IDs or intern schema-only names into a datasource dictionary. Completion
uses a temporary dictionary and returns owning strings.

Finalization validates the graph, prepares indexes and freezes the pool and its
dictionary. Mutable views obtained earlier cannot bypass publication. Published
graphs have no revision counter: replace the owner to change a graph. Plan caches
retain the exact pool, not a temporary view's address or an unverified identity
string. Caller-supplied transport identities are not content authentication.

`reachabilityComplete()` proves field absence; it is not synonymous with
`finalized()`. Unknown/open descendants remain conservative. Point
`fieldSchema(fieldId)` / `itemSchema()` lookups and arena-backed alternatives are
allocation-free. Finalization prepares effective inherited/combined members;
runtime lookup never traverses descriptors or enumerates descendant graphs.
A field declared as one union retains that type. Conflicting or unknown target
declarations return `NoTypeId` without inferring a type from the runtime literal.

### Typed Construction

```cpp
auto schema = std::make_shared<simfil::SchemaPool>();
auto record = schema->newObjectType();
auto count = schema->newScalarType(simfil::SchemaType::Kind::Int);
record.addField("count", count).setRequired(true);
auto category = schema->newEnumType();
category.addSymbol("urban");
category.addSymbol("rural");
record.addField("category", category);
schema->finalize("application-owned-exact-artifact-id");
auto retainedRoot = record.retain();
```

Construction copies borrowed metadata. Finalization discards construction-local
sorting/SCC/deduplication state; retained indexes also use columns and arenas.
A failed finalization makes that unpublished builder unusable; retry with a new
owner. `forEachField` works during construction; prepared `directFields` and
recursive name indexes are available after finalization. Direct names preserve
declaration order; recursive names are sorted numeric StringIds, not lexical text.
Empty field names and empty enum symbols are valid. Prefer `field("name")` for
text lookup: a missing dictionary lookup returning zero is not proof of an empty
field declaration.

`SchemaObjectType::setBase` inherits object members without storing a second copy; local
members take precedence. Inheritance is bounded and acyclic, unlike ordinary
field/element graph edges, which may be recursive. Lookup aliases and `Context`
edge roles remain explicit; a combiner can expose both its alternatives and its
own context fields in the descriptor model. `SchemaField` carries required,
multimap and role metadata on the occurrence, not its shared target type.
`declareField` preserves an explicitly empty target list; `addField(name, {})`
adds an explicit unknown target. Do not collapse these during import.

`seal()` protects one reusable record before whole-pool publication. `cloneType`
copies direct declarations, shares child identities and returns a writable copy.
Derived owners use mutation-policy hooks to protect native envelopes even through
generic typed views; aliases validate both spellings before interning. Their
clone hook copies declarative policy, not runtime use-site associations.

`importGraph(source, reuse)` copies native declarations while preserving sharing
and cycles. Its temporary source-ID table translates only graph-local type
IDs. Nonzero entries in an optional reuse table refer to existing destination
types; the application must establish their semantic compatibility. Generic
import does not guess whether same-named application types may be merged.

The bounded native codec includes the private dictionary and ModelPool payload.
Derived owners embed this payload in their own versioned, content-identified
envelopes. The generic codec deliberately bypasses application dictionary stream
prefixes. Named `finalize`/`read` operations accept an externally verified,
nonempty identity of at most 65535 bytes. Anonymous graphs are valid query owners,
but transports must reject anonymous or ambiguous artifact keys. No ingestion
remap table, per-leaf tag or second JSON
schema tree is retained by this generic graph.

`schemaStatistics()` is prepared once. `schemaMemoryUsage()` reports retained
generic graph storage, excluding the dictionary and ModelPool payload so derived
owners can account for those exactly once. These are allocation lower bounds,
not allocator/RSS estimates. The extraction does not by itself establish a
performance improvement; whole-owner and runtime gates require separate evidence.

Each `ModelPool` installs a model-local shared-ownership group for each bound
graph. The canonical `SchemaPool` pointer, definitions, identity and dictionary
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

The generic native `SchemaPool` codec is version 4. It serializes canonical fixed
columns and arena collections, including exact ordinal bits and their
presence/signedness tag. Readers reject other versions, malformed rows and arena
handles, invalid static names and out-of-budget amplification rather than
silently dropping metadata. `importGraph` preserves ordinals while remapping
graph-local type IDs.

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
optionally owning `SchemaType`. Int/Float/String categories do not change. Identity
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

References also carry `predicateCanMatch`. For a positive string equality such
as `**.value == "SPEED_LIMIT"`, it is false when the referenced value is provably
numeric or a closed enum without that label. Search/style discovery can ignore
these owners without changing the user's query. The reference remains available
for diagnostics. Free strings, unknown metadata and recursive compositions stay
conservative, as do comparisons under negation or used as values rather than
positive predicates.

`hasDirectEnumSymbol(text)` checks the canonical dictionary by spelling even when
that symbol does not exist in the runtime dictionary. Recursive/open schema enum
predicates never intern datasource metadata or reuse compilation-pool IDs in
another environment.

## Lazy Descriptors

```cpp
#include <simfil/model/schema-node.h>

auto root = record.modelNode(); // Retains the same SchemaPool.
auto queryStrings = std::make_shared<simfil::StringPool>(*schema->strings());
simfil::Environment env(queryStrings);
auto query = simfil::compile(env, "fields.category.kind", false);
```

`SchemaType::modelNode()` retains the pool explicitly. Descriptor child addresses
refer directly to canonical records/collections in that same owner. Navigation
does not allocate or cache a parallel descriptor graph and never interns strings.
Repeated `get`, `at` and `keyAt` calls are history-independent, including recursive
edges. Published descriptors can be shared across readers; traversal state belongs
to each query/export, not the graph.

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
of them. Descriptor `describedType()` is the represented type; its runtime
`schemaType()` is empty and `schema()` is `NoTypeId`.
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
JSON export stops active-path cycles with a reference such as
`{"kind":"object","$ref":1,"truncated":"cycle"}`. Shared sibling definitions
remain independently expandable. `SchemaTypeModelNode::JsonOptions` bounds depth,
nodes and charged output bytes; exhaustion throws instead of returning partial
JSON. Optional reserve/scalar callbacks let an embedding application share a
budget or apply exact-integer output policy without another descriptor traversal.
Accessor/callback internals and allocator overhead are not a process sandbox.

Recursive query evaluation separately suppresses active-path back-edges before
emitting them, without changing explicit finite navigation or sibling order and
multiplicity. Work/cancellation checks precede cycle suppression. Ordinary
tree-model evaluation uses its no-cycle-tracking path. Use `EvaluationOptions`
and inspect the returned summary; descriptor access itself has no request-local
allocation counter or sticky exhausted state.

## Direct Completion

Field edges distinguish enumerable `SchemaType::FieldRole::Data` from lookup-only
`Context`. `forEachDirectField` and `directFields` still describe every declared
member; implicit discovery checks each edge's role before descent. Explicit lookup, including
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
auto candidates = simfil::complete(env, "items[17].na", 12, rootType, options);
```

This overload takes a `SchemaType` instead of a `ModelNode`. It reuses tokenization,
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

Model lookup aliases are separate: object/array `addAlias` declares a lookup-only
spelling without duplicating enumerated field paths. `canonicalField`, member-type
resolution, path discovery, wildcard plans and `referencedSchemaPaths` share that
canonical metadata. Reachability includes descendant aliases; a canonical-only
name index cannot prove an alias absent. For example, mapget features
resolve `attributes` to `properties` unless a real `attributes` member is declared.

`limit`, `timeoutMs`, `maxSchemaVisits` (default 10000), and `maxSchemaDepth`
(AST inference depth, default 128) bound candidate/traversal work. Domain-owner
hooks must also be cheap and finite; Simfil cannot interrupt code inside a hook.
The existing model-based overload remains for real data, not
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
copying into another exact graph. Untyped leaves remain untyped and do
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
An independently selected scalar Value with provenance becomes a typed-scalar node
holding a same-pool primitive address and local type ID. Unannotated values
remain ordinary storage. There is no destination-schema inference, generated
result schema, or per-result parallel metadata table. Bitmasks always use their
dedicated column. Numeric/string JSON export remains plain and intentionally
does not preserve scalar schema annotations; native binary results do.

Binary readers require every stored graph identity to resolve exactly before
returning success. Install `setSchemaPool` or `setSchemaResolver` first.
`hasDirectSchemaPool(identity)` checks a payload's single-graph contract
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

The bitmask/typed-scalar columns and common graph bindings change the native ModelPool
wire layout and static dictionary assignments. Dependent binary protocols must
version this break; there is no implicit legacy conversion.

## Validation

`test/schema-domain.cpp` exercises descriptors, specialized kinds, combiners,
completion, derived owners, recursive rewrites, and namespace ownership.
`test/schema-type.cpp`, `test/schema-node.cpp` and `test/schema-append-checkpoint.cpp`
cover typed construction, ownership, direct descriptors, canonical guards,
transactional append/rollback and malformed native storage.
`test/evaluation.cpp` covers streaming limits, interruption, compound ownership,
callback errors, and streaming builtins. `test/model-copy.cpp` covers native
undefined, sequence cardinality, ownership, dictionary remapping and copy limits.
`test/schema-value.cpp` covers exact scalar provenance, named predicates, mask
operators/roundtrips, mixed result graphs, freeze boundaries and malformed reads.
Its constructor-time reader regression checks typed scalar storage without
requiring `shared_from_this()` before a derived model has finished construction.
`test/schema-lookup.cpp` covers direct members, named unions, aliases, duplicate
declarations, incomplete/cyclic metadata and canonical member indexes.
Existing language/model tests continue to cover the unbounded APIs. Descriptors,
direct completion, bounded evaluation and native copying are also available with
`SIMFIL_WITH_MODEL_JSON=OFF`; JSON serialization is not their storage backend.

The JSON option controls the nlohmann dependency, JSON ingestion and the JSON
export methods on model nodes, including schema descriptors. Keep it disabled
for consumers that only need native models and binary transport. Descriptor
navigation, compilation and schema-based completion remain available in that
configuration; enabling descriptor JSON alone would require the base model's
JSON API and dependency too.

Run release benchmarks without other builds/tests consuming the machine:

```bash
build-release/test/test.simfil '[perf.schema],[perf.completion]' \
  --benchmark-samples 40 --benchmark-resamples 2000
```

The sparse-wide fixture compares no schema, basic pruning, generic field plans,
and an exact-path ceiling on identical data. It also repeats basic pruning,
field plans and the exact path with model-owned schema provenance; these costs
must not be hidden behind the Environment-only fallback control. Scalar leaves
have explicit scalar types: unknown child metadata does not mean scalar. Completion
benchmarks compare real-model/schema root suggestions with direct root-type
suggestions and a nested array/union index. Setup and finalization are outside
timed loops. They are diagnostic measurements, not flaky wall-time assertions.
