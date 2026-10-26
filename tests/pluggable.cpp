#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int count = 0;
void check(bool ok, const std::string &what) {
    ++count;
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
template <class F> auto reader(F f) {
    return std::make_shared<const Accessor::ReadBlock>(std::move(f));
}
template <class F> auto writer(F f) {
    return std::make_shared<const Accessor::WriteBlock>(std::move(f));
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream input(std::string(FIXTURES) + "/pluggable-accessors-reference.json");
        J f;
        input >> f;
        DictionaryModel model, plain;
        J calls = J::array();
        auto r = reader([&](const Model &m) {
            calls.push_back(J::array({"read", &m == &model}));
            return m.readSlot("MADictionaryAccessor", "slot");
        });
        auto w = writer([&](Model &m, const Value &v) {
            calls.push_back(J::array({"write", &m == &model, wire(v)}));
            m.writeSlot("MADictionaryAccessor", "slot", v);
        });
        for (const auto &row : f["capabilities"]) {
            auto a = Accessor::pluggable(row["read"].get<bool>() ? r : nullptr,
                                         row["write"].get<bool>() ? w : nullptr);
            calls = J::array();
            const Model &target = row["model"] == "dictionary" ? model : plain;
            check(a.canRead(target) == row["canRead"].get<bool>(),
                  "GT canRead independent of target");
            check(a.canWrite(target) == row["canWrite"].get<bool>(),
                  "GT canWrite independent of target");
            check(calls == row["calls"], "No capability callback execution");
        }
        auto a = Accessor::pluggable(r, w);
        for (const auto &row : f["readWrites"]) {
            calls = J::array();
            a.write(model, value(row["input"]));
            check(a.read(model) == value(row["value"]), "GT read/write value");
            check(calls == row["calls"], "GT callback arguments and order");
        }
        auto b = Accessor::pluggable(r, w),
             c = Accessor::pluggable(
                 reader([](const Model &m) { return m.readSlot("MADictionaryAccessor", "slot"); }),
                 w);
        Accessor selector;
        selector.className = "MASelectorAccessor";
        selector.name = "slot";
        selector.writeName = "slot:";
        J equality = J::array({accessorEqual(a, a), accessorEqual(a, b), accessorEqual(a, c),
                               accessorEqual(a, Accessor::pluggable(r)), accessorEqual(a, selector),
                               accessorEqual(Accessor::pluggable(), Accessor::pluggable()),
                               accessorEqual(a, Accessor(a))});
        check(equality == f["equality"], "GT block identity equality");
        int counter = 0;
        a = Accessor::pluggable(
            reader([&](const Model &) { return Value(std::int64_t(++counter)); }), w);
        b = a;
        J outputs = J::array({std::get<std::int64_t>(a.read(model).data),
                              std::get<std::int64_t>(b.read(model).data)});
        check((a.readBlock == b.readBlock) == f["copy"]["sameReadBlock"].get<bool>(),
              "GT shallow read block copy");
        check((a.writeBlock == b.writeBlock) == f["copy"]["sameWriteBlock"].get<bool>(),
              "GT shallow write block copy");
        b.readBlock = reader([](const Model &) { return Value(std::int64_t(99)); });
        outputs.push_back(std::get<std::int64_t>(a.read(model).data));
        outputs.push_back(std::get<std::int64_t>(b.read(model).data));
        check(outputs == f["copy"]["outputs"] && counter == f["copy"]["counter"].get<int>(),
              "GT shared capture state and independent slots");
        check(accessorEqual(a, b) == f["copy"]["equalAfterReplacement"].get<bool>(),
              "GT equality after replacement");
        auto d = std::make_shared<Description>();
        d->id = "field";
        d->properties["label"] = Value("Value");
        d->properties["required"] = Value(true);
        d->accessor = a;
        auto copy = copyDescription(d, "copy");
        check(descriptionEqual(*d, *copy.root) == f["copy"]["descriptionEqual"].get<bool>(),
              "GT description equality");
        check((copy.root->effectiveAccessor().readBlock == a.readBlock) ==
                  f["copy"]["descriptionReadShared"].get<bool>(),
              "GT description copy shares closures");
        d->accessor.reset();
        d->accessorObject = AccessorObject::fromAccessor("live-accessor", a);
        copy = copyDescription(d, "copy-live");
        check((d->accessorObject != copy.root->accessorObject) ==
                  f["copy"]["descriptionAccessorDistinct"].get<bool>(),
              "GT copied accessor object identity");
        check(accessorEqual(d->effectiveAccessor(), copy.root->effectiveAccessor()),
              "Live accessor copy keeps block identities");
        auto inner = std::make_shared<DictionaryModel>();
        inner->values["slot"] = Value("start");
        Graph resolver;
        resolver.nativeObjects["inner"] = inner;
        auto head = Accessor::pluggable(reader([&](const Model &) {
            calls.push_back("head");
            return Value(Value::Reference{"inner"});
        }));
        auto tail = Accessor::pluggable(reader([&](const Model &m) {
                                            calls.push_back("tailRead");
                                            return m.readSlot("MADictionaryAccessor", "slot");
                                        }),
                                        writer([&](Model &m, const Value &v) {
                                            calls.push_back("tailWrite");
                                            m.writeSlot("MADictionaryAccessor", "slot", v);
                                        }));
        Accessor chain;
        chain.className = "MAChainAccessor";
        chain.steps = {head, tail};
        for (const auto &row : f["chains"]) {
            calls = J::array();
            Value result;
            auto op = row["operation"];
            if (op == "canRead")
                result = Value(chain.canRead(model, &resolver));
            if (op == "canWrite")
                result = Value(chain.canWrite(model, &resolver));
            if (op == "read")
                result = chain.read(model, &resolver);
            if (op == "write")
                chain.write(model, Value("changed"), &resolver);
            check(result == value(row["value"]) && inner->values["slot"] == value(row["inner"]),
                  "GT composed operation");
            check(calls == row["calls"], "GT chain callback order");
        }
        check(chain.canRead(model) && chain.canWrite(model),
              "Pluggable terminal capabilities need no resolver");
        Accessor delegator;
        delegator.className = "MADelegatorAccessor";
        delegator.steps = {tail};
        check(delegator.canRead(*inner) && delegator.canWrite(*inner),
              "Delegator supports block slots");
        check(delegator.read(*inner) == Value("changed"), "Delegator invokes pluggable leaf");
        a = Accessor::pluggable(r, w);
        d->accessorObject.reset();
        d->accessor = a;
        model.values["slot"] = Value("base");
        auto root = std::make_shared<Description>();
        root->id = "root";
        root->className = "MAContainer";
        root->children = {d};
        CachedMemento editor(model, root);
        J snapshots = J::array();
        auto snapshot = [&] {
            snapshots.push_back({{"model", wire(model.values.at("slot"))},
                                 {"editor", wire(editor.readUsing(*d))},
                                 {"changed", editor.hasChanges()}});
        };
        snapshot();
        editor.writeUsing(*d, Value("edited"));
        check(editor.validate().empty(), "Pluggable memento validates");
        snapshot();
        editor.commit();
        snapshot();
        check(snapshots == f["memento"], "GT staged editing and commit");
        rejects([&] { Accessor::pluggable().read(model); });
        rejects([&] { Accessor::pluggable().write(model, Value()); });
        rejects([&] {
            Accessor::pluggable(reader([](const Model &) -> Value {
                throw Error("Read failure");
            })).read(model);
        });
        rejects([&] {
            Accessor::pluggable(
                {}, writer([](Model &, const Value &) { throw Error("Write failure"); }))
                .write(model, Value());
        });
        auto info = ClassRegistry::global().find("MAPluggableAccessor");
        check(info && info->superclass == f["class"]["superclass"] &&
                  info->label == f["class"]["label"] && !info->isAbstract,
              "GT registered class");
        auto object = AccessorObject::fromAccessor("pluggable", a);
        RecursiveDescriptionModel session;
        auto schema = session.describe(object);
        check(schema->children.empty() && f["metadataFields"].empty(),
              "GT empty annotated self-description");
        auto metadata = session.bind(d);
        Accessor name;
        name.className = "MASelectorAccessor";
        name.name = "name";
        name.writeName = "name:";
        check(name.read(*metadata).isNil(), "Pluggable name is nil");
        auto live = AccessorObject::fromAccessor("live", a);
        d->accessorObject = live;
        d->accessor.reset();
        live->value.readBlock = reader([](const Model &) { return Value("replacement"); });
        check(model.readUsing(*d) == Value("replacement"), "Live accessor slot change observed");
        // Executable blocks never enter a portable graph, including absent blocks and hidden nodes.
        rejects([&] { encodeAccessor(a); });
        rejects([&] { encodeAccessor(Accessor::pluggable()); });
        rejects([&] { encodeAccessor(chain); });
        rejects([&] { decodeAccessor("{\"class\":\"MAPluggableAccessor\"}"); });
        Graph graph;
        graph.format = "magritte.graph/4";
        graph.root = "root";
        graph.descriptions = {{"root", root}, {"field", d}};
        d->properties["visible"] = Value(false);
        rejects([&] { graph.encode(); });
        d->materializeAccessor();
        rejects([&] { graph.encode(); });
        d->properties["visible"] = Value(true);
        auto malformed = a;
        malformed.name = "unexpected";
        rejects([&] { malformed.check(); });
        malformed = selector;
        malformed.readBlock = r;
        rejects([&] { malformed.check(); });
        malformed = Accessor::pluggable(std::make_shared<const Accessor::ReadBlock>());
        rejects([&] { malformed.check(); });
        auto mutableState = Accessor::pluggable(
            reader([n = std::int64_t(0)](const Model &) mutable { return Value(++n); }));
        auto stateCopy = mutableState;
        check(mutableState.read(model) == Value(std::int64_t(1)) &&
                  stateCopy.read(model) == Value(std::int64_t(2)),
              "Mutable C++ callable state remains shared");
        auto replacing = Accessor::pluggable();
        replacing.readBlock = reader([&](const Model &) {
            replacing.readBlock.reset();
            return Value("read-once");
        });
        check(replacing.read(model) == Value("read-once") && !replacing.canRead(model),
              "Executing reader survives slot replacement");
        replacing.writeBlock =
            writer([&](Model &, const Value &) { replacing.writeBlock.reset(); });
        replacing.write(model, Value());
        check(!replacing.canWrite(model), "Executing writer survives slot replacement");
        Limits tiny;
        tiny.visits = 0;
        rejects([&] { a.canRead(model, nullptr, tiny); });
        tiny = Limits{};
        tiny.bytes = 8;
        bool called = false;
        auto bounded =
            Accessor::pluggable(reader([](const Model &) { return Value("long result"); }),
                                writer([&](Model &, const Value &) { called = true; }));
        rejects([&] { bounded.read(model, nullptr, tiny); });
        rejects([&] { bounded.write(model, Value("long input"), nullptr, tiny); });
        check(!called, "Invalid write input rejected before callback");
        // Caller explicitly chooses an ordinary portable accessor before export.
        Accessor portable;
        portable.name = "slot";
        d->accessor = portable;
        graph.objects["sample"].id = "sample";
        graph.objects["sample"].description = root;
        graph.objects["sample"].values["field"] = model.values.at("slot");
        auto encoded = graph.encode();
        check(Graph::decode(encoded).descriptions.at("field")->effectiveAccessor().className ==
                  "MADictionaryAccessor",
              "Explicit accessor replacement restores interchange");
        if (argc > 1) {
            std::ofstream out(argv[1]);
            out << encoded << '\n';
        }
        std::cout << count << " pluggable accessor checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "After " << count << " checks: " << e.what() << '\n';
        return 1;
    }
}
