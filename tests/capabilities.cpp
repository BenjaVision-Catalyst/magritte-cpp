#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool result, const std::string &message) {
    ++checks;
    if (!result)
        throw Error(message);
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const Error &) {
        failed = true;
    }
    check(failed, "Expected explicit failure");
}
struct Probe {
    Value contents;
    int reads = 0, writes = 0;
    bool fail = false;
};
std::shared_ptr<NativeModel> bind(Probe &p) {
    auto m = std::make_shared<NativeModel>(&p);
    m->readers["MASelectorAccessor:contents"] = [](const void *raw) {
        auto &p = *const_cast<Probe *>(static_cast<const Probe *>(raw));
        ++p.reads;
        if (p.fail)
            throw Error("Probe read failure");
        return p.contents;
    };
    m->writers["MASelectorAccessor:contents:"] = [](void *raw, const Value &v) {
        auto &p = *static_cast<Probe *>(raw);
        ++p.writes;
        p.contents = v;
    };
    m->readers["MAVariableAccessor:contents"] = [](const void *raw) {
        return static_cast<const Probe *>(raw)->contents;
    };
    m->writers["MAVariableAccessor:contents"] = [](void *raw, const Value &v) {
        static_cast<Probe *>(raw)->contents = v;
    };
    m->readers["MAVariableAccessor:failRead"] = [](const void *raw) {
        return Value(static_cast<const Probe *>(raw)->fail);
    };
    return m;
}
Accessor selector(std::string read = "contents", std::string write = "contents:") {
    Accessor a;
    a.className = "MASelectorAccessor";
    a.name = std::move(read);
    a.writeName = std::move(write);
    return a;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream stream(std::string(FIXTURES) + "/accessor-capabilities-reference.json");
        J fixture;
        stream >> fixture;
        check(fixture.at("absentSelectorMetadataIsNil").get<bool>(),
              "GT absent selector metadata is nil");
        J output = J::array();
        for (const auto &row : fixture.at("cases")) {
            Probe probe, other;
            probe.fail = row.at("failRead");
            other.contents = Value("value");
            auto p = bind(probe), o = bind(other);
            auto inner = std::make_shared<DictionaryModel>();
            inner->values = {{"field", Value("hello")},
                             {"child", Value(Value::Reference{"other"})}};
            probe.contents =
                row.at("nilContents").get<bool>() ? Value() : Value(Value::Reference{"inner"});
            Graph graph;
            graph.nativeObjects = {{"probe", p}, {"other", o}, {"inner", inner}};
            Description description;
            DictionaryModel plain;
            const auto modelName = row.at("model").get<std::string>();
            const Model &model = modelName == "probe"        ? static_cast<const Model &>(*p)
                                 : modelName == "dictionary" ? static_cast<const Model &>(*inner)
                                 : modelName == "description"
                                     ? static_cast<const Model &>(description)
                                     : static_cast<const Model &>(plain);
            auto a = decodeAccessor(row.at("accessor").dump());
            check(J::parse(encodeAccessor(a)) == row.at("accessor"), "Accessor wire round trip");
            auto object = AccessorObject::fromAccessor("accessor", a);
            check(J::parse(encodeAccessor(object->snapshot())) == row.at("accessor"),
                  "Live accessor object snapshot");
            for (bool writing : {false, true}) {
                probe.reads = other.reads = probe.writes = other.writes = 0;
                const auto &expected = row.at(writing ? "write" : "read");
                bool failed = false, result = false;
                try {
                    result = writing ? a.canWrite(model, &graph) : a.canRead(model, &graph);
                } catch (const Error &) {
                    failed = true;
                }
                const auto context =
                    row.at("name").get<std::string>() + (writing ? " write" : " read");
                check(failed == expected.contains("error"), "GT error outcome " + context);
                if (!failed)
                    check(result == expected.at("value").get<bool>(), "GT capability " + context);
                check(J::array({probe.reads, other.reads}) == expected.at("reads"),
                      "GT read trace " + context);
                check(J::array({probe.writes, other.writes}) == expected.at("writes"),
                      "GT write trace " + context);
            }
            auto config = row;
            config.erase("read");
            config.erase("write");
            config["accessor"] = J::parse(encodeAccessor(a));
            output.push_back(config);
        }
        Probe raw;
        auto native = bind(raw);
        auto a = selector();
        check(a.canRead(*native) && a.canWrite(*native), "Bound selectors");
        native->readers.erase("MASelectorAccessor:contents");
        check(!a.canRead(*native) && a.canWrite(*native), "Live binding removal");
        native->writers["MASelectorAccessor:contents"] = [](void *, const Value &) {};
        check(a.canRead(*native), "Response discovery independent of invocation arity");
        native->writers.erase("MASelectorAccessor:contents");
        bool called = false;
        native->readers["MASelectorAccessor:"] = [&](const void *) {
            called = true;
            return Value();
        };
        auto absent = selector("", "");
        check(!absent.canRead(*native) && !absent.canWrite(*native),
              "Absent selectors short circuit");
        rejects([&] { absent.read(*native); });
        rejects([&] { absent.write(*native, Value()); });
        check(!called, "Absent selector does not invoke a binding");
        Accessor variable;
        variable.className = "MAVariableAccessor";
        variable.name = "failRead";
        check(variable.canRead(*native) && variable.canWrite(*native),
              "Variable existence is independent of binding direction");
        Description field;
        field.id = "field";
        field.key = "field";
        field.properties["readOnly"] = Value(true);
        field.properties["visible"] = Value(false);
        DictionaryModel dictionary;
        check(field.effectiveAccessor().canWrite(dictionary),
              "Presentation flags do not restrict accessor capability");
        field.effectiveAccessor().write(dictionary, Value("edited"));
        check(dictionary.values.at("field") == Value("edited"), "Raw writes remain independent");
        auto metadata = std::make_shared<Description>();
        metadata->id = "metadata";
        metadata->key = "metadata";
        RecursiveDescriptionModel session;
        auto binding = session.bind(metadata);
        check(selector("label", "label:").canWrite(*binding),
              "Description binding selector discovery");
        check(selector("children", "setChildren:").canRead(*binding) == false,
              "Class-specific recursive protocol");
        metadata->className = "MAContainer";
        check(selector("children", "setChildren:").canRead(*binding),
              "Runtime class metadata change");
        auto accessorObject =
            AccessorObject::fromAccessor("dictionary-accessor", field.effectiveAccessor());
        auto accessorBinding = session.bind(accessorObject);
        check(selector("key", "key:").canRead(*accessorBinding) &&
                  selector("key", "key:").canWrite(*accessorBinding),
              "Accessor self-description protocol");
        std::shared_ptr<Model> expired;
        {
            RecursiveDescriptionModel shortSession;
            expired = shortSession.bind(metadata);
        }
        rejects([&] { selector("label", "label:").canRead(*expired); });
        auto missingObject = AccessorObject::fromAccessor("missing-selector", absent);
        auto missingBinding = session.bind(missingObject);
        check(selector("selector", "selector:").read(*missingBinding).isNil(),
              "Absent selector metadata preserves nil");
        raw.contents = Value();
        auto emptyNative = bind(raw);
        Accessor independent;
        independent.className = "MAChainAccessor";
        independent.steps = {selector(), field.effectiveAccessor()};
        check(independent.canRead(*emptyNative) && independent.canWrite(*emptyNative),
              "Receiver-independent terminal requires no resolver");
        Limits tiny;
        tiny.visits = 1;
        rejects([&] { a.canRead(*native, nullptr, tiny); });
        Accessor chain;
        chain.className = "MAChainAccessor";
        chain.steps = {selector(), field.effectiveAccessor()};
        tiny = Limits{};
        tiny.depth = 0;
        rejects([&] { chain.canWrite(*native, nullptr, tiny); });
        Accessor malformed;
        malformed.className = "UnknownAccessor";
        rejects([&] { malformed.canRead(dictionary); });
        auto graphField = std::make_shared<Description>();
        graphField->id = "setter";
        graphField->accessor = selector("", "contents:");
        Graph graph;
        graph.format = "magritte.graph/4";
        graph.root = "root";
        graph.descriptions["setter"] = graphField;
        auto root = std::make_shared<Description>();
        root->id = "root";
        root->className = "MAContainer";
        root->children = {graphField};
        graph.descriptions["root"] = root;
        auto decoded = Graph::decode(graph.encode());
        check(decoded.descriptions.at("setter")->effectiveAccessor().canWrite(*native),
              "Setter-only graph interchange");
        if (argc > 1) {
            std::ofstream out(argv[1]);
            out << output.dump(2) << '\n';
        }
        std::cout << checks << " accessor capability checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
