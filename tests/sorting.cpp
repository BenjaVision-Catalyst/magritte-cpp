#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Sorting mismatch") {
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
    check(caught, "Expected sorting failure");
}
Accessor identity() {
    Accessor a;
    a.className = "MAIdentityAccessor";
    return a;
}
Accessor field(std::string name = "rank") {
    Accessor a;
    a.name = std::move(name);
    return a;
}
struct Track : DictionaryModel {
    std::vector<std::string> &trace;
    std::string tag, mode;
    std::function<void()> onRead;
    Track(std::vector<std::string> &t, std::string tag) : trace(t), tag(std::move(tag)) {
        values = {{"rank", Value(std::int64_t(3))}, {"other", Value(std::int64_t(9))}};
    }
    Value readSlot(const std::string &, const std::string &key) const override {
        trace.push_back(tag + ":" + key);
        if (onRead)
            onRead();
        if (mode == "throw")
            throw Error("read");
        auto i = values.find(key);
        return i == values.end() ? Value() : i->second;
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/sorting-scenarios.json");
        J f;
        in >> f;
        for (const auto &row : f.at("comparisons")) {
            SortBlock block(row.at("selector"));
            auto a = decodeValue(row.at("left").dump()), b = decodeValue(row.at("right").dump());
            if (row.at("result").is_null())
                fails([&] { block.value(a, b); });
            else
                check(block.value(a, b) == decodeValue(row.at("result").dump()), row.dump());
            check(&block.fixTemps() == &block);
        }
        for (const auto &row : f.at("sortability")) {
            Description d;
            d.id = "query";
            d.className = row.at("className");
            check(d.isSortable() == row.at("result"));
            check(d.respondsToSelector("isSortable"));
            check(d.readSlot("MASelectorAccessor", "isSortable") ==
                  Value(row.at("result").get<bool>()));
            fails([&] { d.writeSlot("MASelectorAccessor", "isSortable:", Value(true)); });
        }
        ClassRegistry::global().registerClasses({{"MASortTestText", "MAStringDescription"},
                                                 {"MASortTestPassword", "MAPasswordDescription"}});
        Description derived;
        derived.className = "MASortTestText";
        check(derived.isSortable());
        derived.className = "MASortTestPassword";
        check(!derived.isSortable());
        struct Override : Description {
            bool isSortable() const override { return true; }
        } custom;
        custom.className = "MAContainer";
        check(custom.isSortable());
        check(custom.readSlot("MASelectorAccessor", "isSortable") == Value(true));
        std::vector<std::string> trace;
        Track left(trace, "L"), right(trace, "R");
        right.values["rank"] = Value(std::int64_t(7));
        auto live = AccessorObject::fromAccessor("rank", field());
        SortBlock block(live, "<=");
        check(block.value(left, right) == Value(true));
        check(trace == std::vector<std::string>({"L:rank", "R:rank"}));
        auto copy = block;
        check(copy.accessor() == live);
        trace.clear();
        left.onRead = [&] { live->value.name = "other"; };
        check(copy.value(left, right) == Value(true));
        check(trace == std::vector<std::string>({"L:rank", "R:other"}));
        left.onRead = {};
        live->value.name = "rank";
        trace.clear();
        left.mode = "throw";
        fails([&] { block.value(left, right); });
        check(trace == std::vector<std::string>({"L:rank"}));
        left.mode = "";
        trace.clear();
        right.mode = "throw";
        fails([&] { block.value(left, right); });
        check(trace == std::vector<std::string>({"L:rank", "R:rank"}));
        right.mode = "";
        trace.clear();
        SortBlock badArity(live, "yourself");
        fails([&] { badArity.value(left, right); });
        check(trace == std::vector<std::string>({"L:rank", "R:rank"}));
        SortBlock changing(field(), "<");
        right.onRead = [&] { changing = SortBlock(field(), ">"); };
        check(changing.value(left, right) == Value(true));
        check(changing.selector() == ">");
        right.onRead = {};
        // Value-target access uses the same accessors, with identity/delegator/chain scalar stages.
        Graph resolver;
        auto model = std::make_shared<DictionaryModel>();
        model->values["rank"] = Value(std::int64_t(3));
        resolver.nativeObjects["model"] = model;
        Value object(Value::Reference{"model"});
        Accessor chain;
        chain.className = "MAChainAccessor";
        chain.steps = {field(), identity()};
        check(chain.readValue(object, &resolver) == Value(std::int64_t(3)));
        Accessor delegator;
        delegator.className = "MADelegatorAccessor";
        delegator.steps = {identity()};
        check(delegator.readValue(Value("raw")) == Value("raw"));
        fails([&] { field().readValue(Value("raw")); });
        auto read = std::make_shared<const Accessor::ReadBlock>(
            [](const Model &) { return Value(std::int64_t(42)); });
        check(Accessor::pluggable(read).readValue(object, &resolver) == Value(std::int64_t(42)));
        // Arbitrary host binary results survive; reads occur before dispatch and errors preserve
        // order.
        std::string host = "host";
        auto native = std::make_shared<NativeModel>(&host),
             alias = std::make_shared<NativeModel>(&host);
        resolver.nativeObjects["host"] = native;
        resolver.nativeObjects["alias"] = native;
        resolver.nativeObjects["adapter"] = alias;
        Value hostRef(Value::Reference{"host"}), aliasRef(Value::Reference{"alias"}),
            adapterRef(Value::Reference{"adapter"});
        check(SortBlock("=").value(hostRef, aliasRef, &resolver) == Value(true));
        check(SortBlock("=").value(hostRef, adapterRef, &resolver) == Value(true));
        check(SortBlock("~=").value(hostRef, adapterRef, &resolver) == Value(false));
        native->binaryReaders["combine:"] = [&](const void *receiver, const Value &arg,
                                                const Graph *graph) {
            check(receiver == &host && graph == &resolver && arg == aliasRef);
            trace.push_back("send");
            native->binaryReaders.erase("combine:");
            return Value("combined");
        };
        auto projection =
            Accessor::pluggable(std::make_shared<const Accessor::ReadBlock>([&](const Model &m) {
                trace.push_back(&m == &left ? "L" : "R");
                return &m == &left ? hostRef : aliasRef;
            }));
        trace.clear();
        SortBlock dispatch(projection, "combine:");
        check(dispatch.value(left, right, &resolver) == Value("combined"));
        check(trace == std::vector<std::string>({"L", "R", "send"}));
        fails([&] { dispatch.value(left, right, &resolver); });
        native->binaryReaders["="] = [](const void *, const Value &, const Graph *) {
            return Value(false);
        };
        check(SortBlock("~=").value(hostRef, aliasRef, &resolver) == Value(true));
        native->binaryReaders["="] = [](const void *, const Value &, const Graph *) {
            return Value("not boolean");
        };
        fails([&] { SortBlock("~=").value(hostRef, aliasRef, &resolver); });
        fails([&] { SortBlock("<=").value(hostRef, aliasRef); });
        fails([&] {
            SortBlock("<=").value(Value(Value::Reference{"missing"}), aliasRef, &resolver);
        });
        auto cyclic = AccessorObject::fromAccessor("cycle", identity());
        cyclic->value.className = "MADelegatorAccessor";
        cyclic->next = cyclic;
        SortBlock cycle(cyclic, "<=");
        fails([&] { cycle.value(Value(std::int64_t(1)), Value(std::int64_t(2))); });
        cyclic->next.reset();
        Limits low;
        low.visits = 1;
        fails([&] { block.value(left, right, nullptr, low); });
        // Plain graph snapshots remain data; a local SortBlock consumes reimported description
        // properties.
        auto a = std::make_shared<Description>(), b = std::make_shared<Description>(),
             root = std::make_shared<Description>();
        a->id = a->key = "a";
        b->id = b->key = "b";
        a->properties["priority"] = Value(std::int64_t(7));
        b->properties["priority"] = Value(std::int64_t(3));
        root->id = "root";
        root->className = "MAContainer";
        root->children = {a, b};
        Graph graph;
        graph.format = "magritte.graph/4";
        graph.root = root->id;
        graph.descriptions = {{a->id, a}, {b->id, b}, {root->id, root}};
        auto imported = Graph::decode(graph.encode());
        Accessor priority;
        priority.className = "MAPropertyAccessor";
        priority.name = "priority";
        check(SortBlock(priority, "<=")
                  .value(*imported.descriptions.at("a"), *imported.descriptions.at("b")) ==
              Value(false));
        if (argc > 1)
            std::ofstream(argv[1]) << graph.encode();
        std::cout << checks << " sorting checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
