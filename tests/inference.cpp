#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok)
        throw Error(what);
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const Error &) {
        failed = true;
    }
    check(failed, "Expected failure");
}
Value value(const J &j) { return decodeValue(j.dump()); }
J wire(const Value &v) { return J::parse(encodeValue(v)); }
Accessor accessor(const J &j) {
    Accessor a;
    a.className = j.at("class");
    if (j.contains("key"))
        a.name = j.at("key");
    if (j.contains("name"))
        a.name = j.at("name");
    if (j.contains("readSelector")) {
        a.name = j.at("readSelector");
        a.writeName = j.at("writeSelector");
    }
    if (j.contains("uuid"))
        a.uuid = value(j.at("uuid"));
    if (j.contains("steps"))
        for (const auto &s : j.at("steps"))
            a.steps.push_back(accessor(s));
    if (j.contains("next"))
        a.steps.push_back(accessor(j.at("next")));
    return a;
}
template <class F> void reference(F read, const J &expected, const std::string &context) {
    if (expected.contains("error"))
        rejects(read);
    else
        check(read() == value(expected.at("value")), context);
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/accessor-inference-reference.json");
        J f;
        in >> f;
        for (const auto &row : f.at("cases")) {
            auto a = accessor(row.at("accessor"));
            Description d;
            d.id = "target";
            d.accessor = a;
            const auto name = row.at("id").get<std::string>();
            reference([&] { return a.accessorName(); }, row.at("name"), "GT accessor name " + name);
            reference([&] { return d.property("name"); }, row.at("descriptionName"),
                      "GT description name " + name);
            auto object = AccessorObject::fromAccessor("accessor", a);
            d.accessor.reset();
            d.accessorObject = object;
            reference([&] { return d.property("name"); }, row.at("name"),
                      "Live accessor name " + name);
            if (!row.at("ascii").get<bool>()) {
                rejects([&] { a.defaultLabelFor(d); });
                rejects([&] { d.property("label"); });
                d.properties["label"] = Value("Étiquette");
                check(d.property("label") == Value("Étiquette") && d.hasLabel(),
                      "Explicit Unicode label");
                continue;
            }
            reference([&] { return Value(a.defaultLabelFor(d)); }, row.at("label"),
                      "GT accessor label " + name);
            reference([&] { return d.property("label"); }, row.at("descriptionLabel"),
                      "GT description label " + name);
            check(d.hasLabel() == row.at("hasLabel").get<bool>(), "GT hasLabel " + name);
            check(d.readSlot("MASelectorAccessor", "defaultLabel") ==
                          value(row.at("label").at("value")) &&
                      d.readSlot("MASelectorAccessor", "hasLabel") ==
                          Value(row.at("hasLabel").get<bool>()),
                  "Derived getters use the Model protocol");
            auto schema = descriptionModelFor(d);
            check(schema.descriptions.at(schema.root + ".label")->property("default") ==
                      value(row.at("label").at("value")),
                  "Unprojected GT metadata label default " + name);
        }
        for (const auto &row : f.at("labelOverrides")) {
            Description d;
            d.id = "target";
            d.key = "firstName";
            d.properties["label"] = value(row.at("input"));
            check(d.property("label") == value(row.at("label")) &&
                      d.hasLabel() == row.at("hasLabel").get<bool>(),
                  "GT explicit nil/empty/text labels");
            check(Value(d.defaultLabel()) == value(row.at("defaultLabel")),
                  "Default ignores explicit label");
            auto schema = descriptionModelFor(d);
            check(schema.descriptions.at(schema.root + ".label")->property("default") ==
                      value(row.at("metadataDefault")),
                  "GT metadata default ignores explicit label");
        }
        for (const auto &row : f.at("nameOverrides")) {
            Description d;
            d.key = "firstName";
            d.properties["name"] = value(row.at("input"));
            check(d.property("name") == value(row.at("name")) &&
                      d.property("label") == value(row.at("label")),
                  "GT explicit name does not replace label inference");
        }
        for (const auto &row : f.at("classes")) {
            Description d;
            d.id = "target";
            d.className = row.at("class");
            if (!d.isContainer())
                d.key = "firstName";
            check(d.property("label") == value(row.at("label")) &&
                      Value(d.defaultLabel()) == value(row.at("defaultLabel")),
                  "GT class-specific defaults");
            auto schema = descriptionModelFor(d);
            auto label = schema.descriptions.at(schema.root + ".label");
            check(label->property("default") == value(row.at("metadataDefault")),
                  "GT unprojected class metadata default");
            auto second = descriptionModelFor(*label);
            check(second.descriptions.at(second.root + ".label")->property("default") ==
                      Value("Label"),
                  "Recursive label metadata uses selector inference");
        }
        auto d = std::make_shared<Description>();
        d->id = "field";
        d->key = "firstName";
        d->accessorObject = AccessorObject::fromAccessor("live", d->effectiveAccessor());
        J live = J::array();
        auto snapshot = [&] {
            live.push_back({{"name", wire(d->property("name"))},
                            {"label", wire(d->property("label"))},
                            {"hasLabel", d->hasLabel()},
                            {"defaultLabel", wire(Value(d->defaultLabel()))}});
        };
        snapshot();
        d->accessorObject->value.name = "lastName";
        snapshot();
        d->properties["label"] = Value("");
        d->accessorObject->value.name = "postalCode";
        snapshot();
        d->properties.erase("label");
        snapshot();
        Accessor chain;
        chain.className = "MAChainAccessor";
        Accessor head;
        head.name = "accountDetails";
        Accessor tail;
        tail.className = "MASelectorAccessor";
        tail.name = "postalCode";
        tail.writeName = "postalCode:";
        chain.steps = {head, tail};
        d->accessorObject = AccessorObject::fromAccessor("chain", chain);
        snapshot();
        check(live == f.at("live"), "GT live name/label edits");
        d->accessorObject.reset();
        d->accessor.reset();
        d->key = "firstName";
        auto schema = descriptionModelFor(*d);
        auto label = schema.descriptions.at(schema.root + ".label");
        auto root = std::make_shared<Description>();
        root->id = "editor";
        root->className = "MAContainer";
        root->children = {label};
        CachedMemento editor(*d, root);
        J editing = J::array();
        auto editSnapshot = [&] {
            editing.push_back({{"label", wire(d->property("label"))},
                               {"editor", wire(editor.readUsing(*label))},
                               {"hasLabel", d->hasLabel()},
                               {"changed", editor.hasChanges()}});
        };
        editSnapshot();
        editor.writeUsing(*label, Value(""));
        editSnapshot();
        editor.commit();
        editSnapshot();
        check(editing == f.at("editing"), "GT empty-label staged editing");
        RecursiveDescriptionModel session;
        auto binding = session.bind(d);
        Accessor name;
        name.className = "MASelectorAccessor";
        name.name = "name";
        name.writeName = "name:";
        check(name.read(*binding) == d->property("name"),
              "Recursive adapter delegates name protocol");
        auto copy = copyDescription(d, "copy");
        copy.root->key = "lastName";
        copy.root->properties.erase("label");
        check(copy.root->property("label") == Value("Last Name") &&
                  d->property("label") == Value(""),
              "Copy retains explicit property independently");
        int calls = 0;
        auto local =
            Accessor::pluggable(std::make_shared<const Accessor::ReadBlock>([&](const Model &) {
                ++calls;
                return Value();
            }));
        Description localDescription;
        localDescription.accessor = local;
        check(localDescription.property("label") == Value("") &&
                  localDescription.property("name").isNil() && calls == 0,
              "Metadata inference does not invoke blocks");
        Description unconfigured;
        check(unconfigured.effectiveAccessor().className == "MANullAccessor" &&
                  unconfigured.property("name").isNil() && !unconfigured.hasLabel(),
              "Unconfigured description uses null accessor");
        auto fresh = std::make_shared<Description>();
        fresh->id = "fresh";
        auto beforeCopy = copyDescription(fresh, "before");
        check(descriptionEqual(*fresh, *beforeCopy.root) ==
                  f.at("unconfigured").at("copyBeforeAccessEqual").get<bool>(),
              "GT copy before default accessor initialization");
        auto cached = fresh->effectiveAccessor();
        Description other;
        check(accessorEqual(cached, fresh->effectiveAccessor()) ==
                  f.at("unconfigured").at("stable").get<bool>(),
              "GT stable default null token");
        check((!accessorEqual(cached, other.effectiveAccessor())) ==
                  f.at("unconfigured").at("distinct").get<bool>(),
              "GT distinct unconfigured null accessors");
        auto afterCopy = copyDescription(fresh, "after");
        check(descriptionEqual(*fresh, *afterCopy.root) ==
                  f.at("unconfigured").at("copyAfterAccessEqual").get<bool>(),
              "GT copied initialized null token");
        Limits tiny;
        tiny.bytes = 1;
        rejects([&] { chain.defaultLabelFor(*d, tiny); });
        tiny = Limits{};
        tiny.visits = 1;
        rejects([&] { chain.accessorName(tiny); });
        tiny = Limits{};
        tiny.depth = 0;
        rejects([&] { chain.defaultLabelFor(*d, tiny); });
        auto cyclic = AccessorObject::fromAccessor("cyclic", chain);
        cyclic->next = cyclic;
        d->accessorObject = cyclic;
        d->properties["label"] = Value("Explicit");
        check(d->property("label") == Value("Explicit"),
              "Explicit label avoids accessor traversal");
        rejects([&] { d->defaultLabel(); });
        cyclic->next.reset();
        d->accessorObject.reset();
        d->properties.clear();
        d->key = "firstName";
        d->properties["required"] = Value(true);
        auto app = std::make_shared<Description>();
        app->id = "app";
        app->className = "MAContainer";
        app->children = {d};
        Graph graph;
        graph.format = "magritte.graph/4";
        graph.root = "app";
        graph.descriptions = {{"app", app}, {"field", d}};
        graph.objects["sample"].id = "sample";
        graph.objects["sample"].description = app;
        graph.objects["sample"].values["field"] = Value("Ada");
        auto encoded = graph.encode();
        auto decoded = Graph::decode(encoded);
        check(decoded.descriptions.at("field")->property("label") == Value("First Name"),
              "Inferred label survives graph interchange without explicit property");
        check(!decoded.descriptions.at("field")->properties.count("label"),
              "No inferred-label cache on wire");
        if (argc > 1) {
            std::ofstream out(argv[1]);
            out << encoded << '\n';
        }
        std::cout << checks << " inference checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "After " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
