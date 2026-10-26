#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Model read mismatch") {
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
    check(caught, "Expected model read failure");
}
struct Box {
    Value value;
    int reads = 0;
};
struct Slots : Model {
    Box box;
    Value readSlot(const std::string &, const std::string &) const override {
        auto &b = const_cast<Box &>(box);
        ++b.reads;
        return b.value;
    }
    void writeUsing(const Description &, Value) override { throw Error("Unexpected write"); }
};
struct RawOverride : Slots {
    Value readUsing(const Description &) const override { return Value(); }
};
Accessor slot(const std::string &cls, const std::string &name) {
    Accessor a;
    a.className = cls;
    a.name = name;
    if (cls == "MASelectorAccessor")
        a.writeName = name + ":";
    return a;
}
Value val(const J &j) { return decodeValue(j.dump()); }
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/model-read-scenarios.json");
        J fixture;
        in >> fixture;
        for (const auto &row : fixture.at("cases")) {
            Description d;
            d.id = d.key = "field";
            auto current = val(row.at("current")), fallback = val(row.at("undefinedValue"));
            d.properties = {{"undefinedValue", fallback},       {"default", Value("unused")},
                            {"initialAnswer", Value("answer")}, {"required", Value(true)},
                            {"readOnly", Value(true)},          {"visible", Value(false)},
                            {"shouldCacheDefault", Value(true)}};
            int defaults = 0;
            d.defaultFunction = [&] {
                ++defaults;
                return Value("evaluated");
            };
            auto before = d.properties;
            auto kind = row.at("adapter").get<std::string>();
            Value actual;
            if (kind == "generic") {
                Slots m;
                m.box.value = current;
                actual = m.readUsing(d);
                check(m.box.reads == 1 && m.box.value == current);
            } else if (kind == "dictionary") {
                DictionaryModel m;
                m.values[d.key] = current;
                actual = m.readUsing(d);
                check(m.values.at(d.key) == current);
            } else if (kind.rfind("native-", 0) == 0) {
                Box box{current};
                NativeModel m(&box);
                if (kind == "native-direct")
                    d.nativeRead = [](const void *p) {
                        auto &b = *const_cast<Box *>(static_cast<const Box *>(p));
                        ++b.reads;
                        return b.value;
                    };
                else {
                    auto strategy = kind == "native-selector"   ? "MASelectorAccessor"
                                    : kind == "native-variable" ? "MAVariableAccessor"
                                                                : "MAPropertyAccessor";
                    d.accessor = slot(strategy, "field");
                    m.readers[std::string(strategy) + ":field"] = [](const void *p) {
                        auto &b = *const_cast<Box *>(static_cast<const Box *>(p));
                        ++b.reads;
                        return b.value;
                    };
                }
                actual = m.readUsing(d);
                check(box.reads == 1 && box.value == current);
            } else {
                Description m;
                auto key = kind == "description-selector" ? "undefinedValue" : "metadata:value";
                m.properties[key] = current;
                if (kind == "description-direct")
                    d.key = key;
                else
                    d.accessor = slot(kind == "description-selector" ? "MASelectorAccessor"
                                                                     : "MAPropertyAccessor",
                                      key);
                actual = m.readUsing(d);
                check(m.properties.at(key) == current);
            }
            check(actual == val(row.at("result")));
            check(defaults == 0 && d.properties == before);
        }
        auto d = std::make_shared<Description>(), root = std::make_shared<Description>();
        d->id = d->key = "field";
        root->id = "root";
        root->className = "MAContainer";
        root->children = {d};
        d->properties["default"] = Value("default");
        for (const auto &row : fixture.at("editing")) {
            d->properties["undefinedValue"] = val(row.at("undefinedValue"));
            DictionaryModel generic;
            AdaptiveModel adaptive;
            adaptive.description = root;
            check(generic.readUsing(*d) == val(row.at("generic")));
            check(adaptive.readUsing(*d) == val(row.at("adaptive")));
            StraightMemento gs(generic, root), as(adaptive, root);
            CachedMemento gc(generic, root), ac(adaptive, root);
            CheckedMemento checked(generic, root);
            check(gs.readUsing(*d) == val(row.at("straightGeneric")));
            check(as.readUsing(*d) == val(row.at("straightAdaptive")));
            check(gc.readUsing(*d) == val(row.at("cachedGeneric")));
            check(ac.readUsing(*d) == val(row.at("cachedAdaptive")));
            check(checked.original().at(d->id) == val(row.at("generic")));
            gc.writeUsing(*d, Value());
            check(gc.readUsing(*d).isNil());
            check(generic.values.empty());
        }
        // Absent and stored nil have the same read result without changing stored presence.
        DictionaryModel dictionary;
        d->properties["undefinedValue"] = Value(Value::Symbol{"unset"});
        check(dictionary.readUsing(*d) == d->property("undefinedValue") &&
              dictionary.values.empty());
        dictionary.values[d->key] = Value();
        check(dictionary.readUsing(*d) == d->property("undefinedValue") &&
              dictionary.values.at(d->key).isNil());
        check(d->effectiveAccessor().read(dictionary).isNil());
        RawOverride override;
        check(override.readUsing(*d).isNil());
        // One accessor evaluation, followed by the current undefinedValue; errors are not
        // recovered.
        Box box;
        NativeModel native(&box);
        d->nativeRead = [&](const void *p) {
            auto &b = *const_cast<Box *>(static_cast<const Box *>(p));
            ++b.reads;
            d->properties["undefinedValue"] = Value("after");
            return b.value;
        };
        check(native.readUsing(*d) == Value("after") && box.reads == 1);
        box.value = Value(false);
        check(native.readUsing(*d) == Value(false) && box.reads == 2);
        d->nativeRead = [](const void *) -> Value { throw Error("read failed"); };
        fails([&] { native.readUsing(*d); });
        d->nativeRead = {};
        d->accessor = slot("MASelectorAccessor", "value");
        fails([&] { native.readUsing(*d); });
        native.readers["MASelectorAccessor:value"] = [](const void *p) {
            return static_cast<const Box *>(p)->value;
        };
        box.value = Value();
        check(d->accessor->read(native).isNil());
        check(native.readUsing(*d) == Value("after"));
        // Chain stages remain raw; only a successfully completed read can fall back.
        Graph resolver;
        auto target = std::make_shared<DictionaryModel>();
        resolver.nativeObjects["target"] = target;
        native.resolver = &resolver;
        int links = 0;
        bool linked = false;
        native.readers["MASelectorAccessor:next"] = [&](const void *) {
            ++links;
            return linked ? Value(Value::Reference{"target"}) : Value();
        };
        Accessor chain;
        chain.className = "MAChainAccessor";
        chain.steps = {slot("MASelectorAccessor", "next"), slot("MADictionaryAccessor", "value")};
        d->accessor = chain;
        d->properties["undefinedValue"] = Value("outer");
        fails([&] { native.readUsing(*d); });
        check(links == 1);
        linked = true;
        check(native.readUsing(*d) == Value("outer") && links == 2);
        check(chain.read(native, &resolver).isNil() && links == 3);
        d->accessor.reset();
        // Generic fallbacks feed validation/printing/comparison; adaptive overrides still return
        // nil.
        d->properties = {{"undefinedValue", Value(Value::Symbol{"unset"})},
                         {"default", Value("default")},
                         {"required", Value(true)}};
        check(validate(*root, dictionary).empty());
        AdaptiveModel adaptive;
        adaptive.description = root;
        auto adaptiveErrors = validate(*root, adaptive);
        check(adaptiveErrors.size() == 2 && adaptiveErrors[0].code == "MARequiredError" &&
              adaptiveErrors[1].code == "MAKindError");
        check(d->readToString(dictionary) == "unset");
        PrintStream printed;
        d->printFor(dictionary, printed);
        check(printed.contents() == "unset");
        DictionaryModel defaulted;
        defaulted.values[d->key] = Value("default");
        check(!isSameAs(dictionary, defaulted, *root));
        AdaptiveModel defaultedAdaptive;
        defaultedAdaptive.description = root;
        defaultedAdaptive.values[d->id] = Value("default");
        check(isSameAs(adaptive, defaultedAdaptive, *root));
        // Reflective descriptions/accessors and condition objects inherit Object's generic read
        // hook.
        RecursiveDescriptionModel session;
        auto metadata = std::make_shared<Description>();
        metadata->id = "metadata";
        auto bound = session.bind(metadata);
        Description query;
        query.id = "query";
        query.accessor = slot("MASelectorAccessor", "group");
        query.properties["undefinedValue"] = Value("metadata fallback");
        check(query.accessor->read(*bound, &session.resolver()).isNil());
        check(bound->readUsing(query) == Value("metadata fallback"));
        auto accessor = AccessorObject::fromAccessor("accessor", slot("MASelectorAccessor", ""));
        auto boundAccessor = session.bind(accessor);
        query.accessor = slot("MASelectorAccessor", "selector");
        check(boundAccessor->readUsing(query) == Value("metadata fallback"));
        auto condition = std::make_shared<ConditionObject>();
        condition->id = "condition";
        condition->predicate.selector = "isNil";
        auto boundCondition = session.bind(condition);
        query.accessor = Accessor::pluggable(
            std::make_shared<Accessor::ReadBlock>([](const Model &) { return Value(); }));
        check(boundCondition->readUsing(query) == Value("metadata fallback"));
        std::shared_ptr<Model> expired;
        {
            RecursiveDescriptionModel temporary;
            expired = temporary.bind(metadata);
        }
        fails([&] { expired->readUsing(query); });
        Limits low;
        low.bytes = 128;
        RecursiveDescriptionModel bounded(low);
        auto boundedModel = bounded.bind(metadata);
        query.properties["undefinedValue"] = Value(std::string(300, 'x'));
        fails([&] { boundedModel->readUsing(query); });
        // Existing graph data keeps absent adaptive fields, and drives dictionary reads after
        // import.
        d->properties = {{"undefinedValue", Value("missing")}, {"default", Value("default")}};
        root->children = {d};
        Graph g;
        g.format = "magritte.graph/4";
        g.root = root->id;
        g.descriptions = {{root->id, root}, {d->id, d}};
        AdaptiveModel object;
        object.id = "object";
        object.description = root;
        g.objects.emplace(object.id, object);
        auto wire = g.encode();
        auto imported = Graph::decode(wire);
        DictionaryModel importedDictionary;
        auto field = imported.descriptions.at(d->id);
        check(importedDictionary.readUsing(*field) == Value("missing"));
        check(imported.objects.at("object").readUsing(*field).isNil());
        check(field->effectiveAccessor().read(importedDictionary).isNil());
        check(imported.encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        std::cout << checks << " model read checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
