#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Model protocol mismatch") {
    ++checks;
    if (!ok)
        throw Error(message);
}
template <class F> void fails(F f) {
    bool caught = false;
    try {
        f();
    } catch (const Error &) {
        caught = true;
    }
    check(caught, "Expected model protocol failure");
}
struct Trace : Model {
    std::string name;
    std::vector<std::string> &trace;
    std::map<std::string, Value> values;
    bool readable = true;
    std::function<void(const Description &)> beforeRead, afterWrite;
    Trace(std::string n, std::vector<std::string> &t) : name(std::move(n)), trace(t) {}
    bool respondsToSelector(const std::string &s) const override {
        trace.push_back(name + ":can:" + s);
        return readable;
    }
    Value readUsing(const Description &d) const override {
        trace.push_back(name + ":read:" + d.id);
        if (beforeRead)
            beforeRead(d);
        auto i = values.find(d.id);
        return i == values.end() ? Value() : i->second;
    }
    void writeUsing(const Description &d, Value v) override {
        trace.push_back(name + ":write:" + d.id);
        values[d.id] = std::move(v);
        if (afterWrite)
            afterWrite(d);
    }
};
std::shared_ptr<Description> field(const std::string &id) {
    auto d = std::make_shared<Description>();
    d->id = d->key = id;
    return d;
}
void selector(Description &d) {
    Accessor a;
    a.className = "MASelectorAccessor";
    a.name = d.id;
    a.writeName = d.id + ":";
    d.accessor = a;
}
struct EqualModel : Model {
    mutable int calls = 0;
    Value answer = Value(true);
    mutable std::vector<Value> arguments;
    Value binaryMessage(const std::string &s, const Value &v, const Graph *) const override {
        if (s != "=")
            throw Error("selector");
        ++calls;
        arguments.push_back(v);
        return answer;
    }
    Value readUsing(const Description &) const override { return Value(); }
    void writeUsing(const Description &, Value) override {}
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/model-protocol-scenarios.json");
        J fixture;
        in >> fixture;
        auto root = std::make_shared<Description>();
        root->id = "root";
        root->className = "MAContainer";
        for (const auto &row : fixture.at("initialization")) {
            auto d = field("data");
            root->children = {d};
            d->properties = {
                {"default", decodeValue(row.at("default").dump())},
                {"readOnly", Value(row.at("readOnly").get<bool>())},
                {"visible", Value(row.at("visible").get<bool>())},
                {"shouldCacheDefault", Value(row.at("shouldCacheDefault").get<bool>())},
                {"required", Value(true)},
                {"initialAnswer", Value("answer")}};
            std::vector<std::string> trace;
            Trace model("model", trace);
            model.values[d->id] = Value("prior");
            check(&initializeMagritte(model, *root) == &model);
            check(model.values.at(d->id) == decodeValue(row.at("result").dump()));
            check(trace == (row.at("writes").empty()
                                ? std::vector<std::string>{}
                                : std::vector<std::string>{"model:write:data"}));
        }
        for (const auto &row : fixture.at("comparison")) {
            auto d = field("data");
            selector(*d);
            root->children = {d};
            d->properties = {{"default", decodeValue(row.at("default").dump())},
                             {"readOnly", Value(true)},
                             {"visible", Value(false)},
                             {"undefinedValue", Value(Value::Symbol{"unset"})},
                             {"initialAnswer", Value("answer")},
                             {"shouldCacheDefault", Value(true)}};
            std::vector<std::string> trace;
            Trace left("left", trace), right("right", trace);
            left.values[d->id] = decodeValue(row.at("left").dump());
            right.values[d->id] = decodeValue(row.at("right").dump());
            auto before = d->properties;
            check(isSameAs(left, right, *root) == row.at("result").get<bool>());
            check(trace == std::vector<std::string>(
                               {"right:can:data", "left:read:data", "right:read:data"}));
            check(d->properties == before);
        }
        auto a = field("a"), b = field("b"), c = field("c");
        selector(*a);
        selector(*b);
        root->children = {a, b, c};
        std::vector<std::string> trace;
        Trace left("left", trace), right("right", trace);
        a->properties["default"] = Value("A");
        b->properties["default"] = Value("B");
        c->properties["default"] = Value("C");
        // Initialization does not ask for write capability, current values, validation or caching.
        a->className = "MANumberDescription";
        a->properties["visible"] = Value(false);
        c->properties["readOnly"] = Value(true);
        initializeMagritte(left, *root);
        check(trace == std::vector<std::string>({"left:write:a", "left:write:b"}));
        check(left.values["a"] == Value("A") && left.values["b"] == Value("B") &&
              !left.values.count("c"));
        a->className = "MAStringDescription";
        c->properties.erase("readOnly");
        left.afterWrite = [](const Description &d) {
            if (d.id == "b")
                throw Error("write");
        };
        trace.clear();
        fails([&] { initializeMagritte(left, *root); });
        check(trace == std::vector<std::string>({"left:write:a", "left:write:b"}) &&
              !left.values.count("c"));
        left.afterWrite = {};
        // Metadata changes made by one writer affect later fields, not a precomputed default table.
        left.afterWrite = [&](const Description &d) {
            if (d.id == "a") {
                b->properties["default"] = Value("changed");
                c->properties["readOnly"] = Value(true);
            }
        };
        initializeMagritte(left, *root);
        check(left.values["b"] == Value("changed") && !left.values.count("c"));
        left.afterWrite = {};
        root->children = {a, b};
        left.values = {{"a", Value("same")}, {"b", Value("left")}};
        right.values = {{"a", Value("different")}, {"b", Value("right")}};
        trace.clear();
        check(!isSameAs(left, right, *root));
        check(trace == std::vector<std::string>({"right:can:a", "left:read:a", "right:read:a"}));
        root->children = {a};
        right.readable = false;
        trace.clear();
        check(!isSameAs(left, right, *root));
        check(trace == std::vector<std::string>({"right:can:a"}));
        right.readable = true;
        left.readable = false;
        right.values["a"] = Value("same");
        trace.clear();
        check(isSameAs(left, right, *root));
        check(trace == std::vector<std::string>({"right:can:a", "left:read:a", "right:read:a"}));
        left.beforeRead = [](const Description &) { throw Error("left read"); };
        trace.clear();
        fails([&] { isSameAs(left, right, *root); });
        check(trace == std::vector<std::string>({"right:can:a", "left:read:a"}));
        left.beforeRead = {};
        right.beforeRead = [](const Description &) { throw Error("right read"); };
        trace.clear();
        fails([&] { isSameAs(left, right, *root); });
        check(trace == std::vector<std::string>({"right:can:a", "left:read:a", "right:read:a"}));
        right.beforeRead = {};
        // A left read can change the next read's accessor and the subsequently obtained default.
        auto accessor = AccessorObject::fromAccessor("shared", *a->accessor);
        a->accessorObject = accessor;
        left.beforeRead = [&](const Description &) {
            accessor->value.name = "new";
            a->properties["default"] = Value("new default");
        };
        right.beforeRead = [&](const Description &d) {
            check(d.effectiveAccessor().name == "new");
        };
        left.values["a"] = Value();
        right.values["a"] = Value("new default");
        check(isSameAs(left, right, *root));
        left.beforeRead = {};
        right.beforeRead = {};
        a->accessorObject.reset();
        int callbacks = 0;
        a->defaultFunction = [&] {
            ++callbacks;
            return Value("factory");
        };
        a->properties["readOnly"] = Value(true);
        trace.clear();
        initializeMagritte(left, *root);
        check(trace.empty());
        a->properties["readOnly"] = Value(false);
        fails([&] { initializeMagritte(left, *root); });
        check(callbacks == 0);
        trace.clear();
        fails([&] { isSameAs(left, right, *root); });
        check(trace == std::vector<std::string>({"right:can:a", "left:read:a", "right:read:a"}) &&
              callbacks == 0);
        right.readable = false;
        check(!isSameAs(left, right, *root) && callbacks == 0);
        right.readable = true;
        check(a->defaultValue() == Value("factory") && callbacks == 1);
        a->defaultFunction = {};
        // Providers are evaluated once, even for empty descriptions, and errors propagate.
        root->children.clear();
        int providers = 0;
        auto provider = [&] {
            ++providers;
            return root;
        };
        check(&initializeMagritte(left, provider) == &left && providers == 1);
        check(isSameAs(left, right, provider) && providers == 2);
        fails([&] { initializeMagritte(left, std::function<std::shared_ptr<Description>()>{}); });
        fails([&] { isSameAs(left, right, [] { return std::shared_ptr<Description>(); }); });
        fails([&] {
            initializeMagritte(left,
                               []() -> std::shared_ptr<Description> { throw Error("provider"); });
        });
        auto nested = field("nested");
        nested->className = "MAContainer";
        nested->properties["undefinedValue"] = Value("container-default");
        nested->children = {c};
        root->children = {nested};
        trace.clear();
        initializeMagritte(left, *root);
        check(trace == std::vector<std::string>({"left:write:nested"}));
        check(left.values.at("nested") == Value("container-default"));
        left.values["nested"] = Value();
        right.values["nested"] = Value("container-default");
        check(isSameAs(left, right, *root));
        // Reference equality uses the left/default receiver and the established host message
        // bridge.
        root->children = {a};
        a->properties["default"] = Value();
        Graph resolver;
        auto object = std::make_shared<EqualModel>();
        resolver.nativeObjects = {{"one", object}, {"alias", object}};
        left.values["a"] = Value(Value::Reference{"one"});
        right.values["a"] = Value(Value::Reference{"alias"});
        check(isSameAs(left, right, *root, &resolver) && object->calls == 1);
        object->answer = Value(false);
        check(!isSameAs(left, right, *root, &resolver));
        object->answer = Value("bad");
        fails([&] { isSameAs(left, right, *root, &resolver); });
        object->answer = Value(true);
        a->properties["default"] = Value(Value::Reference{"one"});
        left.values["a"] = Value(Value::Reference{"alias"});
        right.values["a"] = Value();
        object->calls = 0;
        check(isSameAs(left, right, *root, &resolver) && object->calls == 1);
        fails([&] { isSameAs(left, right, *root); });
        Description invalid;
        fails([&] { initializeMagritte(left, invalid); });
        Limits items;
        items.items = 0;
        trace.clear();
        fails([&] { initializeMagritte(left, *root, items); });
        check(trace.empty());
        a->properties["default"] = Value(std::string(200, 'x'));
        Limits low;
        low.visits = 100;
        fails([&] { initializeMagritte(left, *root, low); });
        check(left.values["a"] == Value(Value::Reference{"alias"}));
        // Ordinary portable descriptions and objects retain the new behavior after import.
        auto name = field("name"), enabled = field("enabled");
        name->properties["default"] = Value("Ada");
        enabled->className = "MABooleanDescription";
        enabled->properties["default"] = Value(false);
        root->children = {name, enabled};
        Graph g;
        g.format = "magritte.graph/4";
        g.root = root->id;
        g.descriptions = {{root->id, root}, {name->id, name}, {enabled->id, enabled}};
        AdaptiveModel model;
        model.id = "initialized";
        model.description = root;
        model.values = {{name->id, Value("Before")}, {enabled->id, Value(true)}};
        initializeMagritte(model, *root);
        AdaptiveModel peer;
        peer.id = "peer";
        peer.description = root;
        peer.values = {{name->id, Value()}, {enabled->id, Value()}};
        check(isSameAs(model, peer, *root));
        check(validate(*root, model).empty());
        g.objects.emplace(model.id, model);
        g.objects.emplace(peer.id, peer);
        auto wire = g.encode();
        auto imported = Graph::decode(wire);
        check(isSameAs(imported.objects.at(model.id), imported.objects.at(peer.id),
                       *imported.descriptions.at(g.root), &imported));
        initializeMagritte(imported.objects.at(model.id), *imported.descriptions.at(g.root));
        check(imported.encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        std::cout << checks << " model protocol checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
