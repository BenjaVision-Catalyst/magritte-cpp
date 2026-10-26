#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Lazy mismatch") {
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
    check(caught, "Expected lazy failure");
}
struct ModelTrace : Model {
    std::vector<Value> writes;
    bool fail = false;
    Value readUsing(const Description &) const override {
        throw Error("Lazy initializer must not read model");
    }
    void writeUsing(const Description &, Value v) override {
        writes.push_back(v);
        if (fail)
            throw Error("write");
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/lazy-scenarios.json");
        J fixture;
        in >> fixture;
        for (const auto &row : fixture.at("lazy")) {
            Description d;
            d.id = "field";
            d.className = row.at("className");
            d.properties = {{"default", decodeValue(row.at("default").dump())},
                            {"initialAnswer", Value("answer")},
                            {"undefinedValue", Value(Value::Symbol{"unset"})},
                            {"readOnly", Value(true)},
                            {"visible", Value(false)}};
            if (!row.at("cache").is_null())
                d.properties["shouldCacheDefault"] = Value(row.at("cache").get<bool>());
            ModelTrace model;
            auto result = d.lazilyInitializeFrom(decodeValue(row.at("current").dump()), model);
            check(result == decodeValue(row.at("result").dump()));
            check(model.writes.size() == row.at("writes"));
            if (!model.writes.empty())
                check(model.writes.front() == d.property("default"));
        }
        for (const auto &row : fixture.at("answers")) {
            Description d;
            d.id = "answer";
            d.properties["default"] = decodeValue(row.at("default").dump());
            if (!row.at("answer").is_null())
                check(&d.setInitialAnswer(decodeValue(row.at("answer").dump())) == &d);
            auto expected = decodeValue(row.at("result").dump());
            check(d.initialAnswer() == expected && d.property("initialAnswer") == expected &&
                  d.readSlot("MASelectorAccessor", "initialAnswer") == expected);
            check(d.hasProperty("initialAnswer") == !row.at("answer").is_null());
        }
        Description d;
        d.id = "lazy";
        d.properties["shouldCacheDefault"] = Value(true);
        ModelTrace model;
        check(d.lazilyInitializeFrom(Value(), model).isNil());
        check(d.lazilyInitializeFrom(Value(), model).isNil());
        check(model.writes.size() == 2);
        model.fail = true;
        fails([&] { d.lazilyInitializeFrom(Value(), model); });
        check(model.writes.size() == 3);
        model.fail = false;
        int calls = 0;
        d.defaultFunction = [&] {
            ++calls;
            return Value("factory");
        };
        check(d.lazilyInitializeFrom(Value(false), model) == Value(false));
        check(calls == 0);
        fails([&] { d.lazilyInitializeFrom(Value(), model); });
        fails([&] { d.initialAnswer(); });
        check(calls == 0);
        d.setInitialAnswer(Value());
        check(d.initialAnswer().isNil());
        check(d.defaultValue() == Value("factory") && calls == 1);
        d.defaultFunction = {};
        // Raw property lookup avoids recursively invoking the selector getter; writes still use the
        // accessor.
        Description target, field;
        target.id = "target";
        field.id = "field";
        Accessor selector;
        selector.className = "MASelectorAccessor";
        selector.name = "label";
        selector.writeName = "label:";
        field.accessor = selector;
        field.properties["default"] = Value("seed");
        check(target.maLazyPropertyUsing(field) == Value("seed"));
        check(!target.hasProperty("label"));
        field.properties["shouldCacheDefault"] = Value(true);
        check(target.maLazyPropertyUsing(field) == Value("seed"));
        check(target.propertyAt("label") == Value("seed"));
        target.propertyPut("label", Value(false));
        check(target.maLazyPropertyUsing(field) == Value(false));
        target.propertyPutRemovingNil("label", Value());
        field.accessor->writeName = "comment:";
        check(target.maLazyPropertyUsing(field) == Value("seed"));
        check(!target.hasProperty("label") && target.propertyAt("comment") == Value("seed"));
        field.accessor->writeName = "";
        fails([&] { target.maLazyPropertyUsing(field); });
        field.properties["shouldCacheDefault"] = Value(false);
        check(target.maLazyPropertyUsing(field) == Value("seed"));
        field.accessor->name = "";
        fails([&] { target.maLazyPropertyUsing(field); });
        field.accessor = Accessor{};
        fails([&] { target.maLazyPropertyUsing(field); });
        // Cached reference Values keep their declared identity; no default copy/validation is
        // invented.
        AdaptiveModel adaptive;
        adaptive.id = "adaptive";
        Description reference;
        reference.id = "reference";
        reference.properties = {{"default", Value(Value::Reference{"object"})},
                                {"shouldCacheDefault", Value(true)}};
        auto v = reference.lazilyInitializeFrom(Value(), adaptive);
        check(v == reference.property("default") && adaptive.values.at(reference.id) == v);
        auto data = std::make_shared<Description>(), root = std::make_shared<Description>();
        data->id = data->key = "data";
        data->properties["default"] = Value("seed");
        data->writeSlot("MASelectorAccessor", "initialAnswer:", Value());
        auto copied = copyDescription(data, "copy");
        check(copied.root->initialAnswer().isNil());
        copied.root->setInitialAnswer(Value("copy"));
        check(data->initialAnswer().isNil());
        root->id = "root";
        root->className = "MAContainer";
        root->children = {data};
        Graph g;
        g.format = "magritte.graph/4";
        g.root = root->id;
        g.descriptions = {{root->id, root}, {data->id, data}};
        auto wire = g.encode();
        auto imported = Graph::decode(wire);
        check(imported.descriptions.at(data->id)->initialAnswer().isNil());
        check(imported.encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        g.format = "magritte.graph/3";
        check(Graph::decode(g.encode()).descriptions.at(data->id)->initialAnswer().isNil());
        g.format = "magritte.graph/2";
        fails([&] { g.encode(); });
        root->properties["initialAnswer"] = Value();
        g.format = "magritte.graph/4";
        fails([&] { g.encode(); });
        fails([&] { root->initialAnswer(); });
        fails([&] { root->lazilyInitializeFrom(Value("current"), model); });
        Limits low;
        low.bytes = 1;
        fails([&] { d.lazilyInitializeFrom(Value("too long"), model, low); });
        std::cout << checks << " lazy checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
