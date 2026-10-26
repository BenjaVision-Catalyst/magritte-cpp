#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Property mismatch") {
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
    check(caught, "Expected property failure");
}
} // namespace
int main(int argc, char **argv) {
    try {
        const std::string key = "metadata:key";
        std::ifstream in(std::string(FIXTURES) + "/properties-scenarios.json");
        J f;
        in >> f;
        for (const auto &row : f.at("cases")) {
            Description d;
            d.id = "sample";
            if (!row.at("initial").is_null())
                d.properties[key] = decodeValue(row.at("initial").dump());
            std::vector<std::string> trace;
            auto absent = [&] {
                trace.push_back("absent");
                return Value("fallback");
            };
            auto present = [&](const Value &value) {
                check(value == d.propertyAt(key));
                trace.push_back("present");
                return Value("present");
            };
            auto op = row.at("operation").get<std::string>();
            auto run = [&]() -> Value {
                if (op == "get")
                    return d.propertyAt(key);
                if (op == "fallback")
                    return d.propertyAt(key, absent);
                if (op == "absentPut")
                    return d.propertyAtIfAbsentPut(key, absent);
                if (op == "present")
                    return d.propertyAtIfPresent(key, present);
                if (op == "presentElse")
                    return d.propertyAtIfPresentOrElse(key, present, absent);
                if (op == "presentPut")
                    return d.propertyAtIfPresentOrPut(key, present, absent);
                if (op == "put")
                    return d.propertyPut(key, Value("replacement"));
                if (op == "putNil")
                    return d.propertyPut(key, Value());
                if (op == "removingSet")
                    return d.propertyPutRemovingNil(key, Value("replacement"));
                return d.propertyPutRemovingNil(key, Value());
            };
            if (row.at("result").is_null())
                fails(run);
            else
                check(run() == decodeValue(row.at("result").dump()), row.dump());
            check(trace == row.at("trace").get<std::vector<std::string>>());
            check(d.hasProperty(key) == row.at("hasProperty"));
            if (d.hasProperty(key))
                check(d.propertyAt(key) == decodeValue(row.at("stored").dump()));
        }
        Description d;
        d.id = "special";
        check(!d.hasProperty("visible") && d.property("visible") == Value(true));
        fails([&] { d.propertyAt("visible"); });
        check(d.propertyAt("visible", [] { return Value(false); }) == Value(false));
        check(!d.hasProperty("visible"));
        d.propertyPut(key, Value());
        check(d.propertyAtIfAbsentPut(key, {}) == Value());
        check(d.propertyAtIfPresentOrElse(key, [](const Value &v) { return v; }, {}) == Value());
        d.propertyPutRemovingNil(key, Value());
        check(d.propertyAtIfPresent(key, {}) == Value());
        fails([&] { d.propertyAtIfAbsentPut(key, []() -> Value { throw Error("callback"); }); });
        check(!d.hasProperty(key));
        auto reentrant = [&] {
            d.propertyPut(key, Value("inner"));
            return Value("outer");
        };
        check(d.propertyAt(key, reentrant) == Value("outer") &&
              d.propertyAt(key) == Value("inner"));
        d.propertyPutRemovingNil(key, Value());
        check(d.propertyAtIfAbsentPut(key, reentrant) == Value("outer") &&
              d.propertyAt(key) == Value("outer"));
        check(d.propertyAtIfPresent(key, [&](const Value &value) {
            d.properties.clear();
            return value;
        }) == Value("outer"));
        check(!d.hasProperty(key));
        Accessor property;
        property.className = "MAPropertyAccessor";
        property.name = key;
        check(property.read(d).isNil());
        property.write(d, Value(false));
        check(d.hasProperty(key) && d.propertyAt(key) == Value(false));
        d.display(
            std::make_shared<DisplayBlock>(std::function<std::string()>([] { return "local"; })));
        check(d.hasProperty("displayBlockOrSymbol"));
        fails([&] { d.propertyAt("displayBlockOrSymbol"); });
        fails([&] { d.propertyPutRemovingNil("displayBlockOrSymbol", Value()); });
        check(bool(d.displayBlock));
        property.name = "displayBlockOrSymbol";
        property.write(d, Value(Value::Symbol{"asString"}));
        check(!d.displayBlock &&
              d.propertyAt("displayBlockOrSymbol") == Value(Value::Symbol{"asString"}));
        auto owner = std::make_shared<Description>(), reference = std::make_shared<Description>();
        owner->id = "owner";
        owner->className = "MASingleOptionDescription";
        reference->id = "reference";
        owner->reference = reference;
        reference->properties["undefined"] = Value("old");
        owner->propertyPut("undefined", Value("raw"));
        check(reference->property("undefined") == Value("old"));
        owner->setUndefined(Value("setter"));
        check(reference->property("undefined") == Value("setter"));
        auto data = std::make_shared<Description>();
        data->id = data->key = "data";
        data->propertyPut(key, Value());
        auto copied = copyDescription(data, "copied");
        check(copied.root->hasProperty(key));
        copied.root->propertyPut(key, Value("copy"));
        check(data->propertyAt(key).isNil());
        auto root = std::make_shared<Description>();
        root->id = "root";
        root->className = "MAContainer";
        root->children = {data};
        Graph g;
        g.format = "magritte.graph/4";
        g.root = root->id;
        g.descriptions = {{root->id, root}, {data->id, data}};
        auto wire = g.encode();
        auto imported = Graph::decode(wire);
        check(imported.descriptions.at("data")->hasProperty(key) &&
              imported.descriptions.at("data")->propertyAt(key).isNil());
        check(imported.encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        data->propertyPut("customSemantic", Value(true));
        fails([&] { g.encode(); });
        data->properties.erase("customSemantic");
        fails([&] { d.propertyPut("propertyAnnouncer", Value()); });
        check(!d.hasProperty("propertyAnnouncer"));
        d.properties["propertyAnnouncer"] = Value();
        fails([&] { d.propertyPut(key, Value(std::int64_t(7))); });
        d.properties.erase("propertyAnnouncer");
        Limits low;
        low.visits = 1;
        fails([&] { d.propertyAtIfAbsentPut(key, [] { return Value(); }, low); });
        fails([&] { d.propertyPut("bad key", Value()); });
        fails([&] { d.propertyPut("", Value()); });
        std::cout << checks << " property checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
