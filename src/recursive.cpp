#include "detail.hpp"
#include <algorithm>
#include <set>
namespace magritte {
namespace {
Graph expanded(const Description &target, Limits limits) {
    auto graph = descriptionModelFor(target, limits);
    auto root = graph.descriptions.at(graph.root);
    auto add = [&](std::string key, std::string cls, std::int64_t priority) {
        auto d = std::make_shared<Description>();
        d->id = graph.root + "." + key;
        d->className = cls;
        Accessor a;
        a.className = "MASelectorAccessor";
        a.name = key;
        a.writeName = key == "children" ? "setChildren:" : key + ":";
        d->accessor = a;
        d->properties["priority"] = Value(priority);
        root->children.push_back(d);
        graph.descriptions.emplace(d->id, d);
        if (d->isReference()) {
            auto ref = std::make_shared<Description>();
            ref->id = d->id + ".reference";
            Accessor null;
            null.className = "MANullAccessor";
            ref->accessor = null;
            d->reference = ref;
            graph.descriptions.emplace(ref->id, ref);
        }
        return d;
    };
    add("name", "MASymbolDescription", 1);
    if (target.isContainer()) {
        auto d = add("children", "MAToManyRelationDescription", 400);
        d->properties["label"] = Value("Elements");
        d->properties["ordered"] = Value(true);
        d->properties["default"] = Value(Value::Collection{"ordered", {}});
        d->classes = ClassRegistry::global().withAllConcreteClasses("MAElementDescription");
    }
    if (target.isReference()) {
        auto d = add("reference", "MAToOneRelationDescription", 400);
        d->properties["label"] = Value("Description");
        if (target.isRelation()) {
            d->classes = ClassRegistry::global().withAllConcreteClasses("MAContainer");
            d->properties["required"] = Value(false);
        } else {
            d->classes = ClassRegistry::global().withAllConcreteClasses("MADescription");
            d->properties["required"] = Value(true);
        }
    }
    if (target.isOption()) {
        auto d = add("optionsTextual", "MAMemoDescription", 410);
        d->properties["label"] = Value("Options");
        d->properties["default"] = Value(Value::Collection{"ordered", {}});
    }
    // Root insertion order follows the explicitly selected upstream methods.
    return graph;
}

std::vector<std::string> accessorClasses() { return ClassRegistry::global().allSubclasses("MAAccessor"); }
std::shared_ptr<Description> addMetadata(Graph &g, const std::string &key, const std::string &cls,
                                         std::optional<std::int64_t> priority = {}) {
    auto d = std::make_shared<Description>();
    d->id = g.root + "." + key;
    d->className = cls;
    Accessor a;
    a.className = "MASelectorAccessor";
    a.name = key;
    a.writeName = key + ":";
    d->accessor = a;
    if (priority)
        d->properties["priority"] = Value(*priority);
    g.descriptions.at(g.root)->children.push_back(d);
    g.descriptions.emplace(d->id, d);
    if (d->isReference()) {
        auto ref = std::make_shared<Description>();
        ref->id = d->id + ".reference";
        Accessor null;
        null.className = "MANullAccessor";
        ref->accessor = null;
        d->reference = ref;
        g.descriptions.emplace(ref->id, ref);
    }
    return d;
}
Graph objectMetadata(const std::string &cls) {
    Graph g;
    g.format = "magritte.graph/4";
    g.root = "meta." + cls;
    auto root = std::make_shared<Description>();
    root->id = g.root;
    root->className = "MAContainer";
    root->properties["label"] = Value(cls);
    g.descriptions.emplace(root->id, root);
    return g;
}
Graph reflected(const Description &target, Limits limits) {
    auto g = expanded(target, limits);
    g.format = "magritte.graph/4";
    auto &registry = ClassRegistry::global();
    for (const auto &key : {"children", "reference"}) {
        auto i = g.descriptions.find(g.root + "." + key);
        if (i == g.descriptions.end())
            continue;
        auto base = std::string(key) == "children" ? "MAElementDescription"
                    : target.isRelation()          ? "MAContainer"
                                                   : "MADescription";
        i->second->classes = registry.withAllConcreteClasses(base);
    }
    auto a = addMetadata(g, "accessor", "MAToOneRelationDescription", 10);
    a->classes = accessorClasses();
    for (const auto &key : {"stringReader", "stringWriter", "validator"}) {
        auto cls = std::string(key) == "stringReader"   ? "MAStringReader"
                   : std::string(key) == "stringWriter" ? "MAStringWriter"
                                                        : "MAValidatorVisitor";
        auto d = addMetadata(g, key, "MASingleOptionDescription",
                             std::string(key) == "stringReader"   ? 300
                             : std::string(key) == "stringWriter" ? 310
                                                                  : 250);
        d->properties["label"] = Value(std::string(key) == "stringReader"   ? "String Reader"
                                       : std::string(key) == "stringWriter" ? "String Writer"
                                                                            : "Validator");
        d->properties["default"] = Value(Value::Class{cls});
        d->options.clear();
        for (const auto &name : registry.withAllSubclasses(cls))
            d->options.push_back(Value(Value::Class{name}));
        d->referenceDescription()->className = "MAClassDescription";
    }
    if (target.isRelation()) {
        auto d = addMetadata(g, "classes", "MAMultipleOptionDescription", 400);
        d->properties["label"] = Value("Classes");
        d->properties["default"] = Value(Value::Collection{"set", {}});
        d->referenceDescription()->className = "MAClassDescription";
        for (const auto &name : registry.allClasses())
            d->options.push_back(Value(Value::Class{name}));
    }
    return g;
}
Graph accessorMetadata(const AccessorObject &object) {
    auto cls = object.value.className;
    auto g = objectMetadata(cls);
    if (cls == "MADictionaryAccessor" || cls == "MAPropertyAccessor")
        addMetadata(g, "key", "MASymbolDescription")->properties["required"] = Value(true);
    if (cls == "MASelectorAccessor")
        addMetadata(g, "selector", "MASymbolDescription");
    if (cls == "MAChainAccessor" || cls == "MACompositeAccessor" || cls == "MADelegatorAccessor") {
        addMetadata(g, "next", "MAToOneRelationDescription", 100)->classes = accessorClasses();
        if ((cls == "MAChainAccessor" || cls == "MACompositeAccessor"))
            addMetadata(g, "accessor", "MAToOneRelationDescription", 200)->classes = accessorClasses();
    }
    return g;
}
} // namespace
struct RecursiveDescriptionModel::State : std::enable_shared_from_this<State> {
    Graph graph;
    Limits limits;
    std::size_t generation = 0;
    std::set<std::string> metaIds, modelSchemaIds;
    std::map<std::string, std::shared_ptr<Memento>> mementos;
    std::map<std::string, std::shared_ptr<AccessorObject>> accessors;
    std::map<std::string, std::shared_ptr<ConditionObject>> conditions;
    struct Binding : Model {
        std::weak_ptr<State> owner;
        std::shared_ptr<Description> target;
        Binding(std::shared_ptr<State> s, std::shared_ptr<Description> d) : owner(s), target(std::move(d)) {}
        std::shared_ptr<State> state() const {
            auto s = owner.lock();
            if (!s)
                throw Error("Recursive metadata session expired");
            return s;
        }
        bool kindOf(const std::string &name) const override { return target->kindOf(name); }
        Value ref(const std::shared_ptr<Description> &d) const {
            auto s = state();
            if (!s->graph.nativeObjects.count(d->id) || s->graph.descriptions.at(d->id) != d)
                s->bind(d);
            return Value(Value::Reference{d->id});
        }
        std::shared_ptr<Description> description(const Value &v) const {
            auto s = state();
            auto &model = s->graph.resolve(v);
            auto b = dynamic_cast<Binding *>(&model);
            if (!b)
                throw Error("Metadata reference must identify a description");
            return b->target;
        }
        bool respondsToSelector(const std::string &selector) const override {
            (void)state();
            if (selector == "accessor" || selector == "accessor:" || selector == "name" ||
                selector == "name:")
                return true;
            if ((selector == "classes" || selector == "classes:") && target->isRelation())
                return true;
            if ((selector == "children" || selector == "setChildren:") && target->isContainer())
                return true;
            if ((selector == "reference" || selector == "reference:") && target->isReference())
                return true;
            if ((selector == "optionsTextual" || selector == "optionsTextual:") &&
                target->isOption())
                return true;
            return target->respondsToSelector(selector);
        }
        Value readSlot(const std::string &strategy, const std::string &name) const override {
            auto s = state();
            if (strategy != "MASelectorAccessor")
                return target->readSlot(strategy, name);
            if (name == "accessor") {
                if (target->nativeRead || target->nativeWrite)
                    throw Error("Local callbacks are not reflective accessor objects");
                if (!target->accessorObject) {
                    auto key = target->id + ".accessor";
                    std::size_t serial = 1;
                    while (s->graph.nativeObjects.count(key))
                        key = target->id + ".accessor." + std::to_string(++serial);
                    auto candidate =
                        AccessorObject::fromAccessor(key, target->effectiveAccessor(), s->limits);
                    s->bindAccessor(candidate);
                    target->accessorObject = candidate;
                }
                s->bindAccessor(target->accessorObject);
                return Value(Value::Reference{target->accessorObject->id});
            }
            if (name == "classes" && target->isRelation()) {
                Value::Array values;
                for (const auto &cls : target->classes)
                    values.push_back(Value(Value::Class{cls}));
                return Value(Value::Collection{"set", values});
            }
            if (name == "children" && target->isContainer()) {
                Value::Array values;
                for (const auto &d : target->orderedChildren())
                    values.push_back(ref(d));
                return Value(Value::Collection{"ordered", values});
            }
            if (name == "reference" && target->isReference())
                return ref(target->referenceDescription());
            if (name == "name")
                return target->property("name", s->limits);
            if (name == "optionsTextual" && target->isOption()) {
                std::size_t work = 0;
                (void)detail::valueKey(Value(target->options.values(s->limits)), s->limits, work);
                std::string result;
                bool first = true;
                for (const auto &v : target->options.values(s->limits)) {
                    if (!first)
                        result += '\r';
                    first = false;
                    result += writeString(*target->referenceDescription(), v, &s->graph, s->limits);
                }
                return Value(result);
            }
            return target->readSlot(strategy, name);
        }
        void writeSlot(const std::string &strategy, const std::string &name, Value v) override {
            auto s = state();
            if (strategy != "MASelectorAccessor") {
                target->writeSlot(strategy, name, std::move(v));
                return;
            }
            if (name == "accessor:") {
                auto ref = std::get_if<Value::Reference>(&v.data);
                if (!ref || !s->accessors.count(ref->id))
                    throw Error("Accessor requires a bound accessor object");
                target->accessorObject = s->accessors.at(ref->id);
                target->accessor.reset();
                return;
            }
            if (name == "classes:" && target->isRelation()) {
                if (!v.isCollection() || v.collectionSize() > s->limits.items)
                    throw Error("Classes require a bounded collection");
                std::vector<std::string> names;
                std::set<std::string> unique;
                for (const auto &item : v.elements()) {
                    auto cls = std::get_if<Value::Class>(&item.data);
                    if (!cls || !detail::knownClass(cls->name) || !unique.insert(cls->name).second)
                        throw Error("Classes require unique supported class values");
                    names.push_back(cls->name);
                }
                target->classes = std::move(names);
                return;
            }
            if (name == "setChildren:" && target->isContainer()) {
                if (!v.isCollection() || v.collectionSize() > s->limits.items)
                    throw Error("Children require a bounded collection");
                std::vector<std::shared_ptr<Description>> next;
                for (const auto &e : v.elements())
                    next.push_back(description(e));
                auto previous = target->children;
                target->children = std::move(next);
                try {
                    visit(*target, [](const auto &) {}, s->limits);
                } catch (...) {
                    target->children = std::move(previous);
                    throw;
                }
                return;
            }
            if (name == "reference:" && target->isReference()) {
                target->reference = description(v);
                return;
            }
            if (name == "name:") {
                if (!v.isNil() && v.data.index() != 3 && v.data.index() != 8)
                    throw Error("Name requires text or Symbol");
                target->properties["name"] = std::move(v);
                return;
            }
            if (name == "optionsTextual:" && target->isOption()) {
                std::string text;
                if (!v.isNil()) {
                    auto p = std::get_if<std::string>(&v.data);
                    if (!p)
                        throw Error("Options text requires String");
                    text = *p;
                }
                Value::Array values;
                std::size_t start = 0;
                while (start < text.size()) {
                    auto end = text.find_first_of("\r\n", start);
                    if (end == std::string::npos)
                        end = text.size();
                    values.push_back(readString(*target->referenceDescription(),
                                                text.substr(start, end - start), s->limits));
                    if (values.size() > s->limits.items)
                        throw Error("Options item limit");
                    start = end + 1;
                    if (end < text.size() && text[end] == '\r' && start < text.size() && text[start] == '\n')
                        ++start;
                }
                target->options = std::move(values);
                return;
            }
            target->writeSlot(strategy, name, std::move(v));
        }
        Value readUsing(const Description &d) const override {
            auto s = state();
            auto value = d.effectiveAccessor().read(*this, &s->graph, s->limits);
            (void)encodeValue(value, s->limits);
            return value;
        }
        void writeUsing(const Description &d, Value v) override {
            auto s = state();
            (void)encodeValue(v, s->limits);
            d.effectiveAccessor().write(*this, std::move(v), &s->graph, s->limits);
        }
    };

    struct AccessorBinding : Model {
        std::weak_ptr<State> owner;
        std::shared_ptr<AccessorObject> target;
        AccessorBinding(std::shared_ptr<State> s, std::shared_ptr<AccessorObject> a) : owner(s), target(a) {}
        std::shared_ptr<State> state() const {
            auto s = owner.lock();
            if (!s)
                throw Error("Recursive metadata session expired");
            return s;
        }
        bool kindOf(const std::string &cls) const override {
            return ClassRegistry::global().includesBehavior(target->value.className, cls);
        }
        bool respondsToSelector(const std::string &selector) const override {
            (void)state();
            const auto &cls = target->value.className;
            if ((selector == "key" || selector == "key:") &&
                (cls == "MADictionaryAccessor" || cls == "MAPropertyAccessor"))
                return true;
            if ((selector == "selector" || selector == "selector:") && cls == "MASelectorAccessor")
                return true;
            if ((selector == "next" || selector == "next:") &&
                (cls == "MAChainAccessor" || cls == "MACompositeAccessor" ||
                 cls == "MADelegatorAccessor"))
                return true;
            return (selector == "accessor" || selector == "accessor:") &&
                   (cls == "MAChainAccessor" || cls == "MACompositeAccessor");
        }
        Value readSlot(const std::string &strategy, const std::string &name) const override {
            auto s = state();
            if (strategy != "MASelectorAccessor")
                throw Error("Accessor metadata requires selector access");
            auto cls = target->value.className;
            if (name == "selector" && cls == "MASelectorAccessor" && target->value.name.empty())
                return {};
            if ((name == "key" && (cls == "MADictionaryAccessor" || cls == "MAPropertyAccessor")) ||
                (name == "selector" && cls == "MASelectorAccessor"))
                return Value(Value::Symbol{target->value.name});
            auto node = name == "next" ? target->next : name == "accessor" ? target->accessor : nullptr;
            if (!node)
                throw Error("Unsupported accessor metadata getter");
            s->bindAccessor(node);
            return Value(Value::Reference{node->id});
        }
        void writeSlot(const std::string &strategy, const std::string &name, Value value) override {
            auto s = state();
            if (strategy != "MASelectorAccessor")
                throw Error("Accessor metadata requires selector access");
            auto cls = target->value.className;
            if ((name == "key:" && (cls == "MADictionaryAccessor" || cls == "MAPropertyAccessor")) ||
                (name == "selector:" && cls == "MASelectorAccessor")) {
                std::string text;
                if (auto symbol = std::get_if<Value::Symbol>(&value.data))
                    text = symbol->utf8;
                else if (auto str = std::get_if<std::string>(&value.data))
                    text = *str;
                else
                    throw Error("Accessor name requires Symbol or String");
                auto next = target->value;
                next.name = text;
                if (name == "selector:")
                    next.writeName = text + ":";
                next.check(s->limits);
                target->value = std::move(next);
                return;
            }
            auto ref = std::get_if<Value::Reference>(&value.data);
            if (!ref || !s->accessors.count(ref->id))
                throw Error("Accessor slot requires a bound accessor");
            auto node = s->accessors.at(ref->id);
            if ((name == "next:" && (cls == "MAChainAccessor" || cls == "MACompositeAccessor" ||
                                     cls == "MADelegatorAccessor")) ||
                (name == "accessor:" &&
                 (cls == "MAChainAccessor" || cls == "MACompositeAccessor"))) {
                auto &slot = name == "next:" ? target->next : target->accessor;
                auto old = slot;
                slot = node;
                try {
                    (void)target->snapshot(s->limits);
                } catch (...) {
                    slot = old;
                    throw;
                }
                return;
            }
            throw Error("Unsupported accessor metadata setter");
        }
        Value readUsing(const Description &d) const override {
            auto s = state();
            auto v = d.effectiveAccessor().read(*this, &s->graph, s->limits);
            (void)encodeValue(v, s->limits);
            return v;
        }
        void writeUsing(const Description &d, Value v) override {
            auto s = state();
            (void)encodeValue(v, s->limits);
            d.effectiveAccessor().write(*this, std::move(v), &s->graph, s->limits);
        }
    };
    struct ConditionBinding : Model {
        std::weak_ptr<State> owner;
        std::shared_ptr<ConditionObject> target;
        ConditionBinding(std::shared_ptr<State> s, std::shared_ptr<ConditionObject> p)
            : owner(s), target(p) {}
        bool kindOf(const std::string &cls) const override {
            return ClassRegistry::global().includesBehavior(target->predicate.className, cls);
        }
        Value readUsing(const Description &) const override {
            if (owner.expired())
                throw Error("Recursive metadata session expired");
            throw Error("Upstream condition objects have no annotated fields");
        }
        void writeUsing(const Description &, Value) override {
            if (owner.expired())
                throw Error("Recursive metadata session expired");
            throw Error("Upstream condition objects have no annotated fields");
        }
    };
    void checkId(const std::string &id) const {
        if (id.empty() || id.size() > limits.bytes ||
            !std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
            throw Error("Invalid reflection identity");
    }
    std::shared_ptr<Model> bindAccessor(std::shared_ptr<AccessorObject> target) {
        if (!target)
            throw Error("Null accessor object");
        (void)target->snapshot(limits);
        std::map<std::string, std::shared_ptr<AccessorObject>> pending;
        std::set<const AccessorObject *> seen;
        std::function<void(std::shared_ptr<AccessorObject>)> walk = [&](auto a) {
            if (!seen.insert(a.get()).second)
                return;
            checkId(a->id);
            if ((pending.count(a->id) && pending.at(a->id) != a) ||
                (accessors.count(a->id) && accessors.at(a->id) != a) || graph.descriptions.count(a->id) ||
                conditions.count(a->id) || (graph.nativeObjects.count(a->id) && !accessors.count(a->id)))
                throw Error("Reflection identity collision");
            pending.emplace(a->id, a);
            if (a->accessor)
                walk(a->accessor);
            if (a->next)
                walk(a->next);
        };
        walk(target);
        std::size_t added = 0;
        for (const auto &p : pending)
            if (!accessors.count(p.first))
                ++added;
        if (graph.descriptions.size() + accessors.size() + conditions.size() + mementos.size() + added >
            limits.nodes)
            throw Error("Reflection node limit");
        for (const auto &p : pending) {
            accessors.emplace(p);
            if (!graph.nativeObjects.count(p.first))
                graph.nativeObjects.emplace(p.first,
                                            std::make_shared<AccessorBinding>(shared_from_this(), p.second));
        }
        return graph.nativeObjects.at(target->id);
    }
    std::shared_ptr<Model> bindCondition(std::shared_ptr<ConditionObject> target) {
        if (!target)
            throw Error("Null condition object");
        target->predicate.check(limits);
        checkId(target->id);
        if ((graph.nativeObjects.count(target->id) && !conditions.count(target->id)) ||
            graph.descriptions.count(target->id) || accessors.count(target->id) ||
            (conditions.count(target->id) && conditions.at(target->id) != target))
            throw Error("Reflection identity collision");
        if (!conditions.count(target->id) &&
            graph.descriptions.size() + accessors.size() + conditions.size() + mementos.size() >=
                limits.nodes)
            throw Error("Reflection node limit");
        conditions.emplace(target->id, target);
        if (!graph.nativeObjects.count(target->id))
            graph.nativeObjects.emplace(target->id,
                                        std::make_shared<ConditionBinding>(shared_from_this(), target));
        return graph.nativeObjects.at(target->id);
    }
    std::shared_ptr<Model> bindMemento(const std::string &id, std::shared_ptr<Memento> target) {
        checkId(id);
        if (!target)
            throw Error("Null memento target");
        if (graph.nativeObjects.count(id)) {
            if (mementos.count(id) && mementos.at(id) == target)
                return target;
            throw Error("Reflection identity collision");
        }
        for (const auto &p : mementos)
            if (p.second == target)
                throw Error("Memento already has a session identity");
        std::set<std::string> added;
        visit(
            *target->description(),
            [&](const Description &d) {
                if (d.id == id)
                    throw Error("Memento identity collides with its description");
                if (!graph.descriptions.count(d.id))
                    added.insert(d.id);
            },
            limits);
        if (graph.descriptions.size() + accessors.size() + conditions.size() + mementos.size() +
                added.size() + 1 >
            limits.nodes)
            throw Error("Memento node limit");
        bind(target->description());
        mementos.emplace(id, target);
        graph.nativeObjects.emplace(id, target);
        return target;
    }
    Graph bootstrap(std::shared_ptr<Description> target) {
        bind(target); // A valid input can remain bound on a failed batch; no schema is published.
        std::vector<std::shared_ptr<Description>> targets;
        visit(*target, [&](const Description &d) { targets.push_back(graph.descriptions.at(d.id)); }, limits);
        Graph batch;
        batch.format = "magritte.graph/4";
        std::size_t nextGeneration = generation;
        std::vector<std::shared_ptr<Description>> roots;
        for (const auto &d : targets) {
            auto next = reflected(*d, limits);
            auto old = next.root;
            auto prefix = "bootstrap." + std::to_string(++nextGeneration) + "." + d->id;
            auto root = next.descriptions.at(old);
            roots.push_back(root);
            if (batch.root.empty())
                batch.root = prefix;
            for (auto &p : next.descriptions) {
                p.second->id = prefix + p.first.substr(old.size());
                checkId(p.second->id);
                if (graph.nativeObjects.count(p.second->id) ||
                    !batch.descriptions.emplace(p.second->id, p.second).second)
                    throw Error("Bootstrap identity collision");
            }
            if (graph.descriptions.size() + accessors.size() + conditions.size() + mementos.size() +
                    batch.descriptions.size() >
                limits.nodes)
                throw Error("Bootstrap node limit");
        }
        // Validate the entire staged batch before any new binding becomes visible.
        (void)batch.encode(limits);
        for (const auto &root : roots)
            bind(root);
        for (const auto &p : batch.descriptions)
            metaIds.insert(p.first);
        generation = nextGeneration;
        graph.root = batch.root;
        graph.format = batch.format;
        return batch;
    }
    std::shared_ptr<Description> publish(Graph next, const std::string &targetId, const std::string &family) {
        auto root = next.descriptions.at(next.root);
        auto prefix = family + "." + std::to_string(generation + 1) + "." + targetId;
        auto old = next.root;
        std::map<std::string, std::shared_ptr<Description>> renamed;
        for (auto &p : next.descriptions) {
            p.second->id = prefix + p.first.substr(old.size());
            renamed.emplace(p.second->id, p.second);
        }
        if (graph.descriptions.size() + accessors.size() + conditions.size() + mementos.size() +
                renamed.size() >
            limits.nodes)
            throw Error("Recursive metadata node limit");
        for (const auto &p : renamed)
            if (graph.nativeObjects.count(p.first))
                throw Error("Generated metadata identity collision");
        bind(root);
        for (const auto &p : renamed)
            metaIds.insert(p.first);
        ++generation;
        graph.root = root->id;
        if (next.format == "magritte.graph/4")
            graph.format = next.format;
        return root;
    }
    explicit State(Limits l) : limits(l) {}

    std::shared_ptr<Model> bind(std::shared_ptr<Description> target) {
        if (!target)
            throw Error("Null metadata target");
        std::map<std::string, std::shared_ptr<Description>> pending;
        visit(*target, [&](const Description &d) { detail::checkDescription(d); }, limits);
        std::set<const Description *> seen;
        std::function<void(std::shared_ptr<Description>)> walk = [&](auto d) {
            if (!seen.insert(d.get()).second)
                return;
            if (d->id.empty() ||
                !std::all_of(d->id.begin(), d->id.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
                throw Error("Invalid metadata target identity");
            if ((pending.count(d->id) && pending.at(d->id) != d) ||
                (graph.descriptions.count(d->id) && graph.descriptions.at(d->id) != d))
                throw Error("Metadata target identity already bound");
            if ((graph.nativeObjects.count(d->id) && !graph.descriptions.count(d->id)) ||
                accessors.count(d->id) || conditions.count(d->id))
                throw Error("Reflection identity collision");
            pending.emplace(d->id, d);
            for (const auto &c : d->children)
                walk(c);
            if (d->isReference())
                walk(d->referenceDescription());
        };
        walk(target);
        std::size_t added = 0;
        for (const auto &p : pending)
            if (!graph.descriptions.count(p.first))
                ++added;
        if (graph.descriptions.size() + accessors.size() + conditions.size() + mementos.size() + added >
            limits.nodes)
            throw Error("Recursive metadata node limit");
        for (const auto &p : pending) {
            graph.descriptions.emplace(p);
            if (!graph.nativeObjects.count(p.first))
                graph.nativeObjects.emplace(p.first, std::make_shared<Binding>(shared_from_this(), p.second));
        }
        return graph.nativeObjects.at(target->id);
    }
};
RecursiveDescriptionModel::RecursiveDescriptionModel(Limits l) : state_(std::make_shared<State>(l)) {}
Graph &RecursiveDescriptionModel::resolver() { return state_->graph; }
std::shared_ptr<Model> RecursiveDescriptionModel::bind(std::shared_ptr<Description> d) {
    return state_->bind(std::move(d));
}
std::shared_ptr<Description> RecursiveDescriptionModel::describe(std::shared_ptr<Description> target) {
    state_->bind(target);
    return state_->publish(expanded(*target, state_->limits), target->id, "recursive");
}
std::shared_ptr<Description>
RecursiveDescriptionModel::describeReflective(std::shared_ptr<Description> target) {
    state_->bind(target);
    return state_->publish(reflected(*target, state_->limits), target->id, "reflective");
}
std::shared_ptr<Model> RecursiveDescriptionModel::bind(std::shared_ptr<AccessorObject> a) {
    return state_->bindAccessor(a);
}
std::shared_ptr<Model> RecursiveDescriptionModel::bind(std::shared_ptr<ConditionObject> c) {
    return state_->bindCondition(c);
}
std::shared_ptr<Description> RecursiveDescriptionModel::describe(std::shared_ptr<AccessorObject> a) {
    state_->bindAccessor(a);
    return state_->publish(accessorMetadata(*a), a->id, "reflective");
}
std::shared_ptr<Description> RecursiveDescriptionModel::describe(std::shared_ptr<ConditionObject> c) {
    state_->bindCondition(c);
    return state_->publish(objectMetadata(c->predicate.className), c->id, "reflective");
}

Graph RecursiveDescriptionModel::schemas() const {
    Graph result;
    result.format = state_->graph.format;
    result.root = state_->graph.root;
    std::set<const Description *> seen, active, composed;
    std::size_t work = 0;
    auto tick = [&](std::size_t depth) {
        if (++work > state_->limits.visits || depth > state_->limits.depth)
            throw Error("Schema traversal limit");
    };
    std::function<void(std::shared_ptr<Description>, std::size_t)> composition = [&](auto d, auto depth) {
        tick(depth);
        if (active.count(d.get()))
            throw Error("Cyclic schema composition");
        if (composed.count(d.get()))
            return;
        active.insert(d.get());
        for (const auto &c : d->orderedChildren())
            composition(c, depth + 1);
        active.erase(d.get());
        composed.insert(d.get());
    };
    std::function<void(std::shared_ptr<Description>, std::size_t)> include = [&](auto d, auto depth) {
        tick(depth);
        if (!seen.insert(d.get()).second)
            return;
        composition(d, 0);
        state_->checkId(d->id);
        if (result.descriptions.count(d->id) && result.descriptions.at(d->id) != d)
            throw Error("Schema identity collision");
        result.descriptions[d->id] = d;
        if (result.descriptions.size() > state_->limits.nodes)
            throw Error("Schema node limit");
        for (const auto &c : d->orderedChildren())
            include(c, depth + 1);
        if (d->isReference())
            include(d->referenceDescription(), depth + 1);
    };
    for (const auto &id : state_->metaIds)
        include(state_->graph.descriptions.at(id), 0);
    for (const auto &id : state_->modelSchemaIds)
        include(state_->graph.descriptions.at(id), 0);
    return result;
}
std::shared_ptr<Model> RecursiveDescriptionModel::bind(const std::string &id,
                                                       std::shared_ptr<Memento> target) {
    return state_->bindMemento(id, std::move(target));
}
std::shared_ptr<Description> RecursiveDescriptionModel::describe(const std::string &id,
                                                                 std::shared_ptr<Memento> target) {
    state_->bindMemento(id, target);
    auto d = target->description();
    state_->modelSchemaIds.insert(d->id);
    state_->graph.root = d->id;
    state_->graph.format = "magritte.graph/4";
    return d;
}
Graph RecursiveDescriptionModel::bootstrap(std::shared_ptr<Description> target) {
    return state_->bootstrap(std::move(target));
}
Graph RecursiveDescriptionModel::bootstrap(const std::string &id, std::shared_ptr<Memento> target) {
    state_->bindMemento(id, target);
    return state_->bootstrap(target->description());
}
} // namespace magritte
