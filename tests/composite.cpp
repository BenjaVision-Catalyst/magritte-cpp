#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool x, const std::string &msg = "Composite mismatch") {
    ++checks;
    if (!x)
        throw Error(msg);
}
Value local(const J &j) {
    if (j.at("type") == "conflict") {
        auto c = std::make_shared<CompositeConflict>();
        if (!j.at("kind").is_null())
            c->kind = j.at("kind");
        if (!j.at("string").is_null())
            c->stringValue = j.at("string");
        return Value(c);
    }
    return decodeValue(j.dump());
}
J wire(const Value &v) {
    if (auto c = v.conflict())
        return J{{"type", "conflict"},
                 {"kind", c->kind ? J(*c->kind) : J(nullptr)},
                 {"string", c->string()}};
    return J::parse(encodeValue(v));
}
template <class F> J attempt(F f) {
    try {
        return J{{"value", wire(f())}};
    } catch (const Error &) {
        return J{{"error", true}};
    }
}
J normalized(J j) {
    if (j.is_object() && j.contains("error"))
        j["error"] = true;
    else if (j.is_structured())
        for (auto &e : j)
            e = normalized(e);
    return j;
}
Accessor dict(std::string name) {
    Accessor a;
    a.name = std::move(name);
    return a;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/composite-reference.json");
        J f;
        in >> f;
        for (const auto &row : f.at("cases")) {
            Graph g;
            DictionaryModel root;
            std::vector<std::shared_ptr<DictionaryModel>> models;
            Value::Array refs;
            std::vector<std::string> trace;
            for (std::size_t i = 0; i < row.at("values").size(); ++i) {
                auto m = std::make_shared<DictionaryModel>();
                m->values = {{"value", local(row.at("values").at(i))},
                             {"index", Value(std::int64_t(i))}};
                g.nativeObjects[std::to_string(i)] = m;
                models.push_back(m);
            }
            for (const auto &i : row.at("order"))
                refs.emplace_back(Value::Reference{i.dump()});
            std::shared_ptr<const Accessor::ReadBlock> hr, nr;
            std::shared_ptr<const Accessor::WriteBlock> hw, nw;
            if (row.at("headRead"))
                hr = std::make_shared<Accessor::ReadBlock>([&](const Model &) -> Value {
                    trace.push_back("head");
                    auto mode = row.at("headResult");
                    if (mode == "error")
                        throw Error("head");
                    if (mode == "nil")
                        return {};
                    if (mode == "scalar")
                        return Value(std::int64_t(42));
                    return row.at("collection") == "array"
                               ? Value(refs)
                               : Value(Value::Collection{row.at("collection"), refs});
                });
            if (row.at("headWrite"))
                hw = std::make_shared<Accessor::WriteBlock>(
                    [&](Model &, const Value &) { trace.push_back("head-write"); });
            if (row.at("nextRead"))
                nr = std::make_shared<Accessor::ReadBlock>([&](const Model &m) {
                    auto i =
                        std::get<std::int64_t>(m.readSlot("MADictionaryAccessor", "index").data);
                    trace.push_back("read:" + std::to_string(i));
                    if (i == row.at("readFailure"))
                        throw Error("read");
                    return m.readSlot("MADictionaryAccessor", "value");
                });
            if (row.at("nextWrite"))
                nw = std::make_shared<Accessor::WriteBlock>([&](Model &m, const Value &v) {
                    auto i =
                        std::get<std::int64_t>(m.readSlot("MADictionaryAccessor", "index").data);
                    trace.push_back("write:" + std::to_string(i));
                    if (i == row.at("writeFailure"))
                        throw Error("write");
                    m.writeSlot("MADictionaryAccessor", "value", v);
                });
            std::optional<std::string> kind;
            if (!row.at("kind").is_null())
                kind = row.at("kind");
            auto a =
                Accessor::composite(Accessor::pluggable(hr, hw), Accessor::pluggable(nr, nw), kind);
            for (const std::string op : {"canRead", "canWrite", "read", "write"}) {
                trace.clear();
                auto result = attempt([&]() -> Value {
                    if (op == "canRead")
                        return Value(a.canRead(root, &g));
                    if (op == "canWrite")
                        return Value(a.canWrite(root, &g));
                    if (op == "read")
                        return a.read(root, &g);
                    a.write(root, local(row.at("writeValue")), &g);
                    return {};
                });
                J actual = {{"outcome", result}, {"trace", trace}};
                check(actual == normalized(row.at(op)),
                      row.at("id").get<std::string>() + " " + op + " " + actual.dump());
            }
            J after = J::array();
            for (auto &m : models)
                after.push_back(wire(m->values.at("value")));
            check(after == row.at("after"));
        }
        for (const auto &row : f.at("conflicts")) {
            auto c = std::make_shared<CompositeConflict>();
            if (!row.at("kind").is_null())
                c->kind = row.at("kind");
            auto other = std::make_shared<CompositeConflict>();
            other->kind = "Boolean";
            other->stringValue = "Other";
            check(wire(Value(c)).at("string") == row.at("print"));
            check((*c == *other) == row.at("equal"));
            check((c->hash() == other->hash()) == row.at("hashEqual"));
            check(c->copy() != c && *c->copy() == *c);
            check(Value(c) == Value(other));
            check(!Value(c).isNil() && !Value(c).isCollection());
            J kinds = J::array();
            for (const std::string k : {"Object", "MACompositeConflict", "String", "Collection",
                                        "SequenceableCollection", "Number", "Integer", "Boolean"})
                kinds.push_back(c->kindOf(k));
            check(kinds == row.at("kinds"));
            auto message = [&](const std::string &selector) {
                return attempt([&] {
                    return Value(
                        c->doesNotUnderstand(selector, [](const auto &kind, const auto &sel) {
                            return sel == "isEmpty" &&
                                   ClassRegistry::global().includesBehavior(kind, "Collection");
                        }));
                });
            };
            check(message("isEmpty") == normalized(row.at("isEmptyMessage")));
            check(message("aCompletelyUnknownCompositeMessage") ==
                  normalized(row.at("unknownMessage")));
            for (const std::string name :
                 {"stringValidation", "numberValidation", "numberRangeValidation", "stringWriter",
                  "numberWriter", "booleanWriter"}) {
                Description d;
                d.id = "field";
                d.key = "value";
                d.className = name == "booleanWriter"    ? "MABooleanDescription"
                              : name.find("number") == 0 ? "MANumberDescription"
                                                         : "MAStringDescription";
                if (name == "numberRangeValidation")
                    d.properties["min"] = Value(std::int64_t(0));
                auto result = attempt([&]() -> Value {
                    if (name.find("Writer") != std::string::npos)
                        return Value(writeString(d, Value(c)));
                    auto errors = validateValue(d, Value(c));
                    if (!errors.empty())
                        throw Error("Validation");
                    return Value(true);
                });
                check(result == normalized(row.at(name)), name);
            }
            c->stringValue = "";
            check(c->string() == row.at("emptyString"));
            c->stringValue.reset();
            check(c->string() == row.at("resetString"));
            check(attempt([&] { return decodeValue(encodeValue(Value(c))); }).contains("error"));
        }
        for (const auto &row : f.at("defaultKinds")) {
            Description d;
            d.id = "kind";
            d.className = row.at("class");
            check(d.property("kind") == Value(Value::Class{row.at("kind")}));
        }
        for (const auto &row : f.at("declaredKinds")) {
            Description d;
            d.id = "declared";
            d.key = "name";
            d.properties["kind"] =
                row.at("kind").is_null() ? Value() : Value(Value::Class{row.at("kind")});
            auto a = Accessor::compositeVia(dict("people"), d);
            check(a.kind == (row.at("kind").is_null()
                                 ? std::optional<std::string>{}
                                 : std::optional<std::string>{row.at("kind").get<std::string>()}));
            auto result = attempt([&]() {
                if (!validateValue(d, Value(std::int64_t(0))).empty())
                    throw Error("Validation");
                return Value(true);
            });
            check(result == normalized(row.at("validation")));
        }
        for (const auto &row : f.at("labels")) {
            Description d;
            d.id = "field";
            d.key = "name";
            if (!row.at("input").is_null())
                d.properties["label"] = Value(row.at("input").get<std::string>());
            auto a = Accessor::compositeVia(dict("people"), d);
            check(d.property("label") == Value(row.at("label").get<std::string>()));
            check(a.accessorName() == Value(Value::Symbol{row.at("name")}));
            check(a.kind && *a.kind == row.at("kind"));
            auto copy = a;
            copy.kind = "Boolean";
            check(accessorEqual(a, copy));
            auto object = AccessorObject::fromAccessor("accessor", a);
            check(accessorEqual(object->snapshot(), a));
            auto cloned = AccessorObject::fromAccessor("copy", object->snapshot());
            check(cloned->accessor != object->accessor && cloned->next != object->next);
            RecursiveDescriptionModel session;
            auto schema = session.describe(object);
            J keys = J::array();
            for (const auto &field : schema->orderedChildren())
                keys.push_back(
                    std::get<Value::Symbol>(field->effectiveAccessor().accessorName().data).utf8);
            check(keys == row.at("schema"));
            check(schema->children.size() == 2);
        }
        Graph g;
        auto p0 = std::make_shared<DictionaryModel>(), p1 = std::make_shared<DictionaryModel>();
        p0->values["name"] = Value("a");
        p1->values["name"] = Value("b");
        g.nativeObjects = {{"0", p0}, {"1", p1}};
        DictionaryModel root;
        root.resolver = &g;
        root.values["people"] =
            Value(Value::Array{Value(Value::Reference{"0"}), Value(Value::Reference{"1"})});
        auto a = Accessor::composite(dict("people"), dict("name"), "String");
        auto field = std::make_shared<Description>();
        field->id = "field";
        field->accessor = a;
        auto schema = std::make_shared<Description>();
        schema->id = "root";
        schema->className = "MAContainer";
        schema->children = {field};
        CachedMemento editor(root, schema, &g);
        auto snapshot = [&]() {
            return J{{"cached", wire(editor.readUsing(*field))},
                     {"changed", editor.hasChanges()},
                     {"values", J::array({std::get<std::string>(p0->values.at("name").data),
                                          std::get<std::string>(p1->values.at("name").data)})}};
        };
        check(snapshot() == f.at("editing").at(0));
        editor.commit();
        check(snapshot() == f.at("editing").at(1));
        editor.writeUsing(*field, Value("all"));
        check(snapshot() == f.at("editing").at(2));
        editor.commit();
        check(snapshot() == f.at("editing").at(3));
        auto c = std::make_shared<CompositeConflict>();
        c->kind = "String";
        editor.writeUsing(*field, Value(c));
        check(editor.readUsing(*field).conflict() == c);
        editor.commit();
        check(p0->values.at("name") == Value("all"));
        // Composite boundaries survive accessor-object materialization and nested chains.
        Accessor chain;
        chain.className = "MAChainAccessor";
        chain.steps = {dict("nested"), dict("name")};
        auto binary = Accessor::composite(dict("people"), chain, {});
        auto object = AccessorObject::fromAccessor("binary", binary);
        check(accessorEqual(object->snapshot(), binary));
        auto encoded = encodeAccessor(binary);
        check(accessorEqual(decodeAccessor(encoded), binary));
        auto bad = binary;
        bad.steps.push_back(dict("extra"));
        check(attempt([&] {
                  bad.check();
                  return Value();
              }).contains("error"));
        bad = a;
        bad.kind = "UnregisteredCompositeKind";
        check(attempt([&] {
                  bad.check();
                  return Value();
              }).contains("error"));
        Limits low;
        low.items = 1;
        check(attempt([&] { return a.read(root, &g, low); }).contains("error"));
        auto alias = Value(c);
        c->stringValue = "Changed";
        check(alias.conflict()->string() == "Changed");
        auto copied = c->copy();
        copied->kind = "Number";
        copied->stringValue = "Other";
        check(*copied == *c && c->kind == "String");
        check(attempt([&] {
                  return decodeValue(encodeValue(Value(Value::Array{Value(c)})));
              }).contains("error"));

        std::ifstream roundInput(std::string(FIXTURES) + "/composite-roundtrip-reference.json");
        J round;
        roundInput >> round;
        for (const auto &port : round)
            for (std::size_t i = 0; i < port.at("accessors").size(); ++i) {
                auto accessor = decodeAccessor(port.at("accessors").at(i).dump());
                Graph resolver;
                DictionaryModel model;
                auto one = std::make_shared<DictionaryModel>(),
                     two = std::make_shared<DictionaryModel>(),
                     nestedOne = std::make_shared<DictionaryModel>(),
                     nestedTwo = std::make_shared<DictionaryModel>();
                one->values = {{"name", Value("a")}, {"nested", Value(Value::Reference{"n0"})}};
                two->values = {{"name", Value("b")}, {"nested", Value(Value::Reference{"n1"})}};
                nestedOne->values["name"] = Value("a");
                nestedTwo->values["name"] = Value("b");
                resolver.nativeObjects = {
                    {"0", one}, {"1", two}, {"n0", nestedOne}, {"n1", nestedTwo}};
                model.values["people"] =
                    Value(Value::Array{Value(Value::Reference{"0"}), Value(Value::Reference{"1"})});
                J actual = {{"canRead", accessor.canRead(model, &resolver)},
                            {"canWrite", accessor.canWrite(model, &resolver)},
                            {"before", wire(accessor.read(model, &resolver))},
                            {"name", std::get<Value::Symbol>(accessor.accessorName().data).utf8},
                            {"label", accessor.defaultLabelFor(*field)}};
                accessor.write(model, Value("all"), &resolver);
                actual["after"] = wire(accessor.read(model, &resolver));
                check(actual == port.at("cases").at(i));
                check(J::parse(encodeAccessor(accessor)) == port.at("accessors").at(i));
            }
        // The same local object can pass through native callbacks without becoming a string.
        auto callback = Accessor::pluggable(
            std::make_shared<Accessor::ReadBlock>([&](const Model &) { return Value(c); }),
            std::make_shared<Accessor::WriteBlock>(
                [&](Model &, const Value &v) { check(v.conflict() == c); }));
        check(callback.read(root).conflict() == c);
        callback.write(root, Value(c));
        auto invalidSet = Value(Value::Collection{"set", {Value(c), Value(c->copy())}});
        check(attempt([&] {
                  callback.write(root, invalidSet);
                  return Value();
              }).contains("error"));
        auto mixedSet = Value(Value::Collection{
            "set", {Value(c), Value(Value::Array{Value(c->string()), Value(*c->kind)})}});
        auto setCallback = Accessor::pluggable(
            std::make_shared<Accessor::ReadBlock>([&](const Model &) { return mixedSet; }));
        check(setCallback.read(root) == mixedSet);
        DynamicOptions options([&](const std::string &) { return Value(c); });
        check(options.selectMatching("x").conflict() == c);
        Graph portable;
        portable.format = "magritte.graph/4";
        portable.root = schema->id;
        portable.descriptions = {{schema->id, schema}, {field->id, field}};
        field->accessor = binary;
        auto graphWire = portable.encode();
        check(Graph::decode(graphWire).encode() == graphWire);
        auto legacy = J::parse(graphWire);
        legacy["format"] = "magritte.graph/3";
        check(attempt([&] {
                  Graph::decode(legacy.dump());
                  return Value();
              }).contains("error"));
        field->accessor = a;
        if (argc > 1) {
            J accessors = J::array();
            accessors.push_back(J::parse(encodeAccessor(a)));
            auto untyped = a;
            untyped.kind.reset();
            accessors.push_back(J::parse(encodeAccessor(untyped)));
            accessors.push_back(J::parse(encodeAccessor(binary)));
            Accessor delegated;
            delegated.className = "MADelegatorAccessor";
            delegated.steps = {a};
            accessors.push_back(J::parse(encodeAccessor(delegated)));
            std::ofstream(argv[1]) << J{{"accessors", accessors}}.dump(2) << '\n';
        }
        std::cout << checks << " composite checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
