#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Printing mismatch") {
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
    check(caught, "Expected explicit printing failure");
}
struct Trace : Model {
    std::map<std::string, Value> values;
    mutable std::vector<std::string> trace;
    std::function<void(const Description &)> beforeRead;
    Value readUsing(const Description &d) const override {
        trace.push_back("read:" + d.id);
        if (beforeRead)
            beforeRead(d);
        auto i = values.find(d.id);
        return i == values.end() ? Value() : i->second;
    }
    void writeUsing(const Description &, Value) override { throw Error("Printing must not write"); }
};
std::shared_ptr<Description> field(const std::string &id) {
    auto d = std::make_shared<Description>();
    d->id = d->key = id;
    d->properties = {{"required", Value(true)},
                     {"readOnly", Value(true)},
                     {"visible", Value(false)},
                     {"default", Value("unused")},
                     {"undefined", Value("(nil)")}};
    return d;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/printing-scenarios.json");
        J fixture;
        in >> fixture;
        for (const auto &row : fixture.at("containers")) {
            Description root;
            root.id = "root";
            root.className = "MAContainer";
            root.properties["visible"] = Value(false);
            Trace model;
            for (std::size_t i = 0; i < 3; ++i) {
                auto id = std::string(1, "abc"[i]);
                auto d = field(id);
                model.values[id] = decodeValue(row.at("values")[i].dump());
                root.children.push_back(d);
                if (row.at("formatter").get<bool>())
                    d->setPrintString(std::make_shared<PrintValueBlock>([&, id](const Value &v) {
                        model.trace.push_back("format:" + id);
                        return "[" + std::string(writeString(*root.children[0], v)) + "]";
                    }));
            }
            if (!row.at("separator").is_null()) {
                auto sep = row.at("separator").get<std::string>();
                root.setPrintSeparator(
                    std::make_shared<PrintSeparatorBlock>([&, sep](PrintStream &s) {
                        model.trace.push_back("separator");
                        s.append(sep);
                    }));
            }
            PrintStream stream;
            root.print(model, stream);
            check(stream.contents() == row.at("output"));
            check(model.trace == row.at("trace").get<std::vector<std::string>>());
        }
        for (const auto &row : fixture.at("elements")) {
            auto d = field("a");
            Trace model;
            model.values[d->id] = decodeValue(row.at("value").dump());
            d->setPrintString(std::make_shared<PrintValueBlock>([] {
                throw Error("printFor must ignore maPrintString");
                return std::string();
            }));
            check(d->readToString(model) == row.at("readToString"));
            PrintStream stream;
            d->printFor(model, stream);
            check(stream.contents() == row.at("printFor"));
            check(model.trace == std::vector<std::string>({"read:a", "read:a"}));
        }
        // Separator is captured once; field formatters are obtained only after each read.
        auto root = std::make_shared<Description>();
        root->id = "root";
        root->className = "MAContainer";
        auto a = field("a"), b = field("b"), c = field("c");
        root->children = {a, b, c};
        Trace model;
        model.values = {{"a", Value("A")}, {"b", Value("B")}, {"c", Value("C")}};
        auto second = std::make_shared<PrintSeparatorBlock>([](PrintStream &s) { s.append("!"); });
        int separators = 0;
        auto first = std::make_shared<PrintSeparatorBlock>([&](PrintStream &s) {
            ++separators;
            root->setPrintSeparator(second);
            s.append("|");
        });
        root->setPrintSeparator(first);
        auto formatter = std::make_shared<PrintValueBlock>([&](const Value &) {
            a->setPrintString(nullptr);
            return "first";
        });
        a->setPrintString(formatter);
        PrintStream stream;
        root->print(model, stream);
        check(stream.contents() == "first|B|C" && separators == 2);
        check(root->printSeparatorBlock == second && !a->printStringBlock);
        model.beforeRead = [&](const Description &d) {
            if (d.id == "b")
                b->setPrintString(std::make_shared<PrintValueBlock>([] { return "after-read"; }));
        };
        PrintStream again;
        root->print(model, again);
        check(again.contents() == "A!after-read!C");
        model.beforeRead = {};
        b->setPrintString(nullptr);
        // Failures preserve earlier output; separators precede reads, even failing/nil reads.
        root->setPrintSeparator(nullptr);
        PrintStream partial;
        partial.append("prefix:");
        model.trace.clear();
        model.beforeRead = [](const Description &d) {
            if (d.id == "b")
                throw Error("read");
        };
        fails([&] { root->print(model, partial); });
        check(partial.contents() == "prefix:A ");
        check(model.trace == std::vector<std::string>({"read:a", "read:b"}));
        model.beforeRead = {};
        root->setPrintSeparator(std::make_shared<PrintSeparatorBlock>([](PrintStream &s) {
            s.append("|");
            throw Error("separator");
        }));
        PrintStream sepFailure;
        model.trace.clear();
        fails([&] { root->print(model, sepFailure); });
        check(sepFailure.contents() == "A|" && model.trace == std::vector<std::string>({"read:a"}));
        root->setPrintSeparator(nullptr);
        b->setPrintString(std::make_shared<PrintValueBlock>([] {
            throw Error("formatter");
            return std::string();
        }));
        PrintStream formatFailure;
        fails([&] { root->print(model, formatFailure); });
        check(formatFailure.contents() == "A ");
        b->setPrintString(nullptr);
        // Explicit nil is present callable state, not a request for a fallback.
        a->propertyPut("maPrintString", Value());
        model.values["a"] = Value();
        PrintStream nilField;
        root->print(model, nilField);
        check(nilField.contents() == " B C");
        model.values["a"] = Value("A");
        PrintStream nilFormatter;
        fails([&] { root->print(model, nilFormatter); });
        a->propertyPutRemovingNil("maPrintString", Value());
        root->propertyPut("maPrintStringSeparator", Value());
        root->children = {a};
        PrintStream singleton;
        root->print(model, singleton);
        check(singleton.contents() == "A");
        root->children = {a, b};
        PrintStream nilSeparator;
        fails([&] { root->print(model, nilSeparator); });
        check(nilSeparator.contents() == "A");
        root->setPrintSeparator(nullptr);
        // Local callback presence/copy/replace and export refusal, including hidden nodes.
        a->setPrintString(formatter);
        check(a->hasProperty("maPrintString"));
        fails([&] { a->propertyAt("maPrintString"); });
        fails([&] { a->property("maPrintString"); });
        fails([&] { a->propertyPutRemovingNil("maPrintString", Value()); });
        auto copy = copyDescription(a, "copy");
        check(copy.root->printStringBlock == formatter);
        copy.root->setPrintString(nullptr);
        check(a->printStringBlock == formatter);
        a->propertyPut("maPrintString", Value());
        check(!a->printStringBlock && a->propertyAt("maPrintString").isNil());
        a->setPrintString(nullptr);
        fails([&] { a->setPrintSeparator(first); });
        fails([&] { a->print(model, stream); });
        fails([&] { root->printFor(model, stream); });
        Limits small;
        small.bytes = 4;
        PrintStream bounded(small);
        bounded.append("pre");
        model.trace.clear();
        fails([&] { root->print(model, bounded); });
        check(bounded.contents() == "preA" && model.trace == std::vector<std::string>({"read:a"}));
        PrintStream utf8;
        fails([&] { utf8.append(std::string(1, char(0xff))); });
        check(utf8.contents().empty());
        Limits low;
        low.visits = 1;
        PrintStream work(low);
        work.append("");
        fails([&] { work.append(""); });
        Limits items;
        items.items = 1;
        PrintStream itemStream(items);
        model.trace.clear();
        fails([&] { root->print(model, itemStream); });
        check(model.trace.empty());
        int noOutputSeparators = 0;
        root->setPrintSeparator(
            std::make_shared<PrintSeparatorBlock>([&] { ++noOutputSeparators; }));
        PrintStream noOutput;
        root->print(model, noOutput);
        check(noOutput.contents() == "AB" && noOutputSeparators == 1);
        root->setPrintSeparator(nullptr);
        PrintStream alias;
        alias.append("A");
        alias.append(alias.contents());
        check(alias.contents() == "AA");
        root->children = {a, a};
        PrintStream duplicate;
        root->print(model, duplicate);
        check(duplicate.contents() == "A A");
        root->children = {a, b, c};
        root->className = "MAPriorityContainer";
        a->properties["priority"] = Value(std::int64_t(3));
        b->properties["priority"] = Value(std::int64_t(1));
        c->properties["priority"] = Value(std::int64_t(2));
        root->resort();
        PrintStream sorted;
        root->print(model, sorted);
        check(sorted.contents() == "B C A");
        root->className = "MAContainer";
        for (auto d : root->children)
            d->properties.erase("priority");
        root->children = {a, b, c};
        Graph g;
        g.format = "magritte.graph/4";
        g.root = root->id;
        g.descriptions = {{root->id, root}, {"a", a}, {"b", b}, {"c", c}};
        AdaptiveModel adaptive;
        adaptive.id = "object";
        adaptive.description = root;
        adaptive.values = model.values;
        g.objects.emplace(adaptive.id, adaptive);
        root->setPrintString(std::make_shared<PrintValueBlock>([] {
            throw Error("Container printer ignores its own field formatter");
            return std::string();
        }));
        PrintStream own;
        root->print(adaptive, own);
        check(own.contents() == "A B C");
        fails([&] { g.encode(); });
        root->setPrintString(nullptr);
        a->setPrintString(formatter);
        fails([&] { g.encode(); });
        a->setPrintString(nullptr);
        root->setPrintSeparator(first);
        auto rootCopy = copyDescription(root, "rootCopy");
        check(rootCopy.root->printSeparatorBlock == first);
        fails([&] { g.encode(); });
        root->setPrintSeparator(nullptr);
        auto wire = g.encode();
        auto imported = Graph::decode(wire);
        PrintStream importedStream;
        imported.descriptions.at(imported.root)
            ->print(imported.objects.at("object"), importedStream, &imported);
        check(importedStream.contents() == "A B C");
        check(imported.encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        auto bad = J::parse(wire);
        bad["descriptions"][0]["properties"]["maPrintString"] = J{{"type", "nil"}};
        fails([&] { Graph::decode(bad.dump()); });
        root->propertyPut("maPrintStringSeparator", Value());
        fails([&] { g.encode(); });
        std::cout << checks << " printing checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
