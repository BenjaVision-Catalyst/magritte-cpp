#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool x, const std::string &msg = "Builder mismatch") {
    ++checks;
    if (!x)
        throw Error(msg);
}
J normal(J j) {
    if (j.is_object() && j.contains("error"))
        return J{{"error", true}};
    if (j.is_structured())
        for (auto &e : j)
            e = normal(e);
    return j;
}
J wire(const Value &v) {
    if (std::holds_alternative<Value::Reference>(v.data))
        return J{{"type", "model"}};
    return J::parse(encodeValue(v));
}
template <class F> J attempt(F f) {
    try {
        return J{{"value", wire(f())}};
    } catch (const Error &) {
        return J{{"error", true}};
    }
}
Accessor key(std::string s) {
    Accessor a;
    a.name = s;
    return a;
}
std::shared_ptr<Description> field(std::string name, std::int64_t priority) {
    auto d = std::make_shared<Description>();
    d->id = name;
    d->key = name;
    d->properties = {
        {"label", Value(name)}, {"priority", Value(priority)}, {"canExpand", Value(false)}};
    return d;
}
std::shared_ptr<Description> container(std::string id,
                                       std::vector<std::shared_ptr<Description>> children = {}) {
    auto d = std::make_shared<Description>();
    d->id = id;
    d->className = "MAContainer";
    d->children = children;
    return d;
}
J snapshot(const Description &d) {
    auto name = d.effectiveAccessor().accessorName();
    std::string text;
    if (auto s = std::get_if<Value::Symbol>(&name.data))
        text = s->utf8;
    else if (auto s = std::get_if<std::string>(&name.data))
        text = *s;
    return J{{"name", text},
             {"label", wire(d.property("label"))},
             {"group", wire(d.property("group"))},
             {"priority", wire(d.property("priority"))},
             {"container", d.isContainer()}};
}
struct Data {
    std::map<std::string, Value> values;
    std::string tag;
};
struct TrackingModel : Model {
    Data &data;
    std::vector<std::string> &trace;
    Graph &graph;
    TrackingModel(Data &d, std::vector<std::string> &t, Graph &g) : data(d), trace(t), graph(g) {}
    Value readSlot(const std::string &strategy, const std::string &name) const override {
        if (strategy != "MADictionaryAccessor")
            throw Error("Unbound strategy");
        trace.push_back(data.tag + ":" + name);
        auto i = data.values.find(name);
        return i == data.values.end() ? Value() : i->second;
    }
    void writeSlot(const std::string &, const std::string &name, Value v) override {
        data.values[name] = v;
    }
    Value readUsing(const Description &d) const override {
        return d.effectiveAccessor().read(*this, &graph);
    }
    void writeUsing(const Description &d, Value v) override {
        d.effectiveAccessor().write(*this, std::move(v), &graph);
    }
};
std::shared_ptr<TrackingModel> native(Data &data, std::vector<std::string> &trace, Graph &graph) {
    return std::make_shared<TrackingModel>(data, trace, graph);
}

} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/builders-reference.json");
        J f;
        in >> f;
        for (const auto &config : f.at("canExpand")) {
            Description d;
            d.id = "can";
            d.properties = {{"visible", Value(config.at("visible").get<bool>())},
                            {"readOnly", Value(config.at("readOnly").get<bool>())},
                            {"group", config.at("group").is_null()
                                          ? Value()
                                          : Value(config.at("group").get<std::string>())}};
            BuilderEnvironment env;
            env.tabDisplay = [&](const auto &) { return config.at("tab").get<bool>(); };
            check(canExpand(d, env) == config.at("result"));
        }
        for (const auto &config : f.at("flattening")) {
            Graph graph;
            std::vector<std::string> trace;
            auto city = field("city", 30), nestedSchema = container("nestedSchema", {city}),
                 leaf = field("name", 10), childSchema = container("childSchema", {leaf});
            if (config.at("primitiveDrop"))
                leaf->properties["canExpand"] = Value(true);
            if (config.at("emptySchema"))
                childSchema->children.clear();
            auto unused = field("unused", 0), parent = field("child", 100);
            parent->className = "MAToOneRelationDescription";
            auto reference = container("unusedSchema", {unused});
            parent->reference = reference;
            parent->properties["label"] = Value("Child");
            parent->properties["visible"] = Value(config.at("visible").get<bool>());
            parent->properties["readOnly"] = Value(config.at("readOnly").get<bool>());
            parent->properties["group"] = config.at("group").is_null()
                                              ? Value()
                                              : Value(config.at("group").get<std::string>());
            parent->properties.erase("canExpand");
            if (!config.at("explicit").is_null())
                parent->properties["canExpand"] = Value(config.at("explicit").get<bool>());
            auto nestedRelation = field("nested", 20);
            auto nestedReference = container("nestedReference");
            nestedRelation->className = "MAToOneRelationDescription";
            nestedRelation->reference = nestedReference;
            nestedRelation->properties["label"] = Value("Nested");
            nestedRelation->properties["canExpand"] = Value(true);
            if (config.at("recursive"))
                childSchema->children.push_back(nestedRelation);
            auto prefix = field("prefix", 0), suffix = field("suffix", 200),
                 source = container("source", {prefix, parent, suffix});
            Data rootData{{{"prefix", Value("P")},
                           {"suffix", Value("S")},
                           {"child", config.at("nilValue").get<bool>()
                                         ? Value()
                                         : Value(Value::Reference{"childModel"})}},
                          "root"};
            Data childData{
                {{"name", Value("Ada")}, {"nested", Value(Value::Reference{"nestedModel"})}},
                "child"},
                nestedData{{{"city", Value("Paris")}}, "nested"};
            auto root = native(rootData, trace, graph), child = native(childData, trace, graph),
                 nested = native(nestedData, trace, graph);
            graph.nativeObjects = {
                {"rootModel", root}, {"childModel", child}, {"nestedModel", nested}};
            DescriptionFlattener builder;
            builder.environment.graph = &graph;
            builder.environment.describe = [&](const Model *m) {
                if (m == root.get())
                    return source;
                if (m == child.get())
                    return childSchema;
                if (m == nested.get())
                    return nestedSchema;
                return std::shared_ptr<Description>{};
            };
            if (!config.at("missingTab").get<bool>())
                builder.environment.tabDisplay = [&](const auto &) {
                    return config.at("tab").get<bool>();
                };
            if (!config.at("missingGroup").get<bool>())
                builder.environment.groupExpandOnLabel = [&](const auto &) {
                    return config.at("groupLabel").get<bool>();
                };
            if (!config.at("missingAdd").get<bool>())
                builder.environment.addToAccessor = [](Description &d, const Accessor &head) {
                    Accessor a;
                    a.className = "MAChainAccessor";
                    a.steps = {head, d.effectiveAccessor()};
                    d.accessor = a;
                    d.accessorObject.reset();
                };
            J outcome;
            try {
                if (config.at("collectionTarget")) {
                    Value::Array targets;
                    if (!config.at("emptyTarget").get<bool>())
                        targets.push_back(Value(Value::Reference{"rootModel"}));
                    builder.buildFromValue(source, "flat", Value(targets));
                } else
                    builder.buildFrom(source, "flat", root.get());
                outcome = {{"success", true}};
            } catch (const Error &) {
                outcome = {{"error", true}};
            }
            auto result = builder.builtDescription();
            auto readTrace = trace;
            J fields = J::array(), values = J::array();
            bool shared = false;
            if (result)
                for (const auto &d : result->children) {
                    fields.push_back(snapshot(*d));
                    values.push_back(attempt([&] { return root->readUsing(*d); }));
                    shared = shared || d == leaf;
                }
            J actual = {{"outcome", outcome},
                        {"fields", fields},
                        {"values", values},
                        {"trace", readTrace},
                        {"sharedLeaf", shared},
                        {"leaf", snapshot(*leaf)},
                        {"sourceParent", snapshot(*parent)}};
            for (auto i = actual.begin(); i != actual.end(); ++i)
                if (i.value() != normal(config.at(i.key()))) {
                    std::cerr << config.at("id") << " " << i.key() << "\nactual " << i.value()
                              << "\nexpected " << normal(config.at(i.key())) << '\n';
                    throw Error("GT flattening mismatch");
                }
            ++checks;
            if (argc > 1 && config.at("id") == "flat") {
                Graph out;
                out.format = "magritte.graph/4";
                out.root = result->id;
                out.descriptions[result->id] = result;
                AdaptiveModel person;
                person.id = "sample";
                person.description = result;
                for (const auto &d : result->children) {
                    out.descriptions[d->id] = d;
                    person.values[d->id] = root->readUsing(*d);
                }
                out.objects.emplace(person.id, person);
                std::ofstream(argv[1]) << out.encode();
            }
        }
        for (const auto &row : f.at("composition")) {
            auto mode = row.at("mode").get<std::string>();
            bool relation = mode.find("relation") != std::string::npos;
            auto source = mode == "scalar" ? field("source", 0) : container("source");
            source->properties["label"] = Value("Source");
            auto reference = container("reference");
            reference->properties["label"] = Value("Reference");
            if (relation) {
                source->className = mode == "many-relation" ? "MAToManyRelationDescription"
                                                            : "MAToOneRelationDescription";
                source->key = "child";
                source->reference = reference;
            }
            auto replacement = container("replacement");
            replacement->properties["label"] = Value("Replacement");
            auto targetSchema = container("targetSchema");
            Graph graph;
            DictionaryModel root, child, marker;
            root.resolver = &graph;
            auto childOwner = std::make_shared<DictionaryModel>();
            graph.nativeObjects["child"] = childOwner;
            root.values["child"] = mode == "nil-relation" ? Value()
                                   : mode == "many-relation"
                                       ? Value(Value::Array{Value(Value::Reference{"child"})})
                                       : Value(Value::Reference{"child"});
            J events = J::array();
            std::vector<BuilderStep> steps;
            if (mode == "class-step")
                steps = {BuilderStep::named("MADescriptionBuilder")};
            else if (mode != "empty")
                for (const std::string tag : {"A", "B"})
                    steps.push_back({[&, tag](auto d, const auto &, DescriptionBuilder &context) {
                        if (!d)
                            throw Error("Nil input");
                        auto label = std::get<std::string>(d->property("label").data);
                        std::string role =
                            context.targetValue
                                ? (context.targetValue->isCollection() ? "collection" : "child")
                                : "root";
                        events.push_back(J::array({tag, label, role, context.model == &marker}));
                        if (mode == "failure" && tag == "B")
                            throw Error("step");
                        d->properties["label"] = Value(label + tag);
                        return DescriptionCopy{mode == "replace" && tag == "A" ? replacement : d,
                                               {source, reference, replacement}};
                    }});
            ComposedDescriptionBuilder builder;
            builder.environment.graph = &graph;
            builder.environment.describe = [&](const auto *) { return targetSchema; };
            builder.environment.descriptionBuilders = [&](const auto &) { return steps; };
            builder.model = &marker;
            bool success = true;
            try {
                builder.buildFrom(source, "composed", &root);
            } catch (const Error &) {
                success = false;
            }
            auto result = builder.builtDescription();
            J actual = {{"mode", mode},
                        {"success", success},
                        {"resultLabel", wire(result ? result->property("label") : Value())},
                        {"sourceLabel", std::get<std::string>(source->property("label").data)},
                        {"sameSource", result == source},
                        {"replacement", result == replacement},
                        {"events", events}};
            if (relation) {
                actual["referenceLabel"] =
                    std::get<std::string>(source->referenceDescription()->property("label").data);
                actual["sameReference"] = source->referenceDescription() == reference;
            }
            check(actual == row, "GT composition " + mode + " " + actual.dump());
        }
        // Runtime-open factories, portable class metadata, and cyclic compositions are bounded.
        struct Named : DescriptionBuilder {
            using DescriptionBuilder::DescriptionBuilder;
            void visitStringDescription(Description &d) override {
                d.properties["label"] = Value("Native");
                DescriptionBuilder::visitStringDescription(d);
            }
        };
        registerDescriptionBuilder("MABuildersTestNative",
                                   [](Limits l) { return std::make_unique<Named>(l); });
        auto source = container("classes", {field("x", 0)});
        setDescriptionBuilders(*source, {"MABuildersTestNative"});
        ComposedDescriptionBuilder composed;
        auto output = composed.buildFrom(source, "registered");
        check(output.root != source &&
              output.root->children[0]->property("label") == Value("Native"));
        auto cyclic = container("cycle");
        setDescriptionBuilders(*cyclic, {"MAComposedDescriptionBuilder"});
        Limits low;
        low.depth = 3;
        ComposedDescriptionBuilder bounded(low);
        check(attempt([&] {
                  bounded.buildFrom(cyclic, "cycle-copy");
                  return Value();
              }).contains("error"));
        check(Value(Value::Fraction{1, 2}) == Value(0.5));
        check(Value(Value::Fraction{2, 2}) == Value(std::int64_t(1)));
        check(encodeValue(decodeValue(encodeValue(Value(Value::Fraction{-1, 1000})))) ==
              encodeValue(Value(Value::Fraction{-1, 1000})));

        std::ifstream numericIn(std::string(FIXTURES) + "/builders-priorities-reference.json");
        J numeric;
        numericIn >> numeric;
        for (const auto &row : numeric.at("comparisons")) {
            auto left = decodeValue(row.at("left").dump()),
                 right = decodeValue(row.at("right").dump());
            check((left == right) == row.at("equal"), "Fraction equality");
            for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
                     {"<", "less"}, {">", "greater"}}) {
                Predicate p;
                p.selector = entry.first;
                p.arguments = {right};
                check(p.evaluate(left) == row.at(entry.second), "Fraction ordering");
            }
        }
        for (const auto &row : numeric.at("arithmetic")) {
            auto leaf = field("arithName", 0);
            leaf->key = "name";
            leaf->properties["priority"] = decodeValue(row.at("child").dump());
            auto childSchema = container("arithChild", {leaf});
            auto reference = container("arithRef");
            auto parent = field("arithParent", 0);
            parent->className = "MAToOneRelationDescription";
            parent->key = "child";
            parent->reference = reference;
            parent->properties["priority"] = decodeValue(row.at("parent").dump());
            parent->properties["canExpand"] = Value(true);
            auto root = container("arithRoot", {parent});
            Graph graph;
            DictionaryModel model;
            model.resolver = &graph;
            auto child = std::make_shared<DictionaryModel>();
            child->values["name"] = Value("value");
            graph.nativeObjects["arithValue"] = child;
            model.values["child"] = Value(Value::Reference{"arithValue"});
            DescriptionFlattener f;
            f.environment.graph = &graph;
            f.environment.describe = [&](const Model *m) {
                return m == child.get() ? childSchema : std::shared_ptr<Description>{};
            };
            f.environment.groupExpandOnLabel = [](const auto &) { return false; };
            f.environment.addToAccessor = [](Description &d, const Accessor &head) {
                Accessor a;
                a.className = "MAChainAccessor";
                a.steps = {head, d.effectiveAccessor()};
                d.accessor = a;
            };
            auto out = f.buildFrom(root, "arithBuilt", &model);
            check(out.root->children[0]->property("priority") ==
                  decodeValue(row.at("result").dump()));
        }
        auto priorityContainer = container("prioritySort", {field("large", 0), field("small", 0)});
        priorityContainer->className = "MAPriorityContainer";
        priorityContainer->children[0]->properties["priority"] = Value(Value::Fraction{10001, 100});
        priorityContainer->children[1]->properties["priority"] = Value(std::int64_t(100));
        check(priorityContainer->orderedChildren()[0]->id == "small");
        check(attempt([] { return Value(Value::Fraction{1, 0}); }).contains("error"));
        check(attempt([] { return Value(Value::Fraction{1, -1}); }).contains("error"));
        Graph fractions;
        fractions.format = "magritte.graph/4";
        fractions.root = priorityContainer->id;
        fractions.descriptions[priorityContainer->id] = priorityContainer;
        for (const auto &d : priorityContainer->children)
            fractions.descriptions[d->id] = d;
        auto modern = fractions.encode();
        auto legacy = J::parse(modern);
        legacy["format"] = "magritte.graph/3";
        check(attempt([&] {
                  Graph::decode(legacy.dump());
                  return Value();
              }).contains("error"));
        Limits targetBound;
        targetBound.items = 1;
        DescriptionFlattener targetLimited(targetBound);
        check(attempt([&] {
                  targetLimited.buildFromValue(source, "bounded-target",
                                               Value(Value::Array{Value(), Value()}));
                  return Value();
              }).contains("error"));
        Description explicitExpansion;
        explicitExpansion.id = "explicit";
        explicitExpansion.properties = {
            {"visible", Value(false)}, {"readOnly", Value(true)}, {"canExpand", Value(true)}};
        check(canExpand(explicitExpansion));
        auto plain = field("plain", 0);
        DictionaryModel undescribed;
        ComposedDescriptionBuilder noAnnotations;
        check(noAnnotations.buildFrom(plain, "plain-built", &undescribed).root == plain);
        DescriptionFlattener nilBinding;
        auto nilSchema = container("nilSchema", {field("nilField", 0)});
        auto nilRoot = container("nilRoot");
        nilBinding.setBuiltDescription(nilRoot);
        nilBinding.environment.describe = [&](const Model *) { return nilSchema; };
        check(attempt([&] {
                  nilBinding.expandDescription(*plain, Value());
                  return Value();
              }).contains("error"));
        std::cout << checks << " builder checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
