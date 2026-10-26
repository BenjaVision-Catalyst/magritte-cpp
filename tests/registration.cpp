#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <thread>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool yes, const std::string &msg) {
    if (!yes)
        throw Error(msg);
    ++checks;
}
template <class F> void rejects(F f, const std::string &msg) {
    bool yes = false;
    try {
        f();
    } catch (const std::exception &) {
        yes = true;
    }
    check(yes, msg);
}
J fixture(const std::string &n) {
    std::ifstream in(std::string(FIXTURES) + "/" + n + ".json");
    J j;
    in >> j;
    return j;
}
Value value(const J &j) { return decodeValue(j.dump()); }
ClassInfo info(const J &j) {
    return {j.at("name"),
            j.at("superclass").is_null() ? std::string() : j.at("superclass").get<std::string>(),
            j.at("abstract"), j.at("label").get<std::string>()};
}
// Historical GT snapshots have an explicit class environment; registrations remain global.
J projected(J names) {
    static const auto available = fixture("registration-reference").at("classes");
    J result = J::array();
    for (const auto& name : names)
        if (std::find(available.begin(), available.end(), name) != available.end()) result.push_back(name);
    return result;
}
J canonical(J j) {
    for (auto& d : j["descriptions"]) {
        if (d.contains("classes")) {
            d["classes"] = projected(d["classes"]);
            std::sort(d["classes"].begin(),d["classes"].end());
        }
        if (d.contains("options")) {
            J options = J::array();
            for(const auto& option : d["options"])
                if(option.at("type")!="class" || !projected(J::array({option.at("value")})).empty()) options.push_back(option);
            d["options"] = options;
        }
    }
    return j;
}
struct Holder {
    virtual ~Holder() = default;
    std::int64_t contents = 3;
};
struct DescriptionHolder : Holder {};
void reference(const char *output) {
    auto f = fixture("registration-reference");
    auto &registry = ClassRegistry::global();
    for (const auto &row : f.at("core")) {
        auto c = registry.find(row.at("name"));
        auto expected = info(row);
        check(c && c->superclass == expected.superclass && c->isAbstract == expected.isAbstract &&
                  c->label == expected.label,
              "GT bootstrap class declaration");
    }
    // Exercise lookup before registration to catch a stale cached allowlist.
    (void)decodeValue(R"({"type":"class","value":"String"})");
    rejects([] { decodeValue(R"({"type":"class","value":"MAValueHolder"})"); },
            "Unknown classes require registration");
    auto relation = std::make_shared<Description>();
    relation->id = "relation";
    relation->className = "MAToOneRelationDescription";
    relation->key = "relation";
    relation->properties["label"] = Value("Relation");
    auto reference = std::make_shared<Description>();
    reference->id = "relation.reference";
    reference->className = "MAContainer";
    relation->reference = reference;
    RecursiveDescriptionModel before;
    auto old = before.describeReflective(relation);
    auto previous = before.resolver().descriptions.at(old->id + ".classes")->options;
    std::vector<ClassInfo> definitions;
    for (const auto &row : f.at("additional"))
        definitions.push_back(info(row));
    const auto added = std::count_if(definitions.begin(), definitions.end(),
                                     [&](const auto &d) { return !registry.find(d.name); });
    registry.registerClasses(definitions);
    check(projected(J(registry.allClasses())) == f.at("classes"), "Discovered registered classes match GT projection");
    check(previous.size() + added == registry.allClasses().size(),
          "Registration extends the available environment");
    check(before.resolver().descriptions.at(old->id + ".classes")->options == previous,
          "Existing metadata retains its construction-time choices");
    for (const auto &q : f.at("queries")) {
        auto n = q.at("class").get<std::string>();
        check(projected(J(registry.subclasses(n))) == q.at("subclasses"), "GT direct subclasses");
        check(projected(J(registry.allSubclasses(n))) == q.at("allSubclasses"), "GT transitive subclasses");
        check(projected(J(registry.withAllSubclasses(n))) == q.at("withAllSubclasses"), "GT inclusive subclasses");
        check(projected(J(registry.withAllConcreteClasses(n))) == q.at("withAllConcreteClasses"),
              "GT concrete subclasses");
    }
    for (const auto &q : f.at("includes")) {
        check(registry.includesBehavior(q.at("class"), q.at("ancestor")) == q.at("includes"),
              "GT includesBehavior");
        check(registry.inheritsFrom(q.at("class"), q.at("ancestor")) == q.at("inherits"),
              "GT strict inheritance");
    }
    check(registry.commonSuperclass({"MAValueHolder", "MADescriptionHolder"}) ==
              std::optional<std::string>(f.at("commonClass")),
          "GT common superclass");
    registry.registerNativeClass<Holder>(*registry.find("MAValueHolder"));
    registry.registerNativeClass<DescriptionHolder>(*registry.find("MADescriptionHolder"));
    Holder base;
    DescriptionHolder derived;
    Holder *dynamic = &derived;
    auto native = std::make_shared<NativeModel>(NativeModel::registered(&base)),
         child = std::make_shared<NativeModel>(NativeModel::registered(dynamic));
    check(child->kindOf("MAValueHolder") && child->kindOf("MADescriptionHolder"),
          "Native RTTI selects the registered dynamic type and inherited classes");
    native->readers["MASelectorAccessor:contents"] = [](const void *p) {
        return Value(static_cast<const Holder *>(p)->contents);
    };
    native->writers["MASelectorAccessor:contents:"] = [](void *p, const Value &v) {
        static_cast<Holder *>(p)->contents = std::get<std::int64_t>(v.data);
    };
    Description contents;
    contents.id = "contents";
    contents.className = "MANumberDescription";
    Accessor accessor;
    accessor.className = "MASelectorAccessor";
    accessor.name = "contents";
    accessor.writeName = "contents:";
    contents.accessor = accessor;
    check(native->readUsing(contents) == value(f.at("reads").at(0)), "GT registered object read");
    native->writeUsing(contents, Value(std::int64_t(9)));
    check(native->readUsing(contents) == value(f.at("reads").at(1)), "GT registered object write");
    Graph local;
    local.nativeObjects["base"] = native;
    local.nativeObjects["derived"] = child;
    int object;
    local.nativeObjects["object"] = std::make_shared<NativeModel>(&object);
    relation->classes = {"MAValueHolder"};
    std::vector<Value> candidates{Value(Value::Reference{"base"}), Value(Value::Reference{"derived"}),
                                  Value(Value::Reference{"object"}), Value(std::int64_t(3)), Value("hello")};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        std::vector<std::string> codes;
        for (const auto &e : validateValue(*relation, candidates[i], &local))
            codes.push_back(e.code);
        check(J(codes) == f.at("validation").at(i).at("errors"), "GT registered native relation validation");
    }
    for (const auto &row : f.at("scalars")) {
        relation->classes = {row.at("class")};
        std::vector<std::string> codes;
        for (const auto &e : validateValue(*relation, value(row.at("input"))))
            codes.push_back(e.code);
        check(J(codes) == row.at("errors"), "GT scalar class hierarchy");
    }
    relation->classes = {"MAValueHolder"};
    RecursiveDescriptionModel context;
    auto schema = context.describeReflective(relation);
    auto binding = context.bind(relation);
    auto expected = f.at("schema");
    auto graph = context.schemas();
    check(canonical(J::parse(graph.encode())) == canonical(expected.at("graph")),
          "GT metadata discovers all registered class options");
    for (const auto &read : expected.at("reads"))
        check(binding->readUsing(*context.resolver().descriptions.at(read.at("field"))) ==
                  value(read.at("value")),
              "GT dynamic class metadata getter");
    auto choices = context.resolver().descriptions.at(schema->id + ".classes");
    CheckedMemento edit(*binding, schema, &context.resolver());
    edit.writeUsing(*choices, Value(Value::Collection{"set", {Value(Value::Class{"MADescriptionHolder"})}}));
    check(edit.validate().empty(), "Registered class selection validates");
    edit.commit();
    check(!validateValue(*relation, Value(Value::Reference{"base"}), &local).empty() &&
              validateValue(*relation, Value(Value::Reference{"derived"}), &local).empty(),
          "Class metadata edits change actual inherited relation constraints");
    relation->classes = {"MAValueHolder"};
    auto app = std::make_shared<Description>();
    app->id = "app";
    app->className = "MAContainer";
    auto kind = std::make_shared<Description>();
    kind->id = "kind";
    kind->className = "MAClassDescription";
    kind->key = "kind";
    auto option = std::make_shared<Description>();
    option->id = "choice";
    option->key = "choice";
    option->className = "MASingleOptionDescription";
    auto classRef = std::make_shared<Description>();
    classRef->id = "choice.reference";
    classRef->className = "MAClassDescription";
    Accessor null;
    null.className = "MANullAccessor";
    classRef->accessor = null;
    option->reference = classRef;
    option->options = {Value(Value::Class{"MAValueHolder"}), Value(Value::Class{"MADescriptionHolder"})};
    app->children = {kind, option, relation};
    graph = context.schemas();
    graph.root = "app";
    for (auto d : {app, kind, option, classRef, relation, reference})
        graph.descriptions[d->id] = d;
    relation->materializeAccessor();
    AdaptiveModel sample;
    sample.id = "sample";
    sample.description = app;
    sample.values = {{"kind", Value(Value::Class{"MADescriptionHolder"})},
                     {"choice", Value(Value::Class{"MADescriptionHolder"})}};
    graph.objects[sample.id] = sample;
    auto wire = graph.encode();
    auto imported = Graph::decode(wire);
    check(imported.encode() == wire, "Registered class graph fixed point");
    check(validate(*imported.descriptions.at("app"), imported.objects.at("sample"), {}, &imported).empty(),
          "Registered class graph validates after import");
    if (output) {
        std::ofstream out(output);
        out << wire;
        check(bool(out), "Registration native export");
    }
}
struct Person {
    virtual ~Person() = default;
};
struct Employee : Person {};
struct Unregistered {};
struct Alias {};
void boundaries() {
    auto &r = ClassRegistry::global();
    auto before = r.allClasses();
    rejects([&] { r.registerClasses({{"ExampleA", "ExampleB"}, {"ExampleB", "ExampleA"}}); },
            "Cyclic registration is rejected atomically");
    check(r.allClasses() == before, "Failed batch leaves no classes");
    rejects([&] { r.registerClasses({{"ExampleValid"}, {"ExampleBad", "MissingSuperclass"}}); },
            "Unknown parent rejects the complete batch");
    check(r.allClasses() == before, "Unknown parent leaves no partial registration");
    rejects([&] { r.registerClass({"Bad Name"}); }, "Invalid identity rejected");
    r.registerClasses({{"ExampleEmployee", "ExamplePerson"}, {"ExamplePerson", "Object"}});
    r.registerNativeClass<Person>(*r.find("ExamplePerson"));
    r.registerNativeClass<Employee>(*r.find("ExampleEmployee"));
    r.registerNativeClass<Person>(*r.find("ExamplePerson"));
    Employee employee;
    Person *person = &employee;
    auto model = NativeModel::registered(person);
    check(model.kindOf("ExamplePerson") && model.kindOf("ExampleEmployee") && model.kindOf("Object"),
          "Application class inheritance requires no ancestry list");
    check(decodeValue(encodeValue(Value(Value::Class{"ExampleEmployee"}))) ==
              Value(Value::Class{"ExampleEmployee"}),
          "Application class values use runtime registration");
    rejects([&] { r.registerClass({"ExamplePerson", "Object", true}); },
            "Class identity cannot be rebound to conflicting semantics");
    rejects([&] { r.registerNativeClass<Alias>(*r.find("ExamplePerson")); },
            "A second host type cannot impersonate a registered class");
    rejects([&] { r.registerNativeClass<Person>({"LeakedClass", "Object"}); },
            "A host type cannot be rebound to another class");
    check(!r.find("LeakedClass"), "Failed native binding leaves no class");
    Unregistered unknown;
    rejects([&] { NativeModel::registered(&unknown); }, "Unregistered host type fails explicitly");
    auto names = r.allClasses();
    rejects([] { decodeValue(R"({"type":"class","value":"WireInventedClass"})"); },
            "Untrusted class names cannot register themselves");
    check(r.allClasses() == names, "Import does not mutate class environment");
    r.registerClass({"ExampleEmptyLabel", "Object", false, ""});
    check(r.find("ExampleEmptyLabel")->label.empty(), "Explicit empty labels are preserved");
    r.registerClass({"ExampleUnicodeLabel", "Object", false, "\xc3\xa9"});
    rejects([&] { r.registerClass({"ExampleUnicodeLabel", "Object", false, "e\xcc\x81"}); },
            "Class labels retain exact UTF-8 content");
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            try {
                for (int j = 0; j < 8; ++j) {
                    r.registerClass({"ConcurrentClass" + std::to_string(i * 8 + j)});
                    r.registerClass({"ConcurrentShared"});
                    (void)r.allSubclasses("Object");
                }
            } catch (...) {
                failed = true;
            }
        });
    for (auto &t : threads)
        t.join();
    check(!failed, "Concurrent registration and discovery");
    for (int i = 0; i < 64; ++i)
        check(r.find("ConcurrentClass" + std::to_string(i)).has_value(),
              "Concurrent class remains discoverable");
}
int main(int argc, char **argv) {
    try {
        reference(argc > 1 ? argv[1] : nullptr);
        boundaries();
        std::cout << checks << " registration checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << "After " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
