# Magritte C++

A standalone C++17 port of [Magritte](https://github.com/magritte-metamodel/magritte). **0.2 includes the scalar and relationship/collection phases; it is not a complete Magritte implementation.** No engine, UI, networking or application-framework dependency.

It includes Magritte containers, a string/Boolean/Int64 scalar subset, runtime adaptive objects, native accessors, to-one/to-many/scalar relations, static options, typed collections, cyclic object references, validation, a self-description bootstrap and bounded graph/value interchange. Loaded descriptions govern generic objects and can change at runtime without compiling new structs. Unsupported semantics fail explicitly.

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
