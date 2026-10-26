# Magritte C++

A standalone C++17 port of [Magritte](https://github.com/magritte-metamodel/magritte). **0.26 adds the generic-object nil-read fallback; it is not a complete Magritte implementation.** No engine, UI, networking or application-framework dependency.

It includes Magritte containers, a String/Symbol/Boolean/Int64/binary64 scalar subset, runtime adaptive objects, native accessors, to-one/to-many/scalar relations, static options, typed collections, cyclic object references, validation, a self-description bootstrap and bounded graph/value interchange. Loaded descriptions govern generic objects and can change at runtime without compiling new structs. Unsupported semantics fail explicitly.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/verify-fixtures.py
cmake --install build --prefix /your/install/prefix
```

Consumers use `find_package(Magritte CONFIG REQUIRED)` and link `Magritte::magritte`. The JSON parser is vendored privately; build/install work offline.

```cpp
#include <magritte/magritte.hpp>
// jsonText contains conformance/person-v1.json, loaded by your application.
auto graph = magritte::Graph::decode(jsonText);
auto& person = graph.objects.at("ada");
auto name = graph.descriptions.at("name");
person.writeUsing(*name, magritte::Value("Ada"));
auto errors = magritte::validate(*person.description, person);
auto wire = graph.encode();
```

To adopt person-v2.json without recompilation:

```cpp
auto changed = magritte::Graph::decode(changedJsonText);
person.description = changed.descriptions.at(changed.root);
auto errors = magritte::validate(*person.description, person);
// Errors identify age (new minimum 21) and email (new required field).
// Write through those loaded descriptions, validate again, and register the
// object in changed.objects before exporting the replacement graph.
```

NativeModel borrows an existing object; set Description.nativeRead/nativeWrite callbacks for its native members. DictionaryModel uses MADictionaryAccessor keys; AdaptiveModel keys by stable description identity. Raw writes and validation are separate Magritte operations. Description itself implements Model, so descriptionModel() supports reading/editing common description properties through the same interfaces. See tests/conformance.cpp for executed native-object and meta-edit examples.

Read the [audit and support matrix](docs/audit.md), [wire contract and limits](docs/interchange.md), [reference/testing evidence](docs/conformance.md), and [attribution](NOTICE.md). macOS arm64 was executed with Apple Clang 21/CMake 4.4.3. Linux/Windows are portability intentions, not verified results. APIs are mutable and need caller synchronization. Public failures use magritte::Error; native callbacks may propagate their own exceptions. Int64 is not the complete Magritte numeric domain.

Relationship examples, GT evidence and remaining limits are documented in [relationships.md](docs/relationships.md). For reference-valued validation pass the owning Graph as the resolver context. The graph/2 fixture demonstrates aliases, cycles, options and nested adaptive values without domain-specific native types.

```cpp
auto graph = magritte::Graph::decode(relationalJSON);
auto& ada = graph.objects.at("ada");
auto spouse = graph.descriptions.at("spouse");
const auto& target = graph.resolve(ada.readUsing(*spouse));
auto errors = magritte::validate(*ada.description, ada, {}, &graph);
auto choices = graph.descriptions.at("status")->allOptions();
```

The 0.3 phase adds labelled portable MACondition objects, dictionary/property/selector/variable/identity/null/chain accessors, memo/password/token descriptions, finite binary64, and MAStringReader/Writer behavior. See [portable-behavior.md](docs/portable-behavior.md) for executed GT evidence, APIs, numeric policies and remaining gaps. Graph/3 carries these forms; legacy graph/1 and graph/2 remain readable. Native names require host bindings; callback behavior remains local.

The 0.4 phase adds editing and metadata editors, including original/cache tracking, conflict reset and selective checked commits. Class-specific self-description covers all fourteen supported classes through ordinary selector accessors. See [editing.md](docs/editing.md) for APIs, GT/upstream evidence, collection snapshot boundaries and remaining metadata gaps. Editing state stays local; committed descriptions and objects use graph/3.

The 0.5 phase recursively describes metadata objects and edits their child/reference links through ordinary mementos. Description equality, concatenation, union/intersection, selective copies, filtering and transformation follow the audited Magritte protocol. See [composition.md](docs/composition.md) for ownership, APIs, GT evidence and profile limits. Generated metadata remains ordinary graph/3 data; local adapters are explicit session state.

The 0.6 phase adds MAClassDescription, a finite class registry, class-valued interpreter/relation metadata and live accessor-object editing. Conditions retain their actual upstream empty annotated schemas. See [reflection.md](docs/reflection.md) for APIs, GT evidence, graph/4 and explicit accessor snapshot boundaries.

The 0.7 phase binds actual mementos through their represented descriptions and bootstraps metadata for a reachable description graph in one bounded batch. See [bootstrap.md](docs/bootstrap.md) for identity, nested editing, the 65-class registry, current schema closure and GT evidence.

The 0.8 class environment supports application class registration, RTTI/metatype lookup, inheritance and subclass discovery. Metadata, validation and codecs use that environment. See [registration.md](docs/registration.md) for native examples, identity/ownership rules and GT evidence.

The 0.9 phase adds `acceptMagritte`, native visitor overrides with inherited fallback, and the stateful `MADescriptionBuilder` copy/enrichment protocol. See [visitors.md](docs/visitors.md) for APIs, the upstream/GT audit, ownership and remaining semantics.

The 0.10 phase implements `MADynamicOptions` completion, membership, reader and shallow-copy semantics. `Options` preserves collection/provider results; known static callers use `.values()` explicitly. See [dynamic-options.md](docs/dynamic-options.md) for APIs, the source migration, GT evidence and export boundaries.

The 0.11 phase adds `Accessor.canRead`/`canWrite`, explicit host selector/variable discovery, and independent absent read/write selectors. See [accessor-capabilities.md](docs/accessor-capabilities.md) for the exact upstream rules, GT traces, APIs and remaining reflection boundaries.

The 0.12 phase adds local `MAPluggableAccessor` block slots, block-identity equality, shallow copying and chain/memento integration. See [pluggable-accessors.md](docs/pluggable-accessors.md) for APIs, GT evidence, ownership and explicit closure interchange boundaries.

The 0.13 phase implements accessor-derived names/default labels, explicit nil/empty overrides, faithful lazy null defaults and metadata integration. Swift property reads now require `try`. See [accessor-inference.md](docs/accessor-inference.md) for APIs, GT evidence, bounds and the remaining Unicode inflection boundary.

Container query and enumeration APIs, GT traces and remaining boundaries are documented in [container-enumeration.md](docs/container-enumeration.md).

Composite accessors, conflict object semantics and GT verification are documented in [composites.md](docs/composites.md).

Builder composition, flattener extension hooks, exact priorities and GT evidence are documented in [builders.md](docs/builders.md).

Runtime method registration, recursive discovery, defining contexts and the deferred fresh-GT verification are documented in [pragma-discovery.md](docs/pragma-discovery.md).

Description display conversion, option label/sort behavior, source evidence and native API boundaries are documented in [display.md](docs/display.md). GT was not accessed for this phase.

The MASortBlock protocol, Value-target accessors and source/native verification are documented in [sorting.md](docs/sorting.md). GT was not accessed for this phase.

Reference getter side effects, option undefined cascades and snapshot boundaries are documented in [reference-access.md](docs/reference-access.md). GT was not accessed for this phase.

Stored property semantics, nil/presence rules and source/native evidence are documented in [properties.md](docs/properties.md). GT was not accessed for this phase.

Nil-only lazy initialization, the raw stored-property bridge and initial-answer fallback are documented in [lazy.md](docs/lazy.md), with source-derived evidence and explicit snapshot/callback boundaries. GT was not accessed for this phase.

Description error-message overrides, generated numeric range text, source evidence and exception-profile boundaries are documented in [messages.md](docs/messages.md). GT was not accessed for this phase.

Described-object printing, nil/separator/read ordering, local formatter hooks and native stream boundaries are documented in [printing.md](docs/printing.md). GT was not accessed for this phase.

Default initialization and nil/default-aware model comparison are documented in [model-protocol.md](docs/model-protocol.md), including provider evaluation, host equality, existing adapter gaps and source-derived evidence. GT was not accessed for this phase.

Generic model reads now substitute `undefinedValue` for nil, with raw-accessor, adaptive-model and memento distinctions documented in [model-read.md](docs/model-read.md). GT was not accessed for this phase.
