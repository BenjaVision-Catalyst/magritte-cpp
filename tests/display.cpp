#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Display mismatch") {
    ++checks;
    if (!ok)
        throw Error(message);
}
template <class F> void fails(F body) {
    bool threw = false;
    try {
        body();
    } catch (const Error &) {
        threw = true;
    }
    check(threw, "Expected display failure");
}
std::string plain(const Value &v) {
    Description d;
    return d.displayStringFor(v);
}
void configure(Description &d, const std::string &mode) {
    if (mode == "default")
        return;
    if (mode == "block0")
        d.display(
            std::make_shared<DisplayBlock>(std::function<std::string()>([] { return "fixed"; })));
    else if (mode == "block1")
        d.display(std::make_shared<DisplayBlock>(std::function<std::string(const Value &)>(
            [](const Value &v) { return "[" + plain(v) + "]"; })));
    else if (mode == "block2")
        d.display(std::make_shared<DisplayBlock>(
            DisplayBlock::Operation([](const Value &v, const Description &d) {
                return std::get<std::string>(d.property("label").data) + ":" + plain(v);
            })));
    else if (mode == "throw")
        d.display(std::make_shared<DisplayBlock>(
            std::function<std::string()>([]() -> std::string { throw Error("display"); })));
    else if (mode == "nil")
        d.properties["displayBlockOrSymbol"] = Value();
    else
        d.display(mode == "too-many" ? "one:two:" : mode);
}
std::shared_ptr<Description> scalar(std::string id, std::string cls = "MAStringDescription") {
    auto d = std::make_shared<Description>();
    d->id = id;
    d->key = id;
    d->className = cls;
    return d;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/display-scenarios.json");
        J fixture;
        in >> fixture;
        for (const auto &row : fixture.at("cases")) {
            Description d;
            d.id = "display";
            d.className = row.at("description");
            d.properties = {{"label", Value("Display")}, {"undefined", Value("(none)")}};
            configure(d, row.at("mode"));
            auto v = decodeValue(row.at("input").dump());
            auto run = [&](const char *key, auto operation) {
                if (row.at(key).is_null())
                    fails(operation);
                else
                    check(operation() == row.at(key), std::string(key) + " " + row.dump());
            };
            run("direct", [&] { return d.displayStringFor(v); });
            run("writer", [&] { return writeString(d, v); });
        }
        auto d = scalar("name");
        int calls = 0;
        auto block = std::make_shared<DisplayBlock>(
            DisplayBlock::Operation([&](const Value &v, const Description &description) {
                ++calls;
                check(&description == d.get());
                return plain(v) + "!";
            }));
        d->display(block);
        check(readString(*d, "Ada") == Value("Ada") && calls == 0);
        check(writeString(*d, Value("Ada")) == "Ada!" && calls == 1);
        auto copied = copyDescription(d, "copied");
        check(copied.root->displayBlock == block);
        d->display("asString");
        check(!d->displayBlock && copied.root->displayBlock == block);
        copied.root->display("asString");
        // Native message sends support zero or one optional description argument; bindings return
        // String.
        Graph native;
        std::string person = "Ada";
        auto model = std::make_shared<NativeModel>(&person);
        native.nativeObjects["person"] = model;
        Value ref(Value::Reference{"person"});
        model->readers["MASelectorAccessor:shortName"] = [](const void *p) {
            return Value(*static_cast<const std::string *>(p));
        };
        d->display("shortName");
        check(d->displayStringFor(ref, &native) == "Ada");
        check(writeString(*d, ref, &native) == "Ada");
        d->properties["label"] = Value("Caption");
        model->displayReaders["caption:"] = [&](const void *p, const Description *arg) {
            check(arg == d.get());
            return Value(*static_cast<const std::string *>(p) + "/" +
                         std::get<std::string>(arg->property("label").data));
        };
        d->display("caption:");
        check(writeString(*d, ref, &native) == "Ada/Caption");
        fails([&] { writeString(*d, ref); });
        model->displayReaders["bad"] = [](const void *, const Description *) {
            return Value(std::int64_t(1));
        };
        d->display("bad");
        fails([&] { writeString(*d, ref, &native); });
        d->display("unknown");
        fails([&] { d->displayStringFor(ref, &native); });
        // Option labels use the option description's display; sort keys use the reference's writer.
        auto option = scalar("option", "MASingleOptionDescription"),
             reference = scalar("reference");
        option->reference = reference;
        option->properties = {{"required", Value(true)}, {"sorted", Value(true)}};
        option->options = {Value("Ada"), Value("Bob")};
        option->display(std::make_shared<DisplayBlock>(std::function<std::string(const Value &)>(
            [](const Value &v) { return "Option" + plain(v); })));
        int sortCalls = 0;
        reference->display(std::make_shared<DisplayBlock>(
            std::function<std::string(const Value &)>([&](const Value &v) {
                ++sortCalls;
                return plain(v) == "Ada" ? "z" : "a";
            })));
        check(option->labelForOption(Value("Ada")) == "OptionAda");
        check(writeString(*option, Value("Ada")) == "OptionAda");
        auto sorted = option->allOptions().values();
        check(sorted == Value::Array({Value("Bob"), Value("Ada")}) && sortCalls > 0);
        sortCalls = 0;
        option->options = {Value("Ada")};
        check(option->allOptions().size() == 1 && sortCalls == 0);
        option->options = {};
        check(option->allOptions().empty() && sortCalls == 0);
        option->labels = {{Value(Value::Symbol{"Ada"}), "Pinned"}};
        check(writeString(*option, Value(Value::Symbol{"Ada"})) == "Pinned");
        check(writeString(*option, Value("Ada")) == "OptionAda");
        option->className = "MAMultipleOptionDescription";
        option->properties.erase("sorted");
        check(writeString(*option, Value(Value::Array{Value(), Value("Ada")})) ==
              "Optionnil, OptionAda");
        check(writeString(*option, Value(Value::Array{})) == "");
        auto relation = scalar("relation", "MAToOneRelationDescription");
        relation->reference = d;
        d->display("shortName");
        relation->display(std::make_shared<DisplayBlock>(std::function<std::string()>(
            []() -> std::string { throw Error("unused relation display"); })));
        check(writeString(*relation, ref, &native) == "Ada");
        auto container = scalar("container", "MAContainer");
        container->children = {d};
        container->display(relation->displayBlock);
        DictionaryModel dictionary;
        dictionary.values["name"] = Value("Ada");
        d->display("asString");
        check(writeString(*container, dictionary) == "Ada");
        auto token = scalar("token", "MATokenDescription");
        token->reference = reference;
        reference->display(std::make_shared<DisplayBlock>(std::function<std::string(const Value &)>(
            [](const Value &v) { return "[" + plain(v) + "]"; })));
        check(writeString(*token, Value(Value::Array{Value(), Value("Ada")})) == "[nil] [Ada]");
        model->displayReaders["+"] = [&](const void *, const Description *arg) {
            check(arg == d.get());
            return Value("binary");
        };
        d->display("+");
        check(d->displayStringFor(ref, &native) == "binary");
        d->display(std::make_shared<DisplayBlock>(std::function<std::string()>([&] {
            d->display("asString");
            return "once";
        })));
        check(writeString(*d, Value("Ada")) == "once");
        check(writeString(*d, Value("Ada")) == "Ada");
        model->displayReaders["once"] = [&](const void *, const Description *) {
            model->displayReaders.erase("once");
            return Value("once");
        };
        d->display("once");
        check(d->displayStringFor(ref, &native) == "once");
        fails([&] { d->displayStringFor(ref, &native); });
        auto bob = std::make_shared<NativeModel>(&person);
        bob->readers["MASelectorAccessor:shortName"] = [](const void *) { return Value("Bob"); };
        native.nativeObjects["bob"] = bob;
        reference->display("shortName");
        option->className = "MASingleOptionDescription";
        option->properties["sorted"] = Value(true);
        option->options = {Value(Value::Reference{"bob"}), ref};
        option->display("shortName");
        check(option->labelForOption(ref, {}, &native) == "Ada");
        auto nativeSorted = option->allOptions({}, &native).values();
        check(nativeSorted.front() == ref);
        // Codec snapshots selectors, rejects callbacks anywhere, and keeps existing Value/wire
        // shapes.
        Graph g;
        g.format = "magritte.graph/4";
        container->displayBlock.reset();
        container->properties.clear();
        d->properties.clear();
        g.root = container->id;
        g.descriptions = {{d->id, d}, {container->id, container}};
        d->display("asString");
        AdaptiveModel sample;
        sample.id = "sample";
        sample.description = container;
        sample.values[d->id] = Value("Ada");
        g.objects[sample.id] = sample;
        check(validate(*container, sample).empty());
        check(writeString(*container, sample) == "Ada");
        auto wire = g.encode();
        check(Graph::decode(wire).encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        auto invalidWire = J::parse(wire);
        for (auto &node : invalidWire["descriptions"])
            if (node["id"] == "name")
                node["properties"]["displayBlockOrSymbol"] = {{"type", "string"},
                                                              {"value", "asString"}};
        fails([&] { Graph::decode(invalidWire.dump()); });
        d->properties["displayBlockOrSymbol"] = Value();
        check(Graph::decode(g.encode())
                  .descriptions.at(d->id)
                  ->property("displayBlockOrSymbol")
                  .isNil());
        g.format = "magritte.graph/2";
        fails([&] { g.encode(); });
        g.format = "magritte.graph/4";
        for (const auto &bad :
             std::vector<Value>{Value("asString"), Value(std::int64_t(1)), Value(Value::Array{})}) {
            d->properties["displayBlockOrSymbol"] = bad;
            fails([&] { g.encode(); });
        }
        d->display("asString");
        d->display(block);
        fails([&] { g.encode(); });
        container->displayBlock.reset();
        d->properties["visible"] = Value(false);
        g.descriptions[container->id] = container;
        g.root = container->id;
        fails([&] { g.encode(); });
        d->display("asString");
        d->properties.erase("visible");
        Limits low;
        low.bytes = 3;
        fails([&] { d->displayStringFor(Value("long"), nullptr, low); });
        d->display(
            std::make_shared<DisplayBlock>(std::function<std::string()>([] { return "long"; })));
        fails([&] { d->displayStringFor(Value("x"), nullptr, low); });
        d->display(std::make_shared<DisplayBlock>(
            std::function<std::string()>([] { return std::string(1, char(0xff)); })));
        fails([&] { writeString(*d, Value("x")); });
        d->display("asString");
        check(d->displayStringFor(Value("xy"), nullptr, low) == "xy");
        d->display(
            std::make_shared<DisplayBlock>(std::function<std::string()>([] { return "x"; })));
        check(d->displayStringFor(Value("long input"), nullptr, low) == "x");
        std::cout << checks << " display checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
