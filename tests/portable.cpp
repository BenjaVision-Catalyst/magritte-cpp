#include <fstream>
#include <iostream>
#include <limits>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool b, const std::string &message) {
    if (!b)
        throw Error(message);
    ++checks;
}
template <class F> void rejects(F f, const std::string &message) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, message);
}
std::string file(const char *name) {
    std::ifstream in(std::string(FIXTURES) + "/" + name);
    check(bool(in), "Missing fixture");
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
Value value(const J &j) { return decodeValue(j.dump()); }
int main(int argc, char **argv) {
    try {
        auto g = Graph::decode(file("portable-v3.json"));
        auto report = J::parse(file("portable-reference.json"));
        auto overrides = J::parse(file("portable-upstream.json"));
        for (const auto &c : report.at("cases")) {
            auto d = g.descriptions.at(c.at("description"));
            auto v = value(c.at("input"));
            if (c.at("errors") == J::array({"runtimeError"})) {
                rejects([&] { validateValue(*d, v, &g); }, "Expected condition runtime error");
                continue;
            }
            J actual = J::array();
            for (const auto &e : validateValue(*d, v, &g))
                actual.push_back(e.code);
            check(actual == c.at("errors"), "GT validation: " + c.at("description").get<std::string>() + " " +
                                                c.at("input").dump() + " got " + actual.dump());
        }
        for (const auto &c : report.at("reads")) {
            J expected = c;
            for (const auto &o : overrides.at("reads"))
                if (o.at("description") == c.at("description") && o.at("text") == c.at("text"))
                    expected = o;
            auto d = g.descriptions.at(c.at("description"));
            auto input = c.at("text").get<std::string>();
            if (expected.contains("error"))
                rejects([&] { readString(*d, input); }, "GT read rejection " + c.dump());
            else
                check(readString(*d, input) == value(expected.at("value")), "GT read " + c.dump());
        }
        for (const auto &c : report.at("writes")) {
            auto text = writeString(*g.descriptions.at(c.at("description")), value(c.at("input")), &g);
            check(text == c.at("text"), "GT write " + c.dump() + " got " + text);
        }
        for (const auto &c : report.at("predicates")) {
            auto p = decodePredicate(c.at("predicate").dump());
            auto v = value(c.at("input"));
            if (c.contains("error"))
                rejects([&] { p.evaluate(v); }, "GT predicate error");
            else
                check(p.evaluate(v) == c.at("result"), "GT predicate result");
            check(J::parse(encodePredicate(p)) == c.at("predicate"), "Predicate structural round trip");
        }
        for (const auto &c : report.at("accessors")) {
            auto d = g.descriptions.at(c.at("description"));
            auto a = d->effectiveAccessor();
            auto encoded = encodeAccessor(a);
            check(encodeAccessor(decodeAccessor(encoded)) == encoded, "Accessor structural round trip");
            DictionaryModel dict;
            Description properties;
            struct Host {
                Value value;
            };
            Host host{value(c.at("before"))};
            NativeModel native(&host);
            native.readers["MASelectorAccessor:value"] = [](const void *p) {
                return static_cast<const Host *>(p)->value;
            };
            native.writers["MASelectorAccessor:value:"] = [](void *p, const Value &v) {
                static_cast<Host *>(p)->value = v;
            };
            native.readers["MAVariableAccessor:value"] = native.readers.at("MASelectorAccessor:value");
            native.writers["MAVariableAccessor:value"] = native.writers.at("MASelectorAccessor:value:");
            native.resolver = &g;
            dict.resolver = &g;
            Model *target = &native;
            if (d->id == "dictField") {
                dict.values[a.name] = value(c.at("before"));
                target = &dict;
            } else if (d->id == "propertyField") {
                properties.properties[a.name] = value(c.at("before"));
                target = &properties;
            } else if (d->id == "chainField") {
                auto inner = std::make_shared<NativeModel>(&host);
                inner->readers = native.readers;
                inner->writers = native.writers;
                g.nativeObjects["inner"] = inner;
                native.readers["MASelectorAccessor:value"] = [](const void *) {
                    return Value(Value::Reference{"inner"});
                };
            }
            check(a.read(*target, &g) == value(c.at("before")), "GT accessor read");
            a.write(*target, value(c.at("after")), &g);
            check(a.read(*target, &g) == value(c.at("after")), "GT accessor write");
            check(target->readUsing(*d) == value(c.at("after")), "Model access applies the loaded strategy");
            target->writeUsing(*d, value(c.at("before")));
            check(a.read(*target, &g) == value(c.at("before")), "Raw model write applies the strategy");
            g.nativeObjects.clear();
        }
        auto edges = J::parse(file("portable-edges.json"));
        Description number;
        number.className = "MANumberDescription";
        for (const auto &c : edges.at("floats")) {
            auto v = value(c.at("value"));
            check(writeString(number, v) == c.at("text"), "GT float formatting");
            check(J::parse(encodeValue(v)) == c.at("value"), "Float bits preserved including signed zero");
        }
        for (const auto &c : edges.at("reads")) {
            auto text = c.at("text").get<std::string>();
            if (c.contains("error"))
                rejects([&] { readString(number, text); }, "GT numeric syntax rejection");
            else
                check(J::parse(encodeValue(readString(number, text))) == c.at("value"),
                      "GT numeric reader bits");
        }
        for (std::size_t i = 0; i < edges.at("comparisons").size(); ++i) {
            const auto &c = edges.at("comparisons")[i];
            auto left = value(c.at("left")), right = value(c.at("right"));
            check((left == right) == c.at("equal"), "GT exact numeric equality");
            for (const auto &s : {"<", ">"}) {
                Predicate p;
                p.selector = s;
                p.arguments = {right};
                bool expected = c.at(std::string(s) == "<" ? "less" : "greater");
                if (i == 0)
                    expected =
                        overrides.at("comparisonPolicy").at(std::string(s) == "<" ? "less" : "greater");
                check(p.evaluate(left) == expected,
                      "Portable exact numeric order with recorded GT policy difference");
            }
        }
        Description password;
        password.className = "MAPasswordDescription";
        for (const auto &c : edges.at("passwords")) {
            auto v = value(c.at("input"));
            check(password.obfuscated(v) == c.at("obfuscated"), "GT password mask");
            check(password.isObfuscated(v) == c.at("isObfuscated"), "GT password mask recognition");
        }
        auto boolean = g.descriptions.at("boolean");
        check(Value(boolean->allOptions()) == value(edges.at("booleanOptions")), "GT Boolean options");
        for (const auto &c : edges.at("booleanLabels"))
            check(boolean->labelForOption(value(c.at("input"))) == c.at("label"), "GT Boolean labels");
        Description token;
        token.className = "MATokenDescription";
        token.reference = g.descriptions.at("number");
        token.properties["separators"] = Value("");
        for (const auto &c : edges.at("emptySeparatorWrites"))
            check(writeString(token, value(c.at("input"))) == c.at("text"),
                  "GT separator is only needed between tokens");
        auto container = std::make_shared<Description>();
        container->className = "MAContainer";
        for (const auto &k : {"first", "hidden", "last"}) {
            auto d = std::make_shared<Description>();
            d->key = k;
            if (d->key == "hidden")
                d->properties["visible"] = Value(false);
            container->children.push_back(d);
        }
        DictionaryModel model;
        model.values["first"] = Value("A");
        model.values["last"] = Value("Z");
        check(writeString(*container, model) == edges.at("containerText"), "GT hidden container separators");
        for (const auto &c : J::parse(file("portable-invalid.json")))
            rejects([&] { Graph::decode(c.at("graph").dump()); }, c.at("name"));
        auto encoded = g.encode();
        check(Graph::decode(encoded).encode() == encoded, "v3 graph fixed point");
        auto &sample = g.objects.at("sample");
        auto identity = g.descriptions.at("identityField")->effectiveAccessor();
        check(&g.resolve(identity.read(sample, &g)) == &sample, "Identity actual target");
        rejects([&] { identity.write(sample, {}, &g); }, "Identity write");
        auto null = g.descriptions.at("nullField")->effectiveAccessor();
        rejects([&] { null.read(sample, &g); }, "Null read");
        rejects([&] { null.write(sample, {}, &g); }, "Null write");
        check(encodeAccessor(decodeAccessor(encodeAccessor(null))) == encodeAccessor(null),
              "Null comparison token preserved");
        check(Value(std::int64_t(1)) == Value(1.0), "Numeric equality");
        check(Value("ready") == Value(Value::Symbol{"ready"}), "Symbol/String equality");
        check(Value(std::int64_t(9007199254740993LL)) != Value(9007199254740992.0),
              "Mixed equality keeps integer precision");
        Predicate p;
        p.selector = ">";
        p.arguments = {Value(9007199254740992.0)};
        check(p.evaluate(Value(std::int64_t(9007199254740993LL))), "Mixed ordering keeps precision");
        check(readString(*g.descriptions.at("number"), "9007199254740993e0") ==
                  Value(std::int64_t(9007199254740993LL)),
              "Exponent integer parsing remains exact");
        rejects([&] { readString(number, "1.0e-400"); }, "Nonzero numeric underflow");
        rejects([] { (void)Value{std::numeric_limits<double>::infinity()}; }, "Nonfinite local float");
        Limits l;
        l.visits = 5;
        rejects(
            [&] {
                g.descriptions.at("composite")
                    ->portableConditions.front()
                    .predicate.evaluate(Value("Ada"), l);
            },
            "Condition bound");
        l = {};
        l.depth = 1;
        rejects(
            [&] {
                g.descriptions.at("composite")
                    ->portableConditions.front()
                    .predicate.evaluate(Value("Ada"), l);
            },
            "Condition depth bound");
        l = {};
        l.bytes = 2;
        rejects([&] { readString(*g.descriptions.at("string"), "long", l); }, "Reader bytes");
        rejects([&] { writeString(*g.descriptions.at("string"), Value("long"), nullptr, l); },
                "Writer bytes");
        Description hiddenConfig;
        hiddenConfig.properties["visible"] = Value(false);
        Predicate deep;
        deep.selector = "isNil";
        for (int i = 0; i < 10; ++i) {
            Predicate outer;
            outer.className = "MANegationCondition";
            outer.children = {deep};
            deep = outer;
        }
        hiddenConfig.portableConditions.push_back({"bounded", deep});
        l = {};
        l.visits = 5;
        rejects([&] { validateValue(hiddenConfig, {}, nullptr, l); },
                "Hidden configuration consumes the caller's work budget");
        auto cycle = std::make_shared<Description>();
        cycle->id = "cycle";
        cycle->className = "MATokenDescription";
        cycle->reference = cycle;
        rejects([&] { readString(*cycle, "a"); }, "Recursive reader bounded");
        sample.writeUsing(*g.descriptions.at("number"), Value(2.5));
        sample.writeUsing(*g.descriptions.at("symbol"), Value(Value::Symbol{"next"}));
        sample.writeUsing(*g.descriptions.at("positive"), Value(std::int64_t(2)));
        check(validate(*sample.description, sample, {}, &g).empty(), "Edited native object validation");
        if (argc > 1) {
            std::ofstream out(argv[1]);
            out << g.encode();
            check(bool(out), "Native v3 export");
        }
        std::cout << checks << " portable checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
