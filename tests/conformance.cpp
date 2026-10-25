#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool ok, const std::string& message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f, const std::string& message) {
    ++checks;
    try { f(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected explicit rejection: " + message);
}
std::string file(const std::string& name) {
    std::ifstream s(std::string(FIXTURES) + "/" + name);
    if (!s) throw std::runtime_error("Missing fixture: " + name);
    return {std::istreambuf_iterator<char>(s), {}};
}
Value v(const J& j) { return decodeValue(j.dump()); }
int main(int argc, char** argv) {
    try {
        auto g = Graph::decode(file("person-v1.json"));
        auto reference = J::parse(file("reference.json"));
        for (const auto& c : reference.at("cases")) {
            auto errors = validateValue(*g.descriptions.at(c.at("description")), v(c.at("input")));
            auto result = errors.empty() ? "valid" : errors.front().code;
            check(result == c.at("result"), "GT scalar reference disagreement");
        }
        auto& ada = g.objects.at("ada");
        auto name = g.descriptions.at("name"), age = g.descriptions.at("age"), active = g.descriptions.at("active");
        check(ada.description == g.descriptions.at(g.root), "Graph preserves description identity");
        check(validate(*ada.description, ada).empty(), "Valid Person");
        check(ada.readUsing(*active) == v(reference.at("rawMissing")), "Missing read does not substitute default");
        check(active->defaultValue() == v(reference.at("mementoDefault")), "Declared default");
        check(name->property("label") == Value(reference.at("selfLabel").get<std::string>()), "Self label");
        auto meta = descriptionModel();
        auto requiredMeta = meta->children.at(3);
        check(name->readUsing(*requiredMeta) == Value(reference.at("selfRequired").get<bool>()), "Self required");
        name->writeUsing(*requiredMeta, Value(false));
        check(validateValue(*name, {}).empty(), "Meta edit changes validation");
        name->writeUsing(*requiredMeta, Value(true));
        check(validate(*meta, *name).empty(), "Description is a described model");
        auto originalOrder = ada.description->orderedChildren();
        for (std::size_t i=0; i<originalOrder.size(); ++i) check(originalOrder[i]->id == reference.at("containerOrder").at(i), "Insertion order");
        ada.description->className = "MAPriorityContainer";
        auto ordered = ada.description->orderedChildren();
        for (std::size_t i=0; i<ordered.size(); ++i) check(ordered[i]->id == reference.at("priorityOrder").at(i), "Priority order");
        ada.description->className = "MAContainer";
        name->properties["readOnly"] = Value(true);
        check(validateValue(*name, {}).empty(), "Read-only skips validation");
        ada.writeUsing(*name, Value("Raw write"));
        check(ada.readUsing(*name) == Value("Raw write"), "Raw writes ignore presentation flags");
        name->properties.erase("readOnly"); name->properties["visible"] = Value(false);
        check(validateValue(*name, {}).empty(), "Hidden skips validation");
        name->properties.erase("visible"); name->properties["undefinedValue"] = Value("unset");
        check(validateValue(*name, Value("unset")).empty(), "Custom undefined sentinel");
        name->properties.erase("undefinedValue"); ada.writeUsing(*name, Value("Ada"));
        auto changed = Graph::decode(file("person-v2.json"));
        ada.description = changed.descriptions.at(changed.root);
        auto errors = validate(*ada.description, ada);
        check(errors.size() == 2 && errors[0].descriptionId == "age" && errors[1].descriptionId == "email", "Existing object adopts changed shape and constraint");
        ada.writeUsing(*changed.descriptions.at("age"), Value(std::int64_t(21)));
        ada.writeUsing(*changed.descriptions.at("email"), Value("ada@example.org"));
        check(validate(*ada.description, ada).empty(), "Generic edit validates new shape");
        AdaptiveModel fresh; fresh.description = ada.description;
        check(validate(*fresh.description, fresh).size() == 2, "New objects use changed shape");
        // Export a native-produced graph for the independent Smalltalk reader check.
        changed.objects.at("ada") = ada;
        auto output = changed.encode();
        check(Graph::decode(output).encode() == output, "Description/value round trip");
        if (argc == 2) { std::ofstream out(argv[1]); out << output; check(bool(out), "Write native export"); }
        // A graph with an unregistered description pointer must not silently flatten identity.
        rejects([&] { (void)g.encode(); }, "stale description identity");
        g = Graph::decode(file("person-v1.json"));
        auto second = std::make_shared<Description>();
        second->id = "second"; second->className = "MAContainer";
        second->children = {g.descriptions.at("name")};
        g.descriptions.emplace(second->id, second);
        auto aliases = Graph::decode(g.encode());
        check(aliases.descriptions.at("second")->children[0] == aliases.descriptions.at("name"), "Shared description identity survives round trip");
        g.descriptions.erase("second");
        g.objects.at("ada").writeUsing(*g.descriptions.at("active"), {});
        check(Graph::decode(g.encode()).objects.at("ada").values.count("active") == 1, "Explicit nil stays present");
        g.objects.at("ada").values.erase("active");
        check(Value("é") != Value("e\xcc\x81"), "Value string equality does not normalize Unicode");
        auto encoded = g.encode();
        check(Graph::decode(encoded).encode() == encoded, "Initial graph round trip");
        g.descriptions.at("name")->properties["metadata:hint"] = Value(Value::Array{Value("text"), Value(std::int64_t(3))});
        check(Graph::decode(g.encode()).descriptions.at("name")->properties.at("metadata:hint") == g.descriptions.at("name")->properties.at("metadata:hint"), "Opaque metadata preserved");
        auto values = J::parse(file("values.json"));
        for (const auto& e : values.at("roundtrips")) check(J::parse(encodeValue(v(e))) == e, "Exact value round trip");
        for (const auto& e : values.at("rejected")) rejects([&] { (void)v(e); }, "unsupported or invalid value");
        for (const auto& e : J::parse(file("invalid-graphs.json"))) rejects([&] { (void)Graph::decode(e.at("graph").dump()); }, e.at("name"));
        rejects([] { (void)decodeValue("{\"type\":\"nil\",\"type\":\"nil\"}"); }, "duplicate JSON key");
        rejects([] { (void)decodeValue("{\"type\":\"nil\",\"ty\\u0070e\":\"nil\"}"); }, "escaped duplicate JSON key");
        rejects([] { (void)decodeValue("{\"type\":\"nil\"} trailing"); }, "malformed JSON");
        Limits l; l.bytes = 10;
        rejects([&] { (void)Graph::decode(encoded, l); }, "byte bound");
        l = {}; l.nodes = 1; rejects([&] { (void)Graph::decode(encoded, l); }, "node bound");
        l = {}; l.depth = 1; rejects([&] { (void)Graph::decode(encoded, l); }, "depth bound");
        l = {}; l.items = 5; rejects([&] { (void)Graph::decode(encoded, l); }, "item bound");
        l = {}; l.visits = 1; rejects([&] { (void)validate(*g.descriptions.at(g.root), g.objects.at("ada"), l); }, "traversal bound");
        auto local = g.descriptions.at("name");
        local->conditions.push_back({"Must start with A", [](const Value& e) { return std::get<std::string>(e.data).rfind("A", 0) == 0; }});
        check(validateValue(*local, Value("Bob")).at(0).code == "MAConditionError", "Local condition executes");
        rejects([&] { (void)g.encode(); }, "closure export");
        local->conditions.clear(); local->defaultFunction = [] { return Value("Generated"); };
        check(local->defaultValue() == Value("Generated"), "Local functional default");
        rejects([&] { (void)g.encode(); }, "functional default export"); local->defaultFunction = {};
        struct Person { std::string name; } native{"Before"};
        NativeModel adapter(&native);
        local->nativeRead = [](const void* p) { return Value(static_cast<const Person*>(p)->name); };
        local->nativeWrite = [](void* p, const Value& e) { static_cast<Person*>(p)->name = std::get<std::string>(e.data); };
        adapter.writeUsing(*local, Value("After"));
        check(adapter.readUsing(*local) == Value("After") && native.name == "After", "Native pluggable accessor");
        rejects([&] { (void)g.encode(); }, "native accessor export");
        DictionaryModel dictionary; dictionary.writeUsing(*local, Value("Mapped"));
        check(dictionary.readUsing(*local) == Value("Mapped"), "Dictionary accessor");
        auto cyclic = descriptionModel(); cyclic->children.push_back(cyclic);
        rejects([&] { visit(*cyclic, [](const auto&) {}); }, "local graph cycle");
        cyclic->children.pop_back();
        cyclic->className = "MAPriorityContainer"; cyclic->children.push_back(nullptr);
        rejects([&] { visit(*cyclic, [](const auto&) {}); }, "null local child");
        cyclic->children.pop_back();
        cyclic->properties["priority"] = Value("wrong");
        rejects([&] { (void)cyclic->priority(); }, "malformed local priority");
        std::cout << checks << " checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
